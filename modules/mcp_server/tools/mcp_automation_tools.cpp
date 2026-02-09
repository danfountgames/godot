/**************************************************************************/
/*  mcp_automation_tools.cpp                                              */
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

#include "mcp_automation_tools.h"

#include "core/object/script_language.h"

#include "../mcp_debugger_bridge.h"
#include "../mcp_protocol.h"
#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/config/project_settings.h"
#include "core/input/input_map.h"
#include "core/variant/typed_array.h"

// ============================================================================
// Registration
// ============================================================================

void MCPAutomationTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// debug/send_input
	{
		Dictionary props;
		props["action"] = make_prop("string",
				"Input action name as defined in the Input Map (e.g., 'jump', 'move_left')");
		props["pressed"] = make_prop("boolean",
				"Press (true) or release (false) the action (default: true)");
		props["hold_frames"] = make_prop("integer",
				"Frames to hold before auto-release (default: 1, max: 600)");
		props["strength"] = make_prop("number",
				"Analog strength 0.0-1.0 (default: 1.0)");
		Array required;
		required.push_back("action");
		p_registry->register_tool(
				"debug/send_input", "Send Input Action",
				"Send an input action to the running game. Action must exist in Input Map "
				"(use project/get_input_map). Supports hold_frames for sustained input and "
				"strength for analog control. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPAutomationTools::handle_send_input));
	}

	// debug/click_control
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to the Control node (e.g., '/root/Main/UI/StartButton')");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"debug/click_control", "Click UI Control",
				"Simulate a mouse click on a UI Control node. Sends press+release at control "
				"center. Works with Button, TextureButton, CheckBox, and any Control. "
				"Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPAutomationTools::handle_click_control));
	}

	// debug/evaluate
	{
		Dictionary props;
		props["expression"] = make_prop("string",
				"GDScript expression. Use get_root().get_node(\"path\") for node access.");
		Array required;
		required.push_back("expression");
		p_registry->register_tool(
				"debug/evaluate", "Evaluate Expression",
				"Evaluate a GDScript expression in the running game's SceneTree context. "
				"SceneTree is the base instance, so get_root(), get_nodes_in_group(), "
				"current_scene work. $ shorthand does NOT work -- use get_root().get_node() "
				"instead. No var declarations or control flow.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPAutomationTools::handle_evaluate));
	}

	// debug/wait_frames
	{
		Dictionary props;
		props["frames"] = make_prop("integer",
				"Frames to wait (default: 1, max: 600). 60 frames = ~1 second at 60fps.");
		Array required;
		p_registry->register_tool(
				"debug/wait_frames", "Wait Frames",
				"Wait for N game frames using frame counting via process_frame signal "
				"(NOT timers). Use between automation actions to let the game process changes. "
				"Game must be running. Max 600 frames.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPAutomationTools::handle_wait_frames));
	}

	// debug/get_screenshot
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"debug/get_screenshot", "Get Screenshot",
				"Capture the running game's viewport as a base64-encoded PNG image. "
				"Useful for visual verification. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPAutomationTools::handle_get_screenshot));
	}
}

// ============================================================================
// Common Helpers
// ============================================================================

MCPDebuggerBridge *MCPAutomationTools::_get_bridge() {
	MCPProtocol *protocol = MCPProtocol::get_singleton();
	if (!protocol) {
		return nullptr;
	}
	return protocol->get_debugger_bridge();
}

Dictionary MCPAutomationTools::_require_game_running() {
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Internal error: debugger bridge not available.");
	}
	if (!bridge->is_game_running()) {
		return make_tool_error(
				"No game is currently running.\n\n"
				"If the game stopped unexpectedly, check debug/get_errors for runtime errors.\n"
				"To start a game: debug/run_project (main scene) or debug/run_scene (specific scene).\n"
				"To check status: debug/get_status (includes stop_reason when stopped).");
	}
	return Dictionary();
}

bool MCPAutomationTools::_action_exists(const String &p_action) {
	return InputMap::get_singleton()->has_action(p_action);
}

Vector<String> MCPAutomationTools::_find_similar_actions(const String &p_action) {
	Vector<String> suggestions;
	String lower_action = p_action.to_lower();

	// Get all action names.
	TypedArray<StringName> actions = InputMap::get_singleton()->get_actions();

	for (int idx = 0; idx < actions.size(); idx++) {
		String action_str = String(StringName(actions[idx]));
		// Simple similarity: contains as substring.
		if (action_str.to_lower().contains(lower_action) ||
				lower_action.contains(action_str.to_lower())) {
			suggestions.push_back(action_str);
		}
		if (suggestions.size() >= 5) {
			break;
		}
	}

	return suggestions;
}

// ============================================================================
// Tool Handlers
// ============================================================================

Dictionary MCPAutomationTools::handle_send_input(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String action = p_args.get("action", "");
	if (action.is_empty()) {
		return make_tool_error(
				"Missing required parameter: action\n\n"
				"Provide an input action name from the Input Map.\n"
				"Use project/get_input_map to see available actions.");
	}

	bool pressed = (bool)p_args.get("pressed", true);
	int hold_frames = (int)p_args.get("hold_frames", 1);
	float strength = (float)(double)p_args.get("strength", 1.0);

	// Clamp parameters.
	hold_frames = CLAMP(hold_frames, 0, 600);
	strength = CLAMP(strength, 0.0f, 1.0f);

	// Validate action exists in InputMap BEFORE sending to game.
	if (!_action_exists(action)) {
		String error_text = "Unknown input action: '" + action + "'. "
															"This action is not defined in the project's Input Map.\n\n";

		Vector<String> suggestions = _find_similar_actions(action);
		if (suggestions.size() > 0) {
			error_text += "Did you mean one of these?\n";
			for (int i = 0; i < suggestions.size(); i++) {
				error_text += "  " + suggestions[i] + "\n";
			}
			error_text += "\n";
		}

		error_text += "Use project/get_input_map to see all available actions.";
		return make_tool_error(error_text);
	}

	// Send to game via bridge.
	MCPDebuggerBridge *bridge = _get_bridge();
	Dictionary result = bridge->send_inject_action(action, pressed, hold_frames, strength);

	if (!(bool)result.get("success", false)) {
		return make_tool_error(
				"Failed to send input action '" + action + "': " +
				String(result.get("error", "Unknown error")));
	}

	// Build response.
	String text = "Input action '" + action + "' " +
			(pressed ? "pressed" : "released") +
			" (hold: " + itos(hold_frames) + " frames, strength: " +
			String::num(strength, 1) + ")";

	Dictionary structured;
	structured["action"] = action;
	structured["pressed"] = pressed;
	structured["hold_frames"] = hold_frames;
	structured["strength"] = strength;
	structured["success"] = true;

	return make_tool_result(text, structured);
}

Dictionary MCPAutomationTools::handle_click_control(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String node_path = p_args.get("node_path", "");
	if (node_path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: node_path\n\n"
				"Provide the scene tree path of a Control node.\n"
				"Example: { \"node_path\": \"/root/Main/UI/StartButton\" }\n"
				"Use debug/search_scene_tree to find Control nodes.");
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	Dictionary result = bridge->send_click_control(node_path);

	if (!(bool)result.get("success", false)) {
		String error_msg = result.get("message", result.get("error", "Unknown error"));

		// Provide contextual guidance based on the error.
		String guidance;
		if (String(error_msg).contains("not found")) {
			guidance = "\n\nUse debug/search_scene_tree with name_pattern to find the correct path.";
		} else if (String(error_msg).contains("not a Control")) {
			guidance = "\n\ndebug/click_control only works on UI Control nodes "
					   "(Button, Label, etc.). For non-UI interaction, use "
					   "debug/send_input with input actions.";
		}

		return make_tool_error(String(error_msg) + guidance);
	}

	String msg = result.get("message", "Click sent");
	String text = "Clicked " + node_path + ": " + msg;

	Dictionary structured;
	structured["success"] = true;
	structured["node_path"] = node_path;
	structured["message"] = msg;

	return make_tool_result(text, structured);
}

Dictionary MCPAutomationTools::handle_evaluate(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String expression = p_args.get("expression", "");
	if (expression.is_empty()) {
		return make_tool_error(
				"Missing required parameter: expression\n\n"
				"Provide a GDScript expression to evaluate.\n"
				"Examples:\n"
				"  get_root().get_node(\"Main/Player\").position\n"
				"  get_nodes_in_group(\"enemies\").size()\n"
				"  current_scene.name");
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	Dictionary result = bridge->send_evaluate(expression);

	if (!(bool)result.get("success", false)) {
		String error_msg = result.get("value", result.get("error", "Unknown error"));

		// Detect common mistakes and provide guidance.
		String guidance;
		if (String(error_msg).contains("Unexpected token") &&
				expression.begins_with("$")) {
			guidance = "\n\nNote: The $ node path shorthand is not supported "
					   "by the Expression evaluator. Use get_root().get_node(\"path\") "
					   "instead.\n\nExample: Instead of '$Player.position', use:\n"
					   "  get_root().get_node(\"Main/Player\").position";
		} else if (String(error_msg).contains("null instance")) {
			guidance = "\n\nThe node path may be incorrect. "
					   "Use debug/search_scene_tree to find the correct path.";
		}

		return make_tool_error(
				"Expression error: " + String(error_msg) + guidance);
	}

	String value_str = result.get("value", "");

	// Parse type and value from the bridge response format "Type: value".
	String type_name;
	String display_value = value_str;
	int colon_pos = value_str.find(": ");
	if (colon_pos >= 0) {
		type_name = value_str.substr(0, colon_pos);
		display_value = value_str.substr(colon_pos + 2);
	}

	String text = "Result: " + display_value;
	if (!type_name.is_empty()) {
		text += "\nType: " + type_name;
	}

	Dictionary structured;
	structured["success"] = true;
	structured["value"] = display_value;
	structured["type"] = type_name;
	structured["raw"] = value_str;

	return make_tool_result(text, structured);
}

Dictionary MCPAutomationTools::handle_wait_frames(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	int frames = (int)p_args.get("frames", 1);
	frames = CLAMP(frames, 1, 600);

	MCPDebuggerBridge *bridge = _get_bridge();

	// Generous timeout: frames * 200ms upper bound.
	// At 60fps, 600 frames = 10 seconds; our timeout = 120 seconds (very safe).
	int timeout_ms = MAX(frames * 200, 10000);
	Dictionary result = bridge->send_wait_frames(frames, timeout_ms);

	if (!(bool)result.get("success", false)) {
		return make_tool_error(
				"Wait failed: " + String(result.get("error", "Unknown error")) +
				"\n\nThe game may have crashed or stopped during the wait.\n"
				"Try debug/get_status to check game state.");
	}

	String text = "Waited " + itos(frames) + " frames.";

	Dictionary structured;
	structured["frames_waited"] = frames;

	return make_tool_result(text, structured);
}

Dictionary MCPAutomationTools::handle_get_screenshot(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	Dictionary result = bridge->send_screenshot();

	if (!(bool)result.get("success", false)) {
		return make_tool_error(
				"Screenshot failed: " + String(result.get("error", "Unknown error")));
	}

	String base64_png = result.get("base64_png", "");
	if (base64_png.is_empty()) {
		return make_tool_error(
				"Screenshot captured but returned empty data.\n\n"
				"This may indicate a rendering issue. Try again after a few frames.");
	}

	// Extract dimensions from bridge result.
	int width = result.get("width", 0);
	int height = result.get("height", 0);

	// Estimate size.
	int data_size = base64_png.length() * 3 / 4; // Approximate decoded size.
	double size_kb = (double)data_size / 1024.0;

	// Build response with both text and image content blocks.
	Dictionary text_content;
	text_content["type"] = "text";
	text_content["text"] = "Screenshot captured (" +
			itos(width) + "x" + itos(height) + ", " +
			String::num(size_kb, 1) + " KB PNG)";

	Dictionary image_content;
	image_content["type"] = "image";
	image_content["data"] = base64_png;
	image_content["mimeType"] = "image/png";

	Array content;
	content.push_back(text_content);
	content.push_back(image_content);

	Dictionary structured;
	structured["width"] = width;
	structured["height"] = height;
	structured["format"] = "png";
	structured["size_bytes"] = data_size;

	Dictionary response;
	response["content"] = content;
	response["structuredContent"] = structured;

	return response;
}
