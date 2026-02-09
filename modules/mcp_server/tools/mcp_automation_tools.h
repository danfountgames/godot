/**************************************************************************/
/*  mcp_automation_tools.h                                                */
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

class MCPAutomationTools {
public:
	// Register all automation tools into the registry.
	static void register_tools(MCPToolRegistry *p_registry);

	// Tool handlers.
	static Dictionary handle_send_input(const Dictionary &p_args);
	static Dictionary handle_click_control(const Dictionary &p_args);
	static Dictionary handle_evaluate(const Dictionary &p_args);
	static Dictionary handle_wait_frames(const Dictionary &p_args);
	static Dictionary handle_get_screenshot(const Dictionary &p_args);

private:
	// Helper: get the debugger bridge pointer.
	static class MCPDebuggerBridge *_get_bridge();

	// Helper: check game is running.
	static Dictionary _require_game_running();

	// Helper: validate input action exists in InputMap.
	static bool _action_exists(const String &p_action);

	// Helper: find similar action names for "did you mean?" suggestions.
	static Vector<String> _find_similar_actions(const String &p_action);
};
