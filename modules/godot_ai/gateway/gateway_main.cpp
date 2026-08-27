/**************************************************************************/
/*  gateway_main.cpp                                                      */
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

#include "gateway_main.h"

#include "relay.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

bool godot_ai_gateway_requested(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--godot-ai-stdio") == 0) {
			return true;
		}
	}
	return false;
}

int godot_ai_gateway_main(int argc, char **argv) {
	// The selector flag is ours, not the option parser's; everything after it is the
	// gateway's own command line.
	std::vector<char *> arguments;
	arguments.push_back(argv[0]);
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--godot-ai-stdio") != 0) {
			arguments.push_back(argv[i]);
		}
	}

	godot_ai::RelayOptions options;
	std::string error;
	bool exit_immediately = false;
	std::string immediate_output;

	if (!godot_ai::relay_parse_options((int)arguments.size(), arguments.data(), options, error, exit_immediately, immediate_output)) {
		// Usage diagnostics never touch stdout: a client is already reading it as a
		// protocol stream.
		fprintf(stderr, "godot --godot-ai-stdio: %s\n", error.c_str());
		fprintf(stderr, "Run 'godot --godot-ai-stdio --help' for usage.\n");
		return 2;
	}
	if (exit_immediately) {
		fputs(immediate_output.c_str(), stderr);
		return 0;
	}

	// POSIX forgave a missing initialize; Windows would not have - without WSAStartup
	// every socket call fails with WSANOTINITIALISED.
	if (!godot_ai::platform::initialize(error)) {
		fprintf(stderr, "godot --godot-ai-stdio: %s\n", error.c_str());
		return 2;
	}

	int status;
	{
		godot_ai::Relay relay(options);
		if (!options.call_tool.empty() || options.list_tools || options.batch) {
			status = relay.run_one_shot();
		} else {
			status = relay.run();
		}
	}
	godot_ai::platform::finalize();
	return status;
}
