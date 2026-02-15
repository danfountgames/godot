/**************************************************************************/
/*  mcp_input_tools.cpp                                                   */
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

#include "mcp_input_tools.h"

#include "../mcp_debugger_bridge.h"
#include "../mcp_protocol.h"
#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/object/script_language.h"

// ============================================================================
// Static Lookup Tables
// ============================================================================

bool MCPInputTools::_tables_initialized = false;
HashMap<String, int> MCPInputTools::key_name_map;
HashMap<String, int> MCPInputTools::button_name_map;
HashMap<String, int> MCPInputTools::axis_name_map;

void MCPInputTools::_ensure_lookup_tables() {
	if (_tables_initialized) {
		return;
	}
	_tables_initialized = true;

	// --- Key name map (string -> Godot Key enum integer value) ---
	// Letters a-z.
	for (int i = 0; i < 26; i++) {
		char c = 'a' + i;
		key_name_map[String::chr(c)] = 65 + i; // KEY_A = 65
	}

	// Digits 0-9.
	for (int i = 0; i < 10; i++) {
		char c = '0' + i;
		key_name_map[String::chr(c)] = 48 + i; // KEY_0 = 48
	}

	// Function keys f1-f12.
	for (int i = 1; i <= 12; i++) {
		key_name_map["f" + itos(i)] = 4194332 + (i - 1); // KEY_F1 = 4194332
	}

	// Special keys.
	key_name_map["space"] = 32;
	key_name_map["enter"] = 4194309;
	key_name_map["return"] = 4194309;
	key_name_map["escape"] = 4194305;
	key_name_map["esc"] = 4194305;
	key_name_map["tab"] = 4194306;
	key_name_map["backspace"] = 4194308;
	key_name_map["insert"] = 4194311;
	key_name_map["delete"] = 4194312;
	key_name_map["home"] = 4194313;
	key_name_map["end"] = 4194314;
	key_name_map["pageup"] = 4194315;
	key_name_map["page_up"] = 4194315;
	key_name_map["pagedown"] = 4194316;
	key_name_map["page_down"] = 4194316;

	// Arrow keys.
	key_name_map["left"] = 4194319;
	key_name_map["right"] = 4194321;
	key_name_map["up"] = 4194320;
	key_name_map["down"] = 4194322;

	// Modifier keys.
	key_name_map["shift"] = 4194325;
	key_name_map["ctrl"] = 4194326;
	key_name_map["control"] = 4194326;
	key_name_map["alt"] = 4194327;
	key_name_map["meta"] = 4194328;
	key_name_map["super"] = 4194328;

	// Misc.
	key_name_map["capslock"] = 4194329;
	key_name_map["caps_lock"] = 4194329;
	key_name_map["numlock"] = 4194330;
	key_name_map["num_lock"] = 4194330;
	key_name_map["scrolllock"] = 4194331;
	key_name_map["scroll_lock"] = 4194331;
	key_name_map["pause"] = 4194310;
	key_name_map["print_screen"] = 4194307;
	key_name_map["printscreen"] = 4194307;

	// Punctuation / symbols.
	key_name_map["minus"] = 45;
	key_name_map["equal"] = 61;
	key_name_map["bracketleft"] = 91;
	key_name_map["bracketright"] = 93;
	key_name_map["backslash"] = 92;
	key_name_map["semicolon"] = 59;
	key_name_map["apostrophe"] = 39;
	key_name_map["quoteleft"] = 96; // Backtick.
	key_name_map["comma"] = 44;
	key_name_map["period"] = 46;
	key_name_map["slash"] = 47;

	// --- Joypad button name map (string -> JoyButton enum integer value) ---
	button_name_map["a"] = 0;
	button_name_map["b"] = 1;
	button_name_map["x"] = 2;
	button_name_map["y"] = 3;
	button_name_map["back"] = 4;
	button_name_map["guide"] = 5;
	button_name_map["start"] = 6;
	button_name_map["left_stick"] = 7;
	button_name_map["right_stick"] = 8;
	button_name_map["left_shoulder"] = 9;
	button_name_map["lb"] = 9;
	button_name_map["right_shoulder"] = 10;
	button_name_map["rb"] = 10;
	button_name_map["dpad_up"] = 11;
	button_name_map["dpad_down"] = 12;
	button_name_map["dpad_left"] = 13;
	button_name_map["dpad_right"] = 14;

	// --- Joypad axis name map (string -> JoyAxis enum integer value) ---
	axis_name_map["left_x"] = 0;
	axis_name_map["left_y"] = 1;
	axis_name_map["right_x"] = 2;
	axis_name_map["right_y"] = 3;
	axis_name_map["trigger_left"] = 4;
	axis_name_map["lt"] = 4;
	axis_name_map["trigger_right"] = 5;
	axis_name_map["rt"] = 5;
}

// ============================================================================
// Tool Registration
// ============================================================================

void MCPInputTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// runtime/input/send_key
	{
		Dictionary props;
		props["key"] = make_prop("string",
				"Key name matching Godot's Key enum (e.g., 'A', 'space', 'escape', "
				"'f1', 'shift', 'ctrl', 'up', 'down', 'enter'). Case-insensitive.");
		props["pressed"] = make_prop("boolean",
				"Press (true) or release (false) the key (default: true)");
		props["hold_frames"] = make_prop("integer",
				"Frames to hold before auto-release. 0 = stay held. (default: 1, max: 600)");
		props["modifiers"] = make_prop("array",
				"Modifier keys: 'shift', 'ctrl', 'alt', 'meta'. Example: ['ctrl', 'shift']");
		props["echo"] = make_prop("boolean",
				"Whether this is a key repeat event (default: false)");
		Array required;
		required.push_back("key");
		p_registry->register_tool(
				"runtime/input/send_key", "Send Key Input",
				"Send a keyboard key event to the running game. Supports modifier combos "
				"(Ctrl+Shift+S), hold duration, and all Godot Key enum names. Use for games "
				"that bind raw keys rather than input actions. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPInputTools::handle_send_key));
	}

	// runtime/input/send_joypad
	{
		Dictionary props;
		props["type"] = make_prop("string",
				"Event type: 'button' or 'axis'");
		props["button"] = make_prop("string",
				"Joypad button name (for type='button'). Values: 'a', 'b', 'x', 'y', "
				"'back', 'guide', 'start', 'left_stick', 'right_stick', 'left_shoulder', "
				"'right_shoulder', 'dpad_up', 'dpad_down', 'dpad_left', 'dpad_right'. "
				"Case-insensitive.");
		props["axis"] = make_prop("string",
				"Joypad axis name (for type='axis'). Values: 'left_x', 'left_y', "
				"'right_x', 'right_y', 'trigger_left', 'trigger_right'. Case-insensitive.");
		props["value"] = make_prop("number",
				"Axis value from -1.0 to 1.0 (for sticks) or 0.0 to 1.0 (for triggers). "
				"Default: 1.0 for buttons.");
		props["pressed"] = make_prop("boolean",
				"Press (true) or release (false) the button (for type='button'). Default: true.");
		props["hold_frames"] = make_prop("integer",
				"Frames to hold before auto-release/reset. 0 = stay held. Default: 1. Max: 600.");
		props["device"] = make_prop("integer",
				"Joypad device index. Default: 0.");
		Array required;
		required.push_back("type");
		p_registry->register_tool(
				"runtime/input/send_joypad", "Send Joypad Input",
				"Send a gamepad button press or analog axis value to the running game. "
				"Supports all standard gamepad buttons and axes. No physical controller "
				"required. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPInputTools::handle_send_joypad));
	}

	// runtime/input/type_text
	{
		Dictionary props;
		props["text"] = make_prop("string",
				"The text to type. Each character is sent as a key press+release pair.");
		props["interval_frames"] = make_prop("integer",
				"Frames to wait between each character. Default: 0 (as fast as possible). Max: 60.");
		Array required;
		required.push_back("text");
		p_registry->register_tool(
				"runtime/input/type_text", "Type Text",
				"Type a string of characters into the running game. Each character is sent "
				"as an InputEventKey press+release pair with proper Unicode handling. Useful "
				"for filling LineEdit and TextEdit controls. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPInputTools::handle_type_text));
	}

	// runtime/input/send_input_sequence
	{
		Dictionary props;
		// steps is an array of objects -- represented as array type in the schema.
		Dictionary steps_prop;
		steps_prop["type"] = "array";
		steps_prop["description"] = "Ordered list of input steps. Each step is an object with: "
									"type ('key','action','joypad_button','joypad_axis','wait'), "
									"plus type-specific fields (key, action, button, axis, value, "
									"pressed, modifiers, frames).";
		Dictionary step_items;
		step_items["type"] = "object";
		steps_prop["items"] = step_items;
		props["steps"] = steps_prop;
		Array required;
		required.push_back("steps");
		p_registry->register_tool(
				"runtime/input/send_input_sequence", "Send Input Sequence",
				"Execute a timed sequence of input steps atomically on the game side. "
				"Each step can be a key press, action, joypad event, or a wait. Holds and "
				"waits run correctly with frame-level precision. Max 50 steps, 1800 total "
				"wait frames. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPInputTools::handle_send_input_sequence));
	}

	// runtime/input/get_held_inputs
	{
		Dictionary props;
		props["release_all"] = make_prop("boolean",
				"If true, release all currently held inputs before returning. Default: false.");
		Array required;
		p_registry->register_tool(
				"runtime/input/get_held_inputs", "Get Held Inputs",
				"Query the current held-input state on the game side. Shows all keys, "
				"actions, joypad buttons and axes currently held by MCP input injection. "
				"Set release_all=true for emergency reset. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPInputTools::handle_get_held_inputs));
	}
}

// ============================================================================
// Common Helpers
// ============================================================================

MCPDebuggerBridge *MCPInputTools::_get_bridge() {
	return MCPDebuggerBridge::get_singleton();
}

Dictionary MCPInputTools::_require_game_running() {
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Internal error: debugger bridge not available.");
	}
	if (!bridge->is_game_running()) {
		return make_tool_error(
				"No game is currently running.\n\n"
				"If the game stopped unexpectedly, check runtime/get_errors for runtime errors.\n"
				"To start a game: runtime/run_project (main scene) or runtime/run_scene (specific scene).\n"
				"To check status: runtime/get_status (includes stop_reason when stopped).");
	}
	return Dictionary();
}

int MCPInputTools::_parse_modifier_flags(const Array &p_modifiers) {
	int flags = 0;
	for (int i = 0; i < p_modifiers.size(); i++) {
		String mod = String(p_modifiers[i]).to_lower().strip_edges();
		if (mod == "shift") {
			flags |= MOD_SHIFT;
		} else if (mod == "ctrl" || mod == "control") {
			flags |= MOD_CTRL;
		} else if (mod == "alt") {
			flags |= MOD_ALT;
		} else if (mod == "meta" || mod == "super") {
			flags |= MOD_META;
		}
	}
	return flags;
}

bool MCPInputTools::_validate_key_name(const String &p_key) {
	_ensure_lookup_tables();
	return key_name_map.has(p_key.to_lower().strip_edges());
}

bool MCPInputTools::_validate_button_name(const String &p_button) {
	_ensure_lookup_tables();
	return button_name_map.has(p_button.to_lower().strip_edges());
}

bool MCPInputTools::_validate_axis_name(const String &p_axis) {
	_ensure_lookup_tables();
	return axis_name_map.has(p_axis.to_lower().strip_edges());
}

String MCPInputTools::_get_valid_key_names_hint() {
	return "Valid key names include: a-z, 0-9, f1-f12, space, enter, escape, tab, "
		   "backspace, up, down, left, right, shift, ctrl, alt, meta, delete, home, "
		   "end, insert, pageup, pagedown, pause, print_screen, capslock, numlock, "
		   "minus, equal, comma, period, slash, semicolon, apostrophe, backslash, "
		   "bracketleft, bracketright, quoteleft";
}

String MCPInputTools::_get_valid_button_names_hint() {
	return "Valid button names: a, b, x, y, back, guide, start, left_stick, "
		   "right_stick, left_shoulder (lb), right_shoulder (rb), dpad_up, "
		   "dpad_down, dpad_left, dpad_right";
}

String MCPInputTools::_get_valid_axis_names_hint() {
	return "Valid axis names: left_x, left_y, right_x, right_y, "
		   "trigger_left (lt), trigger_right (rt)";
}

// ============================================================================
// Tool Handler: runtime/input/send_key
// ============================================================================

Dictionary MCPInputTools::handle_send_key(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String key = p_args.get("key", "");
	if (key.is_empty()) {
		return make_tool_error(
				"Missing required parameter: key\n\n"
				"Provide a key name such as 'space', 'a', 'enter', 'f1'.\n" +
				_get_valid_key_names_hint());
	}

	String key_lower = key.to_lower().strip_edges();

	if (!_validate_key_name(key_lower)) {
		// Build "did you mean?" suggestions.
		_ensure_lookup_tables();
		String suggestions;
		for (const KeyValue<String, int> &kv : key_name_map) {
			if (kv.key.contains(key_lower) || key_lower.contains(kv.key)) {
				suggestions += "  " + kv.key + "\n";
			}
		}

		String error_text = "Unknown key name: '" + key + "'.\n\n";
		if (!suggestions.is_empty()) {
			error_text += "Did you mean one of these?\n" + suggestions + "\n";
		}
		error_text += _get_valid_key_names_hint();
		return make_tool_error(error_text);
	}

	bool pressed = (bool)p_args.get("pressed", true);
	int hold_frames = (int)p_args.get("hold_frames", 1);
	bool echo = (bool)p_args.get("echo", false);

	// Clamp hold_frames.
	hold_frames = CLAMP(hold_frames, 0, 600);

	// Parse modifiers.
	int modifier_flags = 0;
	if (p_args.has("modifiers")) {
		Array modifiers = p_args["modifiers"];
		modifier_flags = _parse_modifier_flags(modifiers);
	}

	// Send to game via bridge.
	MCPDebuggerBridge *bridge = _get_bridge();
	Dictionary result = bridge->send_inject_key(key_lower, pressed, hold_frames, modifier_flags, echo);

	if (!(bool)result.get("success", false)) {
		return make_tool_error(
				"Failed to send key '" + key + "': " +
				String(result.get("error", result.get("message", "Unknown error"))));
	}

	// Build response.
	String mod_text;
	if (modifier_flags & MOD_SHIFT) {
		mod_text += "shift+";
	}
	if (modifier_flags & MOD_CTRL) {
		mod_text += "ctrl+";
	}
	if (modifier_flags & MOD_ALT) {
		mod_text += "alt+";
	}
	if (modifier_flags & MOD_META) {
		mod_text += "meta+";
	}

	String text = "Key '" + mod_text + key_lower + "' " +
			(pressed ? "pressed" : "released") +
			" (hold: " + itos(hold_frames) + " frames)";
	if (echo) {
		text += " [echo]";
	}

	Dictionary structured;
	structured["success"] = true;
	structured["key"] = key_lower;
	structured["pressed"] = pressed;
	structured["hold_frames"] = hold_frames;
	structured["modifier_flags"] = modifier_flags;
	structured["echo"] = echo;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool Handler: runtime/input/send_joypad
// ============================================================================

Dictionary MCPInputTools::handle_send_joypad(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String type = p_args.get("type", "");
	if (type.is_empty()) {
		return make_tool_error(
				"Missing required parameter: type\n\n"
				"Provide 'button' or 'axis'.");
	}

	type = type.to_lower().strip_edges();
	int device = (int)p_args.get("device", 0);
	device = CLAMP(device, 0, 7);
	int hold_frames = (int)p_args.get("hold_frames", 1);
	hold_frames = CLAMP(hold_frames, 0, 600);

	MCPDebuggerBridge *bridge = _get_bridge();

	if (type == "button") {
		String button = p_args.get("button", "");
		if (button.is_empty()) {
			return make_tool_error(
					"Missing parameter: button\n\n"
					"When type='button', provide a button name.\n" +
					_get_valid_button_names_hint());
		}

		String button_lower = button.to_lower().strip_edges();
		if (!_validate_button_name(button_lower)) {
			String error_text = "Unknown joypad button: '" + button + "'.\n\n" +
					_get_valid_button_names_hint();
			return make_tool_error(error_text);
		}

		bool pressed = (bool)p_args.get("pressed", true);

		Dictionary result = bridge->send_inject_joypad_button(button_lower, pressed, hold_frames, device);

		if (!(bool)result.get("success", false)) {
			return make_tool_error(
					"Failed to send joypad button '" + button + "': " +
					String(result.get("error", result.get("message", "Unknown error"))));
		}

		String text = "Joypad button '" + button_lower + "' " +
				(pressed ? "pressed" : "released") +
				" on device " + itos(device) +
				" (hold: " + itos(hold_frames) + " frames)";

		Dictionary structured;
		structured["success"] = true;
		structured["type"] = "button";
		structured["button"] = button_lower;
		structured["pressed"] = pressed;
		structured["hold_frames"] = hold_frames;
		structured["device"] = device;

		return make_tool_result(text, structured);

	} else if (type == "axis") {
		String axis = p_args.get("axis", "");
		if (axis.is_empty()) {
			return make_tool_error(
					"Missing parameter: axis\n\n"
					"When type='axis', provide an axis name.\n" +
					_get_valid_axis_names_hint());
		}

		String axis_lower = axis.to_lower().strip_edges();
		if (!_validate_axis_name(axis_lower)) {
			String error_text = "Unknown joypad axis: '" + axis + "'.\n\n" +
					_get_valid_axis_names_hint();
			return make_tool_error(error_text);
		}

		float value = (float)(double)p_args.get("value", 1.0);
		// Clamp based on axis type.
		_ensure_lookup_tables();
		int axis_idx = axis_name_map[axis_lower];
		if (axis_idx >= 4) {
			// Triggers: 0.0 to 1.0.
			value = CLAMP(value, 0.0f, 1.0f);
		} else {
			// Sticks: -1.0 to 1.0.
			value = CLAMP(value, -1.0f, 1.0f);
		}

		Dictionary result = bridge->send_inject_joypad_axis(axis_lower, value, hold_frames, device);

		if (!(bool)result.get("success", false)) {
			return make_tool_error(
					"Failed to send joypad axis '" + axis + "': " +
					String(result.get("error", result.get("message", "Unknown error"))));
		}

		String text = "Joypad axis '" + axis_lower + "' set to " +
				String::num(value, 2) + " on device " + itos(device) +
				" (hold: " + itos(hold_frames) + " frames)";

		Dictionary structured;
		structured["success"] = true;
		structured["type"] = "axis";
		structured["axis"] = axis_lower;
		structured["value"] = value;
		structured["hold_frames"] = hold_frames;
		structured["device"] = device;

		return make_tool_result(text, structured);

	} else {
		return make_tool_error(
				"Invalid type: '" + type + "'.\n\n"
				"Must be 'button' or 'axis'.");
	}
}

// ============================================================================
// Tool Handler: runtime/input/type_text
// ============================================================================

Dictionary MCPInputTools::handle_type_text(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String text = p_args.get("text", "");
	if (text.is_empty()) {
		return make_tool_error("Missing required parameter: text");
	}

	if (text.length() > 1000) {
		return make_tool_error(
				"Text too long: " + itos(text.length()) + " characters.\n\n"
				"Maximum text length is 1000 characters per call.");
	}

	int interval_frames = (int)p_args.get("interval_frames", 0);
	interval_frames = CLAMP(interval_frames, 0, 60);

	MCPDebuggerBridge *bridge = _get_bridge();

	// Generous timeout: text.length * max(interval_frames, 1) * 200ms upper bound.
	int timeout_ms = MAX(text.length() * MAX(interval_frames, 1) * 200, 10000);
	timeout_ms = MIN(timeout_ms, 60000); // Cap at 60s.
	Dictionary result = bridge->send_type_text(text, interval_frames, timeout_ms);

	if (!(bool)result.get("success", false)) {
		return make_tool_error(
				"Failed to type text: " +
				String(result.get("error", result.get("message", "Unknown error"))));
	}

	int chars_typed = result.get("chars_typed", text.length());

	String response_text = "Typed " + itos(chars_typed) + " characters";
	if (text.length() <= 50) {
		response_text += ": \"" + text + "\"";
	}
	if (interval_frames > 0) {
		response_text += " (interval: " + itos(interval_frames) + " frames between chars)";
	}

	Dictionary structured;
	structured["success"] = true;
	structured["text"] = text;
	structured["char_count"] = chars_typed;
	structured["interval_frames"] = interval_frames;

	return make_tool_result(response_text, structured);
}

// ============================================================================
// Tool Handler: runtime/input/send_input_sequence
// ============================================================================

Dictionary MCPInputTools::handle_send_input_sequence(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	if (!p_args.has("steps")) {
		return make_tool_error("Missing required parameter: steps");
	}

	Array steps = p_args["steps"];

	if (steps.is_empty()) {
		return make_tool_error("Parameter 'steps' must contain at least one step.");
	}

	if (steps.size() > 50) {
		return make_tool_error(
				"Too many steps: " + itos(steps.size()) + ".\n\n"
				"Maximum 50 steps per sequence.");
	}

	// Validate all steps upfront and compute total wait frames.
	_ensure_lookup_tables();
	int total_wait_frames = 0;

	for (int i = 0; i < steps.size(); i++) {
		Dictionary step = steps[i];
		if (!step.has("type")) {
			return make_tool_error("Step " + itos(i) + ": missing 'type' field.");
		}

		String step_type = String(step["type"]).to_lower().strip_edges();

		if (step_type == "key") {
			String key = String(step.get("key", "")).to_lower().strip_edges();
			if (key.is_empty()) {
				return make_tool_error("Step " + itos(i) + ": key step missing 'key' field.");
			}
			if (!key_name_map.has(key)) {
				return make_tool_error("Step " + itos(i) + ": unknown key '" + key + "'.\n" +
						_get_valid_key_names_hint());
			}
		} else if (step_type == "action") {
			String action = step.get("action", "");
			if (String(action).is_empty()) {
				return make_tool_error("Step " + itos(i) + ": action step missing 'action' field.");
			}
		} else if (step_type == "joypad_button") {
			String button = String(step.get("button", "")).to_lower().strip_edges();
			if (button.is_empty()) {
				return make_tool_error("Step " + itos(i) + ": joypad_button step missing 'button' field.");
			}
			if (!button_name_map.has(button)) {
				return make_tool_error("Step " + itos(i) + ": unknown button '" + button + "'.\n" +
						_get_valid_button_names_hint());
			}
		} else if (step_type == "joypad_axis") {
			String axis = String(step.get("axis", "")).to_lower().strip_edges();
			if (axis.is_empty()) {
				return make_tool_error("Step " + itos(i) + ": joypad_axis step missing 'axis' field.");
			}
			if (!axis_name_map.has(axis)) {
				return make_tool_error("Step " + itos(i) + ": unknown axis '" + axis + "'.\n" +
						_get_valid_axis_names_hint());
			}
		} else if (step_type == "wait") {
			int frames = (int)step.get("frames", 0);
			if (frames <= 0) {
				return make_tool_error("Step " + itos(i) + ": wait step requires 'frames' > 0.");
			}
			total_wait_frames += frames;
		} else {
			return make_tool_error("Step " + itos(i) + ": unknown step type '" + step_type + "'.\n"
								   "Valid types: key, action, joypad_button, joypad_axis, wait");
		}
	}

	if (total_wait_frames > 1800) {
		return make_tool_error(
				"Total wait frames exceed limit: " + itos(total_wait_frames) + " > 1800.\n\n"
				"Maximum total wait frames is 1800 (30 seconds at 60fps).");
	}

	MCPDebuggerBridge *bridge = _get_bridge();

	// Generous timeout based on total wait frames.
	int timeout_ms = MAX(total_wait_frames * 200, 15000);
	timeout_ms = MIN(timeout_ms, 60000);
	Dictionary result = bridge->send_input_sequence(steps, timeout_ms);

	if (!(bool)result.get("success", false)) {
		return make_tool_error(
				"Sequence failed: " +
				String(result.get("error", result.get("message", "Unknown error"))));
	}

	int steps_executed = result.get("steps_executed", 0);

	String response_text = "Sequence completed: " + itos(steps_executed) + " steps executed";
	if (total_wait_frames > 0) {
		response_text += " over ~" + itos(total_wait_frames) + " wait frames";
	}
	response_text += ".";

	// Append per-step summary.
	Array step_results = result.get("step_results", Array());
	for (int i = 0; i < step_results.size(); i++) {
		Dictionary sr = step_results[i];
		response_text += "\n  [" + itos(i) + "] " + String(sr.get("type", "?"));
		if (sr.has("key")) {
			response_text += " '" + String(sr["key"]) + "'";
		}
		if (sr.has("action")) {
			response_text += " '" + String(sr["action"]) + "'";
		}
		if (sr.has("button")) {
			response_text += " '" + String(sr["button"]) + "'";
		}
		if (sr.has("axis")) {
			response_text += " '" + String(sr["axis"]) + "'";
		}
		if (sr.has("pressed")) {
			response_text += (bool)sr["pressed"] ? " pressed" : " released";
		}
		if (sr.has("frames")) {
			response_text += " (" + itos((int)sr["frames"]) + " frames)";
		}
	}

	Dictionary structured;
	structured["success"] = true;
	structured["steps_executed"] = steps_executed;
	structured["total_wait_frames"] = total_wait_frames;
	structured["step_results"] = step_results;

	return make_tool_result(response_text, structured);
}

// ============================================================================
// Tool Handler: runtime/input/get_held_inputs
// ============================================================================

Dictionary MCPInputTools::handle_get_held_inputs(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	bool release_all = (bool)p_args.get("release_all", false);

	MCPDebuggerBridge *bridge = _get_bridge();
	Dictionary result = bridge->send_get_held_inputs(release_all);

	if (!(bool)result.get("success", false)) {
		return make_tool_error(
				"Failed to get held inputs: " +
				String(result.get("error", result.get("message", "Unknown error"))));
	}

	// Bridge returns:
	//   data: Array of Dictionaries, each with: name, type, frames_held, auto_release_at, [value], [device]
	//   released_count: int (how many were released if release_all was true)
	Array held_list = result.get("data", Array());
	int released_count = (int)result.get("released_count", 0);
	int total_held = held_list.size();

	// Categorize held inputs for structured output.
	Array held_keys;
	Array held_actions;
	Array held_joypad_buttons;
	Array held_joypad_axes;

	for (int i = 0; i < held_list.size(); i++) {
		Dictionary entry = held_list[i];
		String type = entry.get("type", "");

		if (type == "key") {
			held_keys.push_back(entry);
		} else if (type == "action") {
			held_actions.push_back(entry);
		} else if (type == "joypad_button") {
			held_joypad_buttons.push_back(entry);
		} else if (type == "joypad_axis") {
			held_joypad_axes.push_back(entry);
		}
	}

	// Build text response.
	String text;
	if (release_all && released_count > 0) {
		text = "Released " + itos(released_count) + " held inputs.";
	} else if (total_held == 0) {
		text = "No inputs currently held.";
	} else {
		text = itos(total_held) + " inputs currently held:";
	}

	for (int i = 0; i < held_list.size(); i++) {
		Dictionary entry = held_list[i];
		String type = entry.get("type", "unknown");
		String name = entry.get("name", "?");
		int frames = (int)entry.get("frames_held", 0);

		text += "\n  " + type + " '" + name + "' (held " + itos(frames) + " frames)";

		if (type == "joypad_axis") {
			text += " value=" + String::num((double)entry.get("value", 0.0), 2);
		}
		if (entry.has("device") && (int)entry.get("device", 0) != 0) {
			text += " device=" + itos((int)entry["device"]);
		}
	}

	Dictionary structured;
	structured["success"] = true;
	structured["release_all"] = release_all;
	structured["released_count"] = released_count;
	structured["held_keys"] = held_keys;
	structured["held_actions"] = held_actions;
	structured["held_joypad_buttons"] = held_joypad_buttons;
	structured["held_joypad_axes"] = held_joypad_axes;
	structured["total_held"] = total_held;

	return make_tool_result(text, structured);
}
