/**************************************************************************/
/*  mcp_debug_tools.cpp                                                   */
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

#include "mcp_debug_tools.h"

#include "core/object/script_language.h"

#include "../mcp_debugger_bridge.h"
#include "../mcp_protocol.h"
#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "editor/run/editor_run_bar.h"

// ============================================================================
// Tool Registration
// ============================================================================

void MCPDebugTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// --- Category E: Game Lifecycle ---

	// debug/run_project
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"debug/run_project", "Run Project",
				"Launch the project's main scene in debug mode. If already running, stops first. "
				"Returns immediately. Poll debug/get_status every 1-2s: 'launching' -> 'running' (ready) "
				"or 'stopped' (failed, check debug/get_errors). Times out after 15s.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDebugTools::handle_run_project));
	}

	// debug/run_scene
	{
		Dictionary props;
		props["scene"] = make_prop("string",
				"Scene file path in res:// format (e.g., res://scenes/level1.tscn)");
		Array required;
		required.push_back("scene");
		p_registry->register_tool(
				"debug/run_scene", "Run Scene",
				"Launch a specific scene in debug mode. If already running, stops first. "
				"Returns immediately. Poll debug/get_status every 1-2s. Path must be .tscn in res:// format.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDebugTools::handle_run_scene));
	}

	// debug/stop
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"debug/stop", "Stop Running Game",
				"Stop the currently running game. If no game is running, this is a no-op. "
				"Output and error buffers are preserved after stopping.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDebugTools::handle_stop));
	}

	// --- Category F: Live Inspection ---

	// debug/get_status
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"debug/get_status", "Get Game Status",
				"Get the current state of the game session: stopped, launching, running, or paused. "
				"When running/paused, includes uptime and frame count. When stopped, includes "
				"stop_reason ('not_started', 'normal', or 'timeout').",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDebugTools::handle_get_status));
	}

	// debug/get_output
	{
		Dictionary props;
		props["cursor"] = make_prop("integer",
				"Cursor from a previous call. Omit for latest output.");
		props["limit"] = make_prop("integer",
				"Max lines to return (default: 200, max: 1000)");
		Array required;
		p_registry->register_tool(
				"debug/get_output", "Get Game Output",
				"Get captured print/log output from the running game. Uses cursor-based pagination "
				"so output is NEVER lost. First call returns latest output. Pass returned cursor "
				"to get only new output.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDebugTools::handle_get_output));
	}

	// debug/get_errors
	{
		Dictionary props;
		props["cursor"] = make_prop("integer",
				"Cursor from a previous call. Omit for latest errors.");
		props["limit"] = make_prop("integer",
				"Max errors to return (default: 50, max: 500)");
		Array required;
		p_registry->register_tool(
				"debug/get_errors", "Get Runtime Errors",
				"Get captured runtime errors from the running game. Uses cursor-based pagination. "
				"Each error includes the message text and type.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDebugTools::handle_get_errors));
	}

	// debug/get_scene_tree
	{
		Dictionary props;
		props["max_depth"] = make_prop("integer",
				"Maximum tree depth (default: unlimited). Use 2-3 for overview.");
		Array required;
		p_registry->register_tool(
				"debug/get_scene_tree", "Get Remote Scene Tree",
				"Get the scene tree from the running game. Returns hierarchical view of all "
				"nodes with names, types, IDs, and scene files. Game must be running. "
				"Async round-trip with 10s timeout.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPDebugTools::handle_get_scene_tree));
	}

	// debug/get_node_properties
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to the node in the scene tree (e.g., '/root/Main/Player')");
		props["filter"] = make_prop("string",
				"Glob pattern to filter property names (e.g., 'position*')");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"debug/get_node_properties", "Get Node Properties",
				"Get properties of a specific node in the running game. Returns property names, "
				"types, and values via expression evaluation. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPDebugTools::handle_get_node_properties));
	}

	// debug/search_scene_tree
	{
		Dictionary props;
		props["name_pattern"] = make_prop("string",
				"Glob pattern to match node names (e.g., '*Button*'). Case-insensitive.");
		props["type"] = make_prop("string",
				"Exact type name to filter by (e.g., 'Button', 'Sprite2D')");
		props["refresh"] = make_prop("boolean",
				"Fetch fresh tree before searching (default: false)");
		Array required;
		p_registry->register_tool(
				"debug/search_scene_tree", "Search Scene Tree",
				"Search the running game's scene tree by node name pattern and/or type. "
				"Returns matching nodes with full tree paths and IDs. Uses cached tree by default.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPDebugTools::handle_search_scene_tree));
	}

	// debug/get_performance
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"debug/get_performance", "Get Performance Metrics",
				"Get real-time performance metrics: FPS, frame time, memory, object/node/orphan counts. "
				"Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPDebugTools::handle_get_performance));
	}

	// --- Category H: Session Summary ---

	// debug/get_session_summary
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"debug/get_session_summary", "Get Session Summary",
				"Comprehensive snapshot of current game session: status, scene tree (depth 2), "
				"recent output (last 20 lines), recent errors (last 5), and performance metrics. "
				"If game is not running, returns only status. Much more efficient than 5 separate calls.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPDebugTools::handle_session_summary));
	}
}

// ============================================================================
// Common Helpers
// ============================================================================

MCPDebuggerBridge *MCPDebugTools::_get_bridge() {
	MCPProtocol *protocol = MCPProtocol::get_singleton();
	if (!protocol) {
		return nullptr;
	}
	return protocol->get_debugger_bridge();
}

Dictionary MCPDebugTools::_require_game_running() {
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error(
				"Internal error: debugger bridge not available.");
	}
	if (!bridge->is_game_running()) {
		return make_tool_error(
				"No game is currently running.\n\n"
				"If the game stopped unexpectedly, check debug/get_errors for runtime errors.\n"
				"To start a game: debug/run_project (main scene) or debug/run_scene (specific scene).\n"
				"To check status: debug/get_status (includes stop_reason when stopped).");
	}
	// Return empty dict to signal "OK, proceed"
	return Dictionary();
}

// ============================================================================
// Category E: Game Lifecycle
// ============================================================================

Dictionary MCPDebugTools::handle_run_project(const Dictionary &p_args) {
	// Get main scene path for the response.
	String main_scene = ProjectSettings::get_singleton()->get_setting(
			"application/run/main_scene", "");

	if (main_scene.is_empty()) {
		return make_tool_error(
				"No main scene is configured for this project.\n\n"
				"Set the main scene in Project Settings > Application > Run > Main Scene,\n"
				"or use debug/run_scene to launch a specific scene.");
	}

	// Set launching state before queuing the play action.
	MCPDebuggerBridge *bridge = _get_bridge();
	if (bridge) {
		bridge->set_game_launching();
	}

	// Dispatch to main thread. EditorRunBar::play_main_scene() must run
	// on the main thread because it modifies editor UI state.
	callable_mp(EditorRunBar::get_singleton(),
			&EditorRunBar::play_main_scene)
			.call_deferred();

	// Build response.
	String text = "Project launch queued. Main scene: " + main_scene + "\n"
														  "Use debug/get_status to check when the game is running.";

	Dictionary structured;
	structured["status"] = "launching";
	structured["main_scene"] = main_scene;

	return make_tool_result(text, structured);
}

Dictionary MCPDebugTools::handle_run_scene(const Dictionary &p_args) {
	String scene = p_args.get("scene", "");

	if (scene.is_empty()) {
		return make_tool_error(
				"Missing required parameter: scene\n\n"
				"Provide a .tscn file path in res:// format.\n"
				"Example: debug/run_scene { \"scene\": \"res://scenes/level1.tscn\" }");
	}

	// Validate path format.
	if (!validate_path(scene)) {
		return make_tool_error(vformat(
				"Invalid path: %s\n\n"
				"Paths must start with res:// and cannot contain '..' traversal sequences.",
				scene));
	}

	// Validate extension.
	if (!scene.ends_with(".tscn")) {
		return make_tool_error(vformat(
				"Invalid scene file: %s\n\n"
				"Only .tscn files can be launched. Provide a path ending in .tscn.",
				scene));
	}

	// Verify the file exists.
	if (!FileAccess::exists(scene)) {
		return make_tool_error(vformat(
				"Scene file not found: %s\n\n"
				"Use editor/list_files to find available .tscn files.",
				scene));
	}

	// Set launching state before queuing the play action.
	MCPDebuggerBridge *bridge = _get_bridge();
	if (bridge) {
		bridge->set_game_launching();
	}

	// Dispatch to main thread.
	callable_mp(EditorRunBar::get_singleton(),
			&EditorRunBar::play_custom_scene)
			.call_deferred(scene);

	String text = "Scene launch queued: " + scene + "\n"
												"Use debug/get_status to check when the game is running.";

	Dictionary structured;
	structured["status"] = "launching";
	structured["scene"] = scene;

	return make_tool_result(text, structured);
}

Dictionary MCPDebugTools::handle_stop(const Dictionary &p_args) {
	// Stop is a no-op if no game is running -- returns success either way.
	callable_mp(EditorRunBar::get_singleton(),
			&EditorRunBar::stop_playing)
			.call_deferred();

	String text = "Game stop queued.";

	Dictionary structured;
	structured["status"] = "stopped";

	return make_tool_result(text, structured);
}

// ============================================================================
// Category F: Live Inspection
// ============================================================================

Dictionary MCPDebugTools::handle_get_status(const Dictionary &p_args) {
	MCPDebuggerBridge *bridge = _get_bridge();

	// State machine: stopped -> launching -> running -> paused -> stopped
	String state = "stopped";
	String scene_path;
	double uptime_seconds = 0.0;
	int64_t frame_count = 0;

	if (bridge) {
		if (bridge->is_game_launching()) {
			state = "launching";
		} else if (bridge->is_game_running()) {
			if (bridge->is_game_paused()) {
				state = "paused";
			} else {
				state = "running";
			}

			uptime_seconds = bridge->get_game_uptime_seconds();
			frame_count = bridge->get_game_frame_count();

			// Scene path from cached tree.
			Dictionary cached_tree = bridge->get_cached_scene_tree();
			if (!cached_tree.is_empty()) {
				Array children = cached_tree.get("children", Array());
				if (children.size() > 0) {
					Dictionary first_child = children[0];
					scene_path = first_child.get("scene_file_path", "");
				}
			}
		}
	}

	// Build text response.
	String text = "Game Status: " + state;
	if (!scene_path.is_empty()) {
		text += "\nScene: " + scene_path;
	}
	if (state == "running" || state == "paused") {
		text += "\nUptime: " + String::num(uptime_seconds, 1) + "s";
		text += "\nFrame: " + itos(frame_count);
	}

	Dictionary structured;
	structured["state"] = state;
	structured["scene"] = scene_path.is_empty() ? Variant() : Variant(scene_path);
	structured["uptime_seconds"] = uptime_seconds;
	structured["frame_count"] = frame_count;

	if (state == "stopped" && bridge) {
		structured["stop_reason"] = bridge->get_last_stop_reason();
	}

	return make_tool_result(text, structured);
}

Dictionary MCPDebugTools::handle_get_output(const Dictionary &p_args) {
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Internal error: debugger bridge not available.");
	}

	uint64_t cursor = (uint64_t)(int64_t)p_args.get("cursor", 0);
	int limit = (int)p_args.get("limit", 200);
	limit = CLAMP(limit, 1, 1000);

	Vector<OutputEntry> entries = bridge->get_output_since(cursor, limit);

	// Determine the new cursor value.
	uint64_t new_cursor = cursor;
	if (entries.size() > 0) {
		new_cursor = entries[entries.size() - 1].seq;
	} else {
		new_cursor = bridge->get_output_latest_seq();
	}

	// Build text.
	String text;
	if (entries.is_empty()) {
		if (cursor == 0) {
			text = "No output captured yet.";
		} else {
			text = "No new output since cursor " + itos(new_cursor) + ".";
		}
	} else {
		text = "Game Output (" + itos(entries.size()) + " lines, cursor: " + itos(new_cursor) + "):\n\n";
		for (int i = 0; i < entries.size(); i++) {
			const OutputEntry &e = entries[i];
			String type_tag;
			switch (e.type) {
				case 0:
					type_tag = "[stdout]";
					break;
				case 1:
					type_tag = "[rich]";
					break;
				case 3:
					type_tag = "[warning]";
					break;
				default:
					type_tag = "[stdout]";
					break;
			}
			text += type_tag + " " + e.text + "\n";
		}
	}

	// Build structured.
	Dictionary structured;
	structured["cursor"] = (int64_t)new_cursor;
	structured["has_more"] = entries.size() == limit;
	structured["line_count"] = entries.size();

	Array lines_array;
	for (int i = 0; i < entries.size(); i++) {
		const OutputEntry &e = entries[i];
		Dictionary line;
		line["seq"] = (int64_t)e.seq;
		line["type"] = e.type;
		line["text"] = e.text;
		line["timestamp_msec"] = (int64_t)e.timestamp_msec;
		lines_array.push_back(line);
	}
	structured["lines"] = lines_array;

	return make_tool_result(text, structured);
}

Dictionary MCPDebugTools::handle_get_errors(const Dictionary &p_args) {
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Internal error: debugger bridge not available.");
	}

	uint64_t cursor = (uint64_t)(int64_t)p_args.get("cursor", 0);
	int limit = (int)p_args.get("limit", 50);
	limit = CLAMP(limit, 1, 500);

	Vector<OutputEntry> entries = bridge->get_errors_since(cursor, limit);

	uint64_t new_cursor = cursor;
	if (entries.size() > 0) {
		new_cursor = entries[entries.size() - 1].seq;
	} else {
		new_cursor = bridge->get_error_latest_seq();
	}

	// Build text.
	String text;
	if (entries.is_empty()) {
		if (cursor == 0) {
			text = "No runtime errors captured.";
		} else {
			text = "No new errors since cursor " + itos(new_cursor) + ".";
		}
	} else {
		text = "Runtime Errors (" + itos(entries.size()) + " entries, cursor: " + itos(new_cursor) + "):\n\n";
		for (int i = 0; i < entries.size(); i++) {
			const OutputEntry &e = entries[i];
			String type_tag = (e.type == 1) ? "[warning]" : "[error]";
			text += type_tag + " " + e.text + "\n";
		}
	}

	// Build structured.
	Dictionary structured;
	structured["cursor"] = (int64_t)new_cursor;
	structured["has_more"] = entries.size() == limit;
	structured["error_count"] = entries.size();

	Array errors_array;
	for (int i = 0; i < entries.size(); i++) {
		const OutputEntry &e = entries[i];
		Dictionary err;
		err["seq"] = (int64_t)e.seq;
		err["text"] = e.text;
		err["type"] = e.type;
		err["is_warning"] = (e.type == 1);
		err["timestamp_msec"] = (int64_t)e.timestamp_msec;
		errors_array.push_back(err);
	}
	structured["errors"] = errors_array;

	return make_tool_result(text, structured);
}

Dictionary MCPDebugTools::handle_get_scene_tree(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	int max_depth = (int)p_args.get("max_depth", -1); // -1 = unlimited

	// Async round-trip to the game process.
	Dictionary result = bridge->request_scene_tree();

	if (!(bool)result.get("success", false)) {
		String error_msg = result.get("error", "Unknown error");
		return make_tool_error(
				"Failed to get scene tree: " + error_msg + "\n\n"
																"The game may have crashed or is unresponsive.\n"
																"Try debug/get_status to check game state, "
																"or debug/get_errors for runtime errors.");
	}

	Dictionary tree = result.get("tree", Dictionary());
	int node_count = _count_tree_nodes(tree);

	// Apply max_depth truncation if requested.
	Dictionary display_tree = tree;
	if (max_depth >= 0) {
		display_tree = _truncate_tree(tree, max_depth);
	}

	// Build text with node IDs.
	String text = "Scene Tree (" + itos(node_count) + " nodes):\n\n";
	text += _tree_to_text_with_ids(display_tree, 0, max_depth);

	// Build structured.
	Dictionary structured;
	structured["node_count"] = node_count;
	structured["tree"] = display_tree;

	return make_tool_result(text, structured);
}

Dictionary MCPDebugTools::handle_get_node_properties(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String node_path = p_args.get("node_path", "");
	String filter = p_args.get("filter", "");

	if (node_path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: node_path\n\n"
				"Provide the scene tree path of a node.\n"
				"Get paths from debug/get_scene_tree or debug/search_scene_tree results.");
	}

	MCPDebuggerBridge *bridge = _get_bridge();

	// Use the evaluate mechanism to introspect the node.
	// Build an expression that gets common properties from the node.
	// We evaluate a string representation which gives useful debug info.
	String expr = "get_root().get_node(\"" + node_path.replace("\"", "\\\"") + "\")";
	Dictionary result = bridge->send_evaluate(expr);

	if (!(bool)result.get("success", false)) {
		String error_msg = result.get("value", result.get("error", "Unknown error"));

		String guidance;
		if (error_msg.contains("null instance") || error_msg.contains("not found")) {
			guidance = "\n\nUse debug/search_scene_tree to find the correct path.";
		}

		return make_tool_error(
				"Failed to get properties for node: " + node_path + "\n\n" + error_msg + guidance);
	}

	String value_str = result.get("value", "");
	String text = "Properties of node " + node_path + ":\n\n" + value_str;

	Dictionary structured;
	structured["node_path"] = node_path;
	structured["result"] = value_str;

	return make_tool_result(text, structured);
}

// ============================================================================
// Scene Tree Helper Methods
// ============================================================================

int MCPDebugTools::_count_tree_nodes(const Dictionary &p_tree) {
	if (p_tree.is_empty()) {
		return 0;
	}

	int count = 1; // This node.
	Array children = p_tree.get("children", Array());
	for (int i = 0; i < children.size(); i++) {
		count += _count_tree_nodes(children[i]);
	}
	return count;
}

Dictionary MCPDebugTools::_truncate_tree(const Dictionary &p_tree, int p_max_depth, int p_current_depth) {
	if (p_tree.is_empty()) {
		return p_tree;
	}

	Dictionary result;
	result["name"] = p_tree.get("name", "?");
	result["type"] = p_tree.get("type", "?");
	result["id"] = p_tree.get("id", 0);
	result["scene_file_path"] = p_tree.get("scene_file_path", "");

	if (p_max_depth >= 0 && p_current_depth >= p_max_depth) {
		// At max depth -- count hidden children but do not include them.
		Array original_children = p_tree.get("children", Array());
		if (original_children.size() > 0) {
			int hidden_count = 0;
			for (int i = 0; i < original_children.size(); i++) {
				hidden_count += _count_tree_nodes(original_children[i]);
			}
			result["_hidden_children"] = hidden_count;
		}
		result["children"] = Array();
		return result;
	}

	Array children = p_tree.get("children", Array());
	Array truncated_children;
	for (int i = 0; i < children.size(); i++) {
		truncated_children.push_back(
				_truncate_tree(children[i], p_max_depth, p_current_depth + 1));
	}
	result["children"] = truncated_children;

	return result;
}

String MCPDebugTools::_tree_to_text_with_ids(const Dictionary &p_tree,
		int p_indent, int p_max_depth, int p_current_depth) {
	if (p_tree.is_empty()) {
		return "";
	}

	String indent;
	for (int i = 0; i < p_indent; i++) {
		indent += "  ";
	}

	String name = p_tree.get("name", "?");
	String type = p_tree.get("type", "?");
	int64_t id = (int64_t)p_tree.get("id", 0);
	String scene_path = p_tree.get("scene_file_path", "");

	String line = indent + name + " (" + type + ") [id:" + itos(id) + "]";
	if (!scene_path.is_empty()) {
		line += " [" + scene_path + "]";
	}

	// Show hidden children indicator.
	int hidden = (int)p_tree.get("_hidden_children", 0);
	if (hidden > 0) {
		line += " ... (" + itos(hidden) + " children hidden)";
	}

	line += "\n";

	Array children = p_tree.get("children", Array());
	for (int i = 0; i < children.size(); i++) {
		line += _tree_to_text_with_ids(children[i], p_indent + 1,
				p_max_depth, p_current_depth + 1);
	}

	return line;
}

Array MCPDebugTools::_search_tree(const Dictionary &p_tree,
		const String &p_name_pattern, const String &p_type_filter,
		const String &p_current_path) {
	Array matches;

	if (p_tree.is_empty()) {
		return matches;
	}

	String name = p_tree.get("name", "?");
	String type = p_tree.get("type", "?");
	int64_t id = (int64_t)p_tree.get("id", 0);
	String scene_path = p_tree.get("scene_file_path", "");

	String node_path = p_current_path.is_empty()
			? "/" + name
			: p_current_path + "/" + name;

	// Apply filters.
	bool name_match = p_name_pattern.is_empty() || name.matchn(p_name_pattern);
	bool type_match = p_type_filter.is_empty() || type == p_type_filter;

	if (name_match && type_match) {
		Dictionary match;
		match["name"] = name;
		match["type"] = type;
		match["id"] = id;
		match["path"] = node_path;
		match["scene_file"] = scene_path;
		matches.push_back(match);
	}

	// Recurse into children.
	Array children = p_tree.get("children", Array());
	for (int i = 0; i < children.size(); i++) {
		Array child_matches = _search_tree(children[i],
				p_name_pattern, p_type_filter, node_path);
		for (int j = 0; j < child_matches.size(); j++) {
			matches.push_back(child_matches[j]);
		}
	}

	return matches;
}

Dictionary MCPDebugTools::handle_search_scene_tree(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String name_pattern = p_args.get("name_pattern", "");
	String type_filter = p_args.get("type", "");
	bool refresh = (bool)p_args.get("refresh", false);

	if (name_pattern.is_empty() && type_filter.is_empty()) {
		return make_tool_error(
				"At least one of 'name_pattern' or 'type' must be provided.\n\n"
				"Examples:\n"
				"  { \"name_pattern\": \"*Button*\" }\n"
				"  { \"type\": \"CharacterBody2D\" }\n"
				"  { \"name_pattern\": \"Player*\", \"type\": \"CharacterBody2D\" }");
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	Dictionary tree;

	if (refresh) {
		// Fetch a fresh tree from the game.
		Dictionary result = bridge->request_scene_tree();
		if (!(bool)result.get("success", false)) {
			return make_tool_error(
					"Failed to refresh scene tree: " +
					String(result.get("error", "Unknown error")));
		}
		tree = result.get("tree", Dictionary());
	} else {
		tree = bridge->get_cached_scene_tree();
		if (tree.is_empty()) {
			// No cached tree -- auto-refresh.
			Dictionary result = bridge->request_scene_tree();
			if (!(bool)result.get("success", false)) {
				return make_tool_error(
						"No cached tree available and failed to fetch: " +
						String(result.get("error", "Unknown error")));
			}
			tree = result.get("tree", Dictionary());
		}
	}

	Array matches = _search_tree(tree, name_pattern, type_filter);

	// Build text.
	String text;
	String filter_desc;
	if (!name_pattern.is_empty()) {
		filter_desc += "name '" + name_pattern + "'";
	}
	if (!type_filter.is_empty()) {
		if (!filter_desc.is_empty()) {
			filter_desc += " and ";
		}
		filter_desc += "type '" + type_filter + "'";
	}

	if (matches.is_empty()) {
		text = "No nodes found matching " + filter_desc + ".\n\n"
															  "Note: search uses the cached tree. Set refresh: true "
															  "if the scene may have changed.";
	} else {
		text = "Found " + itos(matches.size()) + " nodes matching " + filter_desc + ":\n\n";
		for (int i = 0; i < matches.size(); i++) {
			Dictionary m = matches[i];
			text += "  " + String(m["path"]) + " (" + String(m["type"]) + ") " +
					"[id:" + itos((int64_t)m["id"]) + "]\n";
		}
	}

	// Build structured.
	Dictionary structured;
	structured["match_count"] = matches.size();
	structured["matches"] = matches;

	return make_tool_result(text, structured);
}

Dictionary MCPDebugTools::handle_get_performance(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	Dictionary result = bridge->send_get_performance();

	if (!(bool)result.get("success", false)) {
		return make_tool_error(
				"Failed to get performance metrics: " +
				String(result.get("error", "Unknown error")));
	}

	// Extract values from bridge result.
	double fps = result.get("fps", 0.0);
	double frame_time = result.get("frame_time", 0.0);
	double physics_fps = result.get("physics_fps", 0.0);
	int64_t static_memory = result.get("static_memory", 0);
	int64_t dynamic_memory = result.get("dynamic_memory", 0);
	int object_count = result.get("object_count", 0);
	int node_count = result.get("node_count", 0);
	int orphan_count = result.get("orphan_count", 0);
	int resource_count = result.get("resource_count", 0);

	// Format memory to MB.
	double static_mb = (double)static_memory / (1024.0 * 1024.0);
	double dynamic_mb = (double)dynamic_memory / (1024.0 * 1024.0);

	String text = "Performance Metrics:\n"
				  "  FPS: " +
			String::num(fps, 1) + "\n"
								  "  Frame Time: " +
			String::num(frame_time * 1000.0, 1) + "ms\n"
												   "  Physics FPS: " +
			String::num(physics_fps, 1) + "\n"
										  "  Static Memory: " +
			String::num(static_mb, 1) + " MB\n"
										"  Dynamic Memory: " +
			String::num(dynamic_mb, 1) + " MB\n"
										 "  Object Count: " +
			itos(object_count) + "\n"
								 "  Node Count: " +
			itos(node_count) + "\n"
							   "  Orphan Nodes: " +
			itos(orphan_count) + "\n"
								 "  Resource Count: " +
			itos(resource_count);

	Dictionary structured;
	structured["fps"] = fps;
	structured["frame_time_msec"] = frame_time * 1000.0;
	structured["physics_fps"] = physics_fps;
	structured["static_memory_bytes"] = static_memory;
	structured["dynamic_memory_bytes"] = dynamic_memory;
	structured["object_count"] = object_count;
	structured["node_count"] = node_count;
	structured["orphan_node_count"] = orphan_count;
	structured["resource_count"] = resource_count;

	return make_tool_result(text, structured);
}

// ============================================================================
// Category H: Session Summary
// ============================================================================

Dictionary MCPDebugTools::handle_session_summary(const Dictionary &p_args) {
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Internal error: debugger bridge not available.");
	}

	bool game_running = bridge->is_game_running();

	if (!game_running) {
		// Stopped -- return minimal response.
		String text = "=== SESSION SUMMARY ===\n\n"
					  "Status: stopped\n"
					  "No game is currently running.\n\n"
					  "Use debug/run_project to launch the main scene, "
					  "or debug/run_scene to launch a specific scene.\n\n"
					  "=== END SUMMARY ===";

		Dictionary structured;
		Dictionary status;
		status["state"] = "stopped";
		if (bridge) {
			status["stop_reason"] = bridge->get_last_stop_reason();
		}
		structured["status"] = status;
		structured["performance"] = Variant(); // null
		structured["scene_tree"] = Variant(); // null
		structured["recent_output"] = Variant(); // null
		structured["recent_errors"] = Variant(); // null

		return make_tool_result(text, structured);
	}

	// Game is running -- gather all data.

	// 1. Status.
	String state = "running";
	if (bridge->is_game_paused()) {
		state = "paused";
	}
	double uptime_seconds = bridge->get_game_uptime_seconds();
	int64_t frame_count = bridge->get_game_frame_count();

	// 2. Scene tree (depth 2) -- one async round-trip.
	Dictionary tree_result = bridge->request_scene_tree();
	Dictionary tree;
	int node_count = 0;
	String tree_text = "[unavailable]";

	if ((bool)tree_result.get("success", false)) {
		tree = tree_result.get("tree", Dictionary());
		node_count = _count_tree_nodes(tree);
		Dictionary truncated = _truncate_tree(tree, 2);
		tree_text = _tree_to_text_with_ids(truncated, 0, 2);
	}

	// Determine scene path from tree root's first child.
	String scene_path;
	if (!tree.is_empty()) {
		Array root_children = tree.get("children", Array());
		if (root_children.size() > 0) {
			Dictionary first_child = root_children[0];
			scene_path = first_child.get("scene_file_path", "");
		}
	}

	// 3. Performance metrics.
	Dictionary perf_result = bridge->send_get_performance();
	double fps = 0;
	double frame_time = 0;
	int perf_node_count = 0;
	int orphan_count = 0;

	if ((bool)perf_result.get("success", false)) {
		fps = perf_result.get("fps", 0.0);
		frame_time = (double)perf_result.get("frame_time", 0.0) * 1000.0;
		perf_node_count = perf_result.get("node_count", 0);
		orphan_count = perf_result.get("orphan_count", 0);
	}

	// 4. Recent output (last 20 lines).
	uint64_t output_seq = bridge->get_output_latest_seq();
	uint64_t output_cursor = (output_seq > 20) ? (output_seq - 20) : 0;
	Vector<OutputEntry> output_entries = bridge->get_output_since(output_cursor, 20);

	// 5. Recent errors (last 5).
	uint64_t error_seq = bridge->get_error_latest_seq();
	uint64_t error_cursor = (error_seq > 5) ? (error_seq - 5) : 0;
	Vector<OutputEntry> error_entries = bridge->get_errors_since(error_cursor, 5);

	// Build text.
	String text = "=== SESSION SUMMARY ===\n\n";

	// Status section.
	text += "Status: " + state + "\n";
	if (!scene_path.is_empty()) {
		text += "Scene: " + scene_path + "\n";
	}
	text += "Uptime: " + String::num(uptime_seconds, 1) + "s | Frame: " + itos(frame_count) + "\n";

	// Performance section.
	text += "\n--- Performance ---\n";
	if ((bool)perf_result.get("success", false)) {
		text += "FPS: " + String::num(fps, 1) +
				" | Frame Time: " + String::num(frame_time, 1) + "ms" +
				" | Nodes: " + itos(perf_node_count) +
				" | Orphans: " + itos(orphan_count) + "\n";
	} else {
		text += "[unavailable]\n";
	}

	// Scene tree section.
	text += "\n--- Scene Tree (depth 2) ---\n";
	text += tree_text;

	// Output section.
	text += "\n--- Recent Output (last 20 lines) ---\n";
	if (output_entries.is_empty()) {
		text += "[no output]\n";
	} else {
		for (int i = 0; i < output_entries.size(); i++) {
			const OutputEntry &e = output_entries[i];
			String tag = (e.type == 3) ? "[warning]" : "[stdout]";
			text += tag + " " + e.text + "\n";
		}
	}

	// Errors section.
	text += "\n--- Recent Errors (last 5) ---\n";
	if (error_entries.is_empty()) {
		text += "[no errors]\n";
	} else {
		for (int i = 0; i < error_entries.size(); i++) {
			const OutputEntry &e = error_entries[i];
			String tag = (e.type == 1) ? "[warning]" : "[error]";
			text += tag + " " + e.text + "\n";
		}
	}

	text += "\n=== END SUMMARY ===";

	// Build structured.
	Dictionary structured;

	Dictionary s_status;
	s_status["state"] = state;
	s_status["scene"] = scene_path;
	s_status["uptime_seconds"] = uptime_seconds;
	s_status["frame_count"] = frame_count;
	structured["status"] = s_status;

	if ((bool)perf_result.get("success", false)) {
		Dictionary s_perf;
		s_perf["fps"] = fps;
		s_perf["frame_time_msec"] = frame_time;
		s_perf["node_count"] = perf_node_count;
		s_perf["orphan_node_count"] = orphan_count;
		structured["performance"] = s_perf;
	}

	if ((bool)tree_result.get("success", false)) {
		Dictionary s_tree;
		s_tree["node_count"] = node_count;
		s_tree["tree"] = _truncate_tree(tree, 2);
		structured["scene_tree"] = s_tree;
	}

	Dictionary s_output;
	s_output["cursor"] = (int64_t)bridge->get_output_latest_seq();
	s_output["line_count"] = output_entries.size();
	structured["recent_output"] = s_output;

	Dictionary s_errors;
	s_errors["cursor"] = (int64_t)bridge->get_error_latest_seq();
	s_errors["error_count"] = error_entries.size();
	structured["recent_errors"] = s_errors;

	return make_tool_result(text, structured);
}
