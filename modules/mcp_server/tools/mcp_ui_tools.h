/**************************************************************************/
/*  mcp_ui_tools.h                                                        */
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

#include "core/variant/dictionary.h"

class MCPToolRegistry;

class MCPUITools {
public:
	// Register all UI navigation/interaction tools into the registry.
	static void register_tools(MCPToolRegistry *p_registry);

	// --- Read-Only Inspection ---
	static Dictionary handle_get_control_info(const Dictionary &p_args);
	static Dictionary handle_get_text(const Dictionary &p_args);
	static Dictionary handle_get_range_value(const Dictionary &p_args);
	static Dictionary handle_get_options(const Dictionary &p_args);
	static Dictionary handle_get_tabs(const Dictionary &p_args);

	// --- Write/Interaction ---
	static Dictionary handle_set_text(const Dictionary &p_args);
	static Dictionary handle_set_range_value(const Dictionary &p_args);
	static Dictionary handle_select_option(const Dictionary &p_args);
	static Dictionary handle_set_tab(const Dictionary &p_args);
	static Dictionary handle_set_checked(const Dictionary &p_args);
	static Dictionary handle_focus(const Dictionary &p_args);

private:
	// Helper: get the MCPDebuggerBridge pointer, or return nullptr.
	static class MCPDebuggerBridge *_get_bridge();

	// Helper: check game is running, return error dict if not.
	// Returns empty Dictionary if game IS running (meaning: proceed).
	static Dictionary _require_game_running();

	// Helper: validate node_path contains only safe characters.
	static bool _validate_node_path(const String &p_path);

	// Helper: send a UI interact request via bridge and return the result.
	static Dictionary _send_ui_request(const String &p_action,
			const String &p_node_path, const Dictionary &p_params);
};
