/**************************************************************************/
/*  mcp_debugger_bridge.cpp                                               */
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

#include "mcp_debugger_bridge.h"

#include "mcp_protocol.h"

#include "core/os/os.h"
#include "core/os/time.h"
#include "core/string/print_string.h"
#include "core/debugger/debugger_marshalls.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/script/script_editor_plugin.h"

// ========================================================================
// OutputRingBuffer Implementation
// ========================================================================

OutputRingBuffer::OutputRingBuffer() {
	entries.resize(CAPACITY);
}

void OutputRingBuffer::push(const String &p_text, int p_type) {
	MutexLock lock(mutex);

	OutputEntry &entry = entries.write[write_pos];
	entry.seq = next_seq++;
	entry.text = p_text;
	entry.type = p_type;
	entry.timestamp_msec = Time::get_singleton()->get_ticks_msec();

	write_pos = (write_pos + 1) % CAPACITY;
	if (count < CAPACITY) {
		count++;
	}
}

Vector<OutputEntry> OutputRingBuffer::read_since(uint64_t p_cursor, int p_limit) const {
	MutexLock lock(mutex);

	Vector<OutputEntry> result;

	if (count == 0) {
		return result;
	}

	// Find the oldest entry's position.
	// If buffer is full, oldest is at write_pos. Otherwise, oldest is at 0.
	int oldest_pos = (count < CAPACITY) ? 0 : write_pos;

	// Special case: cursor 0 means "give me the latest entries."
	uint64_t effective_cursor = p_cursor;
	if (effective_cursor == 0 && count > 0) {
		uint64_t latest = entries[(write_pos == 0 ? CAPACITY - 1 : write_pos - 1)].seq;
		effective_cursor = (latest > (uint64_t)p_limit) ? (latest - (uint64_t)p_limit) : 0;
	}

	// Scan entries from oldest to newest, collecting those with seq > effective_cursor.
	int collected = 0;
	for (int i = 0; i < count && collected < p_limit; i++) {
		int idx = (oldest_pos + i) % CAPACITY;
		const OutputEntry &entry = entries[idx];
		if (entry.seq > effective_cursor) {
			result.push_back(entry);
			collected++;
		}
	}

	return result;
}

uint64_t OutputRingBuffer::latest_seq() const {
	MutexLock lock(mutex);
	if (next_seq <= 1) {
		return 0;
	}
	return next_seq - 1;
}

void OutputRingBuffer::clear() {
	MutexLock lock(mutex);
	entries.clear();
	entries.resize(CAPACITY);
	write_pos = 0;
	count = 0;
	// Keep next_seq monotonic -- don't reset! This way cursors held by
	// clients become "expired" gracefully and won't see stale data.
	// next_seq stays at its current value.
}

// ========================================================================
// MCPDebuggerBridge Implementation
// ========================================================================

MCPDebuggerBridge *MCPDebuggerBridge::singleton = nullptr;

MCPDebuggerBridge::MCPDebuggerBridge() {
	if (singleton != nullptr) {
		// Another bridge already exists — don't overwrite the singleton.
		// This can happen if the editor plugin is instantiated more than once
		// during startup (e.g. add_by_type + saved plugin state).
		print_line("[MCP] MCPDebuggerBridge: another instance already exists at " +
				itos((uint64_t)singleton) + ", this=" + itos((uint64_t)this) +
				" will NOT overwrite singleton.");
	} else {
		singleton = this;
		print_line("[MCP] MCPDebuggerBridge created, singleton set to " + itos((uint64_t)this));
	}
	game_running.clear();
	game_launching.clear();
	game_paused.clear();
	active_session_id.set(-1);
	next_test_run_number.set(0);
	{
		MutexLock lock(stop_reason_mutex);
		last_stop_reason = "not_started";
	}
}

MCPDebuggerBridge::~MCPDebuggerBridge() {
	if (singleton == this) {
		print_line("[MCP] MCPDebuggerBridge destroyed, clearing singleton (was " + itos((uint64_t)singleton) + ")");
		singleton = nullptr;
		_wake_all_pending("Bridge destroyed");
	} else {
		print_line("[MCP] MCPDebuggerBridge non-singleton instance destroyed at " + itos((uint64_t)this) +
				" (singleton is " + itos((uint64_t)singleton) + ")");
	}
}

void MCPDebuggerBridge::_bind_methods() {
	// No GDScript-facing methods needed. All access is from C++.
	// But we must have _bind_methods for GDCLASS.
}

// ------------------------------------------------------------------------
// EditorDebuggerPlugin Overrides
// ------------------------------------------------------------------------

void MCPDebuggerBridge::setup_session(int p_idx) {
	// Call parent implementation first (handles GDVIRTUAL dispatch).
	EditorDebuggerPlugin::setup_session(p_idx);

	Ref<EditorDebuggerSession> session = get_session(p_idx);
	ERR_FAIL_COND(session.is_null());

	// Connect to session lifecycle signals.
	session->connect("started", callable_mp(this, &MCPDebuggerBridge::_on_session_started));
	session->connect("stopped", callable_mp(this, &MCPDebuggerBridge::_on_session_stopped));

	// Connect to the existing "output" signal on ScriptEditorDebugger.
	// This signal is already emitted for all game output -- we tap into it
	// WITHOUT modifying ScriptEditorDebugger at all.
	ScriptEditorDebugger *dbg = EditorDebuggerNode::get_singleton()->get_debugger(p_idx);
	if (dbg) {
		dbg->connect("output", callable_mp(this, &MCPDebuggerBridge::_on_output_received));

		// Breakpoint signals.
		dbg->connect("breaked", callable_mp(this, &MCPDebuggerBridge::_on_breaked));
		dbg->connect("stack_dump", callable_mp(this, &MCPDebuggerBridge::_on_stack_dump));
		dbg->connect("stack_frame_vars", callable_mp(this, &MCPDebuggerBridge::_on_stack_frame_vars));
		dbg->connect("stack_frame_var", callable_mp(this, &MCPDebuggerBridge::_on_stack_frame_var));
	}

	active_session_id.set(p_idx);
}

bool MCPDebuggerBridge::has_capture(const String &p_capture) const {
	return p_capture == "mcp";
}

bool MCPDebuggerBridge::capture(const String &p_message, const Array &p_data, int p_session) {
	// Messages arrive as "mcp:<sub_message>". The EditorDebuggerNode::plugins_capture
	// extracts the prefix "mcp" and calls has_capture("mcp"), then calls this with
	// the FULL message string "mcp:<sub_message>".
	if (!p_message.begins_with("mcp:")) {
		return false;
	}

	String sub_msg = p_message.substr(4); // Skip "mcp:"

	// --- scene_tree_result ---
	if (sub_msg == "scene_tree_result") {
		// Convert flat tree data to hierarchical Dictionary.
		Dictionary tree_dict = _flat_tree_to_hierarchical(p_data);

		// Cache it.
		{
			MutexLock lock(scene_tree_mutex);
			cached_scene_tree = tree_dict;
			scene_tree_timestamp = Time::get_singleton()->get_ticks_msec();
		}

		// Complete any pending scene tree request.
		Dictionary result;
		result["success"] = true;
		result["tree"] = tree_dict;
		result["text"] = _tree_to_text(tree_dict);
		_complete_pending("scene_tree", result);
		return true;
	}

	// --- browse_tree_result ---
	if (sub_msg == "browse_tree_result") {
		// Convert extended flat tree data (8 fields per node) to hierarchical Dictionary.
		Dictionary tree_dict = _flat_tree_to_hierarchical_browse(p_data);

		// Cache it.
		{
			MutexLock lock(browse_tree_mutex);
			cached_browse_tree = tree_dict;
			browse_tree_timestamp = Time::get_singleton()->get_ticks_msec();
		}

		// Complete any pending browse tree request.
		Dictionary result;
		result["success"] = true;
		result["tree"] = tree_dict;
		_complete_pending("browse_scene_tree", result);
		return true;
	}

	// --- eval_result ---
	if (sub_msg == "eval_result") {
		ERR_FAIL_COND_V(p_data.size() < 2, false);
		Dictionary result;
		result["success"] = (bool)p_data[0];
		result["value"] = p_data[1];
		_complete_pending("evaluate", result);
		return true;
	}

	// --- exec_result ---
	if (sub_msg == "exec_result") {
		ERR_FAIL_COND_V(p_data.size() < 2, false);
		Dictionary result;
		result["success"] = (bool)p_data[0];
		result["value"] = p_data[1];
		_complete_pending("execute_code", result);
		return true;
	}

	// --- action_done ---
	if (sub_msg == "action_done") {
		Dictionary result;
		if (p_data.size() >= 2) {
			result["success"] = (bool)p_data[0];
			result["message"] = p_data[1];
		} else {
			result["success"] = true;
			result["message"] = "";
		}
		_complete_pending("inject_action", result);
		return true;
	}

	// --- click_result ---
	if (sub_msg == "click_result") {
		ERR_FAIL_COND_V(p_data.size() < 2, false);
		Dictionary result;
		result["success"] = (bool)p_data[0];
		result["message"] = p_data[1];
		_complete_pending("click_control", result);
		return true;
	}

	// --- wait_done ---
	if (sub_msg == "wait_done") {
		Dictionary result;
		result["success"] = true;
		_complete_pending("wait_frames", result);
		return true;
	}

	// --- screenshot_result ---
	if (sub_msg == "screenshot_result") {
		ERR_FAIL_COND_V(p_data.is_empty(), false);
		Dictionary result;
		result["success"] = true;
		result["base64_png"] = p_data[0];
		if (p_data.size() >= 3) {
			result["width"] = (int)p_data[1];
			result["height"] = (int)p_data[2];
		}
		_complete_pending("screenshot", result);
		return true;
	}

	// --- performance_result ---
	if (sub_msg == "performance_result") {
		ERR_FAIL_COND_V(p_data.size() < 7, false);
		Dictionary result;
		result["success"] = true;
		result["fps"] = p_data[0];
		result["frame_time"] = p_data[1];
		result["physics_frame_time"] = p_data[2];
		result["memory"] = p_data[3];
		result["object_count"] = p_data[4];
		result["node_count"] = p_data[5];
		result["orphan_count"] = p_data[6];
		_complete_pending("get_performance", result);
		return true;
	}

	// --- ui_interact_result ---
	if (sub_msg == "ui_interact_result") {
		ERR_FAIL_COND_V(p_data.size() < 2, false);
		Dictionary result;
		result["success"] = (bool)p_data[0];
		result["data"] = p_data[1];
		_complete_pending("ui_interact", result);
		return true;
	}

	// --- key_done ---
	if (sub_msg == "key_done") {
		ERR_FAIL_COND_V(p_data.size() < 2, false);
		Dictionary result;
		result["success"] = (bool)p_data[0];
		result["message"] = p_data[1];
		_complete_pending("inject_key", result);
		return true;
	}

	// --- joypad_done ---
	if (sub_msg == "joypad_done") {
		ERR_FAIL_COND_V(p_data.size() < 2, false);
		Dictionary result;
		result["success"] = (bool)p_data[0];
		result["message"] = p_data[1];
		_complete_pending("inject_joypad", result);
		return true;
	}

	// --- type_text_done ---
	if (sub_msg == "type_text_done") {
		ERR_FAIL_COND_V(p_data.size() < 2, false);
		Dictionary result;
		result["success"] = (bool)p_data[0];
		result["chars_typed"] = p_data[1];
		_complete_pending("type_text", result);
		return true;
	}

	// --- sequence_done ---
	if (sub_msg == "sequence_done") {
		ERR_FAIL_COND_V(p_data.size() < 3, false);
		Dictionary result;
		result["success"] = (bool)p_data[0];
		result["steps_executed"] = p_data[1];
		result["step_results"] = p_data[2];
		_complete_pending("input_sequence", result);
		return true;
	}

	// --- held_inputs_result ---
	if (sub_msg == "held_inputs_result") {
		ERR_FAIL_COND_V(p_data.size() < 2, false);
		Dictionary result;
		result["success"] = true;
		result["data"] = p_data[0];
		result["released_count"] = p_data[1];
		_complete_pending("get_held_inputs", result);
		return true;
	}

	// --- node_signals_result ---
	if (sub_msg == "node_signals_result") {
		ERR_FAIL_COND_V(p_data.size() < 1, false);
		Dictionary result = p_data[0];
		_complete_pending("get_node_signals", result);
		return true;
	}

	// --- emit_signal_result ---
	if (sub_msg == "emit_signal_result") {
		ERR_FAIL_COND_V(p_data.size() < 1, false);
		Dictionary result = p_data[0];
		_complete_pending("emit_signal", result);
		return true;
	}

	// --- set_property_result ---
	if (sub_msg == "set_property_result") {
		ERR_FAIL_COND_V(p_data.size() < 1, false);
		Dictionary result = p_data[0];
		_complete_pending("set_node_property", result);
		return true;
	}

	// --- test_method_result ---
	if (sub_msg == "test_method_result") {
		ERR_FAIL_COND_V(p_data.size() < 8, false);

		String file = p_data[0];
		String method = p_data[1];
		String status = p_data[2];
		String message = p_data[3];
		String error_file = p_data[4];
		int error_line = p_data[5];
		int duration_ms = p_data[6];
		Array output = p_data[7];

		// Build result dictionary and accumulate.
		Dictionary result;
		result["file"] = file;
		result["method"] = method;
		result["status"] = status;
		result["message"] = message;
		result["error_file"] = error_file;
		result["error_line"] = error_line;
		result["duration_ms"] = duration_ms;
		result["output"] = output;

		{
			MutexLock lock(request_mutex);
			test_run_state.results.push_back(result);
		}

		// Push event to the test event buffer for the status panel.
		MCPTestEvent evt;
		evt.type = MCPTestEvent::TEST_METHOD_RESULT;
		evt.run_number = test_run_state.run_number;
		evt.file_path = file;
		evt.method_name = method;
		evt.status = status;
		evt.message = message;
		evt.error_file = error_file;
		evt.error_line = error_line;
		evt.duration_ms = duration_ms;
		test_event_buffer.push(evt);

		return true;
	}

	// --- test_complete ---
	if (sub_msg == "test_complete") {
		ERR_FAIL_COND_V(p_data.size() < 6, false);

		int total = p_data[0];
		int passed = p_data[1];
		int failed = p_data[2];
		int errors = p_data[3];
		int skipped = p_data[4];
		int duration_ms = p_data[5];

		// Build the final result dictionary.
		Dictionary result;
		Dictionary summary;
		summary["total"] = total;
		summary["passed"] = passed;
		summary["failed"] = failed;
		summary["errors"] = errors;
		summary["skipped"] = skipped;
		summary["duration_ms"] = duration_ms;
		result["summary"] = summary;
		result["success"] = true;

		{
			MutexLock lock(request_mutex);
			result["results"] = test_run_state.results;
			test_run_state.complete = true;
		}

		// Push completion event to the test event buffer.
		MCPTestEvent evt;
		evt.type = MCPTestEvent::TEST_RUN_COMPLETE;
		evt.run_number = test_run_state.run_number;
		evt.total = total;
		evt.passed = passed;
		evt.failed = failed;
		evt.errors = errors;
		evt.skipped = skipped;
		evt.total_duration_ms = duration_ms;
		test_event_buffer.push(evt);

		// Wake the waiting tool handler.
		_complete_pending("test_run", result);

		return true;
	}

	// --- heartbeat ---
	// Game-side heartbeat: sent every 60 frames with the current frame count.
	if (sub_msg == "heartbeat") {
		if (p_data.size() >= 1) {
			game_frame_count.set((int64_t)p_data[0]);
		}
		return true;
	}

	return false;
}

// ------------------------------------------------------------------------
// Signal Callbacks (editor main thread)
// ------------------------------------------------------------------------

void MCPDebuggerBridge::_on_session_started() {
	game_running.set();
	game_launching.clear(); // No longer launching -- session is connected.
	game_paused.clear();
	{
		MutexLock lock(break_state_mutex);
		cached_break_state.clear();
	}
	game_start_time_msec.set(Time::get_singleton()->get_ticks_msec());
	game_frame_count.set(0);
	output_buffer.clear();
	error_buffer.clear();

	// Clear cached scene trees.
	{
		MutexLock lock(scene_tree_mutex);
		cached_scene_tree = Dictionary();
		scene_tree_timestamp = 0;
	}
	{
		MutexLock lock(browse_tree_mutex);
		cached_browse_tree = Dictionary();
		browse_tree_timestamp = 0;
	}

	// If suspend-on-start was requested, send the suspend message immediately
	// so the game pauses at the earliest possible moment after connecting.
	if (suspend_on_start.is_set()) {
		suspend_on_start.clear();
		int sid = active_session_id.get();
		Ref<EditorDebuggerSession> session = get_session(sid >= 0 ? sid : 0);
		if (session.is_valid()) {
			Array suspend_data;
			suspend_data.push_back(true);
			session->send_message("scene:suspend_changed", suspend_data);
		}
		game_paused.set();
		print_verbose("[MCP] Debugger bridge: game session started (suspended on start).");
	} else {
		print_verbose("[MCP] Debugger bridge: game session started.");
	}

	// Notify MCP clients that tool/resource lists have changed (debug tools now available).
	MCPProtocol *protocol = MCPProtocol::get_singleton();
	if (protocol) {
		protocol->notify_tools_changed();
		protocol->notify_resources_list_changed();
		protocol->send_log("info", "mcp.debug", "Game started");

		// Notify subscribers that game status changed.
		protocol->get_resource_registry()->notify_changed("godot://game/status");
	}
}

void MCPDebuggerBridge::_on_session_stopped() {
	// Determine stop reason BEFORE clearing state flags.
	// If game_launching is still set, this stop is from a previous run being
	// terminated as part of a re-launch -- don't overwrite the stop reason
	// or clear the launching flag.
	bool is_relaunch = game_launching.is_set() && game_running.is_set();

	if (!is_relaunch) {
		String reason;
		if (game_launching.is_set() && !game_running.is_set()) {
			reason = "timeout";
		} else {
			reason = "normal";
		}

		{
			MutexLock lock(stop_reason_mutex);
			last_stop_reason = reason;
		}

		game_launching.clear();
	}

	game_running.clear();
	game_paused.clear();
	{
		MutexLock lock(break_state_mutex);
		cached_break_state.clear();
	}
	break_state_ready.post(); // Wake anyone waiting for break state.
	active_session_id.set(-1);

	// If there is an active test run that did not complete, finalize it with
	// partial results and a game_crashed flag so the tool handler gets something useful.
	if (test_run_state.run_number > 0 && !test_run_state.complete) {
		Dictionary result;
		Dictionary summary;
		summary["total"] = 0;
		summary["passed"] = 0;
		summary["failed"] = 0;
		summary["errors"] = 0;
		summary["skipped"] = 0;
		summary["duration_ms"] = 0;
		result["summary"] = summary;
		result["success"] = false;
		result["game_crashed"] = true;

		{
			MutexLock lock(request_mutex);
			result["results"] = test_run_state.results;
		}

		// Push a completion event so the status panel can update.
		MCPTestEvent evt;
		evt.type = MCPTestEvent::TEST_RUN_COMPLETE;
		evt.run_number = test_run_state.run_number;
		test_event_buffer.push(evt);

		_complete_pending("test_run", result);
	}

	// Wake ALL pending requests with an error -- the game is gone.
	_wake_all_pending("Game session ended");

	if (is_relaunch) {
		print_verbose("[MCP] Debugger bridge: old session stopped (re-launching).");
	} else {
		String reason;
		{
			MutexLock lock(stop_reason_mutex);
			reason = last_stop_reason;
		}
		print_verbose("[MCP] Debugger bridge: game session stopped (reason: " + reason + ").");

		// Notify MCP clients that tool/resource lists have changed (debug tools unavailable).
		MCPProtocol *protocol = MCPProtocol::get_singleton();
		if (protocol) {
			protocol->notify_tools_changed();
			protocol->notify_resources_list_changed();
			protocol->send_log("info", "mcp.debug", "Game stopped (reason: " + reason + ")");

			// Notify subscribers that game status changed.
			protocol->get_resource_registry()->notify_changed("godot://game/status");
		}
	}
}

void MCPDebuggerBridge::_on_output_received(const String &p_msg, int p_type) {
	// p_type values come from EditorLog::MessageType:
	//   MSG_TYPE_STD = 0
	//   MSG_TYPE_STD_RICH = 1
	//   MSG_TYPE_ERROR = 2
	//   MSG_TYPE_WARNING = 3
	//   MSG_TYPE_EDITOR = 4
	//   MSG_TYPE_INSPECTOR = 5

	// Route errors to error buffer, everything else to output buffer.
	if (p_type == 2) { // MSG_TYPE_ERROR
		error_buffer.push(p_msg, 2);
	} else if (p_type == 3) { // MSG_TYPE_WARNING
		error_buffer.push(p_msg, 1);
	} else {
		output_buffer.push(p_msg, p_type);
	}
}

// ------------------------------------------------------------------------
// State Queries
// ------------------------------------------------------------------------

bool MCPDebuggerBridge::is_game_running() const {
	return game_running.is_set();
}

bool MCPDebuggerBridge::is_game_launching() const {
	if (!game_launching.is_set()) {
		return false;
	}
	// Auto-timeout after 15 seconds.
	uint64_t elapsed = Time::get_singleton()->get_ticks_msec() - game_launch_time_msec.get();
	if (elapsed > 15000) {
		return false;
	}
	return true;
}

bool MCPDebuggerBridge::is_game_paused() const {
	return game_paused.is_set();
}

double MCPDebuggerBridge::get_game_uptime_seconds() const {
	uint64_t start = game_start_time_msec.get();
	if (start == 0 || !game_running.is_set()) {
		return 0.0;
	}
	uint64_t now = Time::get_singleton()->get_ticks_msec();
	return (double)(now - start) / 1000.0;
}

int64_t MCPDebuggerBridge::get_game_frame_count() const {
	return game_frame_count.get();
}

String MCPDebuggerBridge::get_last_stop_reason() const {
	MutexLock lock(stop_reason_mutex);
	return last_stop_reason;
}

void MCPDebuggerBridge::set_game_launching(bool p_suspend_on_start) {
	game_launching.set();
	game_launch_time_msec.set(Time::get_singleton()->get_ticks_msec());
	if (p_suspend_on_start) {
		suspend_on_start.set();
	} else {
		suspend_on_start.clear();
	}
}

// ------------------------------------------------------------------------
// Output/Error Access (thread-safe)
// ------------------------------------------------------------------------

Vector<OutputEntry> MCPDebuggerBridge::get_output_since(uint64_t p_cursor, int p_limit) const {
	return output_buffer.read_since(p_cursor, p_limit);
}

Vector<OutputEntry> MCPDebuggerBridge::get_errors_since(uint64_t p_cursor, int p_limit) const {
	return error_buffer.read_since(p_cursor, p_limit);
}

uint64_t MCPDebuggerBridge::get_output_latest_seq() const {
	return output_buffer.latest_seq();
}

uint64_t MCPDebuggerBridge::get_error_latest_seq() const {
	return error_buffer.latest_seq();
}

void MCPDebuggerBridge::clear_output_buffer() {
	output_buffer.clear();
}

void MCPDebuggerBridge::clear_error_buffer() {
	error_buffer.clear();
}

// ------------------------------------------------------------------------
// Async Request Infrastructure
// ------------------------------------------------------------------------

PendingRequest *MCPDebuggerBridge::_create_pending(const String &p_type) {
	MutexLock lock(request_mutex);

	// If there is already a pending request of this type, cancel it.
	if (pending_requests.has(p_type)) {
		PendingRequest *old = pending_requests[p_type];
		old->result["error"] = "Superseded by new request";
		old->completed.set();
		old->semaphore.post();
		// Do NOT delete -- the waiting thread still holds a pointer.
		// It will clean up after being woken.
		pending_requests.erase(p_type);
	}

	PendingRequest *req = memnew(PendingRequest);
	req->type = p_type;
	pending_requests[p_type] = req;
	return req;
}

Dictionary MCPDebuggerBridge::_wait_for_pending(PendingRequest *p_request, int p_timeout_msec) {
	// Block the calling thread until the request completes or times out.
	// We use a polling loop with try_wait to implement timeout, because
	// Godot's Semaphore class does not support timed waits.

	uint64_t start_msec = Time::get_singleton()->get_ticks_msec();
	const int POLL_INTERVAL_MSEC = 50;

	while (!p_request->completed.is_set()) {
		uint64_t elapsed = Time::get_singleton()->get_ticks_msec() - start_msec;
		if ((int64_t)elapsed >= p_timeout_msec) {
			// Timeout. Remove from pending map and clean up.
			{
				MutexLock lock(request_mutex);
				if (pending_requests.has(p_request->type) && pending_requests[p_request->type] == p_request) {
					pending_requests.erase(p_request->type);
				}
			}
			Dictionary timeout_result;
			timeout_result["error"] = "Request timed out after " + itos(p_timeout_msec) + "ms";
			timeout_result["success"] = false;
			memdelete(p_request);
			return timeout_result;
		}

		// Try to acquire the semaphore with a short sleep to prevent busy-waiting.
		bool got_it = p_request->semaphore.try_wait();
		if (got_it) {
			break;
		}

		// Sleep briefly to avoid spinning.
		OS::get_singleton()->delay_usec(POLL_INTERVAL_MSEC * 1000);
	}

	Dictionary result = p_request->result;

	// Clean up.
	{
		MutexLock lock(request_mutex);
		if (pending_requests.has(p_request->type) && pending_requests[p_request->type] == p_request) {
			pending_requests.erase(p_request->type);
		}
	}
	memdelete(p_request);

	return result;
}

void MCPDebuggerBridge::_complete_pending(const String &p_request_id, const Dictionary &p_result) {
	MutexLock lock(request_mutex);

	if (!pending_requests.has(p_request_id)) {
		// No one is waiting for this. That is fine -- the response may have
		// arrived after a timeout or after the game stopped.
		return;
	}

	PendingRequest *req = pending_requests[p_request_id];
	req->result = p_result;
	req->completed.set();
	req->semaphore.post();
	// Do NOT erase from map here -- the waiting thread does that in _wait_for_pending.
}

void MCPDebuggerBridge::_wake_all_pending(const String &p_error_message) {
	MutexLock lock(request_mutex);

	for (KeyValue<String, PendingRequest *> &E : pending_requests) {
		PendingRequest *req = E.value;
		req->result["error"] = p_error_message;
		req->result["success"] = false;
		req->completed.set();
		req->semaphore.post();
	}
	// Clear map. Waiting threads check completed.is_set() first.
	pending_requests.clear();
}

// ------------------------------------------------------------------------
// Async Request Methods (called from MCP HTTP threads)
// ------------------------------------------------------------------------

// Helper macro to reduce boilerplate in request methods.
#define MCP_BRIDGE_CHECK_RUNNING()                       \
	if (!game_running.is_set()) {                        \
		Dictionary err;                                  \
		err["error"] = "Game is not running";            \
		err["success"] = false;                          \
		return err;                                      \
	}

#define MCP_BRIDGE_SEND_OR_FAIL(p_type, p_message, p_data)                                                    \
	PendingRequest *req = _create_pending(p_type);                                                             \
	int _sid = active_session_id.get();                                                                        \
	Ref<EditorDebuggerSession> session = get_session(_sid >= 0 ? _sid : 0);                                    \
	if (session.is_valid()) {                                                                                  \
		/* Dispatch send_message to the main thread via call_deferred.                  */                     \
		/* This method may be called from MCP background threads, but                   */                     \
		/* EditorDebuggerSession::send_message() is only safe on the main thread.       */                     \
		callable_mp(session.ptr(), &EditorDebuggerSession::send_message).call_deferred(p_message, p_data);     \
	} else {                                                                                                   \
		Dictionary err;                                                                                        \
		err["error"] = "No active debugger session";                                                           \
		err["success"] = false;                                                                                \
		MutexLock lock(request_mutex);                                                                         \
		pending_requests.erase(p_type);                                                                        \
		memdelete(req);                                                                                        \
		return err;                                                                                            \
	}

Dictionary MCPDebuggerBridge::request_scene_tree(int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();
	MCP_BRIDGE_SEND_OR_FAIL("scene_tree", "mcp:get_scene_tree", Array());
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::request_browse_scene_tree(int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();
	MCP_BRIDGE_SEND_OR_FAIL("browse_scene_tree", "mcp:get_scene_tree_browse", Array());
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::send_evaluate(const String &p_expression, int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();

	Array data;
	data.push_back(p_expression);

	MCP_BRIDGE_SEND_OR_FAIL("evaluate", "mcp:evaluate", data);
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::send_execute_code(const String &p_code, int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();

	Array data;
	data.push_back(p_code);

	MCP_BRIDGE_SEND_OR_FAIL("execute_code", "mcp:execute_code", data);
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::send_inject_action(const String &p_action, bool p_pressed, int p_hold_frames, float p_strength, int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();

	Array data;
	data.push_back(p_action);
	data.push_back(p_pressed);
	data.push_back(p_hold_frames);
	data.push_back(p_strength);

	MCP_BRIDGE_SEND_OR_FAIL("inject_action", "mcp:inject_action", data);
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::send_click_control(const String &p_node_path, int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();

	Array data;
	data.push_back(p_node_path);

	MCP_BRIDGE_SEND_OR_FAIL("click_control", "mcp:click_control", data);
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::send_wait_frames(int p_frame_count, int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();

	Array data;
	data.push_back(p_frame_count);

	MCP_BRIDGE_SEND_OR_FAIL("wait_frames", "mcp:wait_frames", data);
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::send_screenshot(int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();
	MCP_BRIDGE_SEND_OR_FAIL("screenshot", "mcp:screenshot", Array());
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::send_get_performance(int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();
	MCP_BRIDGE_SEND_OR_FAIL("get_performance", "mcp:get_performance", Array());
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::send_ui_interact(const String &p_action, const String &p_node_path,
		const Dictionary &p_params, int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();

	Array data;
	data.push_back(p_action);
	data.push_back(p_node_path);
	data.push_back(p_params);

	MCP_BRIDGE_SEND_OR_FAIL("ui_interact", "mcp:ui_interact", data);
	return _wait_for_pending(req, p_timeout_msec);
}

// --- Input Simulation Bridge Methods ---

Dictionary MCPDebuggerBridge::send_inject_key(const String &p_key_name, bool p_pressed,
		int p_hold_frames, int p_modifier_flags, bool p_echo, int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();

	Array data;
	data.push_back(p_key_name);
	data.push_back(p_pressed);
	data.push_back(p_hold_frames);
	data.push_back(p_modifier_flags);
	data.push_back(p_echo);

	MCP_BRIDGE_SEND_OR_FAIL("inject_key", "mcp:inject_key", data);
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::send_inject_joypad_button(const String &p_button_name, bool p_pressed,
		int p_hold_frames, int p_device, int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();

	Array data;
	data.push_back(p_button_name);
	data.push_back(p_pressed);
	data.push_back(p_hold_frames);
	data.push_back(p_device);

	MCP_BRIDGE_SEND_OR_FAIL("inject_joypad", "mcp:inject_joypad_button", data);
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::send_inject_joypad_axis(const String &p_axis_name, float p_value,
		int p_hold_frames, int p_device, int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();

	Array data;
	data.push_back(p_axis_name);
	data.push_back(p_value);
	data.push_back(p_hold_frames);
	data.push_back(p_device);

	MCP_BRIDGE_SEND_OR_FAIL("inject_joypad", "mcp:inject_joypad_axis", data);
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::send_type_text(const String &p_text, int p_interval_frames,
		int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();

	Array data;
	data.push_back(p_text);
	data.push_back(p_interval_frames);

	MCP_BRIDGE_SEND_OR_FAIL("type_text", "mcp:type_text", data);
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::send_input_sequence(const Array &p_steps, int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();

	Array data;
	data.push_back(p_steps);

	MCP_BRIDGE_SEND_OR_FAIL("input_sequence", "mcp:run_input_sequence", data);
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::send_get_held_inputs(bool p_release_all, int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();

	Array data;
	data.push_back(p_release_all);

	MCP_BRIDGE_SEND_OR_FAIL("get_held_inputs", "mcp:get_held_inputs", data);
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::send_get_node_signals(const String &p_node_path,
		bool p_include_inherited, const String &p_signal_name,
		int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();
	Array data;
	data.push_back(p_node_path);
	data.push_back(p_include_inherited);
	data.push_back(p_signal_name);
	MCP_BRIDGE_SEND_OR_FAIL("get_node_signals", "mcp:get_node_signals", data);
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::send_emit_signal(const String &p_node_path,
		const String &p_signal_name, const Array &p_args,
		int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();
	Array data;
	data.push_back(p_node_path);
	data.push_back(p_signal_name);
	data.push_back(p_args);
	MCP_BRIDGE_SEND_OR_FAIL("emit_signal", "mcp:emit_signal", data);
	return _wait_for_pending(req, p_timeout_msec);
}

Dictionary MCPDebuggerBridge::send_set_node_property(const String &p_node_path,
		const String &p_property, const Variant &p_value, const String &p_field,
		int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();
	Array data;
	data.push_back(p_node_path);
	data.push_back(p_property);
	data.push_back(p_value);
	data.push_back(p_field);
	MCP_BRIDGE_SEND_OR_FAIL("set_node_property", "mcp:set_node_property", data);
	return _wait_for_pending(req, p_timeout_msec);
}

#undef MCP_BRIDGE_CHECK_RUNNING
#undef MCP_BRIDGE_SEND_OR_FAIL

// ------------------------------------------------------------------------
// Time Control (fire-and-forget, called from MCP HTTP threads)
// ------------------------------------------------------------------------

// Helper: send a message to the game via call_deferred (fire-and-forget).
// Returns silently if no active session.
#define MCP_BRIDGE_FIRE_AND_FORGET(p_message, p_data)                                                           \
	int _sid = active_session_id.get();                                                                         \
	Ref<EditorDebuggerSession> session = get_session(_sid >= 0 ? _sid : 0);                                     \
	if (session.is_valid()) {                                                                                   \
		callable_mp(session.ptr(), &EditorDebuggerSession::send_message).call_deferred(p_message, p_data);      \
	}

void MCPDebuggerBridge::send_set_time_scale(double p_scale) {
	Array data;
	data.push_back(p_scale);
	MCP_BRIDGE_FIRE_AND_FORGET("scene:speed_changed", data);
}

void MCPDebuggerBridge::send_suspend(bool p_enabled) {
	Array data;
	data.push_back(p_enabled);
	MCP_BRIDGE_FIRE_AND_FORGET("scene:suspend_changed", data);
}

void MCPDebuggerBridge::send_next_frame() {
	MCP_BRIDGE_FIRE_AND_FORGET("scene:next_frame", Array());
}

void MCPDebuggerBridge::send_advance_frames(int p_count, bool p_instant) {
	Array data;
	data.push_back(p_count);
	data.push_back(p_instant);
	MCP_BRIDGE_FIRE_AND_FORGET("scene:advance_frames", data);
}

void MCPDebuggerBridge::send_set_debug_pause_enabled(bool p_enabled) {
	Array data;
	data.push_back(p_enabled);
	MCP_BRIDGE_FIRE_AND_FORGET("scene:set_debug_pause_enabled", data);
}

void MCPDebuggerBridge::send_set_debug_pause_tag_enabled(const String &p_tag, bool p_enabled) {
	Array data;
	data.push_back(p_tag);
	data.push_back(p_enabled);
	MCP_BRIDGE_FIRE_AND_FORGET("scene:set_debug_pause_tag_enabled", data);
}

void MCPDebuggerBridge::send_clear_debug_pause_hits() {
	MCP_BRIDGE_FIRE_AND_FORGET("scene:clear_debug_pause_hits", Array());
}

#undef MCP_BRIDGE_FIRE_AND_FORGET

// ------------------------------------------------------------------------
// Cached Scene Tree
// ------------------------------------------------------------------------

Dictionary MCPDebuggerBridge::get_cached_scene_tree() const {
	MutexLock lock(scene_tree_mutex);
	return cached_scene_tree;
}

Dictionary MCPDebuggerBridge::get_cached_browse_tree() const {
	MutexLock lock(browse_tree_mutex);
	return cached_browse_tree;
}

// ------------------------------------------------------------------------
// Breakpoint Signal Handlers (editor main thread)
// ------------------------------------------------------------------------

void MCPDebuggerBridge::_on_breaked(bool p_reallydid, bool p_can_debug, const String &p_reason, bool p_has_stackdump) {
	MutexLock lock(break_state_mutex);

	if (p_reallydid) {
		cached_break_state.paused = true;
		cached_break_state.can_debug = p_can_debug;
		cached_break_state.reason = p_reason;
		cached_break_state.has_stackdump = p_has_stackdump;
		game_paused.set();
		break_state_ready.post(); // Wake anyone waiting for break state (step-and-wait).
		print_verbose("[MCP] Debugger bridge: game paused (reason: " + p_reason + ").");
	} else {
		cached_break_state.clear();
		game_paused.clear();
		print_verbose("[MCP] Debugger bridge: game resumed.");
	}
}

void MCPDebuggerBridge::_on_stack_dump(const Array &p_stack_dump) {
	MutexLock lock(break_state_mutex);

	cached_break_state.stack.clear();
	for (int i = 0; i < p_stack_dump.size(); i++) {
		Dictionary d = p_stack_dump[i];
		BreakState::StackFrame sf;
		sf.frame_index = d.get("frame", 0);
		sf.file = d.get("file", "");
		sf.function = d.get("function", "");
		sf.line = d.get("line", 0);
		cached_break_state.stack.push_back(sf);
	}

	if (cached_break_state.inspected_frame < 0) {
		break_state_ready.post();
	}

	print_verbose("[MCP] Debugger bridge: received stack dump (" + itos(cached_break_state.stack.size()) + " frames).");
}

void MCPDebuggerBridge::_on_stack_frame_vars(int p_num_vars) {
	MutexLock lock(break_state_mutex);

	cached_break_state.expected_var_count = p_num_vars;
	cached_break_state.variables.clear();

	if (p_num_vars == 0) {
		break_state_ready.post();
	}

	print_verbose("[MCP] Debugger bridge: expecting " + itos(p_num_vars) + " stack frame variables.");
}

void MCPDebuggerBridge::_on_stack_frame_var(const Array &p_data) {
	MutexLock lock(break_state_mutex);

	DebuggerMarshalls::ScriptStackVariable var_data;
	if (var_data.deserialize(p_data)) {
		BreakState::Variable var;
		var.name = var_data.name;
		var.value = var_data.value.stringify();
		var.type_name = Variant::get_type_name(var_data.value.get_type());
		var.category = var_data.type; // 0=local, 1=member, 2=global
		cached_break_state.variables.push_back(var);
	}

	if (cached_break_state.variables.size() >= cached_break_state.expected_var_count &&
			cached_break_state.expected_var_count > 0) {
		break_state_ready.post();
	}
}

// ------------------------------------------------------------------------
// Break State Query Methods (thread-safe, called from MCP HTTP threads)
// ------------------------------------------------------------------------

Dictionary MCPDebuggerBridge::get_break_state_snapshot() const {
	MutexLock lock(break_state_mutex);

	Dictionary result;
	result["paused"] = cached_break_state.paused;
	result["reason"] = cached_break_state.reason;
	result["can_debug"] = cached_break_state.can_debug;
	result["has_stackdump"] = cached_break_state.has_stackdump;

	Array stack_arr;
	for (int i = 0; i < cached_break_state.stack.size(); i++) {
		const BreakState::StackFrame &sf = cached_break_state.stack[i];
		Dictionary fd;
		fd["frame"] = sf.frame_index;
		fd["file"] = sf.file;
		fd["function"] = sf.function;
		fd["line"] = sf.line;
		stack_arr.push_back(fd);
	}
	result["stack"] = stack_arr;

	if (cached_break_state.inspected_frame >= 0) {
		Array locals;
		Array members;
		Array globals;
		for (int i = 0; i < cached_break_state.variables.size(); i++) {
			const BreakState::Variable &v = cached_break_state.variables[i];
			Dictionary vd;
			vd["name"] = v.name;
			vd["value"] = v.value;
			vd["type"] = v.type_name;
			if (v.category == 0) {
				locals.push_back(vd);
			} else if (v.category == 1) {
				members.push_back(vd);
			} else {
				globals.push_back(vd);
			}
		}
		result["inspected_frame"] = cached_break_state.inspected_frame;
		result["locals"] = locals;
		result["members"] = members;
		result["globals"] = globals;
	}

	return result;
}

Dictionary MCPDebuggerBridge::request_frame_variables(int p_frame, int p_timeout_msec) {
	{
		MutexLock lock(break_state_mutex);
		if (!cached_break_state.paused) {
			Dictionary err;
			err["error"] = "Game is not paused at a breakpoint";
			err["success"] = false;
			return err;
		}
		cached_break_state.inspected_frame = p_frame;
		cached_break_state.variables.clear();
		cached_break_state.expected_var_count = 0;
	}

	// Request stack dump for the given frame on the main thread.
	ScriptEditorDebugger *dbg = EditorDebuggerNode::get_singleton()->get_debugger(active_session_id.get() >= 0 ? active_session_id.get() : 0);
	if (dbg) {
		callable_mp(dbg, &ScriptEditorDebugger::request_stack_dump).call_deferred(p_frame);
	} else {
		Dictionary err;
		err["error"] = "No active debugger session";
		err["success"] = false;
		return err;
	}

	// Wait for variables to arrive.
	uint64_t start = Time::get_singleton()->get_ticks_msec();
	while (Time::get_singleton()->get_ticks_msec() - start < (uint64_t)p_timeout_msec) {
		bool ready = false;
		{
			MutexLock lock(break_state_mutex);
			if (cached_break_state.expected_var_count > 0 &&
					cached_break_state.variables.size() >= cached_break_state.expected_var_count) {
				ready = true;
			}
		}
		if (ready) {
			return get_break_state_snapshot();
		}
		OS::get_singleton()->delay_usec(10000); // 10ms
	}

	// Timeout -- return whatever we have.
	return get_break_state_snapshot();
}

void MCPDebuggerBridge::wait_for_rebreak(int p_timeout_msec) {
	uint64_t start = Time::get_singleton()->get_ticks_msec();
	while (Time::get_singleton()->get_ticks_msec() - start < (uint64_t)p_timeout_msec) {
		{
			MutexLock lock(break_state_mutex);
			if (cached_break_state.paused && !cached_break_state.stack.is_empty()) {
				return; // Re-paused with stack info.
			}
		}
		OS::get_singleton()->delay_usec(10000); // 10ms
	}
}

// ------------------------------------------------------------------------
// Breakpoint Management (deferred to main thread)
// ------------------------------------------------------------------------

Dictionary MCPDebuggerBridge::get_all_breakpoints(int p_timeout_msec) {
	// Called from tool handler (MCP HTTP thread).
	// Create a pending request, dispatch to main thread, wait for result.
	PendingRequest *req = _create_pending("get_breakpoints");
	callable_mp(this, &MCPDebuggerBridge::_do_get_breakpoints).call_deferred(String("get_breakpoints"));
	return _wait_for_pending(req, p_timeout_msec);
}

void MCPDebuggerBridge::_do_get_breakpoints(const String &p_request_id) {
	// Runs on main thread.
	Dictionary result;
	Array breakpoints;

	ScriptEditor *se = ScriptEditor::get_singleton();
	if (se) {
		List<String> bp_strings;
		se->get_breakpoints(&bp_strings);
		for (const String &bp : bp_strings) {
			int colon = bp.rfind(":");
			if (colon < 0) {
				continue;
			}
			Dictionary bpd;
			bpd["path"] = bp.substr(0, colon);
			bpd["line"] = bp.substr(colon + 1).to_int();
			bpd["enabled"] = true;
			breakpoints.push_back(bpd);
		}
	}

	result["breakpoints"] = breakpoints;
	_complete_pending(p_request_id, result);
}

// ------------------------------------------------------------------------
// Status Panel Queries
// ------------------------------------------------------------------------

int MCPDebuggerBridge::get_pending_request_count() const {
	MutexLock lock(request_mutex);
	return pending_requests.size();
}

// ------------------------------------------------------------------------
// Test Runner
// ------------------------------------------------------------------------

void MCPDebuggerBridge::reset_test_run_state() {
	MutexLock lock(request_mutex);

	test_run_state.results.clear();
	test_run_state.complete = false;
	test_run_state.run_number = next_test_run_number.increment();

	// Push a TEST_RUN_STARTED event so the status panel knows a new run began.
	MCPTestEvent evt;
	evt.type = MCPTestEvent::TEST_RUN_STARTED;
	evt.run_number = test_run_state.run_number;
	test_event_buffer.push(evt);
}

void MCPDebuggerBridge::push_test_compile_error(const String &p_file, const Array &p_errors, int p_run_number) {
	MCPTestEvent evt;
	evt.type = MCPTestEvent::TEST_FILE_COMPILE_ERROR;
	evt.run_number = p_run_number;
	evt.file_path = p_file;
	evt.compile_errors = p_errors;
	test_event_buffer.push(evt);
}

// ------------------------------------------------------------------------
// Scene Tree Serialization
// ------------------------------------------------------------------------

Dictionary MCPDebuggerBridge::_flat_tree_to_hierarchical(const Array &p_flat_data) const {
	// The flat data from SceneDebuggerTree::serialize() is:
	//   [child_count, name, type_name, id, scene_file_path, view_flags, ...]
	// Each node has 6 fields. child_count tells how many direct children follow
	// (recursively -- the flat list is depth-first).

	if (p_flat_data.is_empty()) {
		return Dictionary();
	}

	struct NodeInfo {
		String name;
		String type;
		uint64_t id = 0;
		String scene_file_path;
		int view_flags = 0;
		int child_count = 0;
	};

	// Parse all nodes from flat array.
	Vector<NodeInfo> nodes;
	int idx = 0;
	while (idx + 5 < p_flat_data.size()) {
		NodeInfo info;
		info.child_count = p_flat_data[idx];
		info.name = p_flat_data[idx + 1];
		info.type = p_flat_data[idx + 2];
		info.id = (uint64_t)(int64_t)p_flat_data[idx + 3];
		info.scene_file_path = p_flat_data[idx + 4];
		info.view_flags = p_flat_data[idx + 5];
		nodes.push_back(info);
		idx += 6;
	}

	if (nodes.is_empty()) {
		return Dictionary();
	}

	// Build hierarchical structure using an explicit stack (not recursion).

	struct StackEntry {
		int node_index;
		int children_remaining;
		Dictionary dict;
		Array children_array;
	};

	auto make_dict = [](const NodeInfo &info) -> Dictionary {
		Dictionary d;
		d["name"] = info.name;
		d["type"] = info.type;
		d["id"] = info.id;
		d["scene_file_path"] = info.scene_file_path;
		d["view_flags"] = info.view_flags;
		d["children"] = Array();
		return d;
	};

	Vector<StackEntry> stack;
	int parse_idx = 0;

	if (parse_idx >= nodes.size()) {
		return Dictionary();
	}

	StackEntry root_entry;
	root_entry.node_index = parse_idx;
	root_entry.children_remaining = nodes[parse_idx].child_count;
	root_entry.dict = make_dict(nodes[parse_idx]);
	root_entry.children_array = root_entry.dict["children"];
	parse_idx++;

	stack.push_back(root_entry);

	while (parse_idx < nodes.size() && !stack.is_empty()) {
		StackEntry &current = stack.write[stack.size() - 1];

		if (current.children_remaining <= 0) {
			// This node is done. Pop it and add to parent.
			current.dict["children"] = current.children_array;
			Dictionary completed_dict = current.dict;
			stack.resize(stack.size() - 1);

			if (!stack.is_empty()) {
				StackEntry &parent = stack.write[stack.size() - 1];
				parent.children_array.push_back(completed_dict);
				parent.children_remaining--;
			}
			continue;
		}

		// Process next child.
		if (parse_idx >= nodes.size()) {
			break;
		}

		StackEntry child_entry;
		child_entry.node_index = parse_idx;
		child_entry.children_remaining = nodes[parse_idx].child_count;
		child_entry.dict = make_dict(nodes[parse_idx]);
		child_entry.children_array = child_entry.dict["children"];
		parse_idx++;

		if (child_entry.children_remaining > 0) {
			// This child has its own children. Push it onto the stack.
			stack.push_back(child_entry);
		} else {
			// Leaf node. Add directly to current node.
			child_entry.dict["children"] = child_entry.children_array;
			current.children_array.push_back(child_entry.dict);
			current.children_remaining--;
		}
	}

	// Unwind remaining stack.
	while (stack.size() > 1) {
		StackEntry &current = stack.write[stack.size() - 1];
		current.dict["children"] = current.children_array;
		Dictionary completed_dict = current.dict;
		stack.resize(stack.size() - 1);

		StackEntry &parent = stack.write[stack.size() - 1];
		parent.children_array.push_back(completed_dict);
		parent.children_remaining--;
	}

	if (!stack.is_empty()) {
		stack.write[0].dict["children"] = stack[0].children_array;
		return stack[0].dict;
	}

	return Dictionary();
}

Dictionary MCPDebuggerBridge::_flat_tree_to_hierarchical_browse(const Array &p_flat_data) const {
	// Extended flat data from mcp:get_scene_tree_browse is 8 fields per node:
	//   [child_count, name, type_name, id, scene_file_path, view_flags, has_script, group_count]

	if (p_flat_data.is_empty()) {
		return Dictionary();
	}

	struct NodeInfo {
		String name;
		String type;
		uint64_t id = 0;
		String scene_file_path;
		int view_flags = 0;
		bool has_script = false;
		int group_count = 0;
		int child_count = 0;
	};

	// Parse all nodes from flat array (8 fields per node).
	Vector<NodeInfo> nodes;
	int idx = 0;
	while (idx + 7 < p_flat_data.size()) {
		NodeInfo info;
		info.child_count = p_flat_data[idx];
		info.name = p_flat_data[idx + 1];
		info.type = p_flat_data[idx + 2];
		info.id = (uint64_t)(int64_t)p_flat_data[idx + 3];
		info.scene_file_path = p_flat_data[idx + 4];
		info.view_flags = p_flat_data[idx + 5];
		info.has_script = (bool)p_flat_data[idx + 6];
		info.group_count = (int)p_flat_data[idx + 7];
		nodes.push_back(info);
		idx += 8;
	}

	if (nodes.is_empty()) {
		return Dictionary();
	}

	// Build hierarchical structure using an explicit stack.
	struct StackEntry {
		int node_index;
		int children_remaining;
		Dictionary dict;
		Array children_array;
	};

	auto make_dict = [](const NodeInfo &info) -> Dictionary {
		Dictionary d;
		d["name"] = info.name;
		d["type"] = info.type;
		d["id"] = info.id;
		d["scene_file_path"] = info.scene_file_path;
		d["view_flags"] = info.view_flags;
		d["has_script"] = info.has_script;
		d["group_count"] = info.group_count;
		d["children"] = Array();
		return d;
	};

	Vector<StackEntry> stack;
	int parse_idx = 0;

	if (parse_idx >= nodes.size()) {
		return Dictionary();
	}

	StackEntry root_entry;
	root_entry.node_index = parse_idx;
	root_entry.children_remaining = nodes[parse_idx].child_count;
	root_entry.dict = make_dict(nodes[parse_idx]);
	root_entry.children_array = root_entry.dict["children"];
	parse_idx++;

	stack.push_back(root_entry);

	while (parse_idx < nodes.size() && !stack.is_empty()) {
		StackEntry &current = stack.write[stack.size() - 1];

		if (current.children_remaining <= 0) {
			current.dict["children"] = current.children_array;
			Dictionary completed_dict = current.dict;
			stack.resize(stack.size() - 1);

			if (!stack.is_empty()) {
				StackEntry &parent = stack.write[stack.size() - 1];
				parent.children_array.push_back(completed_dict);
				parent.children_remaining--;
			}
			continue;
		}

		if (parse_idx >= nodes.size()) {
			break;
		}

		StackEntry child_entry;
		child_entry.node_index = parse_idx;
		child_entry.children_remaining = nodes[parse_idx].child_count;
		child_entry.dict = make_dict(nodes[parse_idx]);
		child_entry.children_array = child_entry.dict["children"];
		parse_idx++;

		if (child_entry.children_remaining > 0) {
			stack.push_back(child_entry);
		} else {
			child_entry.dict["children"] = child_entry.children_array;
			current.children_array.push_back(child_entry.dict);
			current.children_remaining--;
		}
	}

	// Unwind remaining stack.
	while (stack.size() > 1) {
		StackEntry &current = stack.write[stack.size() - 1];
		current.dict["children"] = current.children_array;
		Dictionary completed_dict = current.dict;
		stack.resize(stack.size() - 1);

		StackEntry &parent = stack.write[stack.size() - 1];
		parent.children_array.push_back(completed_dict);
		parent.children_remaining--;
	}

	if (!stack.is_empty()) {
		stack.write[0].dict["children"] = stack[0].children_array;
		return stack[0].dict;
	}

	return Dictionary();
}

String MCPDebuggerBridge::_tree_to_text(const Dictionary &p_tree, int p_indent, int p_max_depth) const {
	if (p_tree.is_empty()) {
		return "[empty tree]";
	}

	// Guard against pathologically deep scene trees (stack overflow prevention).
	if (p_indent > p_max_depth) {
		return "";
	}

	String result;
	String indent_str;
	for (int i = 0; i < p_indent; i++) {
		indent_str += "  ";
	}

	String name = p_tree.get("name", "?");
	String type = p_tree.get("type", "?");
	String scene_path = p_tree.get("scene_file_path", "");

	result += indent_str + name + " (" + type + ")";
	if (!scene_path.is_empty()) {
		result += " [" + scene_path + "]";
	}
	result += "\n";

	Array children = p_tree.get("children", Array());
	for (int i = 0; i < children.size(); i++) {
		Dictionary child = children[i];
		result += _tree_to_text(child, p_indent + 1, p_max_depth);
	}

	return result;
}
