/**************************************************************************/
/*  mcp_breakpoint_tools.cpp                                             */
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

#include "mcp_breakpoint_tools.h"

#include "../mcp_debugger_bridge.h"
#include "../mcp_protocol.h"
#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/input/shortcut.h"
#include "core/object/script_language.h"
#include "editor/debugger/editor_debugger_node.h"

// ============================================================================
// Common Helpers
// ============================================================================

MCPDebuggerBridge *MCPBreakpointTools::_get_bridge() {
	MCPProtocol *protocol = MCPProtocol::get_singleton();
	if (!protocol) {
		return nullptr;
	}
	return protocol->get_debugger_bridge();
}

Dictionary MCPBreakpointTools::_require_game_running() {
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

// ============================================================================
// Tool Registration
// ============================================================================

void MCPBreakpointTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ---- debug/set_breakpoint ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Script resource path (must start with 'res://'). "
				"Example: 'res://scripts/player.gd'");
		props["line"] = make_prop("integer",
				"Line number to set the breakpoint on (1-based)");
		props["enabled"] = make_prop("boolean",
				"Whether the breakpoint is enabled (default: true). "
				"Pass false to remove/disable a breakpoint.");
		Array required;
		required.push_back("path");
		required.push_back("line");
		p_registry->register_tool(
				"debug/set_breakpoint", "Set Breakpoint",
				"Set or remove a breakpoint at a specific line in a GDScript file. "
				"The breakpoint persists in the editor session. Use enabled=false to "
				"remove a breakpoint. Does not require the game to be running.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPBreakpointTools::handle_set_breakpoint));
	}

	// ---- debug/get_breakpoints ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Optional: filter breakpoints to this script path only. "
				"If omitted, returns all breakpoints.");
		Array required;
		p_registry->register_tool(
				"debug/get_breakpoints", "Get Breakpoints",
				"List all breakpoints currently set in the editor. Optionally filter "
				"by script path. Returns path, line number, and enabled state for each "
				"breakpoint.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPBreakpointTools::handle_get_breakpoints));
	}

	// ---- debug/get_break_state ----
	{
		Dictionary props;
		props["frame"] = make_prop("integer",
				"Stack frame index to inspect variables for (default: 0 = top frame). "
				"Pass -1 to skip variable retrieval (faster, returns only stack trace).");
		props["max_variables"] = make_prop("integer",
				"Maximum number of variables to retrieve per frame (default: 50)");
		Array required;
		p_registry->register_tool(
				"debug/get_break_state", "Get Break State",
				"Get the current debugger break state when the game is paused at a "
				"breakpoint or after a step. Returns the pause reason, full stack trace, "
				"and local/member variables for the selected frame. If the game is not "
				"paused, returns the current running/stopped state.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPBreakpointTools::handle_get_break_state));
	}

	// ---- debug/step ----
	{
		Dictionary props;
		props["action"] = make_prop("string",
				"Step action to perform: 'into' (step into function call), "
				"'over' (step over to next line), 'out' (step out of current function), "
				"'continue' (resume execution), 'break' (pause the running game).");
		Array required;
		required.push_back("action");
		p_registry->register_tool(
				"debug/step", "Step Debugger",
				"Control debugger execution: step into, step over, step out, continue, "
				"or break. For 'into' and 'over', waits for the game to re-pause and "
				"returns the new location. The game must be paused for into/over/out/continue, "
				"and running (not paused) for break.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPBreakpointTools::handle_step));
	}
}

// ============================================================================
// handle_set_breakpoint
// ============================================================================

Dictionary MCPBreakpointTools::handle_set_breakpoint(const Dictionary &p_args) {
	String path = p_args.get("path", "");
	int line = p_args.get("line", 0);
	bool enabled = p_args.get("enabled", true);

	// Validate path.
	if (path.is_empty() || !path.begins_with("res://")) {
		return make_tool_error("'path' must start with 'res://'. Example: 'res://scripts/player.gd'");
	}

	// Validate line.
	if (line < 1) {
		return make_tool_error("'line' must be >= 1.");
	}

	// Dispatch to main thread.
	callable_mp(EditorDebuggerNode::get_singleton(), &EditorDebuggerNode::set_breakpoint)
			.call_deferred(path, line, enabled);

	String action_text = enabled ? "set" : "removed";
	String text = vformat("Breakpoint %s: %s:%d", action_text, path, line);

	Dictionary structured;
	structured["path"] = path;
	structured["line"] = line;
	structured["enabled"] = enabled;

	return make_tool_result(text, structured);
}

// ============================================================================
// handle_get_breakpoints
// ============================================================================

Dictionary MCPBreakpointTools::handle_get_breakpoints(const Dictionary &p_args) {
	String filter_path = p_args.get("path", "");

	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Debugger bridge not available");
	}

	// This method uses PendingRequest pattern internally.
	Dictionary result = bridge->get_all_breakpoints(5000);
	if (result.has("error")) {
		return make_tool_error(result["error"]);
	}

	Array breakpoints = result.get("breakpoints", Array());

	// Apply path filter.
	if (!filter_path.is_empty()) {
		Array filtered;
		for (int i = 0; i < breakpoints.size(); i++) {
			Dictionary bpd = breakpoints[i];
			if (String(bpd["path"]) == filter_path) {
				filtered.push_back(bpd);
			}
		}
		breakpoints = filtered;
	}

	// Build text.
	String text;
	if (breakpoints.is_empty()) {
		text = "No breakpoints set.";
	} else {
		text = vformat("Breakpoints (%d total):", breakpoints.size());
		for (int i = 0; i < breakpoints.size(); i++) {
			Dictionary bpd = breakpoints[i];
			text += vformat("\n  %s:%d", String(bpd["path"]), (int)bpd["line"]);
		}
	}

	Dictionary structured;
	structured["breakpoints"] = breakpoints;
	structured["count"] = breakpoints.size();
	structured["filter"] = filter_path.is_empty() ? Variant() : Variant(filter_path);

	return make_tool_result(text, structured);
}

// ============================================================================
// handle_get_break_state
// ============================================================================

Dictionary MCPBreakpointTools::handle_get_break_state(const Dictionary &p_args) {
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Debugger bridge not available");
	}

	bool paused = bridge->is_game_paused();

	if (!paused) {
		// Game is not paused -- report running/stopped state.
		bool running = bridge->is_game_running();
		String state = running ? "running" : "stopped";
		String text = running ? "Game is running (not paused at breakpoint)."
							  : "Game is not running.";

		Dictionary structured;
		structured["paused"] = false;
		structured["state"] = state;

		return make_tool_result(text, structured);
	}

	// Game is paused -- get break state.
	int frame = p_args.get("frame", 0);

	Dictionary break_state;
	if (frame >= 0) {
		// Request variables for the specified frame.
		break_state = bridge->request_frame_variables(frame, 10000);
	} else {
		// Skip variable retrieval, just get the snapshot.
		break_state = bridge->get_break_state_snapshot();
	}

	// Build text from the break state.
	String text;
	String reason = break_state.get("reason", "Unknown");
	Array stack = break_state.get("stack", Array());

	text += vformat("Game paused: %s", reason);

	// Show top frame location.
	if (stack.size() > 0) {
		Dictionary top = stack[0];
		text += vformat("\n  at %s:%d in function %s",
				String(top.get("file", "")),
				(int)top.get("line", 0),
				String(top.get("function", "")));
	}

	// Stack trace.
	if (stack.size() > 0) {
		text += "\n\nStack trace:";
		for (int i = 0; i < stack.size(); i++) {
			Dictionary sf = stack[i];
			text += vformat("\n  #%d  %s:%d  %s()",
					i,
					String(sf.get("file", "")),
					(int)sf.get("line", 0),
					String(sf.get("function", "")));
		}
	}

	// Variables (if frame >= 0 and variables were fetched).
	// The break_state snapshot returns "locals", "members", "globals" arrays.
	if (frame >= 0 && break_state.has("locals")) {
		Array locals = break_state.get("locals", Array());
		Array members = break_state.get("members", Array());
		Array globals = break_state.get("globals", Array());

		if (locals.size() > 0) {
			text += vformat("\n\nFrame #%d locals (%d variables):", frame, locals.size());
			for (int i = 0; i < locals.size(); i++) {
				Dictionary var = locals[i];
				text += vformat("\n  %s: %s  [%s]",
						String(var.get("name", "")),
						String(var.get("value", "")),
						String(var.get("type", "")));
			}
		}

		if (members.size() > 0) {
			text += vformat("\n\nFrame #%d members (%d variables):", frame, members.size());
			for (int i = 0; i < members.size(); i++) {
				Dictionary var = members[i];
				text += vformat("\n  %s: %s  [%s]",
						String(var.get("name", "")),
						String(var.get("value", "")),
						String(var.get("type", "")));
			}
		}

		if (globals.size() > 0) {
			text += vformat("\n\nFrame #%d globals (%d variables):", frame, globals.size());
			for (int i = 0; i < globals.size(); i++) {
				Dictionary var = globals[i];
				text += vformat("\n  %s: %s  [%s]",
						String(var.get("name", "")),
						String(var.get("value", "")),
						String(var.get("type", "")));
			}
		}
	}

	return make_tool_result(text, break_state);
}

// ============================================================================
// handle_step
// ============================================================================

Dictionary MCPBreakpointTools::handle_step(const Dictionary &p_args) {
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Debugger bridge not available");
	}

	String action = p_args.get("action", "");

	if (action == "break") {
		// Break: requires game running but not paused.
		Dictionary err = _require_game_running();
		if (!err.is_empty()) {
			return err;
		}
		if (bridge->is_game_paused()) {
			return make_tool_error("Game is already paused. Use 'continue', 'into', or 'over' instead.");
		}

		callable_mp(EditorDebuggerNode::get_singleton(), &EditorDebuggerNode::debug_break)
				.call_deferred();

		Dictionary structured;
		structured["action"] = "break";
		structured["result"] = "break_requested";

		return make_tool_result("Break requested. Game will pause at the next opportunity.", structured);
	}

	if (action == "continue") {
		// Continue: requires game paused.
		if (!bridge->is_game_paused()) {
			return make_tool_error("Game is not paused. Use 'break' to pause the game first.");
		}

		callable_mp(EditorDebuggerNode::get_singleton(), &EditorDebuggerNode::debug_continue)
				.call_deferred();

		Dictionary structured;
		structured["action"] = "continue";
		structured["result"] = "resumed";

		return make_tool_result("Resumed execution.", structured);
	}

	if (action == "over") {
		// Step over: requires game paused.
		if (!bridge->is_game_paused()) {
			return make_tool_error("Game is not paused. Use 'break' to pause the game first.");
		}

		callable_mp(EditorDebuggerNode::get_singleton(), &EditorDebuggerNode::debug_next)
				.call_deferred();

		// Wait for the game to re-pause at the next line.
		bridge->wait_for_rebreak(2000);

		// Get the new break state.
		Dictionary new_state = bridge->get_break_state_snapshot();
		bool re_paused = (bool)new_state.get("paused", false);

		String text;
		if (re_paused) {
			Array stack = new_state.get("stack", Array());
			if (stack.size() > 0) {
				Dictionary top = stack[0];
				text = vformat("Stepped over to %s:%d in %s()",
						String(top.get("file", "")),
						(int)top.get("line", 0),
						String(top.get("function", "")));
			} else {
				text = "Stepped over (new location unknown).";
			}
		} else {
			text = "Stepped over. Game did not re-pause (may have resumed or hit no more breakpoints).";
		}

		Dictionary structured;
		structured["action"] = "over";
		structured["re_paused"] = re_paused;
		if (re_paused) {
			structured["break_state"] = new_state;
		}

		return make_tool_result(text, structured);
	}

	if (action == "into") {
		// Step into: requires game paused.
		if (!bridge->is_game_paused()) {
			return make_tool_error("Game is not paused. Use 'break' to pause the game first.");
		}

		callable_mp(EditorDebuggerNode::get_singleton(), &EditorDebuggerNode::debug_step)
				.call_deferred();

		// Wait for the game to re-pause.
		bridge->wait_for_rebreak(2000);

		// Get the new break state.
		Dictionary new_state = bridge->get_break_state_snapshot();
		bool re_paused = (bool)new_state.get("paused", false);

		String text;
		if (re_paused) {
			Array stack = new_state.get("stack", Array());
			if (stack.size() > 0) {
				Dictionary top = stack[0];
				text = vformat("Stepped into %s:%d in %s()",
						String(top.get("file", "")),
						(int)top.get("line", 0),
						String(top.get("function", "")));
			} else {
				text = "Stepped into (new location unknown).";
			}
		} else {
			text = "Stepped into. Game did not re-pause (may have resumed or hit no more breakpoints).";
		}

		Dictionary structured;
		structured["action"] = "into";
		structured["re_paused"] = re_paused;
		if (re_paused) {
			structured["break_state"] = new_state;
		}

		return make_tool_result(text, structured);
	}

	if (action == "out") {
		return make_tool_error(
				"'out' (step out) is not yet supported by Godot's debugger. "
				"Use 'continue' or 'over' instead.");
	}

	return make_tool_error(
			vformat("Unknown step action '%s'. Valid actions: 'into', 'over', 'out', 'continue', 'break'.", action));
}
