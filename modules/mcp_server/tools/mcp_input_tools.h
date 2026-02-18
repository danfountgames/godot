/**************************************************************************/
/*  mcp_input_tools.h                                                     */
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
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"

class MCPToolRegistry;

class MCPInputTools {
public:
	// Register all input simulation tools into the registry.
	static void register_tools(MCPToolRegistry *p_registry);

	// Tool handlers.
	static Dictionary handle_send_key(const Dictionary &p_args);
	static Dictionary handle_send_joypad(const Dictionary &p_args);
	static Dictionary handle_type_text(const Dictionary &p_args);
	static Dictionary handle_send_input_sequence(const Dictionary &p_args);
	static Dictionary handle_get_held_inputs(const Dictionary &p_args);

	// Modifier flag constants (bitmask).
	enum ModifierFlags {
		MOD_SHIFT = 1,
		MOD_CTRL = 2,
		MOD_ALT = 4,
		MOD_META = 8,
	};

private:
	// Helper: get the MCPDebuggerBridge pointer.
	static class MCPDebuggerBridge *_get_bridge();

	// Helper: check game is running, return error dict if not.
	static Dictionary _require_game_running();

	// Helper: parse modifier array into bitmask.
	static int _parse_modifier_flags(const Array &p_modifiers);

	// Helper: validate key name. Returns true if valid.
	static bool _validate_key_name(const String &p_key);

	// Helper: validate joypad button name. Returns true if valid.
	static bool _validate_button_name(const String &p_button);

	// Helper: validate joypad axis name. Returns true if valid.
	static bool _validate_axis_name(const String &p_axis);

	// Helper: get list of valid key names for error messages.
	static String _get_valid_key_names_hint();

	// Helper: get list of valid button names for error messages.
	static String _get_valid_button_names_hint();

	// Helper: get list of valid axis names for error messages.
	static String _get_valid_axis_names_hint();

	// Lookup tables (lazy-initialized).
	static bool _tables_initialized;
	static void _ensure_lookup_tables();
	static HashMap<String, int> key_name_map;
	static HashMap<String, int> button_name_map;
	static HashMap<String, int> axis_name_map;
};
