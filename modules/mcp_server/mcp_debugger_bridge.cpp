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
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"

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
	write_pos = 0;
	count = 0;
	next_seq = 1;
}

// ========================================================================
// MCPDebuggerBridge Implementation
// ========================================================================

MCPDebuggerBridge::MCPDebuggerBridge() {
	game_running.clear();
	game_launching.clear();
	game_paused.clear();
	active_session_id.set(-1);
}

MCPDebuggerBridge::~MCPDebuggerBridge() {
	_wake_all_pending("Bridge destroyed");
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

	// --- eval_result ---
	if (sub_msg == "eval_result") {
		ERR_FAIL_COND_V(p_data.size() < 2, false);
		Dictionary result;
		result["success"] = (bool)p_data[0];
		result["value"] = p_data[1];
		_complete_pending("evaluate", result);
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
	game_start_time_msec.set(Time::get_singleton()->get_ticks_msec());
	game_frame_count.set(0);
	output_buffer.clear();
	error_buffer.clear();

	// Clear cached scene tree.
	{
		MutexLock lock(scene_tree_mutex);
		cached_scene_tree = Dictionary();
		scene_tree_timestamp = 0;
	}

	print_verbose("[MCP] Debugger bridge: game session started.");

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

	game_running.clear();
	game_launching.clear();
	game_paused.clear();
	active_session_id.set(-1);

	// Wake ALL pending requests with an error -- the game is gone.
	_wake_all_pending("Game session ended");

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

void MCPDebuggerBridge::set_game_launching() {
	game_launching.set();
	game_launch_time_msec.set(Time::get_singleton()->get_ticks_msec());
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

Dictionary MCPDebuggerBridge::send_evaluate(const String &p_expression, int p_timeout_msec) {
	MCP_BRIDGE_CHECK_RUNNING();

	Array data;
	data.push_back(p_expression);

	MCP_BRIDGE_SEND_OR_FAIL("evaluate", "mcp:evaluate", data);
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

#undef MCP_BRIDGE_CHECK_RUNNING
#undef MCP_BRIDGE_SEND_OR_FAIL

// ------------------------------------------------------------------------
// Cached Scene Tree
// ------------------------------------------------------------------------

Dictionary MCPDebuggerBridge::get_cached_scene_tree() const {
	MutexLock lock(scene_tree_mutex);
	return cached_scene_tree;
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

String MCPDebuggerBridge::_tree_to_text(const Dictionary &p_tree, int p_indent) const {
	if (p_tree.is_empty()) {
		return "[empty tree]";
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
		result += _tree_to_text(child, p_indent + 1);
	}

	return result;
}
