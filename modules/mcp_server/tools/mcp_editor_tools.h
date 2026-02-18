/**************************************************************************/
/*  mcp_editor_tools.h                                                    */
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
#include "core/variant/dictionary.h"

class MCPToolRegistry;

class MCPEditorTools {
public:
	// Register all editor/project tools into the registry.
	static void register_tools(MCPToolRegistry *p_registry);

	// Tool handlers -- each takes a Dictionary of arguments and returns
	// an MCP tool result Dictionary.
	static Dictionary handle_get_overview(const Dictionary &p_args);
	static Dictionary handle_get_input_map(const Dictionary &p_args);
	static Dictionary handle_reimport(const Dictionary &p_args);
	static Dictionary handle_scan_filesystem(const Dictionary &p_args);
	static Dictionary handle_get_uid(const Dictionary &p_args);
	static Dictionary handle_resolve_uid(const Dictionary &p_args);
	static Dictionary handle_execute_script(const Dictionary &p_args);
	static Dictionary handle_get_editor_screenshot(const Dictionary &p_args);

private:
	// Helper: recursively list files from a DirAccess.
	static void _list_files_recursive(const String &p_dir,
			const String &p_extension, Vector<String> &r_files);

	// Helper: clean up the MCP temp directory.
	static void _cleanup_mcp_temp();

	// Main-thread deferred helpers for thread-safe tool execution.
	// These are called via call_deferred when the tool handler runs on
	// the poll thread (threaded mode). They store results in the static
	// _deferred_result and post _deferred_semaphore when complete.
	static void _do_execute_script_main(const String &p_full_script);
	static void _do_get_screenshot_main();
};
