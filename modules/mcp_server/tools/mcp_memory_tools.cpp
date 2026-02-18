/**************************************************************************/
/*  mcp_memory_tools.cpp                                                  */
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

#include "mcp_memory_tools.h"

#include "../mcp_debugger_bridge.h"
#include "../mcp_protocol.h"
#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/variant/array.h"

// Static snapshot storage.
HashMap<String, Dictionary> MCPMemoryTools::snapshot_store;

// ============================================================================
// Tool Registration
// ============================================================================

void MCPMemoryTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ---- memory/get_stats ----
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"memory/get_stats",
				"Get Memory Stats",
				"Quick memory and performance health check. Returns object counts (total, nodes, "
				"orphan nodes, resources), memory usage in bytes and MB, render stats (draw calls, "
				"primitives), and physics stats. Use this as the first step when investigating "
				"performance issues. If orphan_nodes > 0, nodes are leaking -- follow up with "
				"memory/get_orphans. Much lighter than runtime/get_session_summary. "
				"Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPMemoryTools::handle_get_stats));
	}

	// ---- memory/get_orphans ----
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"memory/get_orphans",
				"Get Orphan Nodes",
				"Detect and list orphan nodes in the running game. Orphan nodes are the #1 source "
				"of memory leaks in Godot -- nodes removed from the tree with remove_child() but "
				"never freed with queue_free(). Returns orphan count and the raw output from "
				"Node.print_orphan_nodes(). Use after memory/get_stats shows orphan_nodes > 0. "
				"Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPMemoryTools::handle_get_orphans));
	}

	// ---- memory/take_snapshot ----
	{
		Dictionary props;
		props["label"] = make_prop("string",
				"Human-readable label for this snapshot (e.g., 'before_boss_fight'). "
				"Used to reference the snapshot later in memory/diff.");
		Array required;
		required.push_back("label");
		p_registry->register_tool(
				"memory/take_snapshot",
				"Take Memory Snapshot",
				"Capture a memory snapshot of the running game's performance counters and object "
				"counts. Labels the snapshot for later comparison. Use the snapshot-action-snapshot-diff "
				"workflow to find leaks: "
				"1. memory/take_snapshot label=\"before\" "
				"2. (game does something) "
				"3. memory/take_snapshot label=\"after\" "
				"4. memory/diff label_a=\"before\" label_b=\"after\". "
				"Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPMemoryTools::handle_take_snapshot));
	}

	// ---- memory/diff ----
	{
		Dictionary props;
		props["label_a"] = make_prop("string", "Label of the first (earlier) snapshot");
		props["label_b"] = make_prop("string", "Label of the second (later) snapshot");
		Array required;
		required.push_back("label_a");
		required.push_back("label_b");
		p_registry->register_tool(
				"memory/diff",
				"Diff Memory Snapshots",
				"Compare two previously-taken memory snapshots and identify what changed. Returns "
				"deltas for object counts, memory usage, and per-class instance counts. Automatically "
				"detects leak patterns and provides fix suggestions. Warning severities: HIGH (definite "
				"leak), MEDIUM (suspicious growth), LOW (minor concern), OK (healthy). "
				"Both snapshots must have been taken with memory/take_snapshot.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPMemoryTools::handle_diff));
	}

	// ---- memory/track_trend ----
	{
		Dictionary props;
		props["samples"] = make_prop("integer",
				"Number of samples to take (default: 10, max: 60)");
		props["interval_ms"] = make_prop("integer",
				"Milliseconds between samples (default: 2000, min: 500, max: 10000)");
		Array required;
		p_registry->register_tool(
				"memory/track_trend",
				"Track Memory Trend",
				"Poll memory counters repeatedly over a time period and compute growth trends. "
				"Returns all raw samples plus automated trend analysis. Status values: 'stable' "
				"(no growth), 'concerning' (mild growth), 'leaking' (definite unbounded growth). "
				"Use this to confirm a suspected leak is ongoing, not just a one-time allocation. "
				"Longer sampling periods give more confident results. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPMemoryTools::handle_track_trend));
	}

	// ---- memory/detect_leaks ----
	{
		Dictionary props;
		props["duration_seconds"] = make_prop("number",
				"How long to let the game run between snapshots (default: 10, max: 60)");
		Array required;
		p_registry->register_tool(
				"memory/detect_leaks",
				"Detect Memory Leaks",
				"All-in-one leak detection. Takes a snapshot, lets the game run for the specified "
				"duration, takes another snapshot, diffs them, and returns a diagnosis. This is the "
				"'easy button' -- use it when you want a quick answer to 'is there a memory leak?' "
				"For more control, use the manual take_snapshot -> diff workflow. "
				"Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPMemoryTools::handle_detect_leaks));
	}

	// ---- memory/class_breakdown ----
	{
		Dictionary props;
		props["sort_by"] = make_prop("string",
				"Sort order: 'count' (default, most instances first) or 'class' (alphabetical)");
		props["limit"] = make_prop("integer",
				"Max classes to return (default: 30, max: 100)");
		Array required;
		p_registry->register_tool(
				"memory/class_breakdown",
				"Class Breakdown",
				"Get a breakdown of all nodes in the running game grouped by class. Shows which "
				"classes have the most instances. Useful for identifying unexpected object "
				"accumulation -- if 'BulletTrail' has 10,000 instances, something is not cleaning up. "
				"Analyzes the live scene tree. Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPMemoryTools::handle_class_breakdown));
	}
}

// ============================================================================
// Common Helpers
// ============================================================================

MCPDebuggerBridge *MCPMemoryTools::_get_bridge() {
	MCPProtocol *protocol = MCPProtocol::get_singleton();
	if (!protocol) {
		WARN_PRINT("MCP: _get_bridge() failed -- MCPProtocol singleton is null.");
		return nullptr;
	}
	MCPDebuggerBridge *bridge = protocol->get_debugger_bridge();
	if (!bridge) {
		WARN_PRINT("MCP: _get_bridge() failed -- debugger bridge pointer is null on protocol.");
	}
	return bridge;
}

Dictionary MCPMemoryTools::_require_game_running() {
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

Dictionary MCPMemoryTools::_collect_snapshot() {
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Internal error: debugger bridge not available.");
	}

	// 1. Get basic performance counters from the bridge.
	Dictionary perf = bridge->send_get_performance();
	if (!(bool)perf.get("success", false)) {
		return make_tool_error(
				"Failed to collect performance data: " +
				String(perf.get("error", "Unknown error")));
	}

	// 2. Get extended stats via GDScript evaluation.
	// The base performance data gives us: fps, frame_time, physics_frame_time,
	// memory, object_count, node_count, orphan_count.
	// We augment with additional Performance monitors.
	String expr =
			"var r = []\n"
			"r.append(Performance.get_monitor(Performance.MEMORY_STATIC))\n"
			"r.append(Performance.get_monitor(Performance.MEMORY_STATIC_MAX))\n"
			"r.append(Performance.get_monitor(Performance.OBJECT_RESOURCE_COUNT))\n"
			"r.append(Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME))\n"
			"r.append(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME))\n"
			"r.append(Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME))\n"
			"r.append(Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED))\n"
			"r.append(Performance.get_monitor(Performance.PHYSICS_2D_ACTIVE_OBJECTS))\n"
			"r.append(Performance.get_monitor(Performance.PHYSICS_2D_COLLISION_PAIRS))\n"
			"r.append(Performance.get_monitor(Performance.PHYSICS_3D_ACTIVE_OBJECTS))\n"
			"r.append(Performance.get_monitor(Performance.PHYSICS_3D_COLLISION_PAIRS))\n"
			"str(r)";

	Dictionary eval_result = bridge->send_evaluate(expr);

	// 3. Build snapshot dictionary from the basic performance data.
	Dictionary snapshot;
	snapshot["timestamp_msec"] = (int64_t)OS::get_singleton()->get_ticks_msec();
	snapshot["timestamp_iso"] = Time::get_singleton()->get_datetime_string_from_system(true, true);
	snapshot["fps"] = perf.get("fps", 0.0);
	snapshot["frame_time"] = perf.get("frame_time", 0.0);
	snapshot["physics_frame_time"] = perf.get("physics_frame_time", 0.0);
	snapshot["memory_bytes"] = perf.get("memory", 0);
	snapshot["object_count"] = perf.get("object_count", 0);
	snapshot["node_count"] = perf.get("node_count", 0);
	snapshot["orphan_count"] = perf.get("orphan_count", 0);

	// 4. Parse extended stats if evaluation succeeded.
	if ((bool)eval_result.get("success", false)) {
		String value = eval_result.get("value", "");
		// The value is a string like "[1234.0, 5678.0, ...]"
		// Parse the array values.
		value = value.strip_edges();
		if (value.begins_with("[") && value.ends_with("]")) {
			value = value.substr(1, value.length() - 2);
			PackedStringArray parts = value.split(",");
			if (parts.size() >= 11) {
				snapshot["memory_static"] = parts[0].strip_edges().to_float();
				snapshot["memory_static_max"] = parts[1].strip_edges().to_float();
				snapshot["resource_count"] = (int)parts[2].strip_edges().to_float();
				snapshot["render_objects_in_frame"] = (int)parts[3].strip_edges().to_float();
				snapshot["render_draw_calls"] = (int)parts[4].strip_edges().to_float();
				snapshot["render_primitives"] = (int)parts[5].strip_edges().to_float();
				snapshot["video_mem_bytes"] = parts[6].strip_edges().to_float();
				snapshot["physics_2d_active"] = (int)parts[7].strip_edges().to_float();
				snapshot["physics_2d_collision_pairs"] = (int)parts[8].strip_edges().to_float();
				snapshot["physics_3d_active"] = (int)parts[9].strip_edges().to_float();
				snapshot["physics_3d_collision_pairs"] = (int)parts[10].strip_edges().to_float();
			}
		}
	}

	return snapshot;
}

Dictionary MCPMemoryTools::_compute_diff(const Dictionary &p_snapshot_a, const Dictionary &p_snapshot_b) {
	Dictionary diff;

	// Time between snapshots.
	int64_t time_a = (int64_t)p_snapshot_a.get("timestamp_msec", 0);
	int64_t time_b = (int64_t)p_snapshot_b.get("timestamp_msec", 0);
	double elapsed_sec = (double)(time_b - time_a) / 1000.0;
	diff["elapsed_seconds"] = elapsed_sec;

	// Object deltas.
	Dictionary objects;
	int obj_a = (int)p_snapshot_a.get("object_count", 0);
	int obj_b = (int)p_snapshot_b.get("object_count", 0);
	objects["total_before"] = obj_a;
	objects["total_after"] = obj_b;
	objects["total_delta"] = obj_b - obj_a;

	int nodes_a = (int)p_snapshot_a.get("node_count", 0);
	int nodes_b = (int)p_snapshot_b.get("node_count", 0);
	objects["nodes_before"] = nodes_a;
	objects["nodes_after"] = nodes_b;
	objects["nodes_delta"] = nodes_b - nodes_a;

	int orphans_a = (int)p_snapshot_a.get("orphan_count", 0);
	int orphans_b = (int)p_snapshot_b.get("orphan_count", 0);
	objects["orphans_before"] = orphans_a;
	objects["orphans_after"] = orphans_b;
	objects["orphans_delta"] = orphans_b - orphans_a;

	int res_a = (int)p_snapshot_a.get("resource_count", 0);
	int res_b = (int)p_snapshot_b.get("resource_count", 0);
	objects["resources_before"] = res_a;
	objects["resources_after"] = res_b;
	objects["resources_delta"] = res_b - res_a;

	diff["objects"] = objects;

	// Memory delta.
	Dictionary memory;
	double mem_a = (double)p_snapshot_a.get("memory_static", (double)p_snapshot_a.get("memory_bytes", 0));
	double mem_b = (double)p_snapshot_b.get("memory_static", (double)p_snapshot_b.get("memory_bytes", 0));
	memory["before_mb"] = mem_a / (1024.0 * 1024.0);
	memory["after_mb"] = mem_b / (1024.0 * 1024.0);
	memory["delta_mb"] = (mem_b - mem_a) / (1024.0 * 1024.0);
	memory["before_bytes"] = mem_a;
	memory["after_bytes"] = mem_b;
	memory["delta_bytes"] = mem_b - mem_a;
	diff["memory"] = memory;

	// FPS delta.
	Dictionary fps;
	double fps_a = (double)p_snapshot_a.get("fps", 0.0);
	double fps_b = (double)p_snapshot_b.get("fps", 0.0);
	fps["before"] = fps_a;
	fps["after"] = fps_b;
	fps["delta"] = fps_b - fps_a;
	diff["fps"] = fps;

	// Warnings.
	diff["warnings"] = _generate_warnings(diff);

	return diff;
}

Array MCPMemoryTools::_generate_warnings(const Dictionary &p_diff) {
	Array warnings;

	Dictionary objects = p_diff.get("objects", Dictionary());
	Dictionary memory = p_diff.get("memory", Dictionary());

	int orphan_delta = (int)objects.get("orphans_delta", 0);
	int object_delta = (int)objects.get("total_delta", 0);
	int resource_delta = (int)objects.get("resources_delta", 0);
	double memory_delta_mb = (double)memory.get("delta_mb", 0.0);

	// Rule 1: Orphan nodes growing = definite leak.
	if (orphan_delta > 0) {
		Dictionary w;
		w["severity"] = "HIGH";
		w["type"] = "orphan_leak";
		w["message"] = vformat("%d new orphan nodes detected. "
							   "Orphan nodes are nodes removed from the scene tree but never freed.",
				orphan_delta);
		w["fix"] = "Find remove_child() calls without matching queue_free(). "
				   "Use memory/get_orphans to see which classes are leaking.";
		warnings.push_back(w);
	}

	// Rule 2: Object count growing significantly.
	if (object_delta > 100) {
		Dictionary w;
		w["severity"] = "MEDIUM";
		w["type"] = "object_growth";
		w["message"] = vformat("Object count grew by %d (from %d to %d).",
				object_delta,
				(int)objects.get("total_before", 0),
				(int)objects.get("total_after", 0));
		w["fix"] = "Check for unbounded spawning patterns. Use memory/class_breakdown "
				   "to identify which class is accumulating.";
		warnings.push_back(w);
	}

	// Rule 3: Resource count growing significantly.
	if (resource_delta > 50) {
		Dictionary w;
		w["severity"] = "MEDIUM";
		w["type"] = "resource_growth";
		w["message"] = vformat("Resource count grew by %d.", resource_delta);
		w["fix"] = "Resources may be loaded repeatedly without caching. "
				   "Check for preload/load calls inside _process or tight loops.";
		warnings.push_back(w);
	}

	// Rule 4: Memory spike.
	if (memory_delta_mb > 50.0) {
		Dictionary w;
		w["severity"] = "HIGH";
		w["type"] = "memory_spike";
		w["message"] = vformat("Memory grew by %.1f MB.", memory_delta_mb);
		w["fix"] = "Large memory growth indicates heavy allocation. Check for "
				   "large textures, meshes, or data structures being created without release.";
		warnings.push_back(w);
	} else if (memory_delta_mb > 10.0) {
		Dictionary w;
		w["severity"] = "MEDIUM";
		w["type"] = "memory_growth";
		w["message"] = vformat("Memory grew by %.1f MB.", memory_delta_mb);
		w["fix"] = "Moderate memory growth. May be normal if the game loaded new content. "
				   "Take additional snapshots to confirm if growth continues.";
		warnings.push_back(w);
	}

	// If no warnings, add an OK status.
	if (warnings.is_empty()) {
		Dictionary w;
		w["severity"] = "OK";
		w["type"] = "healthy";
		w["message"] = "No concerning memory changes detected.";
		w["fix"] = "";
		warnings.push_back(w);
	}

	return warnings;
}

// ============================================================================
// Tool 1: memory/get_stats
// ============================================================================

Dictionary MCPMemoryTools::handle_get_stats(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	Dictionary snapshot = _collect_snapshot();
	// Check if _collect_snapshot returned an error.
	if (snapshot.has("isError") && (bool)snapshot.get("isError", false)) {
		return snapshot;
	}

	// Build text output.
	double fps = (double)snapshot.get("fps", 0.0);
	double frame_time = (double)snapshot.get("frame_time", 0.0) * 1000.0;
	int object_count = (int)snapshot.get("object_count", 0);
	int node_count = (int)snapshot.get("node_count", 0);
	int orphan_count = (int)snapshot.get("orphan_count", 0);
	int resource_count = (int)snapshot.get("resource_count", 0);
	double memory_static = (double)snapshot.get("memory_static", 0.0);
	double memory_static_max = (double)snapshot.get("memory_static_max", 0.0);
	double video_mem = (double)snapshot.get("video_mem_bytes", 0.0);
	int render_objects = (int)snapshot.get("render_objects_in_frame", 0);
	int draw_calls = (int)snapshot.get("render_draw_calls", 0);
	int primitives = (int)snapshot.get("render_primitives", 0);
	int phys2d_active = (int)snapshot.get("physics_2d_active", 0);
	int phys2d_pairs = (int)snapshot.get("physics_2d_collision_pairs", 0);
	int phys3d_active = (int)snapshot.get("physics_3d_active", 0);
	int phys3d_pairs = (int)snapshot.get("physics_3d_collision_pairs", 0);

	String text = "=== MEMORY STATS ===\n\n";

	text += vformat("FPS: %.1f | Frame Time: %.1f ms\n\n", fps, frame_time);

	text += "Objects:\n";
	text += vformat("  Total: %d\n", object_count);
	text += vformat("  Nodes: %d\n", node_count);
	text += vformat("  Orphan Nodes: %d%s\n", orphan_count,
			orphan_count > 0 ? " [LEAKING]" : "");
	text += vformat("  Resources: %d\n", resource_count);

	text += "\nMemory:\n";
	text += vformat("  Static: %.1f MB\n", memory_static / (1024.0 * 1024.0));
	text += vformat("  Static Max: %.1f MB\n", memory_static_max / (1024.0 * 1024.0));
	text += vformat("  Video: %.1f MB\n", video_mem / (1024.0 * 1024.0));

	text += "\nRender:\n";
	text += vformat("  Objects in Frame: %d\n", render_objects);
	text += vformat("  Draw Calls: %d\n", draw_calls);
	text += vformat("  Primitives: %d\n", primitives);

	text += "\nPhysics:\n";
	text += vformat("  2D Active: %d | Pairs: %d\n", phys2d_active, phys2d_pairs);
	text += vformat("  3D Active: %d | Pairs: %d\n", phys3d_active, phys3d_pairs);

	if (orphan_count > 0) {
		text += vformat("\n** WARNING: %d orphan nodes detected. Use memory/get_orphans for details. **\n", orphan_count);
	}

	text += "\n=== END STATS ===";

	// Build structured output.
	Dictionary structured;
	structured["fps"] = fps;
	structured["frame_time_msec"] = frame_time;

	Dictionary s_memory;
	s_memory["static_bytes"] = memory_static;
	s_memory["static_mb"] = memory_static / (1024.0 * 1024.0);
	s_memory["static_max_mb"] = memory_static_max / (1024.0 * 1024.0);
	s_memory["video_mem_mb"] = video_mem / (1024.0 * 1024.0);
	structured["memory"] = s_memory;

	Dictionary s_objects;
	s_objects["total"] = object_count;
	s_objects["nodes"] = node_count;
	s_objects["orphan_nodes"] = orphan_count;
	s_objects["resources"] = resource_count;
	structured["objects"] = s_objects;

	Dictionary s_render;
	s_render["objects_in_frame"] = render_objects;
	s_render["draw_calls"] = draw_calls;
	s_render["primitives_in_frame"] = primitives;
	structured["render"] = s_render;

	Dictionary s_physics;
	s_physics["active_objects_2d"] = phys2d_active;
	s_physics["collision_pairs_2d"] = phys2d_pairs;
	s_physics["active_objects_3d"] = phys3d_active;
	s_physics["collision_pairs_3d"] = phys3d_pairs;
	structured["physics"] = s_physics;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 2: memory/get_orphans
// ============================================================================

Dictionary MCPMemoryTools::handle_get_orphans(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	MCPDebuggerBridge *bridge = _get_bridge();

	// 1. Save the current output cursor so we can read new output after evaluation.
	uint64_t cursor_before = bridge->get_output_latest_seq();

	// 2. Evaluate Node.print_orphan_nodes() in the game.
	// This prints "STRAY NODE:" lines to the game's stdout.
	Dictionary eval_result = bridge->send_evaluate("Node.print_orphan_nodes()");

	if (!(bool)eval_result.get("success", false)) {
		// Even if the eval "fails" (returns null), the side-effect (printing) may
		// have worked. Continue to check output regardless.
	}

	// 3. Wait a couple of frames for the output to flush through.
	bridge->send_wait_frames(3);

	// 4. Read new output since the cursor.
	Vector<OutputEntry> new_output = bridge->get_output_since(cursor_before, 500);

	// 5. Parse "STRAY NODE:" lines.
	Array orphans;
	HashMap<String, int> class_counts;
	int stray_count = 0;

	for (int i = 0; i < new_output.size(); i++) {
		String line = new_output[i].text.strip_edges();
		if (line.begins_with("STRAY NODE:")) {
			stray_count++;
			// Parse the line. Format varies, but typically:
			// STRAY NODE: NodeName (Type)
			String info = line.substr(11).strip_edges();

			// Try to extract class from parentheses.
			String node_name = info;
			String node_class = "Unknown";

			int paren_start = info.rfind("(");
			int paren_end = info.rfind(")");
			if (paren_start != -1 && paren_end > paren_start) {
				node_class = info.substr(paren_start + 1, paren_end - paren_start - 1).strip_edges();
				node_name = info.substr(0, paren_start).strip_edges();
			}

			// Remove the leading dash if present (common in Godot output).
			if (node_name.begins_with("-")) {
				node_name = node_name.substr(1).strip_edges();
			}

			Dictionary orphan;
			orphan["name"] = node_name;
			orphan["class"] = node_class;
			orphan["raw_line"] = line;
			orphans.push_back(orphan);

			// Count by class.
			if (class_counts.has(node_class)) {
				class_counts[node_class]++;
			} else {
				class_counts[node_class] = 1;
			}
		}
	}

	// 6. Build class summary.
	Array class_summary;
	for (const KeyValue<String, int> &kv : class_counts) {
		Dictionary entry;
		entry["class"] = kv.key;
		entry["count"] = kv.value;
		class_summary.push_back(entry);
	}

	// 7. Build text output.
	String text;
	if (stray_count == 0) {
		// Check the basic orphan count from performance counters.
		Dictionary perf = bridge->send_get_performance();
		int perf_orphans = (int)perf.get("orphan_count", 0);

		if (perf_orphans > 0) {
			text = vformat("Performance counters report %d orphan nodes, but "
						   "Node.print_orphan_nodes() did not produce detailed output.\n\n"
						   "This can happen if orphan nodes were freed between the counter read "
						   "and the print call, or if the orphans are RefCounted objects rather "
						   "than nodes.",
					perf_orphans);
		} else {
			text = "No orphan nodes detected. Memory is clean.";
		}
	} else {
		text = vformat("=== ORPHAN NODES (%d found) ===\n\n", stray_count);

		// Class summary first.
		text += "By class:\n";
		for (const KeyValue<String, int> &kv : class_counts) {
			text += vformat("  %s: %d\n", kv.key, kv.value);
		}

		text += "\nDetails:\n";
		for (int i = 0; i < orphans.size() && i < 50; i++) {
			Dictionary o = orphans[i];
			text += vformat("  %s (%s)\n", String(o["name"]), String(o["class"]));
		}
		if (stray_count > 50) {
			text += vformat("  ... and %d more\n", stray_count - 50);
		}

		text += "\n=== END ORPHANS ===";
	}

	// 8. Build structured output.
	Dictionary structured;
	structured["orphan_count"] = stray_count;
	structured["orphans"] = orphans;
	structured["class_summary"] = class_summary;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 3: memory/take_snapshot
// ============================================================================

Dictionary MCPMemoryTools::handle_take_snapshot(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String label = p_args.get("label", "");
	if (label.is_empty()) {
		return make_tool_error("Missing required parameter: label\n\n"
							   "Provide a descriptive label for this snapshot, e.g., "
							   "'before_boss_fight' or 'after_level_load'.");
	}

	// Validate label: only allow safe characters.
	for (int i = 0; i < label.length(); i++) {
		char32_t c = label[i];
		if (!is_ascii_alphanumeric_char(c) && c != '_' && c != '-' && c != '.') {
			return make_tool_error(
					"Invalid label: contains disallowed character.\n\n"
					"Labels may contain: alphanumeric, underscore, hyphen, period.");
		}
	}

	// Collect the snapshot data.
	Dictionary snapshot = _collect_snapshot();
	if (snapshot.has("isError") && (bool)snapshot.get("isError", false)) {
		return snapshot;
	}

	// Store the snapshot.
	bool overwrite = snapshot_store.has(label);
	snapshot["label"] = label;
	snapshot_store[label] = snapshot;

	// Build text output.
	int object_count = (int)snapshot.get("object_count", 0);
	int node_count = (int)snapshot.get("node_count", 0);
	int orphan_count = (int)snapshot.get("orphan_count", 0);
	int resource_count = (int)snapshot.get("resource_count", 0);
	double memory_static = (double)snapshot.get("memory_static", 0.0);
	double fps = (double)snapshot.get("fps", 0.0);

	String text = vformat("Snapshot '%s' %s.\n\n",
			label, overwrite ? "updated (overwritten)" : "saved");
	text += vformat("Summary:\n");
	text += vformat("  Objects: %d (Nodes: %d, Orphans: %d, Resources: %d)\n",
			object_count, node_count, orphan_count, resource_count);
	text += vformat("  Memory: %.1f MB | FPS: %.1f\n",
			memory_static / (1024.0 * 1024.0), fps);
	text += vformat("\nStored snapshots: %d\n", snapshot_store.size());
	text += "Tip: Take another snapshot after an action, then use memory/diff to compare.";

	// Build structured output.
	Dictionary structured;
	structured["label"] = label;
	structured["overwritten"] = overwrite;
	structured["timestamp_iso"] = snapshot.get("timestamp_iso", "");

	Dictionary summary;
	summary["object_count"] = object_count;
	summary["node_count"] = node_count;
	summary["orphan_count"] = orphan_count;
	summary["resource_count"] = resource_count;
	summary["memory_static_mb"] = memory_static / (1024.0 * 1024.0);
	summary["fps"] = fps;
	structured["summary"] = summary;

	structured["stored_snapshot_count"] = snapshot_store.size();

	Array stored_labels;
	for (const KeyValue<String, Dictionary> &kv : snapshot_store) {
		stored_labels.push_back(kv.key);
	}
	structured["stored_labels"] = stored_labels;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 4: memory/diff
// ============================================================================

Dictionary MCPMemoryTools::handle_diff(const Dictionary &p_args) {
	String label_a = p_args.get("label_a", "");
	String label_b = p_args.get("label_b", "");

	if (label_a.is_empty()) {
		return make_tool_error("Missing required parameter: label_a");
	}
	if (label_b.is_empty()) {
		return make_tool_error("Missing required parameter: label_b");
	}

	if (!snapshot_store.has(label_a)) {
		String available;
		for (const KeyValue<String, Dictionary> &kv : snapshot_store) {
			if (!available.is_empty()) {
				available += ", ";
			}
			available += "'" + kv.key + "'";
		}
		return make_tool_error(
				"Snapshot not found: '" + label_a + "'\n\n" +
				(available.is_empty()
								? "No snapshots stored. Use memory/take_snapshot first."
								: "Available snapshots: " + available));
	}

	if (!snapshot_store.has(label_b)) {
		String available;
		for (const KeyValue<String, Dictionary> &kv : snapshot_store) {
			if (!available.is_empty()) {
				available += ", ";
			}
			available += "'" + kv.key + "'";
		}
		return make_tool_error(
				"Snapshot not found: '" + label_b + "'\n\n" +
				(available.is_empty()
								? "No snapshots stored. Use memory/take_snapshot first."
								: "Available snapshots: " + available));
	}

	Dictionary snap_a = snapshot_store[label_a];
	Dictionary snap_b = snapshot_store[label_b];

	Dictionary diff = _compute_diff(snap_a, snap_b);

	// Build text output.
	Dictionary objects = diff.get("objects", Dictionary());
	Dictionary memory = diff.get("memory", Dictionary());
	Dictionary fps_dict = diff.get("fps", Dictionary());
	Array warnings = diff.get("warnings", Array());
	double elapsed = (double)diff.get("elapsed_seconds", 0.0);

	String text = vformat("=== MEMORY DIFF: '%s' vs '%s' (%.1fs apart) ===\n\n",
			label_a, label_b, elapsed);

	text += "Objects:\n";
	text += vformat("  Total: %d -> %d (%+d)\n",
			(int)objects.get("total_before", 0),
			(int)objects.get("total_after", 0),
			(int)objects.get("total_delta", 0));
	text += vformat("  Nodes: %d -> %d (%+d)\n",
			(int)objects.get("nodes_before", 0),
			(int)objects.get("nodes_after", 0),
			(int)objects.get("nodes_delta", 0));
	text += vformat("  Orphans: %d -> %d (%+d)\n",
			(int)objects.get("orphans_before", 0),
			(int)objects.get("orphans_after", 0),
			(int)objects.get("orphans_delta", 0));
	text += vformat("  Resources: %d -> %d (%+d)\n",
			(int)objects.get("resources_before", 0),
			(int)objects.get("resources_after", 0),
			(int)objects.get("resources_delta", 0));

	text += vformat("\nMemory: %.1f MB -> %.1f MB (%+.1f MB)\n",
			(double)memory.get("before_mb", 0.0),
			(double)memory.get("after_mb", 0.0),
			(double)memory.get("delta_mb", 0.0));

	text += vformat("FPS: %.1f -> %.1f (%+.1f)\n",
			(double)fps_dict.get("before", 0.0),
			(double)fps_dict.get("after", 0.0),
			(double)fps_dict.get("delta", 0.0));

	text += "\nDiagnosis:\n";
	for (int i = 0; i < warnings.size(); i++) {
		Dictionary w = warnings[i];
		String severity = w.get("severity", "");
		String message = w.get("message", "");
		String fix = w.get("fix", "");
		text += vformat("  [%s] %s\n", severity, message);
		if (!fix.is_empty()) {
			text += vformat("    Fix: %s\n", fix);
		}
	}

	text += "\n=== END DIFF ===";

	// Build structured output.
	Dictionary structured;
	structured["label_a"] = label_a;
	structured["label_b"] = label_b;
	structured["elapsed_seconds"] = elapsed;
	structured["objects"] = objects;
	structured["memory"] = memory;
	structured["fps"] = fps_dict;
	structured["warnings"] = warnings;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 5: memory/track_trend
// ============================================================================

Dictionary MCPMemoryTools::handle_track_trend(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	int samples = (int)p_args.get("samples", 10);
	int interval_ms = (int)p_args.get("interval_ms", 2000);

	// Clamp parameters.
	samples = CLAMP(samples, 2, 60);
	interval_ms = CLAMP(interval_ms, 500, 10000);

	MCPDebuggerBridge *bridge = _get_bridge();
	Array sample_array;

	// Collect samples.
	for (int i = 0; i < samples; i++) {
		if (!bridge->is_game_running()) {
			return make_tool_error(
					vformat("Game stopped during sampling (collected %d of %d samples).",
							i, samples));
		}

		Dictionary perf = bridge->send_get_performance();
		if (!(bool)perf.get("success", false)) {
			return make_tool_error(
					vformat("Failed to collect sample %d: %s",
							i + 1, String(perf.get("error", "Unknown"))));
		}

		Dictionary sample;
		sample["index"] = i;
		sample["timestamp_msec"] = (int64_t)OS::get_singleton()->get_ticks_msec();
		sample["fps"] = perf.get("fps", 0.0);
		sample["frame_time"] = perf.get("frame_time", 0.0);
		sample["object_count"] = perf.get("object_count", 0);
		sample["node_count"] = perf.get("node_count", 0);
		sample["orphan_count"] = perf.get("orphan_count", 0);
		sample["memory"] = perf.get("memory", 0);
		sample_array.push_back(sample);

		// Wait between samples (except after the last one).
		if (i < samples - 1) {
			// Use OS delay so we don't depend on the game's frame rate.
			OS::get_singleton()->delay_usec(interval_ms * 1000);
		}
	}

	// Compute trends.
	int64_t first_ts = (int64_t)((Dictionary)sample_array[0]).get("timestamp_msec", 0);
	int64_t last_ts = (int64_t)((Dictionary)sample_array[sample_array.size() - 1]).get("timestamp_msec", 0);
	double elapsed_sec = (double)(last_ts - first_ts) / 1000.0;

	// Object trend.
	int first_objects = (int)((Dictionary)sample_array[0]).get("object_count", 0);
	int last_objects = (int)((Dictionary)sample_array[sample_array.size() - 1]).get("object_count", 0);
	int object_growth = last_objects - first_objects;
	double objects_per_sec = elapsed_sec > 0 ? (double)object_growth / elapsed_sec : 0.0;

	// Orphan trend.
	int first_orphans = (int)((Dictionary)sample_array[0]).get("orphan_count", 0);
	int last_orphans = (int)((Dictionary)sample_array[sample_array.size() - 1]).get("orphan_count", 0);
	int orphan_growth = last_orphans - first_orphans;
	double orphans_per_sec = elapsed_sec > 0 ? (double)orphan_growth / elapsed_sec : 0.0;

	// Memory trend.
	double first_mem = (double)((Dictionary)sample_array[0]).get("memory", 0);
	double last_mem = (double)((Dictionary)sample_array[sample_array.size() - 1]).get("memory", 0);
	double mem_growth_mb = (last_mem - first_mem) / (1024.0 * 1024.0);
	double mem_mb_per_sec = elapsed_sec > 0 ? mem_growth_mb / elapsed_sec : 0.0;

	// FPS trend.
	double first_fps = (double)((Dictionary)sample_array[0]).get("fps", 0.0);
	double last_fps = (double)((Dictionary)sample_array[sample_array.size() - 1]).get("fps", 0.0);
	double fps_change = last_fps - first_fps;

	// Classify status.
	String status = "stable";
	if (objects_per_sec > 10.0 || orphans_per_sec > 1.0 || mem_mb_per_sec > 1.0) {
		status = "leaking";
	} else if (objects_per_sec > 1.0 || orphan_growth > 0 || mem_mb_per_sec > 0.1) {
		status = "concerning";
	}

	// Build concerns array.
	Array concerns;
	if (objects_per_sec > 10.0) {
		concerns.push_back(vformat("Objects growing at %.1f/sec -- unbounded spawning", objects_per_sec));
	}
	if (orphan_growth > 0) {
		concerns.push_back(vformat("%d new orphan nodes -- nodes not being freed", orphan_growth));
	}
	if (mem_mb_per_sec > 1.0) {
		concerns.push_back(vformat("Memory growing at %.2f MB/sec", mem_mb_per_sec));
	}
	if (fps_change < -10.0) {
		concerns.push_back(vformat("FPS dropped by %.1f -- leak may be causing performance degradation", -fps_change));
	}
	if (concerns.is_empty()) {
		concerns.push_back("All counters stable during sampling period.");
	}

	// Build text output.
	String text = vformat("=== MEMORY TREND (%d samples over %.1fs) ===\n\n", samples, elapsed_sec);
	text += vformat("Status: %s\n\n", status.to_upper());

	text += "Objects:\n";
	text += vformat("  Start: %d | End: %d | Growth: %+d (%.1f/sec)\n",
			first_objects, last_objects, object_growth, objects_per_sec);

	text += "Orphans:\n";
	text += vformat("  Start: %d | End: %d | Growth: %+d (%.2f/sec)\n",
			first_orphans, last_orphans, orphan_growth, orphans_per_sec);

	text += "Memory:\n";
	text += vformat("  Start: %.1f MB | End: %.1f MB | Growth: %+.1f MB (%.2f MB/sec)\n",
			first_mem / (1024.0 * 1024.0), last_mem / (1024.0 * 1024.0),
			mem_growth_mb, mem_mb_per_sec);

	text += "FPS:\n";
	text += vformat("  Start: %.1f | End: %.1f | Change: %+.1f\n",
			first_fps, last_fps, fps_change);

	text += "\nConcerns:\n";
	for (int i = 0; i < concerns.size(); i++) {
		text += "  - " + String(concerns[i]) + "\n";
	}

	text += "\n=== END TREND ===";

	// Build structured output.
	Dictionary structured;
	structured["status"] = status;
	structured["elapsed_seconds"] = elapsed_sec;
	structured["sample_count"] = samples;
	structured["samples"] = sample_array;

	Dictionary s_objects;
	s_objects["start"] = first_objects;
	s_objects["end"] = last_objects;
	s_objects["growth"] = object_growth;
	s_objects["per_second"] = objects_per_sec;
	structured["objects"] = s_objects;

	Dictionary s_orphans;
	s_orphans["start"] = first_orphans;
	s_orphans["end"] = last_orphans;
	s_orphans["growth"] = orphan_growth;
	s_orphans["per_second"] = orphans_per_sec;
	structured["orphans"] = s_orphans;

	Dictionary s_memory;
	s_memory["start_mb"] = first_mem / (1024.0 * 1024.0);
	s_memory["end_mb"] = last_mem / (1024.0 * 1024.0);
	s_memory["growth_mb"] = mem_growth_mb;
	s_memory["mb_per_second"] = mem_mb_per_sec;
	structured["memory"] = s_memory;

	Dictionary s_fps;
	s_fps["start"] = first_fps;
	s_fps["end"] = last_fps;
	s_fps["change"] = fps_change;
	structured["fps"] = s_fps;

	structured["concerns"] = concerns;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 6: memory/detect_leaks
// ============================================================================

Dictionary MCPMemoryTools::handle_detect_leaks(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	double duration_seconds = (double)p_args.get("duration_seconds", 10.0);
	duration_seconds = CLAMP(duration_seconds, 1.0, 60.0);

	MCPDebuggerBridge *bridge = _get_bridge();

	// 1. Take snapshot A.
	Dictionary snap_a = _collect_snapshot();
	if (snap_a.has("isError") && (bool)snap_a.get("isError", false)) {
		return snap_a;
	}

	// 2. Wait for the specified duration.
	int wait_msec = (int)(duration_seconds * 1000.0);
	OS::get_singleton()->delay_usec(wait_msec * 1000);

	// 3. Verify game is still running.
	if (!bridge->is_game_running()) {
		return make_tool_error(
				"Game stopped during leak detection wait period.\n\n"
				"Check runtime/get_errors for crash details.");
	}

	// 4. Take snapshot B.
	Dictionary snap_b = _collect_snapshot();
	if (snap_b.has("isError") && (bool)snap_b.get("isError", false)) {
		return snap_b;
	}

	// 5. Compute diff.
	Dictionary diff = _compute_diff(snap_a, snap_b);

	// 6. Build the diagnosis.
	Dictionary objects = diff.get("objects", Dictionary());
	Dictionary memory = diff.get("memory", Dictionary());
	Dictionary fps_diff = diff.get("fps", Dictionary());
	Array warnings = diff.get("warnings", Array());
	double elapsed = (double)diff.get("elapsed_seconds", 0.0);

	// Determine overall verdict.
	String verdict = "CLEAN";
	for (int i = 0; i < warnings.size(); i++) {
		Dictionary w = warnings[i];
		String severity = w.get("severity", "");
		if (severity == "HIGH") {
			verdict = "LEAKING";
			break;
		} else if (severity == "MEDIUM" && verdict != "LEAKING") {
			verdict = "SUSPICIOUS";
		}
	}

	// Build text output.
	String text = vformat("=== LEAK DETECTION (%.1fs observation) ===\n\n", elapsed);
	text += vformat("Verdict: %s\n\n", verdict);

	text += "Changes during observation:\n";
	text += vformat("  Objects: %+d (from %d to %d)\n",
			(int)objects.get("total_delta", 0),
			(int)objects.get("total_before", 0),
			(int)objects.get("total_after", 0));
	text += vformat("  Nodes: %+d\n", (int)objects.get("nodes_delta", 0));
	text += vformat("  Orphans: %+d\n", (int)objects.get("orphans_delta", 0));
	text += vformat("  Resources: %+d\n", (int)objects.get("resources_delta", 0));
	text += vformat("  Memory: %+.1f MB\n", (double)memory.get("delta_mb", 0.0));
	text += vformat("  FPS: %+.1f\n", (double)fps_diff.get("delta", 0.0));

	text += "\nDiagnosis:\n";
	for (int i = 0; i < warnings.size(); i++) {
		Dictionary w = warnings[i];
		text += vformat("  [%s] %s\n", String(w["severity"]), String(w["message"]));
		String fix = w.get("fix", "");
		if (!fix.is_empty()) {
			text += vformat("    Fix: %s\n", fix);
		}
	}

	if (verdict == "CLEAN") {
		text += "\n  No leaks detected during the observation period.";
		text += "\n  Tip: Try a longer duration or perform game actions during the wait.";
	}

	text += "\n\n=== END LEAK DETECTION ===";

	// Build structured output.
	Dictionary structured;
	structured["verdict"] = verdict;
	structured["duration_seconds"] = elapsed;
	structured["snapshot_before"] = snap_a;
	structured["snapshot_after"] = snap_b;
	structured["diff"] = diff;
	structured["warnings"] = warnings;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 7: memory/class_breakdown
// ============================================================================

Dictionary MCPMemoryTools::handle_class_breakdown(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String sort_by = p_args.get("sort_by", "count");
	int limit = (int)p_args.get("limit", 30);
	limit = CLAMP(limit, 1, 100);

	MCPDebuggerBridge *bridge = _get_bridge();

	// Use GDScript evaluation to traverse the scene tree and count nodes by class.
	// This is a recursive traversal done game-side to avoid multiple round trips.
	String expr =
			"var counts = {}\n"
			"var queue = [get_tree().root]\n"
			"var total = 0\n"
			"while queue.size() > 0:\n"
			"\tvar node = queue.pop_front()\n"
			"\tvar cls = node.get_class()\n"
			"\tif counts.has(cls):\n"
			"\t\tcounts[cls] += 1\n"
			"\telse:\n"
			"\t\tcounts[cls] = 1\n"
			"\ttotal += 1\n"
			"\tfor child in node.get_children():\n"
			"\t\tqueue.push_back(child)\n"
			"var sorted = []\n"
			"for cls in counts:\n"
			"\tsorted.append([counts[cls], cls])\n"
			"sorted.sort()\n"
			"sorted.reverse()\n"
			"var result = str(total) + '|'\n"
			"for item in sorted:\n"
			"\tresult += str(item[1]) + ':' + str(item[0]) + ','\n"
			"result";

	Dictionary eval_result = bridge->send_evaluate(expr, 15000);

	if (!(bool)eval_result.get("success", false)) {
		return make_tool_error(
				"Failed to collect class breakdown: " +
				String(eval_result.get("value", eval_result.get("error", "Unknown error"))) +
				"\n\nThe scene tree may be too large to traverse within the timeout.");
	}

	String value = eval_result.get("value", "");

	// Parse the result string: "total|ClassName:count,ClassName:count,..."
	int pipe_pos = value.find("|");
	if (pipe_pos == -1) {
		return make_tool_error(
				"Unexpected response format from class breakdown.\n\n"
				"Raw value: " + value);
	}

	int total_nodes = value.substr(0, pipe_pos).to_int();
	String classes_str = value.substr(pipe_pos + 1);

	// Parse class entries.
	Array class_array;
	PackedStringArray entries = classes_str.split(",");

	int shown = 0;
	for (int i = 0; i < entries.size() && shown < limit; i++) {
		String entry = entries[i].strip_edges();
		if (entry.is_empty()) {
			continue;
		}

		int colon_pos = entry.find(":");
		if (colon_pos == -1) {
			continue;
		}

		String class_name = entry.substr(0, colon_pos);
		int count = entry.substr(colon_pos + 1).to_int();

		Dictionary cls;
		cls["class"] = class_name;
		cls["count"] = count;
		cls["percentage"] = total_nodes > 0 ? (double)count / (double)total_nodes * 100.0 : 0.0;
		class_array.push_back(cls);
		shown++;
	}

	// Sort by class name if requested (default is already by count desc).
	if (sort_by == "class") {
		// Simple bubble sort for the small result set.
		for (int i = 0; i < class_array.size(); i++) {
			for (int j = i + 1; j < class_array.size(); j++) {
				Dictionary a = class_array[i];
				Dictionary b = class_array[j];
				if (String(a["class"]) > String(b["class"])) {
					class_array[i] = b;
					class_array[j] = a;
				}
			}
		}
	}

	// Build text output.
	String text = vformat("=== CLASS BREAKDOWN (%d total nodes) ===\n\n", total_nodes);

	for (int i = 0; i < class_array.size(); i++) {
		Dictionary cls = class_array[i];
		text += vformat("  %-30s %5d  (%4.1f%%)\n",
				String(cls["class"]),
				(int)cls["count"],
				(double)cls["percentage"]);
	}

	if (class_array.size() < (int)entries.size() - 1) {
		text += vformat("\n  ... and %d more classes (increase 'limit' to see more)\n",
				(int)entries.size() - 1 - class_array.size());
	}

	text += "\n=== END BREAKDOWN ===";

	// Build structured output.
	Dictionary structured;
	structured["total_nodes"] = total_nodes;
	structured["classes"] = class_array;
	structured["class_count"] = class_array.size();
	structured["sort_by"] = sort_by;
	structured["limit"] = limit;

	return make_tool_result(text, structured);
}
