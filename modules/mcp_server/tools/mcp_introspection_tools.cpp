/**************************************************************************/
/*  mcp_introspection_tools.cpp                                           */
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

#include "mcp_introspection_tools.h"

#include "mcp_debug_tools.h"

#include "../mcp_debugger_bridge.h"
#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/io/json.h"

// ============================================================================
// Tool Registration
// ============================================================================

void MCPIntrospectionTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// debug/describe_class
	{
		Dictionary props;
		props["class"] = make_prop("string",
				"The class name to describe (e.g., 'Node2D', 'CharacterBody3D'). "
				"Must be a valid Godot class name registered in ClassDB or available "
				"via the script parser.");
		props["include_private"] = make_prop("boolean",
				"If true, include private/internal members (underscore-prefixed). "
				"Default: false.");
		props["include_docs"] = make_prop("boolean",
				"If true, include ## doc comments for classes, methods, properties, "
				"signals, and constants. Default: false (compact output).");
		Array required;
		required.push_back("class");
		p_registry->register_tool(
				"debug/describe_class", "Describe Class",
				"Look up class reference information for a Godot class. Returns methods, "
				"properties, signals, and inheritance chain. Compact by default — use "
				"include_docs: true to see ## doc comments.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPIntrospectionTools::handle_describe_class));
	}

	// debug/browse_tree
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Subtree root path to browse from (default: '/root'). "
				"Use absolute node paths like '/root/Main/Player'.");
		props["depth"] = make_prop("integer",
				"Max depth relative to path (default: 2). "
				"1 = immediate children only. 0 = root only.");
		props["child_limit"] = make_prop("integer",
				"Max direct children to return at the root level (default: 50, max: 200). "
				"For pagination of large node lists.");
		props["child_offset"] = make_prop("integer",
				"Skip N direct children of root for pagination (default: 0).");
		Array required;
		p_registry->register_tool(
				"debug/browse_tree", "Browse Tree",
				"Browse the live scene tree of the running game. A convenience wrapper around "
				"runtime/browse_scene_tree with simpler parameter names. Returns lightweight "
				"per-node summaries (name, type, path, child count) for quick orientation. "
				"Start here to understand the scene structure, then use debug/get for property "
				"details on specific nodes.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPIntrospectionTools::handle_browse_tree));
	}

	// debug/get
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Node path or glob pattern (e.g., '/root/Main/Player' or "
				"'/root/Main/Enemy*'). Glob patterns match against node paths.");
		props["property"] = make_prop("string",
				"Property name or glob pattern to read (e.g., 'position' or "
				"'global_*'). Supports shell-style globs.");
		props["include_private"] = make_prop("boolean",
				"If true, include private/internal properties. Default: false.");
		props["where"] = make_prop("string",
				"Filter expression to narrow results (e.g., 'health > 0'). "
				"Applied after glob matching.");
		props["summarize"] = make_prop("boolean",
				"If true, return a summary (min/max/avg for numerics, counts for "
				"booleans) instead of individual values. Default: false.");
		props["group_by_value"] = make_prop("boolean",
				"If true, group results by property value. Useful with globs to see "
				"which nodes share the same value. Default: false.");
		Array required;
		required.push_back("path");
		required.push_back("property");
		p_registry->register_tool(
				"debug/get", "Get Property",
				"Read one or more properties from live nodes in the running game. Supports "
				"glob patterns for both path and property to batch-read across multiple nodes. "
				"Use 'where' to filter, 'summarize' for aggregate stats, or 'group_by_value' "
				"for distribution analysis. Paths are absolute from '/root'.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPIntrospectionTools::handle_get));
	}

	// debug/set
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Node path or glob pattern (e.g., '/root/Main/Player' or "
				"'/root/Main/Enemy*'). Glob patterns apply the set to all matches.");
		props["property"] = make_prop("string",
				"Property name to set (e.g., 'position', 'visible').");
		// For "value" we use a schema without a fixed type to accept any JSON value.
		Dictionary value_prop;
		value_prop["description"] = "The value to assign. Type must match the property "
									"(e.g., number for float properties, object for Vector2, etc.).";
		props["value"] = value_prop;
		props["where"] = make_prop("string",
				"Filter expression to narrow which nodes are affected "
				"(e.g., 'health > 0'). Applied after glob matching.");
		Array required;
		required.push_back("path");
		required.push_back("property");
		required.push_back("value");
		p_registry->register_tool(
				"debug/set", "Set Property",
				"Set a property on one or more live nodes in the running game. Supports "
				"glob patterns in path to batch-set across multiple nodes. Use 'where' to "
				"conditionally apply. Changes take effect immediately in the running game "
				"but do not modify the saved scene files.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPIntrospectionTools::handle_set));
	}

	// debug/call
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Node path or glob pattern (e.g., '/root/Main/Player' or "
				"'/root/Main/Enemy*'). Glob patterns call the method on all matches.");
		props["method"] = make_prop("string",
				"Method name to call (e.g., 'queue_free', 'apply_damage').");
		Dictionary args_prop;
		args_prop["type"] = "array";
		args_prop["description"] = "Positional arguments to pass to the method. "
								   "Each element is passed as-is. Default: [] (no arguments).";
		props["args"] = args_prop;
		props["where"] = make_prop("string",
				"Filter expression to narrow which nodes the method is called on "
				"(e.g., 'is_in_group(\"enemies\")'). Applied after glob matching.");
		Array required;
		required.push_back("path");
		required.push_back("method");
		p_registry->register_tool(
				"debug/call", "Call Method",
				"Call a method on one or more live nodes in the running game. Supports "
				"glob patterns in path to batch-call across multiple nodes. Use 'where' to "
				"conditionally apply. The method executes immediately in the game process.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPIntrospectionTools::handle_call));
	}

	// console/execute
	{
		Dictionary props;
		props["command"] = make_prop("string",
				"The command string to execute in the game's debug console. "
				"This is evaluated as a GDScript expression in the context of "
				"the running scene tree.");
		Array required;
		required.push_back("command");
		p_registry->register_tool(
				"console/execute", "Execute Console Command",
				"Execute a command in the running game's debug console. The command is "
				"evaluated as a GDScript expression with access to the scene tree. "
				"Returns the result or error. The game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPIntrospectionTools::handle_console_execute));
	}
}

// ============================================================================
// Common Helpers
// ============================================================================

MCPDebuggerBridge *MCPIntrospectionTools::_get_bridge() {
	MCPDebuggerBridge *b = MCPDebuggerBridge::get_singleton();
	if (!b) {
		WARN_PRINT("[MCP] MCPIntrospectionTools::_get_bridge() -- singleton is null!");
	}
	return b;
}

Dictionary MCPIntrospectionTools::_require_game_running() {
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error(
				"Internal error: debugger bridge not available.");
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

// ============================================================================
// Handler Implementations
// ============================================================================

// --- debug/describe_class ---

Dictionary MCPIntrospectionTools::handle_describe_class(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String class_name = p_args.get("class", "");
	if (class_name.is_empty()) {
		return make_tool_error(
				"Missing required parameter: class\n\n"
				"Provide the name of a Godot class (e.g., 'Node2D', 'CharacterBody3D').");
	}

	bool include_private = (bool)p_args.get("include_private", false);
	bool include_docs = (bool)p_args.get("include_docs", false);

	MCPDebuggerBridge *bridge = _get_bridge();

	Array data;
	data.push_back(class_name);
	data.push_back(include_private);
	data.push_back(include_docs);

	Dictionary result = bridge->send_debug_command("debug_describe_class", data);

	if (!(bool)result.get("success", false)) {
		String error_msg = result.get("value", result.get("error", "Unknown error"));
		return make_tool_error(
				"Failed to describe class '" + class_name + "': " + error_msg + "\n\n"
				"Make sure the class name is spelled correctly and exists in ClassDB.");
	}

	String value = result.get("value", "");
	return make_tool_result(value);
}

// --- debug/browse_tree ---

Dictionary MCPIntrospectionTools::handle_browse_tree(const Dictionary &p_args) {
	// This wraps the existing runtime/browse_scene_tree by forwarding to
	// MCPDebugTools::handle_browse_scene_tree with translated parameter names.
	// Rather than coupling to that class, we replicate the core logic using
	// the same bridge calls.

	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String path = p_args.get("path", "/root");
	int depth = (int)p_args.get("depth", 2);
	int child_limit = (int)p_args.get("child_limit", 50);
	int child_offset = (int)p_args.get("child_offset", 0);

	// Build args in the format expected by runtime/browse_scene_tree.
	Dictionary browse_args;
	browse_args["root_path"] = path;
	browse_args["max_depth"] = depth;
	browse_args["offset"] = child_offset;
	browse_args["limit"] = child_limit;
	browse_args["include_indicators"] = true;
	browse_args["include_stats"] = true;
	browse_args["refresh"] = false;

	// Delegate to the existing browse handler.
	return MCPDebugTools::handle_browse_scene_tree(browse_args);
}

// --- debug/get ---

Dictionary MCPIntrospectionTools::handle_get(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String path = p_args.get("path", "");
	String property = p_args.get("property", "");

	if (path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: path\n\n"
				"Provide a node path (e.g., '/root/Main/Player') or glob pattern "
				"(e.g., '/root/Main/Enemy*').");
	}
	if (property.is_empty()) {
		return make_tool_error(
				"Missing required parameter: property\n\n"
				"Provide a property name (e.g., 'position') or glob pattern "
				"(e.g., 'global_*').");
	}

	// Build options dictionary for JSON serialization.
	Dictionary options;
	if (p_args.has("include_private")) {
		options["include_private"] = (bool)p_args["include_private"];
	}
	if (p_args.has("where")) {
		options["where"] = (String)p_args["where"];
	}
	if (p_args.has("summarize")) {
		options["summarize"] = (bool)p_args["summarize"];
	}
	if (p_args.has("group_by_value")) {
		options["group_by_value"] = (bool)p_args["group_by_value"];
	}

	String options_json = JSON::stringify(options);

	MCPDebuggerBridge *bridge = _get_bridge();

	Array data;
	data.push_back(path);
	data.push_back(property);
	data.push_back(options_json);

	Dictionary result = bridge->send_debug_command("debug_get_property", data);

	if (!(bool)result.get("success", false)) {
		String error_msg = result.get("value", result.get("error", "Unknown error"));
		return make_tool_error(
				"Failed to get property '" + property + "' on '" + path + "': " + error_msg + "\n\n"
				"Use debug/browse_tree to verify the node path exists, or "
				"debug/describe_class to check available properties.");
	}

	String value = result.get("value", "");
	return make_tool_result(value);
}

// --- debug/set ---

Dictionary MCPIntrospectionTools::handle_set(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String path = p_args.get("path", "");
	String property = p_args.get("property", "");

	if (path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: path\n\n"
				"Provide a node path (e.g., '/root/Main/Player') or glob pattern.");
	}
	if (property.is_empty()) {
		return make_tool_error(
				"Missing required parameter: property\n\n"
				"Provide the property name to set (e.g., 'position', 'visible').");
	}
	if (!p_args.has("value")) {
		return make_tool_error(
				"Missing required parameter: value\n\n"
				"Provide the value to assign to the property.");
	}

	Variant value = p_args["value"];
	String value_json = JSON::stringify(value);

	// Build options dictionary.
	Dictionary options;
	if (p_args.has("where")) {
		options["where"] = (String)p_args["where"];
	}

	String options_json = JSON::stringify(options);

	MCPDebuggerBridge *bridge = _get_bridge();

	Array data;
	data.push_back(path);
	data.push_back(property);
	data.push_back(value_json);
	data.push_back(options_json);

	Dictionary result = bridge->send_debug_command("debug_set_property", data);

	if (!(bool)result.get("success", false)) {
		String error_msg = result.get("value", result.get("error", "Unknown error"));
		return make_tool_error(
				"Failed to set property '" + property + "' on '" + path + "': " + error_msg + "\n\n"
				"Use debug/browse_tree to verify the node path, or debug/describe_class "
				"to check the property type and name.");
	}

	String result_value = result.get("value", "");
	return make_tool_result(result_value);
}

// --- debug/call ---

Dictionary MCPIntrospectionTools::handle_call(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String path = p_args.get("path", "");
	String method = p_args.get("method", "");

	if (path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: path\n\n"
				"Provide a node path (e.g., '/root/Main/Player') or glob pattern.");
	}
	if (method.is_empty()) {
		return make_tool_error(
				"Missing required parameter: method\n\n"
				"Provide the method name to call (e.g., 'queue_free', 'apply_damage').");
	}

	Array args = p_args.get("args", Array());
	String args_json = JSON::stringify(args);

	// Build options dictionary.
	Dictionary options;
	if (p_args.has("where")) {
		options["where"] = (String)p_args["where"];
	}

	String options_json = JSON::stringify(options);

	MCPDebuggerBridge *bridge = _get_bridge();

	Array data;
	data.push_back(path);
	data.push_back(method);
	data.push_back(args_json);
	data.push_back(options_json);

	Dictionary result = bridge->send_debug_command("debug_call_method", data);

	if (!(bool)result.get("success", false)) {
		String error_msg = result.get("value", result.get("error", "Unknown error"));
		return make_tool_error(
				"Failed to call '" + method + "' on '" + path + "': " + error_msg + "\n\n"
				"Use debug/browse_tree to verify the node path, or debug/describe_class "
				"to check available methods and their signatures.");
	}

	String result_value = result.get("value", "");
	return make_tool_result(result_value);
}

// --- console/execute ---

Dictionary MCPIntrospectionTools::handle_console_execute(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String command = p_args.get("command", "");
	if (command.is_empty()) {
		return make_tool_error(
				"Missing required parameter: command\n\n"
				"Provide a GDScript expression to evaluate in the running game.");
	}

	MCPDebuggerBridge *bridge = _get_bridge();

	// Use the evaluate mechanism to execute the command as a GDScript expression.
	Dictionary result = bridge->send_evaluate(command);

	if (!(bool)result.get("success", false)) {
		String error_msg = result.get("value", result.get("error", "Unknown error"));
		return make_tool_error(
				"Console command failed: " + error_msg + "\n\n"
				"The command is evaluated as a GDScript expression. Make sure the syntax "
				"is valid and any referenced nodes/variables exist in the running scene.");
	}

	String value = result.get("value", "");
	if (value.is_empty()) {
		return make_tool_result("Command executed successfully (no return value).");
	}
	return make_tool_result(value);
}
