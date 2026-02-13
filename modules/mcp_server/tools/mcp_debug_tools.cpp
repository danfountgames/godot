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

	// runtime/run_project
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"runtime/run_project", "Run Project",
				"Launch the project's main scene in debug mode. If already running, stops first. "
				"Returns immediately. Poll runtime/get_status every 1-2s: 'launching' -> 'running' (ready) "
				"or 'stopped' (failed, check runtime/get_errors). Times out after 15s. "
				"Tip: use runtime/run_scene instead to test individual scenes in isolation.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDebugTools::handle_run_project));
	}

	// runtime/run_scene
	{
		Dictionary props;
		props["scene"] = make_prop("string",
				"Scene file path in res:// format (e.g., res://scenes/level1.tscn). "
				"Use your native file tools to discover available .tscn scenes.");
		Array required;
		required.push_back("scene");
		p_registry->register_tool(
				"runtime/run_scene", "Run Scene",
				"Launch a specific scene in debug mode for targeted testing. Prefer this over "
				"runtime/run_project when testing a single feature, UI screen, or component in "
				"isolation — it's faster and produces cleaner debug output. Combine with "
				"debug/set_breakpoint, runtime/get_scene_tree, and runtime/get_node_signals to "
				"inspect behavior. If already running, stops first. "
				"Returns immediately. Poll runtime/get_status every 1-2s. Path must be .tscn in res:// format.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDebugTools::handle_run_scene));
	}

	// runtime/stop
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"runtime/stop", "Stop Running Game",
				"Stop the currently running game. If no game is running, this is a no-op. "
				"Output and error buffers are preserved after stopping.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDebugTools::handle_stop));
	}

	// --- Category F: Live Inspection ---

	// runtime/get_status
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"runtime/get_status", "Get Game Status",
				"Get the current state of the game session: stopped, launching, running, or paused. "
				"IMPORTANT: after runtime/run_project or runtime/run_scene, the game is NOT ready "
				"immediately — you MUST poll this every 1-2s until state='running' before using "
				"any runtime/ inspection tools. When running/paused, includes uptime and frame count. "
				"When stopped, includes stop_reason ('not_started', 'normal', or 'timeout').",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDebugTools::handle_get_status));
	}

	// runtime/get_output
	{
		Dictionary props;
		props["cursor"] = make_prop("integer",
				"Cursor from a previous call. Omit for latest output.");
		props["limit"] = make_prop("integer",
				"Max lines to return (default: 200, max: 1000)");
		Array required;
		p_registry->register_tool(
				"runtime/get_output", "Get Game Output",
				"Get captured print/log output from the running game. Uses cursor-based pagination "
				"so output is NEVER lost. First call returns latest output. Pass returned cursor "
				"to get only new output.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDebugTools::handle_get_output));
	}

	// runtime/get_errors
	{
		Dictionary props;
		props["cursor"] = make_prop("integer",
				"Cursor from a previous call. Omit for latest errors.");
		props["limit"] = make_prop("integer",
				"Max errors to return (default: 50, max: 500)");
		Array required;
		p_registry->register_tool(
				"runtime/get_errors", "Get Runtime Errors",
				"Get captured runtime errors from the running game. Uses cursor-based pagination. "
				"Each error includes the message text and type.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDebugTools::handle_get_errors));
	}

	// runtime/get_scene_tree
	{
		Dictionary props;
		props["max_depth"] = make_prop("integer",
				"Maximum tree depth (default: unlimited). Use 2-3 for overview.");
		Array required;
		p_registry->register_tool(
				"runtime/get_scene_tree", "Get Remote Scene Tree",
				"Get the full verbose scene tree from the running game. Returns hierarchical "
				"view of all nodes with names, types, IDs, and scene files. "
				"WARNING: This is expensive on large scenes. Prefer runtime/browse_scene_tree "
				"for exploration (lightweight, paginated, filterable) and only use this when "
				"you need complete node data. Game must be running. Async round-trip with 10s timeout.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPDebugTools::handle_get_scene_tree));
	}

	// runtime/get_node_properties
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to the node in the scene tree (e.g., '/root/Main/Player')");
		props["filter"] = make_prop("string",
				"Glob pattern to filter property names (e.g., 'position*')");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"runtime/get_node_properties", "Get Node Properties",
				"Get properties of a specific node in the running game. Returns property names, "
				"types, and values via expression evaluation. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPDebugTools::handle_get_node_properties));
	}

	// runtime/set_node_property
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Absolute path to the node (e.g., '/root/Main/Player'). "
				"Get paths from runtime/browse_scene_tree or runtime/search_scene_tree.");
		props["property"] = make_prop("string",
				"Property name to set (e.g., 'position', 'visible', 'modulate', 'text'). "
				"Use runtime/get_node_properties to discover available properties and their types.");
		Dictionary value_prop;
		value_prop["description"] = "The new value. Type must be compatible with the property: "
				"numbers for int/float, strings for String, booleans for bool, "
				"arrays like [x,y] for Vector2, [x,y,z] for Vector3, "
				"[r,g,b,a] for Color. Resource paths (res://...) auto-load for Object properties.";
		props["value"] = value_prop;
		props["field"] = make_prop("string",
				"Optional: set only one component of a compound type. "
				"e.g., field='x' with a Vector2 position sets only X. "
				"Valid fields: x/y for Vector2, x/y/z for Vector3, r/g/b/a for Color.");
		Array required;
		required.push_back("node_path");
		required.push_back("property");
		required.push_back("value");
		p_registry->register_tool(
				"runtime/set_node_property", "Set Node Property",
				"Set a property on a node in the running game — the live 'edit CSS' equivalent "
				"for Godot. Modify positions, visibility, colors, text, and any other property "
				"without restarting. Returns old and new values for confirmation. Supports "
				"field-level updates (e.g., just position.x) and auto-loads resource paths. "
				"Use runtime/get_node_properties to discover properties first. "
				"Key Godot facts: 'position' is LOCAL (relative to parent), 'global_position' "
				"is read-only — move nodes by setting 'position'. Rotation is in RADIANS "
				"(1.5708 = 90 degrees). 2D Y-axis points DOWN (positive Y = lower on screen). "
				"Color is [r,g,b,a] with 0.0-1.0 range. "
				"Tip: edit .gd scripts with normal file tools and they hot-reload automatically.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPDebugTools::handle_set_node_property));
	}

	// runtime/search_scene_tree
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
				"runtime/search_scene_tree", "Search Scene Tree",
				"Search the running game's scene tree by node name pattern and/or type. "
				"Returns matching nodes with full tree paths and IDs. Uses cached tree by default.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPDebugTools::handle_search_scene_tree));
	}

	// runtime/browse_scene_tree
	{
		Dictionary props;
		props["root_path"] = make_prop("string",
				"Subtree root path to browse from (default: '/root'). "
				"Use paths from previous browse results to drill in.");
		props["max_depth"] = make_prop("integer",
				"Max depth relative to root_path (default: 2). "
				"1 = immediate children. 0 = root only. -1 = unlimited.");
		props["type_filter"] = make_prop("string",
				"Exact type name filter (e.g., 'Button'). Ancestors always shown.");
		props["name_pattern"] = make_prop("string",
				"Glob pattern for node names (e.g., '*Enemy*'). Case-insensitive.");
		props["include_indicators"] = make_prop("boolean",
				"Include has_script/is_visible/group_count per node (default: true).");
		props["include_stats"] = make_prop("boolean",
				"Include summary statistics (default: true).");
		props["offset"] = make_prop("integer",
				"Skip N direct children of root (default: 0). For pagination.");
		props["limit"] = make_prop("integer",
				"Max direct children to return (default: 50, max: 200).");
		props["refresh"] = make_prop("boolean",
				"Fetch fresh tree from game (default: false, uses cache).");
		Array required;
		p_registry->register_tool(
				"runtime/browse_scene_tree", "Browse Scene Tree",
				"Preferred way to explore the running scene tree. Returns lightweight per-node "
				"summaries (name, type, path, child count, script/group indicators) for quick "
				"orientation. Supports subtree drilling, depth control, type/name filtering, "
				"and pagination. Start here to understand scene structure, then use "
				"runtime/get_node_properties for details on specific nodes. "
				"Node paths are absolute from '/root' (e.g., '/root/Main/Player'). "
				"Node names are case-sensitive. Uses cached tree by default.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false,
						/*idempotent=*/false),
				callable_mp_static(&MCPDebugTools::handle_browse_scene_tree));
	}

	// --- Category H: Session Summary ---

	// runtime/get_session_summary
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"runtime/get_session_summary", "Get Session Summary",
				"Comprehensive snapshot of current game session: status, FPS, scene tree (depth 2), "
				"recent output (last 20 lines), and recent errors (last 5). "
				"If game is not running, returns only status. Much more efficient than 4 separate calls.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPDebugTools::handle_session_summary));
	}
}

// ============================================================================
// Common Helpers
// ============================================================================

MCPDebuggerBridge *MCPDebugTools::_get_bridge() {
	MCPDebuggerBridge *b = MCPDebuggerBridge::get_singleton();
	if (!b) {
		WARN_PRINT("[MCP] MCPDebugTools::_get_bridge() — singleton is null!");
	}
	return b;
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
				"If the game stopped unexpectedly, check runtime/get_errors for runtime errors.\n"
				"To start a game: runtime/run_project (main scene) or runtime/run_scene (specific scene).\n"
				"To check status: runtime/get_status (includes stop_reason when stopped).");
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
				"or use runtime/run_scene to launch a specific scene.");
	}

	// Set launching state before queuing the play action.
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error(
				"Internal error: debugger bridge not available.\n"
				"The MCP server may not be fully initialized yet. Try again in a moment.");
	}
	bridge->set_game_launching();

	// Dispatch to main thread. EditorRunBar::play_main_scene() must run
	// on the main thread because it modifies editor UI state.
	callable_mp(EditorRunBar::get_singleton(),
			&EditorRunBar::play_main_scene)
			.bind(false, Vector<String>())
			.call_deferred();

	// Build response.
	String text = "Project launch queued. Main scene: " + main_scene + "\n"
														  "Use runtime/get_status to check when the game is running.";

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
				"Example: runtime/run_scene { \"scene\": \"res://scenes/level1.tscn\" }");
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
				"Use your native file tools to find available .tscn files.",
				scene));
	}

	// Set launching state before queuing the play action.
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error(
				"Internal error: debugger bridge not available.\n"
				"The MCP server may not be fully initialized yet. Try again in a moment.");
	}
	bridge->set_game_launching();

	// Dispatch to main thread.
	callable_mp(EditorRunBar::get_singleton(),
			&EditorRunBar::play_custom_scene)
			.bind(scene, Vector<String>())
			.call_deferred();

	String text = "Scene launch queued: " + scene + "\n"
												"Use runtime/get_status to check when the game is running.";

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

	if (!bridge) {
		Dictionary structured;
		structured["state"] = "error";
		structured["error"] = "debugger bridge not available";
		return make_tool_result("Game Status: error\nDebugger bridge not available. The MCP server may not be fully initialized.", structured);
	}

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
																"Try runtime/get_status to check game state, "
																"or runtime/get_errors for runtime errors.");
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
				"Get paths from runtime/get_scene_tree or runtime/search_scene_tree results.");
	}

	// Validate node_path contains only safe characters to prevent expression injection.
	// Valid Godot node paths contain: alphanumeric, underscore, forward slash,
	// period, at-sign, colon, hyphen, and space.
	for (int i = 0; i < node_path.length(); i++) {
		char32_t c = node_path[i];
		if (!is_ascii_alphanumeric_char(c) && c != '_' && c != '/' && c != '.' && c != '@' && c != ':' && c != '-' && c != ' ') {
			return make_tool_error("Invalid node path: contains disallowed character");
		}
	}

	MCPDebuggerBridge *bridge = _get_bridge();

	// Use the evaluate mechanism to introspect the node.
	// Build an expression that gets common properties from the node.
	// We evaluate a string representation which gives useful debug info.
	// Escape backslashes BEFORE quotes to prevent injection via crafted paths.
	String safe_path = node_path.replace("\\", "\\\\").replace("\"", "\\\"");
	String expr = "get_root().get_node(\"" + safe_path + "\")";
	Dictionary result = bridge->send_evaluate(expr);

	if (!(bool)result.get("success", false)) {
		String error_msg = result.get("value", result.get("error", "Unknown error"));

		String guidance;
		if (error_msg.contains("null instance") || error_msg.contains("not found")) {
			guidance = "\n\nUse runtime/search_scene_tree to find the correct path.";
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

// ============================================================================
// Set Node Property
// ============================================================================

Dictionary MCPDebugTools::handle_set_node_property(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String node_path = p_args.get("node_path", "");
	String property = p_args.get("property", "");
	Variant value = p_args.get("value", Variant());
	String field = p_args.get("field", "");

	// Validate required params.
	if (node_path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: node_path\n\n"
				"Provide the scene tree path of a node.\n"
				"Get paths from runtime/browse_scene_tree or runtime/search_scene_tree.");
	}
	if (property.is_empty()) {
		return make_tool_error(
				"Missing required parameter: property\n\n"
				"Provide the property name to set.\n"
				"Use runtime/get_node_properties to discover available properties.");
	}

	// Validate node_path characters (same sanitization as get_node_properties).
	for (int i = 0; i < node_path.length(); i++) {
		char32_t c = node_path[i];
		if (!is_ascii_alphanumeric_char(c) && c != '_' && c != '/' &&
				c != '.' && c != '@' && c != ':' && c != '-' && c != ' ') {
			return make_tool_error(vformat(
					"'node_path' contains invalid character '%c' at position %d. "
					"Allowed: alphanumeric, _ / . @ : - and space.",
					(char)c, i));
		}
	}

	// Validate property name characters (alphanumeric, underscore, slash, colon, period).
	for (int i = 0; i < property.length(); i++) {
		char32_t c = property[i];
		if (!is_ascii_alphanumeric_char(c) && c != '_' && c != '/' &&
				c != ':' && c != '.') {
			return make_tool_error(vformat(
					"'property' contains invalid character '%c' at position %d. "
					"Allowed: alphanumeric, _ / : and period.",
					(char)c, i));
		}
	}

	// Validate field name if present.
	if (!field.is_empty()) {
		for (int i = 0; i < field.length(); i++) {
			char32_t c = field[i];
			if (!is_ascii_alphanumeric_char(c) && c != '_') {
				return make_tool_error(vformat(
						"'field' contains invalid character '%c' at position %d. "
						"Allowed: alphanumeric and underscore.",
						(char)c, i));
			}
		}
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	Dictionary result = bridge->send_set_node_property(node_path, property, value, field);

	if (!(bool)result.get("success", false)) {
		String error_msg = result.get("error", "Unknown error");
		String guidance;
		if (error_msg.contains("not found")) {
			guidance = "\n\nUse runtime/search_scene_tree to find the correct path, "
					"or runtime/get_node_properties to see available properties.";
		}
		return make_tool_error("Failed to set property: " + error_msg + guidance);
	}

	// Build text output.
	String old_val = result.get("old_value", "");
	String old_type = result.get("old_type", "");
	String new_val = result.get("new_value", "");
	String new_type = result.get("new_type", "");

	String text = vformat("Property '%s' on %s updated.\n  Old: %s (%s)\n  New: %s (%s)",
			property, node_path, old_val, old_type, new_val, new_type);
	if (!field.is_empty()) {
		text += "\n  Field: " + field;
	}

	Dictionary structured;
	structured["node_path"] = node_path;
	structured["property"] = property;
	structured["old_value"] = old_val;
	structured["old_type"] = old_type;
	structured["new_value"] = new_val;
	structured["new_type"] = new_type;
	structured["success"] = true;
	if (!field.is_empty()) {
		structured["field"] = field;
	}

	return make_tool_result(text, structured);
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

// ============================================================================
// Browse Scene Tree - Handler
// ============================================================================

// Comparator for sorting type distribution entries by count descending.
// Defined here (before the handler) because it is used by handle_browse_scene_tree.
struct _SortTypeCountDesc {
	bool operator()(const Pair<String, int> &a, const Pair<String, int> &b) const {
		return a.second > b.second;
	}
};

Dictionary MCPDebugTools::handle_browse_scene_tree(const Dictionary &p_args) {
	// 1. Require game running.
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	// 2. Parse parameters with defaults.
	String root_path = p_args.get("root_path", "/root");
	int max_depth = (int)p_args.get("max_depth", 2);
	String type_filter = p_args.get("type_filter", "");
	String name_pattern = p_args.get("name_pattern", "");
	bool include_indicators = (bool)p_args.get("include_indicators", true);
	bool include_stats = (bool)p_args.get("include_stats", true);
	int offset = (int)p_args.get("offset", 0);
	int limit = (int)p_args.get("limit", 50);
	bool refresh = (bool)p_args.get("refresh", false);

	// 3. Clamp pagination values.
	offset = MAX(offset, 0);
	limit = CLAMP(limit, 1, 200);

	// 4. Validate root_path characters (prevent injection).
	for (int i = 0; i < root_path.length(); i++) {
		char32_t c = root_path[i];
		if (!is_ascii_alphanumeric_char(c) && c != '_' && c != '/' && c != '.' && c != '@' && c != ':' && c != '-' && c != ' ') {
			return make_tool_error(
					"Invalid root_path: contains disallowed character.\n\n"
					"Valid characters: alphanumeric, _, /, ., @, :, -, space.");
		}
	}

	// 5. Get the scene tree (cached or fresh).
	MCPDebuggerBridge *bridge = _get_bridge();
	Dictionary full_tree;
	bool used_cache = false;

	if (refresh) {
		Dictionary result = bridge->request_browse_scene_tree();
		if (!(bool)result.get("success", false)) {
			return make_tool_error("Failed to fetch scene tree: " +
					String(result.get("error", "Unknown")));
		}
		full_tree = result.get("tree", Dictionary());
	} else {
		full_tree = bridge->get_cached_browse_tree();
		if (full_tree.is_empty()) {
			// No browse-specific cache -- try regular cached tree as fallback.
			full_tree = bridge->get_cached_scene_tree();
		}
		if (full_tree.is_empty()) {
			// Auto-refresh on cache miss.
			Dictionary result = bridge->request_browse_scene_tree();
			if (!(bool)result.get("success", false)) {
				return make_tool_error("No cached tree and failed to fetch: " +
						String(result.get("error", "Unknown")));
			}
			full_tree = result.get("tree", Dictionary());
		} else {
			used_cache = true;
		}
	}

	if (full_tree.is_empty()) {
		return make_tool_error(
				"Scene tree is empty. The game may not have initialized yet.\n\n"
				"Try runtime/get_status to check game state.");
	}

	// 6. Navigate to the requested subtree root.
	Dictionary subtree = _find_subtree(full_tree, root_path);
	if (subtree.is_empty()) {
		return make_tool_error("Subtree not found: " + root_path +
				"\n\nUse runtime/browse_scene_tree with no root_path to see the full tree, "
				"or runtime/search_scene_tree to find a node by name.");
	}

	// 7. Apply filters if requested.
	Dictionary working_tree = subtree;
	bool has_filters = !type_filter.is_empty() || !name_pattern.is_empty();
	if (has_filters) {
		working_tree = _filter_tree(subtree, type_filter, name_pattern);
		if (working_tree.is_empty()) {
			// No matches.
			String filter_desc;
			if (!type_filter.is_empty()) {
				filter_desc += "type='" + type_filter + "'";
			}
			if (!name_pattern.is_empty()) {
				if (!filter_desc.is_empty()) {
					filter_desc += " and ";
				}
				filter_desc += "name='" + name_pattern + "'";
			}

			String text = "No nodes matching " + filter_desc + " found under " + root_path + ".\n\n"
						  "Try runtime/search_scene_tree to search by name, or call runtime/browse_scene_tree "
						  "without filters to see the full structure.";

			Dictionary structured;
			structured["root_path"] = root_path;
			structured["node"] = Dictionary();
			structured["cached"] = used_cache || !refresh;
			Dictionary filters_applied;
			filters_applied["type_filter"] = type_filter;
			filters_applied["name_pattern"] = name_pattern;
			structured["filters_applied"] = filters_applied;
			structured["match_count"] = 0;

			return make_tool_result(text, structured);
		}
	}

	// 8. Cap max_depth for safety (-1 = unlimited, capped at 100).
	int effective_depth = max_depth;
	if (effective_depth < 0) {
		effective_depth = 100;
	}

	// 9. Build the browse-format tree with pagination.
	Dictionary browse_node = _build_browse_node(working_tree, root_path,
			effective_depth, include_indicators, offset, limit, true);

	// 10. Build statistics if requested.
	Dictionary stats;
	if (include_stats) {
		int total_descendants = _count_descendants(subtree);
		stats["total_nodes"] = total_descendants + 1;
		stats["type_distribution"] = _build_type_distribution(subtree);
		stats["groups"] = _build_group_stats(subtree);
	}

	// 11. Build pagination info (based on unfiltered subtree children for correct total).
	Array all_children = subtree.get("children", Array());
	int total_children_for_pagination = all_children.size();
	if (has_filters) {
		// When filtered, report filtered children count.
		Array filtered_children = working_tree.get("children", Array());
		total_children_for_pagination = filtered_children.size();
	}

	Dictionary pagination;
	pagination["offset"] = offset;
	pagination["limit"] = limit;
	pagination["total_children"] = total_children_for_pagination;
	pagination["has_more"] = (offset + limit) < total_children_for_pagination;

	// 12. Build text representation.
	int total_nodes = _count_descendants(subtree) + 1;
	String text = "Scene Tree Browser: " + root_path +
			" (" + itos(total_nodes) + " nodes";
	if (!type_filter.is_empty()) {
		text += ", filtered: type='" + type_filter + "'";
	}
	if (!name_pattern.is_empty()) {
		text += ", filtered: name='" + name_pattern + "'";
	}
	text += ")\n\n";
	text += _browse_node_to_text(browse_node, 0, include_indicators);

	// Append stats summary.
	if (include_stats && stats.has("type_distribution")) {
		Dictionary type_dist = stats.get("type_distribution", Dictionary());
		if (!type_dist.is_empty()) {
			// Sort by count descending, show top 5.
			Vector<Pair<String, int>> type_vec;
			LocalVector<Variant> keys = type_dist.get_key_list();
			for (const Variant &k : keys) {
				type_vec.push_back(Pair<String, int>(k, type_dist[k]));
			}
			type_vec.sort_custom<_SortTypeCountDesc>();

			text += "\nTop types: ";
			int shown = 0;
			for (int i = 0; i < type_vec.size() && shown < 5; i++) {
				if (shown > 0) {
					text += " ";
				}
				text += type_vec[i].first + "(" + itos(type_vec[i].second) + ")";
				shown++;
			}
			text += "\n";
		}

		Dictionary groups = stats.get("groups", Dictionary());
		if (!groups.is_empty()) {
			text += "Groups: ";
			LocalVector<Variant> group_keys = groups.get_key_list();
			bool first = true;
			for (const Variant &k : group_keys) {
				if (!first) {
					text += " ";
				}
				text += String(k) + "(" + itos((int)groups[k]) + ")";
				first = false;
			}
			text += "\n";
		}
	}

	// Pagination footer.
	if (total_children_for_pagination > limit) {
		int end_idx = MIN(offset + limit, total_children_for_pagination) - 1;
		text += "Page: showing children " + itos(offset) + "-" + itos(end_idx) +
				" of " + itos(total_children_for_pagination);
		int remaining = total_children_for_pagination - (offset + limit);
		if (remaining > 0) {
			text += " (" + itos(remaining) + " more)";
		}
		text += "\n";
	}

	// 13. Build structured response.
	Dictionary structured;
	structured["root_path"] = root_path;
	structured["node"] = browse_node;
	if (include_stats) {
		structured["stats"] = stats;
	}
	structured["pagination"] = pagination;

	Dictionary filters_applied;
	filters_applied["type_filter"] = type_filter;
	filters_applied["name_pattern"] = name_pattern;
	structured["filters_applied"] = filters_applied;
	structured["cached"] = used_cache || !refresh;

	return make_tool_result(text, structured);
}

// ============================================================================
// Browse Scene Tree - Helpers
// ============================================================================

Dictionary MCPDebugTools::_find_subtree(const Dictionary &p_tree,
		const String &p_target_path, const String &p_current_path) {
	if (p_tree.is_empty()) {
		return Dictionary();
	}

	String name = p_tree.get("name", "");
	String current_path;
	if (p_current_path.is_empty()) {
		// Root node: path is "/" + name, but for the Godot root it is "/root".
		current_path = "/" + name;
	} else if (p_current_path == "/") {
		current_path = "/" + name;
	} else {
		current_path = p_current_path + "/" + name;
	}

	// Check if this node matches the target path.
	if (current_path == p_target_path) {
		return p_tree;
	}

	// Only recurse if the target path starts with our current path.
	if (!p_target_path.begins_with(current_path + "/") && current_path != "/") {
		return Dictionary();
	}

	// Search children.
	Array children = p_tree.get("children", Array());
	for (int i = 0; i < children.size(); i++) {
		Dictionary child = children[i];
		Dictionary found = _find_subtree(child, p_target_path, current_path);
		if (!found.is_empty()) {
			return found;
		}
	}

	return Dictionary();
}

int MCPDebugTools::_count_descendants(const Dictionary &p_tree) {
	if (p_tree.is_empty()) {
		return 0;
	}

	int count = 0;
	Array children = p_tree.get("children", Array());
	for (int i = 0; i < children.size(); i++) {
		count += 1 + _count_descendants(children[i]);
	}
	return count;
}

Dictionary MCPDebugTools::_build_browse_node(const Dictionary &p_tree_node,
		const String &p_current_path, int p_remaining_depth,
		bool p_include_indicators, int p_child_offset,
		int p_child_limit, bool p_is_root) {
	if (p_tree_node.is_empty()) {
		return Dictionary();
	}

	String name = p_tree_node.get("name", "?");
	String type = p_tree_node.get("type", "?");

	Dictionary node;
	node["name"] = name;
	node["type"] = type;
	node["path"] = p_current_path;

	Array original_children = p_tree_node.get("children", Array());
	int child_count = original_children.size();
	node["child_count"] = child_count;
	node["descendant_count"] = _count_descendants(p_tree_node);

	// Scene file.
	String scene_file = p_tree_node.get("scene_file_path", "");
	if (!scene_file.is_empty()) {
		node["scene_file"] = scene_file;
	}

	// Indicators.
	if (p_include_indicators) {
		// is_visible: derived from view_flags.
		int view_flags = (int)p_tree_node.get("view_flags", 0);
		// Bit 2 = VIEW_VISIBLE (see SceneDebuggerTree::RemoteNode::ViewFlags).
		bool is_visible = (view_flags & 4) != 0;
		// If the node does not have a visible method, consider it visible.
		bool has_visible_method = (view_flags & 2) != 0;
		if (!has_visible_method) {
			is_visible = true;
		}
		node["is_visible"] = is_visible;

		// has_script and group_count: available from extended browse tree.
		if (p_tree_node.has("has_script")) {
			node["has_script"] = (bool)p_tree_node.get("has_script", false);
		}
		if (p_tree_node.has("group_count")) {
			node["group_count"] = (int)p_tree_node.get("group_count", 0);
		}
	}

	// Children: apply depth control.
	if (p_remaining_depth <= 0 && child_count > 0) {
		node["children"] = "_truncated";
	} else if (child_count == 0) {
		node["children"] = Array();
	} else {
		Array browse_children;

		// Pagination: only apply at root level of the browse call.
		int start = p_is_root ? p_child_offset : 0;
		int end = p_is_root ? MIN(start + p_child_limit, child_count) : child_count;

		// Clamp start.
		if (start >= child_count) {
			// Offset beyond available children -- return empty.
			node["children"] = Array();
			return node;
		}

		for (int i = start; i < end; i++) {
			Dictionary child = original_children[i];
			String child_name = child.get("name", "?");
			String child_path = p_current_path + "/" + child_name;

			Dictionary browse_child = _build_browse_node(child, child_path,
					p_remaining_depth - 1, p_include_indicators,
					0, p_child_limit, false);
			browse_children.push_back(browse_child);
		}

		node["children"] = browse_children;
	}

	return node;
}

void MCPDebugTools::_accumulate_types(const Dictionary &p_tree,
		HashMap<String, int> &r_counts) {
	if (p_tree.is_empty()) {
		return;
	}

	String type = p_tree.get("type", "?");
	if (r_counts.has(type)) {
		r_counts[type]++;
	} else {
		r_counts[type] = 1;
	}

	Array children = p_tree.get("children", Array());
	for (int i = 0; i < children.size(); i++) {
		_accumulate_types(children[i], r_counts);
	}
}

Dictionary MCPDebugTools::_build_type_distribution(const Dictionary &p_tree) {
	HashMap<String, int> counts;
	_accumulate_types(p_tree, counts);

	Dictionary result;
	for (const KeyValue<String, int> &kv : counts) {
		result[kv.key] = kv.value;
	}
	return result;
}

void MCPDebugTools::_accumulate_groups(const Dictionary &p_tree,
		HashMap<String, int> &r_counts) {
	if (p_tree.is_empty()) {
		return;
	}

	// group_count is available in extended browse tree data.
	// We do not have group names from the tree data -- only the count.
	// For now, we track the aggregate group_count.
	// Group names would require additional game-side data.

	Array children = p_tree.get("children", Array());
	for (int i = 0; i < children.size(); i++) {
		_accumulate_groups(children[i], r_counts);
	}
}

Dictionary MCPDebugTools::_build_group_stats(const Dictionary &p_tree) {
	// Group names are not available in the current tree data structure.
	// We return an empty dict. Future: extend game-side to send group names.
	return Dictionary();
}

bool MCPDebugTools::_subtree_has_match(const Dictionary &p_tree,
		const String &p_type_filter, const String &p_name_pattern) {
	if (p_tree.is_empty()) {
		return false;
	}

	String name = p_tree.get("name", "?");
	String type = p_tree.get("type", "?");

	bool name_match = p_name_pattern.is_empty() || name.matchn(p_name_pattern);
	bool type_match = p_type_filter.is_empty() || type == p_type_filter;

	if (name_match && type_match) {
		return true;
	}

	Array children = p_tree.get("children", Array());
	for (int i = 0; i < children.size(); i++) {
		if (_subtree_has_match(children[i], p_type_filter, p_name_pattern)) {
			return true;
		}
	}

	return false;
}

Dictionary MCPDebugTools::_filter_tree(const Dictionary &p_tree,
		const String &p_type_filter, const String &p_name_pattern) {
	if (p_tree.is_empty()) {
		return Dictionary();
	}

	String name = p_tree.get("name", "?");
	String type = p_tree.get("type", "?");

	bool name_match = p_name_pattern.is_empty() || name.matchn(p_name_pattern);
	bool type_match = p_type_filter.is_empty() || type == p_type_filter;
	bool self_matches = name_match && type_match;

	// Recursively filter children, keeping those that match or have matching descendants.
	Array original_children = p_tree.get("children", Array());
	Array filtered_children;

	for (int i = 0; i < original_children.size(); i++) {
		Dictionary child = original_children[i];
		if (_subtree_has_match(child, p_type_filter, p_name_pattern)) {
			Dictionary filtered_child = _filter_tree(child, p_type_filter, p_name_pattern);
			if (!filtered_child.is_empty()) {
				filtered_children.push_back(filtered_child);
			}
		}
	}

	// Include this node if it matches, or if it has matching descendants (ancestor context).
	if (self_matches || filtered_children.size() > 0) {
		Dictionary result;
		// Copy all fields from the original node.
		LocalVector<Variant> keys = p_tree.get_key_list();
		for (const Variant &k : keys) {
			if (String(k) != "children") {
				result[k] = p_tree[k];
			}
		}
		result["children"] = filtered_children;
		return result;
	}

	return Dictionary();
}

String MCPDebugTools::_browse_node_to_text(const Dictionary &p_node,
		int p_indent, bool p_include_indicators) {
	if (p_node.is_empty()) {
		return "";
	}

	String indent;
	for (int i = 0; i < p_indent; i++) {
		indent += "  ";
	}

	String name = p_node.get("name", "?");
	String type = p_node.get("type", "?");
	int child_count = (int)p_node.get("child_count", 0);
	int descendant_count = (int)p_node.get("descendant_count", 0);

	String line = indent + name + " (" + type + ") [" + itos(child_count);
	if (child_count == 1) {
		line += " child";
	} else {
		line += " ch";
	}
	if (descendant_count > 0) {
		line += ", " + itos(descendant_count) + " desc";
	}
	line += "]";

	if (p_include_indicators) {
		if (p_node.has("has_script") && (bool)p_node.get("has_script", false)) {
			line += " [script]";
		}
		if (p_node.has("is_visible") && !(bool)p_node.get("is_visible", true)) {
			line += " [hidden]";
		}
		if (p_node.has("group_count") && (int)p_node.get("group_count", 0) > 0) {
			int gc = (int)p_node["group_count"];
			line += " [" + itos(gc) + " group" + (gc != 1 ? "s" : "") + "]";
		}
	}

	String scene_file = p_node.get("scene_file", "");
	if (!scene_file.is_empty()) {
		line += " [" + scene_file + "]";
	}

	line += "\n";

	// Children.
	Variant children_var = p_node.get("children", Variant());
	if (children_var.get_type() == Variant::STRING) {
		// Truncated indicator.
		if (child_count > 0) {
			// Already shown in the "[N ch]" count, no extra text needed.
		}
	} else if (children_var.get_type() == Variant::ARRAY) {
		Array children = children_var;
		for (int i = 0; i < children.size(); i++) {
			line += _browse_node_to_text(children[i], p_indent + 1, p_include_indicators);
		}
	}

	return line;
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
					  "Use runtime/run_project to launch the main scene, "
					  "or runtime/run_scene to launch a specific scene.\n\n"
					  "=== END SUMMARY ===";

		Dictionary structured;
		Dictionary status;
		status["state"] = "stopped";
		// bridge is guaranteed non-null here (null-checked above).
		status["stop_reason"] = bridge->get_last_stop_reason();
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
