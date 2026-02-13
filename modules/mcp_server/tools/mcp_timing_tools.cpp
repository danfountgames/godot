/**************************************************************************/
/*  mcp_timing_tools.cpp                                                  */
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

#include "mcp_timing_tools.h"

#include "core/object/script_language.h"

#include "../mcp_debugger_bridge.h"
#include "../mcp_protocol.h"
#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

// ============================================================================
// Tool Registration
// ============================================================================

void MCPTimingTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ── runtime/time/set_scale ──────────────────────────────────────────

	{
		Dictionary props;
		props["scale"] = make_prop("number",
				"Time scale multiplier. 1.0 = normal speed, 0.5 = half speed, "
				"2.0 = double speed, 0.0 = frozen. Valid range: 0.0 to 16.0. "
				"Common values: 0.0625 (1/16x), 0.125 (1/8x), 0.25 (1/4x), 0.5 (1/2x), "
				"1.0 (normal), 2.0, 4.0, 8.0, 16.0.");
		Array required;
		required.push_back("scale");
		p_registry->register_tool(
				"runtime/time/set_scale", "Set Time Scale",
				"Set the game's time scale multiplier. This controls Engine.time_scale, which "
				"affects both process (delta) and physics (physics_delta) timing. At 0.5x, the "
				"game runs at half speed; at 2.0x, double speed. Setting to 0.0 effectively "
				"freezes time but does NOT suspend the game loop (frames still render). "
				"Use runtime/time/suspend for a true pause that halts everything. "
				"The time scale persists until changed or the game stops. "
				"Game must be running. This does NOT affect editor time.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPTimingTools::handle_set_time_scale));
	}

	// ── runtime/time/get_scale ──────────────────────────────────────────

	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"runtime/time/get_scale", "Get Time Scale",
				"Get the current game time scale and suspend state. Returns the active "
				"time scale multiplier and whether the game is suspended (paused). "
				"Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPTimingTools::handle_get_time_scale));
	}

	// ── runtime/time/suspend ────────────────────────────────────────────

	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"runtime/time/suspend", "Suspend Game",
				"Suspend (pause) the running game. This is a TRUE pause: the scene tree "
				"stops processing, physics stops, and time freezes. The game window stays "
				"open but nothing advances. Use runtime/time/resume to continue, or "
				"runtime/time/next_frame / runtime/time/advance_frames to step through "
				"frames one at a time while suspended. "
				"Idempotent: calling when already suspended is a no-op. "
				"Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPTimingTools::handle_suspend));
	}

	// ── runtime/time/resume ─────────────────────────────────────────────

	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"runtime/time/resume", "Resume Game",
				"Resume a suspended (paused) game. Processing, physics, and time continue "
				"from where they left off. Idempotent: calling when already running is a no-op. "
				"Game must be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPTimingTools::handle_resume));
	}

	// ── runtime/time/next_frame ─────────────────────────────────────────

	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"runtime/time/next_frame", "Advance One Frame",
				"Advance the game by exactly one frame, then re-suspend. The game must be "
				"suspended first (use runtime/time/suspend). This is the classic 'frame step' "
				"debugger operation: the game processes one full frame (physics + render) and "
				"immediately pauses again. Useful for inspecting per-frame behavior. "
				"Game must be running and suspended.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPTimingTools::handle_next_frame));
	}

	// ── runtime/time/advance_frames ─────────────────────────────────────

	{
		Dictionary props;
		props["count"] = make_prop("integer",
				"Number of frames to advance. Range: 1 to 10000. Default: 1.");
		props["instant"] = make_prop("boolean",
				"If true, all frames execute instantly (no real-time delay between them) "
				"with simulated delta time. The game jumps forward by 'count' frames worth "
				"of time in a single burst. If false (default), frames play out naturally "
				"at normal speed (or the current time scale), one per render tick. "
				"Instant mode is useful for fast-forwarding to a specific state. "
				"Natural mode is useful for observing behavior frame by frame.");
		Array required;
		p_registry->register_tool(
				"runtime/time/advance_frames", "Advance Multiple Frames",
				"Advance the game by a specific number of frames, then re-suspend. "
				"Two modes: 'natural' (default) plays frames at normal speed, and 'instant' "
				"runs all frames in a single burst with simulated delta time. "
				"The game must be suspended first. After all frames complete, the game "
				"re-suspends automatically. "
				"NOTE: This tool requires the debug-time-control feature branch to be merged. "
				"On the base branch, only runtime/time/next_frame is available for frame stepping.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPTimingTools::handle_advance_frames));
	}

	// ── runtime/time/set_debug_pause ────────────────────────────────────

	{
		Dictionary props;
		props["enabled"] = make_prop("boolean",
				"Enable or disable the debug_pause() system. When enabled, any "
				"SceneTree.debug_pause() call in game code will suspend the game. "
				"When disabled, debug_pause() calls are silently ignored (no-op).");
		Array required;
		required.push_back("enabled");
		p_registry->register_tool(
				"runtime/time/set_debug_pause", "Enable/Disable Debug Pause",
				"Control the debug_pause() breakpoint system. When enabled, game code can "
				"call SceneTree.debug_pause(tag), SceneTree.debug_pause_if(condition, tag), "
				"or SceneTree.debug_pause_after(hit_count, tag) to programmatically pause "
				"the game at specific points — like code breakpoints but for game logic. "
				"When disabled, all debug_pause() calls become no-ops with zero overhead. "
				"The enabled state persists across frames but resets when the game stops. "
				"The editor also remembers the setting per-project between sessions. "
				"Game must be running. "
				"NOTE: Requires the debug-time-control feature branch.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPTimingTools::handle_set_debug_pause));
	}

	// ── runtime/time/set_debug_pause_tag ────────────────────────────────

	{
		Dictionary props;
		props["tag"] = make_prop("string",
				"The debug_pause tag to enable or disable. Tags are arbitrary strings "
				"passed to SceneTree.debug_pause(tag). Examples: 'player_died', 'boss_spawned', "
				"'checkpoint'. An empty string matches the default (untagged) pause calls.");
		props["enabled"] = make_prop("boolean",
				"Enable (true) or disable (false) pausing for this tag. "
				"When a tag is disabled, debug_pause() calls with that tag are ignored.");
		Array required;
		required.push_back("tag");
		required.push_back("enabled");
		p_registry->register_tool(
				"runtime/time/set_debug_pause_tag", "Enable/Disable Debug Pause Tag",
				"Control individual debug_pause tags. This allows fine-grained filtering: "
				"enable only the tags you care about while ignoring others. "
				"The global debug_pause system must also be enabled (use runtime/time/set_debug_pause). "
				"Game must be running. "
				"NOTE: Requires the debug-time-control feature branch.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPTimingTools::handle_set_debug_pause_tag));
	}

	// ── runtime/time/clear_debug_pause_hits ─────────────────────────────

	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"runtime/time/clear_debug_pause_hits", "Clear Debug Pause Hit Counts",
				"Reset all debug_pause hit counters to zero. This is relevant when using "
				"SceneTree.debug_pause_after(hit_count, tag), which only triggers after "
				"a certain number of calls. Clearing hit counts causes those pause points "
				"to start counting from zero again. "
				"Game must be running. "
				"NOTE: Requires the debug-time-control feature branch.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPTimingTools::handle_clear_debug_pause_hits));
	}
}

// ============================================================================
// Common Helpers
// ============================================================================

MCPDebuggerBridge *MCPTimingTools::_get_bridge() {
	MCPProtocol *protocol = MCPProtocol::get_singleton();
	if (!protocol) {
		return nullptr;
	}
	return protocol->get_debugger_bridge();
}

Dictionary MCPTimingTools::_require_game_running() {
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Internal error: debugger bridge not available.");
	}
	if (!bridge->is_game_running()) {
		return make_tool_error(
				"No game is currently running.\n\n"
				"Start a game first with runtime/run_project or runtime/run_scene.\n"
				"Time control tools only work on a running debug session.");
	}
	return Dictionary();
}

// ============================================================================
// Time Scale
// ============================================================================

Dictionary MCPTimingTools::handle_set_time_scale(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	if (!p_args.has("scale")) {
		return make_tool_error("Missing required parameter: scale");
	}

	double scale = (double)p_args["scale"];

	// Validate range.
	if (scale < 0.0 || scale > 16.0) {
		return make_tool_error(
				"Time scale must be between 0.0 and 16.0.\n"
				"Got: " + String::num(scale, 4) + "\n\n"
				"Common values: 0.25 (quarter), 0.5 (half), 1.0 (normal), 2.0 (double), "
				"4.0, 8.0, 16.0 (max).");
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	bridge->send_set_time_scale(scale);

	String speed_label;
	if (Math::is_equal_approx(scale, 1.0)) {
		speed_label = "normal";
	} else if (scale < 1.0) {
		speed_label = "slow-motion";
	} else {
		speed_label = "fast-forward";
	}

	String text = "Time scale set to " + String::num(scale, 4) + "x (" + speed_label + ").";
	if (Math::is_zero_approx(scale)) {
		text += "\nNote: Time is frozen but the game loop still runs. "
				"Use runtime/time/suspend for a true pause.";
	}

	Dictionary structured;
	structured["scale"] = scale;
	structured["mode"] = speed_label;

	return make_tool_result(text, structured);
}

Dictionary MCPTimingTools::handle_get_time_scale(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	MCPDebuggerBridge *bridge = _get_bridge();

	// We can determine suspend state from the bridge.
	bool is_paused = bridge->is_game_paused();
	double uptime = bridge->get_game_uptime_seconds();
	int64_t frames = bridge->get_game_frame_count();

	// NOTE: The actual Engine.time_scale value is on the game side, and
	// we don't have a direct query for it over the debug protocol. The bridge
	// tracks paused state but not the time scale value. We report what we know.
	String text = "Game Time State:\n";
	text += "  Suspended: " + String(is_paused ? "yes" : "no") + "\n";
	text += "  Uptime: " + String::num(uptime, 1) + "s\n";
	text += "  Frame count: " + itos(frames) + "\n";
	text += "\nNote: The exact Engine.time_scale value is maintained on the game side. "
			"Use runtime/evaluate with 'Engine.time_scale' to read the current value.";

	Dictionary structured;
	structured["suspended"] = is_paused;
	structured["uptime_seconds"] = uptime;
	structured["frame_count"] = frames;

	return make_tool_result(text, structured);
}

// ============================================================================
// Suspend / Resume
// ============================================================================

Dictionary MCPTimingTools::handle_suspend(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	bridge->send_suspend(true);

	String text = "Game suspended. The scene tree is now paused.\n\n"
				  "To resume: runtime/time/resume\n"
				  "To step one frame: runtime/time/next_frame\n"
				  "To advance N frames: runtime/time/advance_frames";

	Dictionary structured;
	structured["suspended"] = true;

	return make_tool_result(text, structured);
}

Dictionary MCPTimingTools::handle_resume(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	bridge->send_suspend(false);

	String text = "Game resumed. Processing continues normally.";

	Dictionary structured;
	structured["suspended"] = false;

	return make_tool_result(text, structured);
}

// ============================================================================
// Frame Advance
// ============================================================================

Dictionary MCPTimingTools::handle_next_frame(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	bridge->send_next_frame();

	String text = "Advanced one frame. Game is suspended again.\n\n"
				  "Use runtime/time/next_frame again to step another frame, "
				  "or runtime/time/resume to continue normally.";

	Dictionary structured;
	structured["frames_advanced"] = 1;

	return make_tool_result(text, structured);
}

Dictionary MCPTimingTools::handle_advance_frames(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	int count = (int)p_args.get("count", 1);
	bool instant = (bool)p_args.get("instant", false);

	// Validate count.
	if (count < 1 || count > 10000) {
		return make_tool_error(
				"Frame count must be between 1 and 10000.\n"
				"Got: " + itos(count));
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	bridge->send_advance_frames(count, instant);

	String mode = instant ? "instant" : "natural";
	String text = "Advancing " + itos(count) + " frame" + (count > 1 ? "s" : "") +
			" in " + mode + " mode.\n";

	if (instant) {
		text += "All frames execute in a single burst with simulated delta time.\n"
				"The game will re-suspend after all frames complete.";
	} else {
		text += "Frames will play out naturally at the current time scale.\n"
				"The game will re-suspend after all frames complete.";
	}

	Dictionary structured;
	structured["count"] = count;
	structured["instant"] = instant;
	structured["mode"] = mode;

	return make_tool_result(text, structured);
}

// ============================================================================
// Debug Pause
// ============================================================================

Dictionary MCPTimingTools::handle_set_debug_pause(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	if (!p_args.has("enabled")) {
		return make_tool_error("Missing required parameter: enabled");
	}

	bool enabled = (bool)p_args["enabled"];

	MCPDebuggerBridge *bridge = _get_bridge();
	bridge->send_set_debug_pause_enabled(enabled);

	String text;
	if (enabled) {
		text = "Debug pause system ENABLED.\n\n"
			   "Game code can now trigger pauses via:\n"
			   "  SceneTree.debug_pause(tag)         — pause unconditionally\n"
			   "  SceneTree.debug_pause_if(cond, tag) — pause if condition is true\n"
			   "  SceneTree.debug_pause_after(N, tag) — pause after N calls\n\n"
			   "Use runtime/time/set_debug_pause_tag to filter by tag.\n"
			   "Use runtime/time/resume to continue after a debug pause triggers.";
	} else {
		text = "Debug pause system DISABLED.\n\n"
			   "All SceneTree.debug_pause() calls are now silently ignored (no-op). "
			   "This has zero runtime overhead in the game.";
	}

	Dictionary structured;
	structured["enabled"] = enabled;

	return make_tool_result(text, structured);
}

Dictionary MCPTimingTools::handle_set_debug_pause_tag(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	String tag = p_args.get("tag", "");
	if (!p_args.has("enabled")) {
		return make_tool_error("Missing required parameter: enabled");
	}
	bool enabled = (bool)p_args["enabled"];

	MCPDebuggerBridge *bridge = _get_bridge();
	bridge->send_set_debug_pause_tag_enabled(tag, enabled);

	String tag_display = tag.is_empty() ? "(default/untagged)" : "'" + tag + "'";
	String state = enabled ? "ENABLED" : "DISABLED";

	String text = "Debug pause tag " + tag_display + " " + state + ".\n";
	if (enabled) {
		text += "Calls to debug_pause() with this tag will now trigger a pause.";
	} else {
		text += "Calls to debug_pause() with this tag will be ignored.";
	}

	Dictionary structured;
	structured["tag"] = tag;
	structured["enabled"] = enabled;

	return make_tool_result(text, structured);
}

Dictionary MCPTimingTools::handle_clear_debug_pause_hits(const Dictionary &p_args) {
	Dictionary guard = _require_game_running();
	if (!guard.is_empty()) {
		return guard;
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	bridge->send_clear_debug_pause_hits();

	String text = "Debug pause hit counters cleared.\n\n"
				  "All SceneTree.debug_pause_after(N, tag) calls will start counting "
				  "from zero again.";

	Dictionary structured;
	structured["cleared"] = true;

	return make_tool_result(text, structured);
}
