/**************************************************************************/
/*  mcp_testing_tools.h                                                   */
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

#pragma once

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class MCPToolRegistry;
struct ProgressContext;

class MCPTestingTools {
public:
	static void register_tools(MCPToolRegistry *p_registry);

	// Public API for MCPTestPanel and other consumers.
	static Vector<String> collect_test_files(const String &p_dir_path);
	static Vector<String> extract_test_methods(const String &p_path);
	static Dictionary validate_single_file(const String &p_path);
	static void copy_runner_files_to_project();
	static void cleanup_runner_files();

private:
	// Tool handlers.
	static Dictionary handle_run(const Dictionary &p_args);
	static Dictionary handle_list(const Dictionary &p_args);
	static class MCPDebuggerBridge *_get_bridge();
	static Dictionary _validate_single_file_for_tests(const String &p_path);
	static Dictionary _build_compile_error_response(const Array &p_compile_results, int p_run_number);
	static Dictionary _build_test_results_response(const Dictionary &p_raw_result, bool p_verbose);

	// From MCPGDScriptTools:
	static Dictionary handle_check_script(const Dictionary &p_args);
	static Dictionary handle_check_all_scripts(const Dictionary &p_args);
	static Dictionary handle_check_all_scripts_with_progress(const Dictionary &p_args, ProgressContext *p_ctx);
	static void _find_gd_files_recursive(const String &p_dir, Vector<String> &r_files);
};
