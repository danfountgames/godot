/**************************************************************************/
/*  mcp_debugger_bridge.h                                                 */
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

#pragma once

#include "editor/debugger/editor_debugger_plugin.h"

#include "editor/mcp_status_data.h"

#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"

// ---------------------------------------------------------------------------
// OutputRingBuffer -- cursor-based ring buffer for output/error messages.
//
// Entries are never lost from the reader's perspective (unless the buffer
// wraps past the reader's cursor). Clients read by providing a cursor
// (sequence number) from their previous read. Returns only entries with
// seq > cursor.
// ---------------------------------------------------------------------------

struct OutputEntry {
	uint64_t seq = 0; // Monotonically increasing sequence number.
	String text;
	int type = 0; // 0 = normal, 1 = warning, 2 = error.
	uint64_t timestamp_msec = 0;
};

class OutputRingBuffer {
public:
	static const int CAPACITY = 10000;

private:
	Vector<OutputEntry> entries;
	uint64_t next_seq = 1;
	int write_pos = 0;
	int count = 0;
	mutable Mutex mutex;

public:
	OutputRingBuffer();

	void push(const String &p_text, int p_type);
	Vector<OutputEntry> read_since(uint64_t p_cursor, int p_limit = 200) const;
	uint64_t latest_seq() const;
	void clear();
};

// ---------------------------------------------------------------------------
// PendingRequest -- async request/response pattern.
//
// MCP HTTP threads call bridge methods that block on a semaphore.
// The editor main thread receives the response and posts the semaphore.
// ---------------------------------------------------------------------------

struct PendingRequest {
	String type;
	Semaphore semaphore;
	Dictionary result;
	SafeFlag completed;

	PendingRequest() :
			completed(false) {}
};

// ---------------------------------------------------------------------------
// MCPDebuggerBridge -- the EditorDebuggerPlugin that manages all
// communication between the MCP server and the running game.
// ---------------------------------------------------------------------------

class MCPDebuggerBridge : public EditorDebuggerPlugin {
	GDCLASS(MCPDebuggerBridge, EditorDebuggerPlugin);

private:
	// --- State ---
	SafeFlag game_running;
	SafeFlag game_launching; // Set when run_project/run_scene called, cleared on session started/stopped.
	SafeNumeric<uint64_t> game_launch_time_msec; // When set_game_launching() was called.
	SafeFlag game_paused; // Set when debugger signals pause, cleared on resume or session_stopped.
	SafeNumeric<uint64_t> game_start_time_msec; // Set in _on_session_started().
	SafeNumeric<int64_t> game_frame_count; // Incremented by game-side heartbeat.
	mutable Mutex stop_reason_mutex;
	String last_stop_reason = "not_started"; // Why the game last stopped. Guarded by stop_reason_mutex.
	SafeNumeric<int> active_session_id; // Initialized to -1 in constructor.
	SafeFlag suspend_on_start; // If set, send suspend immediately when session starts.

	// --- Ring Buffers ---
	OutputRingBuffer output_buffer;
	OutputRingBuffer error_buffer;

	// --- Pending Requests ---
	mutable Mutex request_mutex;
	HashMap<String, PendingRequest *> pending_requests;

	// --- Cached Scene Tree ---
	mutable Mutex scene_tree_mutex;
	Dictionary cached_scene_tree;
	uint64_t scene_tree_timestamp = 0;

	// --- Cached Browse Tree (extended: includes has_script, group_count per node) ---
	mutable Mutex browse_tree_mutex;
	Dictionary cached_browse_tree;
	uint64_t browse_tree_timestamp = 0;

	// --- Cached Break State ---
	mutable Mutex break_state_mutex;

	struct BreakState {
		bool paused = false;
		bool can_debug = false;
		String reason;
		bool has_stackdump = false;

		struct StackFrame {
			int frame_index = 0;
			String file;
			String function;
			int line = 0;
		};
		Vector<StackFrame> stack;

		int inspected_frame = -1;
		struct Variable {
			String name;
			String value;
			String type_name;
			int category = 0; // 0=local, 1=member, 2=global
		};
		Vector<Variable> variables;
		int expected_var_count = 0;

		void clear() {
			paused = false;
			can_debug = false;
			reason = "";
			has_stackdump = false;
			stack.clear();
			inspected_frame = -1;
			variables.clear();
			expected_var_count = 0;
		}
	};
	BreakState cached_break_state;
	Semaphore break_state_ready;

	// --- Test Runner State ---
	struct TestRunState {
		Array results; // Accumulated per-method result dictionaries.
		bool complete = false;
		int run_number = 0;
	};
	TestRunState test_run_state;
	MCPTestEventBuffer test_event_buffer;
	SafeNumeric<int> next_test_run_number;

	// --- Internal Helpers ---
	PendingRequest *_create_pending(const String &p_type);
	Dictionary _wait_for_pending(PendingRequest *p_request, int p_timeout_msec = 10000);
	void _complete_pending(const String &p_request_id, const Dictionary &p_result);
	void _wake_all_pending(const String &p_error_message);

	// --- Signal Callbacks (editor main thread) ---
	void _on_session_started();
	void _on_session_stopped();
	void _on_output_received(const String &p_msg, int p_type);
	void _on_breaked(bool p_reallydid, bool p_can_debug, const String &p_reason, bool p_has_stackdump);
	void _on_stack_dump(const Array &p_stack_dump);
	void _on_stack_frame_vars(int p_num_vars);
	void _on_stack_frame_var(const Array &p_data);

	// --- Scene Tree Helpers ---
	Dictionary _flat_tree_to_hierarchical(const Array &p_flat_data) const;
	Dictionary _flat_tree_to_hierarchical_browse(const Array &p_flat_data) const;
	String _tree_to_text(const Dictionary &p_tree, int p_indent = 0, int p_max_depth = 200) const;

protected:
	static void _bind_methods();

public:
	// --- EditorDebuggerPlugin overrides ---
	virtual void setup_session(int p_idx) override;
	virtual bool capture(const String &p_message, const Array &p_data, int p_session) override;
	virtual bool has_capture(const String &p_capture) const override;

	// --- State Queries (thread-safe) ---
	bool is_game_running() const;
	bool is_game_launching() const;
	bool is_game_paused() const;
	double get_game_uptime_seconds() const;
	int64_t get_game_frame_count() const;

	// Returns why the game last stopped. Values:
	// "not_started" - no game session has ever been started
	// "normal"      - game exited (runtime/stop, user closed window, or get_tree().quit())
	// "timeout"     - launching timed out (15s without session connect, likely compile error)
	String get_last_stop_reason() const;

	// Called by debug tool handlers BEFORE call_deferred to EditorRunBar.
	// If p_suspend_on_start is true, the game will be immediately suspended
	// when the debugger session connects.
	void set_game_launching(bool p_suspend_on_start = false);

	// --- Output/Error Access (thread-safe, called from MCP HTTP threads) ---
	Vector<OutputEntry> get_output_since(uint64_t p_cursor, int p_limit = 200) const;
	Vector<OutputEntry> get_errors_since(uint64_t p_cursor, int p_limit = 200) const;
	uint64_t get_output_latest_seq() const;
	uint64_t get_error_latest_seq() const;
	void clear_output_buffer();
	void clear_error_buffer();

	// --- Async Request Methods (block calling thread, called from MCP HTTP threads) ---
	Dictionary request_scene_tree(int p_timeout_msec = 10000);
	Dictionary request_browse_scene_tree(int p_timeout_msec = 10000);
	Dictionary send_evaluate(const String &p_expression, int p_timeout_msec = 10000);
	Dictionary send_execute_code(const String &p_code, int p_timeout_msec = 10000);
	Dictionary send_debug_command(const String &p_command, const Array &p_data, int p_timeout_msec = 10000);
	Dictionary send_inject_action(const String &p_action, bool p_pressed, int p_hold_frames = 0, float p_strength = 1.0f, int p_timeout_msec = 10000);
	Dictionary send_click_control(const String &p_node_path, int p_timeout_msec = 10000);
	Dictionary send_wait_frames(int p_frame_count, int p_timeout_msec = 30000);
	Dictionary send_screenshot(int p_timeout_msec = 10000);
	Dictionary send_get_performance(int p_timeout_msec = 5000);
	Dictionary send_ui_interact(const String &p_action, const String &p_node_path,
			const Dictionary &p_params, int p_timeout_msec = 10000);
	Dictionary send_get_node_signals(const String &p_node_path, bool p_include_inherited = true,
			const String &p_signal_name = String(), int p_timeout_msec = 10000);
	Dictionary send_emit_signal(const String &p_node_path, const String &p_signal_name,
			const Array &p_args = Array(), int p_timeout_msec = 10000);
	Dictionary send_set_node_property(const String &p_node_path, const String &p_property,
			const Variant &p_value, const String &p_field = String(), int p_timeout_msec = 10000);

	// --- Input Simulation Methods (block calling thread, called from MCP HTTP threads) ---
	Dictionary send_inject_key(const String &p_key_name, bool p_pressed, int p_hold_frames, int p_modifier_flags, bool p_echo, int p_timeout_msec = 10000);
	Dictionary send_inject_joypad_button(const String &p_button_name, bool p_pressed, int p_hold_frames, int p_device, int p_timeout_msec = 10000);
	Dictionary send_inject_joypad_axis(const String &p_axis_name, float p_value, int p_hold_frames, int p_device, int p_timeout_msec = 10000);
	Dictionary send_type_text(const String &p_text, int p_interval_frames, int p_timeout_msec = 30000);
	Dictionary send_input_sequence(const Array &p_steps, int p_timeout_msec = 60000);
	Dictionary send_get_held_inputs(bool p_release_all, int p_timeout_msec = 10000);

	// --- Break State (thread-safe, called from MCP HTTP threads) ---
	Dictionary get_break_state_snapshot() const;
	Dictionary request_frame_variables(int p_frame, int p_timeout_msec = 10000);
	void wait_for_rebreak(int p_timeout_msec = 2000);

	// --- Breakpoint Management (deferred to main thread) ---
	Dictionary get_all_breakpoints(int p_timeout_msec = 5000);
	void _do_get_breakpoints(const String &p_request_id);

	// --- Time Control (fire-and-forget, called from MCP HTTP threads) ---
	// These send scene: messages via call_deferred and return immediately.
	void send_set_time_scale(double p_scale);
	void send_suspend(bool p_enabled);
	void send_next_frame();
	void send_advance_frames(int p_count, bool p_instant);
	void send_set_debug_pause_enabled(bool p_enabled);
	void send_set_debug_pause_tag_enabled(const String &p_tag, bool p_enabled);
	void send_clear_debug_pause_hits();

	// --- Cached Scene Tree (thread-safe) ---
	Dictionary get_cached_scene_tree() const;
	Dictionary get_cached_browse_tree() const;

	// --- Status panel queries (thread-safe) ---
	int get_pending_request_count() const;

	// --- Pending Request Access (called from tool handlers on MCP HTTP threads) ---
	PendingRequest *create_pending(const String &p_type) { return _create_pending(p_type); }
	Dictionary wait_for_pending(PendingRequest *p_request, int p_timeout_msec = 10000) { return _wait_for_pending(p_request, p_timeout_msec); }

	// --- Test Runner (called from MCP HTTP threads and tool handlers) ---
	void reset_test_run_state();
	MCPTestEventBuffer *get_test_event_buffer() { return &test_event_buffer; }
	int get_current_test_run_number() const { return test_run_state.run_number; }
	void push_test_compile_error(const String &p_file, const Array &p_errors, int p_run_number);

	static MCPDebuggerBridge *get_singleton() { return singleton; }

	MCPDebuggerBridge();
	~MCPDebuggerBridge();

private:
	static MCPDebuggerBridge *singleton;
};
