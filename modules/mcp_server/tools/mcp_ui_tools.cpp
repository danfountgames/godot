/**************************************************************************/
/*  mcp_ui_tools.cpp                                                      */
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

#include "mcp_ui_tools.h"

#include "../mcp_debugger_bridge.h"
#include "../mcp_protocol.h"
#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/object/script_language.h"

// ============================================================================
// Common Helpers
// ============================================================================

MCPDebuggerBridge *MCPUITools::_get_bridge() {
	return MCPDebuggerBridge::get_singleton();
}

Dictionary MCPUITools::_require_game_running() {
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

bool MCPUITools::_validate_node_path(const String &p_path) {
	for (int i = 0; i < p_path.length(); i++) {
		char32_t c = p_path[i];
		if (!is_ascii_alphanumeric_char(c) && c != '_' && c != '/' && c != '.' && c != '@' && c != ':' && c != '-' && c != ' ') {
			return false;
		}
	}
	return true;
}

Dictionary MCPUITools::_send_ui_request(const String &p_action,
		const String &p_node_path, const Dictionary &p_params) {
	MCPDebuggerBridge *bridge = _get_bridge();
	Dictionary result = bridge->send_ui_interact(p_action, p_node_path, p_params);

	if (!(bool)result.get("success", false)) {
		String error_msg = result.get("error", "Unknown error");
		Dictionary data = result.get("data", Dictionary());
		if (data.has("error")) {
			error_msg = data["error"];
		}
		return make_tool_error(error_msg);
	}

	Dictionary data = result.get("data", Dictionary());

	// Build text representation from the structured data.
	String text;
	if (data.has("node_path")) {
		text += "Control: " + String(data["node_path"]) + "\n";
	}
	if (data.has("class")) {
		text += "Type: " + String(data["class"]) + "\n";
	}

	// Add action-specific text.
	if (data.has("current_text")) {
		text += "Text: " + String(data["current_text"]) + "\n";
	}
	if (data.has("text") && !data.has("current_text")) {
		text += "Text: " + String(data["text"]) + "\n";
	}
	if (data.has("current_value")) {
		text += "Value: " + String::num((double)data["current_value"]) + "\n";
	}
	if (data.has("value") && !data.has("current_value")) {
		text += "Value: " + String::num((double)data["value"]) + "\n";
	}
	if (data.has("selected_text")) {
		text += "Selected: " + String(data["selected_text"]) + "\n";
	}
	if (data.has("current_tab_title")) {
		text += "Tab: " + String(data["current_tab_title"]) + "\n";
	}
	if (data.has("current_checked")) {
		text += "Checked: " + String((bool)data["current_checked"] ? "true" : "false") + "\n";
	}
	if (data.has("focused")) {
		text += "Focused: " + String((bool)data["focused"] ? "true" : "false") + "\n";
	}
	if (data.has("warning")) {
		text += "Warning: " + String(data["warning"]) + "\n";
	}

	// For control_data, add extra detail lines.
	if (data.has("control_data")) {
		Dictionary cd = data["control_data"];
		if (cd.has("visible")) {
			text += "Visible: " + String((bool)cd["visible"] ? "true" : "false") + "\n";
		}
		if (cd.has("editable")) {
			text += "Editable: " + String((bool)cd["editable"] ? "true" : "false") + "\n";
		}
	}

	if (text.is_empty()) {
		text = "UI interaction completed.";
	}

	return make_tool_result(text.strip_edges(), data);
}

// ============================================================================
// Tool Registration
// ============================================================================

void MCPUITools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ---- runtime/ui/get_control_info ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to the Control node (e.g., '/root/Main/UI/StartButton')");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"runtime/ui/get_control_info",
				"Get Control Info",
				"Read accessibility-like metadata from any Control node in the running game. "
				"Returns type, visibility, enabled state, focus state, rect, and type-specific "
				"data (text for LineEdit, value for Slider, items for OptionButton, etc.). "
				"Use this to inspect a control's current state before interacting with it. "
				"IMPORTANT: All runtime/ui/* tools only work on Control nodes (Button, Label, "
				"LineEdit, etc.) — NOT on Node2D sprites or Node3D meshes. Use "
				"runtime/browse_scene_tree to identify Control nodes. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPUITools::handle_get_control_info));
	}

	// ---- runtime/ui/set_text ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to a TextEdit or LineEdit node");
		props["text"] = make_prop("string",
				"The text to set, insert, or append");
		props["mode"] = make_prop("string",
				"How to apply the text: 'replace' (default, replaces all text), "
				"'append' (adds to end), 'insert' (at caret position), 'clear' (ignores text param)");
		props["focus"] = make_prop("boolean",
				"Whether to focus the control before modifying (default: true)");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"runtime/ui/set_text",
				"Set UI Text",
				"Set, clear, append, or insert text in a TextEdit or LineEdit node. "
				"Returns the previous and current text. Automatically focuses the "
				"control before modification unless focus=false. Emits text_changed "
				"signal so game scripts respond to the change. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPUITools::handle_set_text));
	}

	// ---- runtime/ui/get_text ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to a TextEdit or LineEdit node");
		props["selection_only"] = make_prop("boolean",
				"If true, return only the currently selected text (default: false)");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"runtime/ui/get_text",
				"Get UI Text",
				"Read the current text content from a TextEdit or LineEdit. "
				"Optionally read only the selected text. Returns text, line count, "
				"caret position, and selection state. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPUITools::handle_get_text));
	}

	// ---- runtime/ui/set_range_value ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to a Range-based node (HSlider, VSlider, SpinBox, ProgressBar, ScrollBar)");
		props["value"] = make_prop("number",
				"The absolute value to set. Mutually exclusive with 'ratio' and 'delta'.");
		props["ratio"] = make_prop("number",
				"Value as a 0.0-1.0 ratio of the range. 0.0 = min, 1.0 = max. "
				"Mutually exclusive with 'value' and 'delta'.");
		props["delta"] = make_prop("number",
				"Relative change. Added to current value. "
				"Mutually exclusive with 'value' and 'ratio'.");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"runtime/ui/set_range_value",
				"Set Range Value",
				"Set the value of any Range-based control (HSlider, VSlider, SpinBox, "
				"ProgressBar, ScrollBar). Supports absolute value, ratio (0.0-1.0), or "
				"delta (relative change). Returns previous and current value with range info. "
				"Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPUITools::handle_set_range_value));
	}

	// ---- runtime/ui/get_range_value ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to a Range-based node");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"runtime/ui/get_range_value",
				"Get Range Value",
				"Read the current value and range of any Range-based control (HSlider, "
				"VSlider, SpinBox, ProgressBar, ScrollBar). Returns value, min, max, step, "
				"ratio, and editable state. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPUITools::handle_get_range_value));
	}

	// ---- runtime/ui/select_option ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to an OptionButton node");
		props["index"] = make_prop("integer",
				"Item index to select (0-based). Mutually exclusive with 'text'.");
		props["text"] = make_prop("string",
				"Item text to match (case-insensitive, first match wins). "
				"Mutually exclusive with 'index'.");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"runtime/ui/select_option",
				"Select Option",
				"Select an item in an OptionButton (dropdown) by index or text match. "
				"Returns previous and current selection. Use runtime/ui/get_options to see "
				"all available items first. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPUITools::handle_select_option));
	}

	// ---- runtime/ui/get_options ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to an OptionButton node");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"runtime/ui/get_options",
				"Get Option Items",
				"List all items in an OptionButton, including which is currently selected. "
				"Returns item index, text, disabled state, and separator flag for each item. "
				"Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPUITools::handle_get_options));
	}

	// ---- runtime/ui/set_tab ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to a TabContainer or TabBar node");
		props["index"] = make_prop("integer",
				"Tab index to activate (0-based). Mutually exclusive with 'title'.");
		props["title"] = make_prop("string",
				"Tab title to match (case-insensitive, first match wins). "
				"Mutually exclusive with 'index'.");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"runtime/ui/set_tab",
				"Set Tab",
				"Switch the active tab on a TabContainer or TabBar by index or title. "
				"Returns previous and current tab with all tab titles. "
				"Use runtime/ui/get_tabs to see all tabs first. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPUITools::handle_set_tab));
	}

	// ---- runtime/ui/get_tabs ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to a TabContainer or TabBar node");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"runtime/ui/get_tabs",
				"Get Tabs",
				"List all tabs and the current selection on a TabContainer or TabBar. "
				"Returns tab index, title, disabled state, and hidden state for each tab. "
				"Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPUITools::handle_get_tabs));
	}

	// ---- runtime/ui/set_checked ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to a CheckBox or CheckButton node");
		props["checked"] = make_prop("boolean",
				"Desired checked state. Omit to toggle.");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"runtime/ui/set_checked",
				"Set Checked State",
				"Set or toggle the checked state of a CheckBox or CheckButton. "
				"Returns previous and current checked state. Emits toggled signal. "
				"Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPUITools::handle_set_checked));
	}

	// ---- runtime/ui/focus ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to a Control node");
		props["action"] = make_prop("string",
				"'grab' (default) to grab focus, 'release' to release focus");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"runtime/ui/focus",
				"Focus Control",
				"Explicitly focus or unfocus a Control node. Useful before keyboard input "
				"or to verify focus state. Returns the control's focus state and focus_mode. "
				"If focus_mode is NONE, a warning is returned. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPUITools::handle_focus));
	}
}

// ============================================================================
// Tool Handlers
// ============================================================================

Dictionary MCPUITools::handle_get_control_info(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String node_path = p_args.get("node_path", "");
	if (node_path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: node_path\n\n"
				"Provide the scene tree path of a Control node.\n"
				"Use runtime/search_scene_tree to find Control nodes.");
	}
	if (!_validate_node_path(node_path)) {
		return make_tool_error("Invalid node path: contains disallowed character");
	}

	Dictionary params;
	return _send_ui_request("get_info", node_path, params);
}

Dictionary MCPUITools::handle_set_text(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String node_path = p_args.get("node_path", "");
	if (node_path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: node_path\n\n"
				"Provide the scene tree path of a TextEdit or LineEdit node.");
	}
	if (!_validate_node_path(node_path)) {
		return make_tool_error("Invalid node path: contains disallowed character");
	}

	Dictionary params;
	String mode = p_args.get("mode", "replace");
	params["mode"] = mode;
	if (p_args.has("text")) {
		params["text"] = p_args["text"];
	}
	params["focus"] = (bool)p_args.get("focus", true);

	return _send_ui_request("set_text", node_path, params);
}

Dictionary MCPUITools::handle_get_text(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String node_path = p_args.get("node_path", "");
	if (node_path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: node_path\n\n"
				"Provide the scene tree path of a TextEdit or LineEdit node.");
	}
	if (!_validate_node_path(node_path)) {
		return make_tool_error("Invalid node path: contains disallowed character");
	}

	Dictionary params;
	params["selection_only"] = (bool)p_args.get("selection_only", false);

	return _send_ui_request("get_text", node_path, params);
}

Dictionary MCPUITools::handle_set_range_value(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String node_path = p_args.get("node_path", "");
	if (node_path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: node_path\n\n"
				"Provide the scene tree path of a Range-based node.");
	}
	if (!_validate_node_path(node_path)) {
		return make_tool_error("Invalid node path: contains disallowed character");
	}

	// Validate mutually exclusive parameters.
	int mode_count = 0;
	if (p_args.has("value")) {
		mode_count++;
	}
	if (p_args.has("ratio")) {
		mode_count++;
	}
	if (p_args.has("delta")) {
		mode_count++;
	}

	if (mode_count == 0) {
		return make_tool_error(
				"Missing value parameter.\n\n"
				"Provide exactly one of: 'value' (absolute), 'ratio' (0.0-1.0), or 'delta' (relative change).");
	}
	if (mode_count > 1) {
		return make_tool_error(
				"Parameters 'value', 'ratio', and 'delta' are mutually exclusive. Provide exactly one.");
	}

	Dictionary params;
	if (p_args.has("value")) {
		params["value"] = p_args["value"];
	}
	if (p_args.has("ratio")) {
		params["ratio"] = p_args["ratio"];
	}
	if (p_args.has("delta")) {
		params["delta"] = p_args["delta"];
	}

	return _send_ui_request("set_range", node_path, params);
}

Dictionary MCPUITools::handle_get_range_value(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String node_path = p_args.get("node_path", "");
	if (node_path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: node_path\n\n"
				"Provide the scene tree path of a Range-based node.");
	}
	if (!_validate_node_path(node_path)) {
		return make_tool_error("Invalid node path: contains disallowed character");
	}

	Dictionary params;
	return _send_ui_request("get_range", node_path, params);
}

Dictionary MCPUITools::handle_select_option(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String node_path = p_args.get("node_path", "");
	if (node_path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: node_path\n\n"
				"Provide the scene tree path of an OptionButton node.");
	}
	if (!_validate_node_path(node_path)) {
		return make_tool_error("Invalid node path: contains disallowed character");
	}

	// Must provide either index or text.
	if (!p_args.has("index") && !p_args.has("text")) {
		return make_tool_error(
				"Missing selection parameter.\n\n"
				"Provide 'index' (0-based item index) or 'text' (item text to match).\n"
				"Use runtime/ui/get_options to see available items.");
	}
	if (p_args.has("index") && p_args.has("text")) {
		return make_tool_error(
				"Parameters 'index' and 'text' are mutually exclusive. Provide exactly one.");
	}

	Dictionary params;
	if (p_args.has("index")) {
		params["index"] = p_args["index"];
	}
	if (p_args.has("text")) {
		params["text"] = p_args["text"];
	}

	return _send_ui_request("select_option", node_path, params);
}

Dictionary MCPUITools::handle_get_options(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String node_path = p_args.get("node_path", "");
	if (node_path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: node_path\n\n"
				"Provide the scene tree path of an OptionButton node.");
	}
	if (!_validate_node_path(node_path)) {
		return make_tool_error("Invalid node path: contains disallowed character");
	}

	Dictionary params;
	return _send_ui_request("get_options", node_path, params);
}

Dictionary MCPUITools::handle_set_tab(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String node_path = p_args.get("node_path", "");
	if (node_path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: node_path\n\n"
				"Provide the scene tree path of a TabContainer or TabBar node.");
	}
	if (!_validate_node_path(node_path)) {
		return make_tool_error("Invalid node path: contains disallowed character");
	}

	// Must provide either index or title.
	if (!p_args.has("index") && !p_args.has("title")) {
		return make_tool_error(
				"Missing selection parameter.\n\n"
				"Provide 'index' (0-based tab index) or 'title' (tab title to match).\n"
				"Use runtime/ui/get_tabs to see available tabs.");
	}
	if (p_args.has("index") && p_args.has("title")) {
		return make_tool_error(
				"Parameters 'index' and 'title' are mutually exclusive. Provide exactly one.");
	}

	Dictionary params;
	if (p_args.has("index")) {
		params["index"] = p_args["index"];
	}
	if (p_args.has("title")) {
		params["title"] = p_args["title"];
	}

	return _send_ui_request("set_tab", node_path, params);
}

Dictionary MCPUITools::handle_get_tabs(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String node_path = p_args.get("node_path", "");
	if (node_path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: node_path\n\n"
				"Provide the scene tree path of a TabContainer or TabBar node.");
	}
	if (!_validate_node_path(node_path)) {
		return make_tool_error("Invalid node path: contains disallowed character");
	}

	Dictionary params;
	return _send_ui_request("get_tabs", node_path, params);
}

Dictionary MCPUITools::handle_set_checked(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String node_path = p_args.get("node_path", "");
	if (node_path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: node_path\n\n"
				"Provide the scene tree path of a CheckBox or CheckButton node.");
	}
	if (!_validate_node_path(node_path)) {
		return make_tool_error("Invalid node path: contains disallowed character");
	}

	Dictionary params;
	if (p_args.has("checked")) {
		params["checked"] = p_args["checked"];
	}
	// If "checked" is omitted, the game side will toggle.

	return _send_ui_request("set_checked", node_path, params);
}

Dictionary MCPUITools::handle_focus(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String node_path = p_args.get("node_path", "");
	if (node_path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: node_path\n\n"
				"Provide the scene tree path of a Control node.");
	}
	if (!_validate_node_path(node_path)) {
		return make_tool_error("Invalid node path: contains disallowed character");
	}

	Dictionary params;
	params["action"] = p_args.get("action", "grab");

	return _send_ui_request("focus", node_path, params);
}
