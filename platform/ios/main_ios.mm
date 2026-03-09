/**************************************************************************/
/*  main_ios.mm                                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#import "os_ios.h"

#include "core/profiling/profiling.h"
#include "core/string/print_string.h"
#import "drivers/apple_embedded/godot_app_delegate.h"
#import "drivers/apple_embedded/main_utilities.h"
#include "main/main.h"
#include "modules/modules_enabled.gen.h"

#import <UIKit/UIKit.h>
#include <cstdio>

static OS_IOS *os = nullptr;

// Bridge Godot print output to a log file in the app's Documents directory
// AND NSLog so it appears in idevicesyslog/Xcode console.
static FILE *_log_file = nullptr;

static void _file_print_handler(void *p_userdata, const String &p_string, bool p_error, bool p_rich) {
	NSLog(@"%s%@", p_error ? "[ERR] " : "", [NSString stringWithUTF8String:p_string.utf8().get_data()]);
	if (_log_file) {
		fprintf(_log_file, "%s%s\n", p_error ? "[ERR] " : "", p_string.utf8().get_data());
		fflush(_log_file);
	}
}

static PrintHandlerList _nslog_handler;
static bool _nslog_handler_inited = false;

int apple_embedded_main(int argc, char **argv) {
#if defined(VULKAN_ENABLED)
	//MoltenVK - enable full component swizzling support
	setenv("MVK_CONFIG_FULL_IMAGE_VIEW_SWIZZLE", "1", 1);
#endif

	change_to_launch_dir(argv);

	os = new OS_IOS();

	// Open log file in Documents directory.
	NSString *docs = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
	NSString *logPath = [docs stringByAppendingPathComponent:@"godot_log.txt"];
	_log_file = fopen([logPath UTF8String], "w");
	if (_log_file) {
		fprintf(_log_file, "=== Godot LiveMount log started ===\n");
		fflush(_log_file);
	}

	// Bridge all Godot output to NSLog + log file.
	_nslog_handler.printfunc = _file_print_handler;
	_nslog_handler.userdata = nullptr;
	_nslog_handler_inited = true;
	add_print_handler(&_nslog_handler);

	// We must override main when testing is enabled
	TEST_MAIN_OVERRIDE

	char *fargv[64];
	argc = process_args(argc, argv, fargv);

	// Strip bare "--" arguments that devicectl injects as a separator.
	// These cause ERR_INVALID_PARAMETER in Godot's argument parser.
	{
		int write = 0;
		for (int i = 0; i < argc; i++) {
			if (strcmp(fargv[i], "--") != 0) {
				fargv[write++] = fargv[i];
			}
		}
		argc = write;
		fargv[argc] = nullptr;
	}

#ifdef MODULE_LIVEMOUNT_ENABLED
	// In LiveMount builds, automatically inject --livemount flag
	// to boot into the custom shell instead of the standard editor UI.
	bool has_livemount_flag = false;
	for (int i = 0; i < argc; i++) {
		if (strcmp(fargv[i], "--livemount") == 0) {
			has_livemount_flag = true;
			break;
		}
	}
	if (!has_livemount_flag) {
		fargv[argc] = (char *)"--livemount";
		argc++;
		fargv[argc] = nullptr;
	}

	// Inject --main-pack pointing to the PCK in the app bundle so
	// globals->setup() can find project.godot and load project settings.
	static char main_pack_buf[1024];
	NSString *pckPath = [[[NSBundle mainBundle] resourcePath] stringByAppendingPathComponent:@"godot_ios.pck"];
	if ([[NSFileManager defaultManager] fileExistsAtPath:pckPath]) {
		snprintf(main_pack_buf, sizeof(main_pack_buf), "%s", [pckPath UTF8String]);
		fargv[argc] = (char *)"--main-pack";
		argc++;
		fargv[argc] = main_pack_buf;
		argc++;
		fargv[argc] = nullptr;
	}
#endif // MODULE_LIVEMOUNT_ENABLED

	godot_init_profiler();

	Error err = Main::setup(fargv[0], argc - 1, &fargv[1], false);

	if (err != OK) {
		if (err == ERR_HELP) { // Returned by --help and --version, so success.
			return EXIT_SUCCESS;
		}
		NSLog(@"[LiveMount] Main::setup FAILED with error %d", (int)err);
		return EXIT_FAILURE;
	}

	os->initialize_modules();

	return os->get_exit_code();
}

void apple_embedded_finish() {
	Main::cleanup();
	godot_cleanup_profiler();
	delete os;
}
