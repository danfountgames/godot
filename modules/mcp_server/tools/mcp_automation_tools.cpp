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
#include "servers/rendering/rendering_server.h"
#include "core/input/input_map.h"
#include "core/variant/typed_array.h"

// ============================================================================
// Registration
// ============================================================================

void MCPAutomationTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// runtime/input/send_input
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
				"runtime/input/send_input", "Send Input Action",
				"Send an input action to the running game. Action names are project-specific "
				"and must exist in Input Map (use project/get_input_map to discover them). "
				"These are NOT raw key names — use runtime/input/send_key for raw keyboard input. "
				"Actions prefixed 'ui_' are Godot built-ins (ui_accept, ui_cancel, etc). "
				"Supports hold_frames for sustained input and strength for analog. "
				"Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPAutomationTools::handle_send_input));
	}

	// runtime/input/click_control
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to the Control node (e.g., '/root/Main/UI/StartButton')");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"runtime/input/click_control", "Click UI Control",
				"Simulate a mouse click on a UI Control node. Sends press+release at control "
				"center. Works with Button, TextureButton, CheckBox, and any Control. "
				"Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPAutomationTools::handle_click_control));
	}

	// runtime/evaluate
	{
		Dictionary props;
		props["expression"] = make_prop("string",
				"GDScript expression or code. Supports $NodePath shorthand, assignments, "
				"and multi-line statements. Examples:\n"
				"  $Main/Player.position\n"
				"  $Main/Player.health = 100\n"
				"  var p = $Main/Player; p.position.x = 50\n"
				"  get_nodes_in_group(\"enemies\").size()");
		props["timeout_ms"] = make_prop("integer",
				"Timeout in milliseconds (default: 10000, max: 60000). "
				"Increase for operations that spawn many nodes or load resources.");
		Array required;
		required.push_back("expression");
		p_registry->register_tool(
				"runtime/evaluate", "Evaluate Expression",
				"Evaluate a GDScript expression or execute GDScript code in the running "
				"game's SceneTree context.\n\n"
				"SIMPLE EXPRESSIONS: get_root(), get_nodes_in_group(), current_scene "
				"work directly. Use $NodePath for node access: $Main/Player.position\n\n"
				"ASSIGNMENTS & STATEMENTS: Supports var declarations, assignments, "
				"loops, and control flow. Assign to _result to return a value:\n"
				"  $Main/Player.health = 100\n"
				"  for enemy in get_nodes_in_group(\"enemies\"): enemy.queue_free()\n"
				"  _result = $Main/GameManager.score\n\n"
				"Rotation values are in RADIANS (PI/2 = 90 degrees). "
				"2D uses Y-down coordinates (positive Y = down on screen). "
				"print() output goes to runtime/get_output.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPAutomationTools::handle_evaluate));
	}

	// runtime/wait_frames
	{
		Dictionary props;
		props["frames"] = make_prop("integer",
				"Frames to wait (default: 1, max: 600). 60 frames = ~1 second at 60fps.");
		Array required;
		p_registry->register_tool(
				"runtime/wait_frames", "Wait Frames",
				"Wait for N game frames using frame counting via process_frame signal "
				"(NOT timers). Use between automation actions to let the game process changes. "
				"Game must be running. Max 600 frames.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPAutomationTools::handle_wait_frames));
	}

	// runtime/get_screenshot
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"runtime/get_screenshot", "Get Screenshot",
				"IMPORTANT: Screenshots are expensive and rarely needed. In almost all cases, "
				"you should use runtime/get_scene_tree or runtime/get_node_properties instead "
				"to inspect game state programmatically — this is faster, cheaper, and gives you "
				"structured data you can actually reason about. Only use screenshots when you "
				"specifically need to verify VISUAL appearance (rendering, layout, animations) "
				"that cannot be determined from the scene tree. "
				"Captures the running game's viewport as a base64-encoded PNG image. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPAutomationTools::handle_get_screenshot));
	}
}

// ============================================================================
// Common Helpers
// ============================================================================

MCPDebuggerBridge *MCPAutomationTools::_get_bridge() {
	return MCPDebuggerBridge::get_singleton();
}

Dictionary MCPAutomationTools::_require_game_running() {
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

bool MCPAutomationTools::_action_exists(const String &p_action) {
	// First check the editor's InputMap (includes built-in ui_* actions).
	if (InputMap::get_singleton()->has_action(p_action)) {
		return true;
	}

	// If not found, check project settings directly. The editor's InputMap
	// may not include project-specific actions added to project.godot after
	// the editor started, since InputMap::load_from_project_settings() is
	// only called once at startup. Rather than reloading the entire input
	// map (which could disrupt the editor), we check the project settings
	// directly for the action definition.
	String action_key = "input/" + p_action;
	return ProjectSettings::get_singleton()->has_setting(action_key);
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
				"Use runtime/search_scene_tree to find Control nodes.");
	}

	// Validate node_path contains only safe characters (same rules as get_node_properties).
	for (int i = 0; i < node_path.length(); i++) {
		char32_t c = node_path[i];
		if (!is_ascii_alphanumeric_char(c) && c != '_' && c != '/' && c != '.' && c != '@' && c != ':' && c != '-' && c != ' ') {
			return make_tool_error("Invalid node path: contains disallowed character");
		}
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	Dictionary result = bridge->send_click_control(node_path);

	if (!(bool)result.get("success", false)) {
		String error_msg = result.get("message", result.get("error", "Unknown error"));

		// Provide contextual guidance based on the error.
		String guidance;
		if (String(error_msg).contains("not found")) {
			guidance = "\n\nUse runtime/search_scene_tree with name_pattern to find the correct path.";
		} else if (String(error_msg).contains("not a Control")) {
			guidance = "\n\nruntime/input/click_control only works on UI Control nodes "
					   "(Button, Label, etc.). For non-UI interaction, use "
					   "runtime/input/send_input with input actions.";
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

// Helper: Rewrite $NodePath syntax to get_root().get_node("NodePath").
// Handles $Node, $Node/Child, $"Node/With Spaces", and property access after.
static String _rewrite_dollar_paths(const String &p_expr) {
	String result;
	int i = 0;
	while (i < p_expr.length()) {
		if (p_expr[i] == '$') {
			// Found a $ — extract the node path.
			i++; // Skip $
			String node_path;
			bool quoted = (i < p_expr.length() && p_expr[i] == '"');
			if (quoted) {
				// $"Some/Path" form
				i++; // Skip opening "
				while (i < p_expr.length() && p_expr[i] != '"') {
					node_path += p_expr[i];
					i++;
				}
				if (i < p_expr.length()) {
					i++; // Skip closing "
				}
			} else {
				// $Node/Child form — path chars are alphanumeric, _, /
				while (i < p_expr.length()) {
					char32_t c = p_expr[i];
					if (is_ascii_alphanumeric_char(c) || c == '_' || c == '/') {
						node_path += p_expr[i];
						i++;
					} else {
						break;
					}
				}
			}
			if (!node_path.is_empty()) {
				result += "get_root().get_node(\"" + node_path + "\")";
			} else {
				result += "$"; // Lone $ with nothing after, keep as-is
			}
		} else {
			result += p_expr[i];
			i++;
		}
	}
	return result;
}

// Helper: Detect if an expression contains statements that require
// GDScript execution rather than Expression evaluation.
static bool _needs_gdscript_execution(const String &p_expr) {
	// Check for multi-line code.
	if (p_expr.contains("\n")) {
		return true;
	}

	String stripped = p_expr.strip_edges();

	// Assignment operators (but not == comparison).
	// Check for = that's not ==, !=, <=, >=
	for (int i = 0; i < stripped.length(); i++) {
		if (stripped[i] == '=' && i > 0) {
			char32_t prev = stripped[i - 1];
			char32_t next = (i + 1 < stripped.length()) ? stripped[i + 1] : 0;
			// Skip ==, !=, <=, >=
			if (prev == '!' || prev == '<' || prev == '>' || prev == '=') {
				continue;
			}
			if (next == '=') {
				i++; // Skip ==
				continue;
			}
			// This is an assignment: =, +=, -=, *=, /=
			return true;
		}
	}

	// Keywords that indicate statements.
	static const char *statement_keywords[] = {
		"var ", "const ", "for ", "while ", "if ", "elif ", "else:",
		"match ", "return ", "pass", "break", "continue",
		nullptr
	};
	for (const char **kw = statement_keywords; *kw != nullptr; kw++) {
		if (stripped.begins_with(*kw) || stripped.contains(String("\n") + *kw)) {
			return true;
		}
	}

	return false;
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
				"  $Main/Player.position\n"
				"  get_root().get_node(\"Main/Player\").position\n"
				"  get_nodes_in_group(\"enemies\").size()\n"
				"  current_scene.name\n\n"
				"Assignments and statements are also supported:\n"
				"  $Main/Player.position.x = 100\n"
				"  var p = $Main/Player; p.health = 50");
	}

	// Security: block dangerous expressions that could execute arbitrary
	// OS commands or bypass the sandbox.
	{
		String expr_lower = expression.to_lower();

		// Exact API patterns — these use "os." prefix or explicit class names
		// so they won't false-positive on game identifiers like boss.execute().
		static const char *blocked_exact[] = {
			"os.execute(",
			"os.shell_open(",
			"os.create_process(",
			"os.create_instance(",
			"os.kill(",
			nullptr
		};
		for (const char **p = blocked_exact; *p != nullptr; p++) {
			if (expr_lower.find(*p) != -1) {
				return make_tool_error(
						"Blocked: expression contains a disallowed pattern (\"" +
						String(*p) +
						"\"). For security, OS commands are not permitted.");
			}
		}

		// Class/API names that are dangerous when used as identifiers.
		// These are checked with word-boundary awareness: the character before
		// the match must NOT be alphanumeric or underscore (i.e., it's a
		// standalone identifier, not part of a larger word like "bossfileaccess").
		static const char *blocked_identifiers[] = {
			"fileaccess",
			"diraccess",
			"marshalls",
			nullptr
		};
		for (const char **p = blocked_identifiers; *p != nullptr; p++) {
			String pattern(*p);
			int pos = expr_lower.find(pattern);
			while (pos != -1) {
				// Check word boundary before match.
				bool boundary_before = (pos == 0) || (!is_ascii_alphanumeric_char(expr_lower[pos - 1]) && expr_lower[pos - 1] != '_');
				// Check word boundary after match.
				int end = pos + pattern.length();
				bool boundary_after = (end >= expr_lower.length()) || (!is_ascii_alphanumeric_char(expr_lower[end]) && expr_lower[end] != '_');
				if (boundary_before && boundary_after) {
					return make_tool_error(
							"Blocked: expression contains a disallowed identifier (\"" +
							pattern +
							"\"). For security, file system access and "
							"serialization APIs are not permitted.");
				}
				pos = expr_lower.find(pattern, pos + 1);
			}
		}

		// Reflection/threading patterns that are suspicious in any context.
		static const char *blocked_anywhere[] = {
			".callv(",
			nullptr
		};
		for (const char **p = blocked_anywhere; *p != nullptr; p++) {
			if (expr_lower.find(*p) != -1) {
				return make_tool_error(
						"Blocked: expression contains a disallowed pattern (\"" +
						String(*p) +
						"\"). Reflection calls are not permitted.");
			}
		}
	}

	// Rewrite $NodePath syntax to get_root().get_node("NodePath").
	String rewritten = _rewrite_dollar_paths(expression);

	// Optional timeout override (default 10s, max 60s).
	int timeout_ms = (int)p_args.get("timeout_ms", 10000);
	timeout_ms = CLAMP(timeout_ms, 1000, 60000);

	MCPDebuggerBridge *bridge = _get_bridge();
	Dictionary result;

	// Always use full GDScript execution. The Expression evaluator cannot
	// resolve engine singletons (Debug, Engine, Time) and its micro-
	// optimization is not worth the agent failures it causes.
	result = bridge->send_execute_code(rewritten, timeout_ms);

	if (!(bool)result.get("success", false)) {
		String error_msg = result.get("value", result.get("error", "Unknown error"));

		// Detect common mistakes and provide guidance.
		String guidance;
		if (String(error_msg).contains("null instance")) {
			guidance = "\n\nThe node path may be incorrect. "
					   "Use runtime/search_scene_tree to find the correct path.";
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
				"Try runtime/get_status to check game state.");
	}

	int actual_frames = (int)result.get("frames_waited", frames);
	String text = "Waited " + itos(actual_frames) + " frames.";

	Dictionary structured;
	structured["frames_waited"] = actual_frames;

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

	// Detect software renderer (llvmpipe, swrast) — screenshots may be empty.
	String warning;
	String video_adapter = result.get("video_adapter", "");
	if (video_adapter.is_empty()) {
		// Try reading from OS.
		video_adapter = RenderingServer::get_singleton()
				? String(RenderingServer::get_singleton()->get_video_adapter_name())
				: String();
	}
	if (!video_adapter.is_empty()) {
		String adapter_lower = video_adapter.to_lower();
		if (adapter_lower.contains("llvmpipe") || adapter_lower.contains("swrast") ||
				adapter_lower.contains("software") || adapter_lower.contains("mesa")) {
			warning = "Software renderer detected (" + video_adapter +
					"). Screenshot pixel data may be empty or incorrect. "
					"Use runtime/get_scene_tree or runtime/evaluate instead "
					"for reliable state inspection.";
		}
	}

	// Build response with both text and image content blocks.
	String caption = "Screenshot captured (" +
			itos(width) + "x" + itos(height) + ", " +
			String::num(size_kb, 1) + " KB PNG)";
	if (!warning.is_empty()) {
		caption += "\n\nWARNING: " + warning;
	}

	Dictionary text_content;
	text_content["type"] = "text";
	text_content["text"] = caption;

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
	if (!warning.is_empty()) {
		structured["warning"] = warning;
	}

	Dictionary response;
	response["content"] = content;
	response["structuredContent"] = structured;

	return response;
}
