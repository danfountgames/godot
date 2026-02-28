/**************************************************************************/
/*  mcp_protocol.h                                                        */
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

#include "editor/mcp_status_data.h"
#include "mcp_resource_registry.h"
#include "mcp_session.h"
#include "mcp_tool_registry.h"

#include "core/io/tcp_server.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/safe_refcount.h"
#include "modules/jsonrpc/jsonrpc.h"

struct ProgressContext;

class MCPDebuggerBridge;

// MCP session state, independent of TCP connections.
// A client may use multiple TCP connections with the same session ID.
struct MCPSessionState {
	String session_id;
	bool init_response_sent = false;
	bool initialized = false;
	uint64_t last_activity = 0; // Microsecond timestamp.

	// Queued server-initiated notifications (JSON-RPC strings).
	// These are delivered to connected GET SSE streams during poll().
	Vector<String> notification_queue;

	// Per-session agent identity and permissions.
	// agent_id is extracted from the Bearer token (prefix "agent_") during
	// the first request. Used by AgentPanel to toggle runtime tool access.
	String agent_id;
	bool runtime_tools_enabled = true; // When false, runtime/* tools are blocked.
	bool editor_controls_enabled = true; // When false, scene/* editor tools are blocked.
};

class MCPProtocol : public JSONRPC {
	GDCLASS(MCPProtocol, JSONRPC)

private:
	Ref<TCPServer> server;

	// Tool registry (Phase 2).
	MCPToolRegistry tool_registry;

	// Resource registry (Phase 5).
	MCPResourceRegistry resource_registry;

	// Debugger bridge (Phase 3). Owned by MCPServerPlugin; we store a raw
	// pointer for tool handlers to access. Set by MCPServerPlugin after
	// creating the bridge.
	MCPDebuggerBridge *debugger_bridge = nullptr;

	// TCP connections (HTTP-level).
	HashMap<int, Ref<MCPSession>> clients;
	int next_client_id = 0;

	// MCP sessions (protocol-level, independent of TCP connections).
	HashMap<String, MCPSessionState> sessions;

	// Origin allowlist for CORS validation.
	Vector<String> allowed_origin_prefixes;

	// Bearer token for authentication (empty = no auth required).
	// The global token authenticates external clients. Agent tokens
	// (prefix "agent_") are per-AgentPanel and carry an agent_id that
	// maps to per-session permissions.
	String auth_token;
	HashMap<String, bool> agent_tokens; // agent token → accepted (always true).
	HashMap<String, bool> _deferred_runtime_prefs; // agent_id → enabled (set before session exists).
	HashMap<String, bool> _deferred_editor_prefs; // agent_id → enabled (set before session exists).

	// Configuration.
	int session_timeout_sec = MCP_DEFAULT_SESSION_TIMEOUT_SEC;
	int max_clients = MCP_MAX_CLIENTS;

	// Session ID for the current request being processed. Set before any tool
	// registry call (both standard and SSE paths), cleared after. The tool
	// registry's permission callback reads this to check per-session permissions.
	String _current_request_session_id;

	// Permission callback installed on tool_registry. Checks whether the tool
	// is allowed for the session identified by _current_request_session_id.
	static bool _check_tool_permission(const String &p_tool_name, void *p_userdata, String &r_deny_reason);

	// Active request tracking for cancellation (Phase B).
	// Key is String(request_id) to avoid Variant-as-key issues.
	Mutex active_requests_mutex;
	HashMap<String, ProgressContext *> active_requests; // String(request_id) -> ProgressContext*

	// Notification queue mutex: protects `sessions` notification_queue fields
	// from concurrent access by the main thread (debugger bridge callbacks) and
	// the MCP poll thread (flush_sse_notifications).
	Mutex notification_mutex;

	// Logging severity filter (Phase C). RFC 5424: 0=emergency .. 7=debug.
	// Default: 6 (info) -- send levels 0-6, suppress debug.
	SafeNumeric<int> min_log_severity; // Atomic for cross-thread reads.

	// --- Status panel data feed ---
	MCPEventBuffer event_buffer;
	mutable Mutex tool_stats_mutex;
	HashMap<String, MCPToolStats> tool_stats;
	uint64_t server_start_time_usec = 0;
	SafeFlag panel_visible; // Set by panel when it becomes visible.
	String listen_address; // Cached from start() for panel display.
	int listen_port = 0; // Cached from start() for panel display.

	// Internal: emit an event to the event buffer after a request completes.
	void _emit_request_event(const String &p_method, const String &p_session_id,
			const String &p_client_ip, int p_http_status, uint64_t p_duration_usec,
			const String &p_request_json, const String &p_response_json,
			const String &p_tool_name);

	// Internal: update tool stats after a tool call.
	void _update_tool_stats(const String &p_tool_name, uint64_t p_duration_usec, bool p_is_error);

	// Session ID generation using CSPRNG.
	String generate_session_id();

	// HTTP validation.
	bool validate_host(const String &p_host);
	bool validate_origin(const String &p_origin);

	// Request processing.
	void process_request(int p_client_id);

	// MCP method handlers.
	Dictionary handle_initialize(const Dictionary &p_params);
	void handle_notifications_initialized(const String &p_session_id);
	Dictionary handle_ping();

	// Tool dispatch wrappers use _handle_ prefix (legacy convention).
	// Resource dispatch wrappers use handle_ prefix (newer convention).
	// Both are private methods registered via set_method() for JSON-RPC dispatch.

	// Tool dispatch wrappers for set_method() (callable_mp requires Object).
	Dictionary _handle_tools_list(const Dictionary &p_params);
	Dictionary _handle_tools_call(const Dictionary &p_params);

	// SSE-aware tool dispatch: streams progress + final result over POST SSE.
	void dispatch_tool_with_progress(Ref<MCPSession> p_session,
			const Dictionary &p_msg, const String &p_progress_token,
			const Variant &p_request_id, const String &p_origin);

	// Cancellation handler (Phase B).
	void handle_cancelled(const Dictionary &p_params);

	// Logging handler and emitter (Phase C).
	Dictionary handle_logging_set_level(const Dictionary &p_params);
	static int severity_from_string(const String &p_level);

	// Resource dispatch wrappers for set_method().
	Dictionary handle_resources_list(const Dictionary &p_params);
	Dictionary handle_resources_read(const Dictionary &p_params);
	Dictionary handle_resources_templates_list(const Dictionary &p_params);
	Dictionary handle_resources_subscribe(const Dictionary &p_params);
	Dictionary handle_resources_unsubscribe(const Dictionary &p_params);

	// Resource registration helpers.
	void _register_project_resources();
	void _register_game_resources();
	void _register_resource_templates();

	// Resource handler methods (produce content for each resource).
	Dictionary _read_project_info();
	Dictionary _read_project_settings();
	Dictionary _read_file_tree();
	Dictionary _read_input_map();
	Dictionary _read_game_status();
	Dictionary _read_game_scene_tree();
	Dictionary _read_game_output();
	Dictionary _read_game_errors();
	Dictionary _read_debug_events();
	Dictionary _read_file_resource(const Dictionary &p_params);
	Dictionary _read_node_properties_resource(const Dictionary &p_params);

	// File tree helper.
	void _walk_directory(const String &p_path, Array &r_dirs,
			int &r_total_files, int &r_total_dirs, int p_depth = 0);

	// Session termination (DELETE /mcp).
	void terminate_session(int p_client_id, const String &p_origin);

	// GET /mcp handler (opens SSE stream for server-initiated notifications).
	void handle_get_sse(int p_client_id, const String &p_origin);

	// Deliver queued notifications to SSE streams.
	void flush_sse_notifications();

	// Response helpers.
	String make_error_body(int p_code, const String &p_message, const Variant &p_id = Variant());
	String make_result_body(const Dictionary &p_result, const Variant &p_id);

	// Client lifecycle.
	Error on_client_connected();
	void on_client_disconnected(int p_client_id);

	// Garbage collection.
	void gc_stale_sessions();

	// Singleton-like pointer (only one MCPProtocol instance exists).
	static MCPProtocol *singleton;

protected:
	static void _bind_methods();

public:
	MCPProtocol();
	~MCPProtocol();

	static MCPProtocol *get_singleton() { return singleton; }

	void poll();

	Error start(int p_port, const IPAddress &p_bind_ip);
	void stop();

	void set_session_timeout(int p_seconds);
	void set_max_clients(int p_max);
	void set_auth_token(const String &p_token) { auth_token = p_token; }

	// Per-agent token management. AgentPanel registers a unique token
	// before launching Claude Code. The token doubles as the agent_id.
	void register_agent_token(const String &p_agent_token);
	void unregister_agent_token(const String &p_agent_token);

	// Toggle runtime tool access for a specific agent (identified by token).
	// When disabled, tools in runtime/*, automation/*, debug/* (runtime-only
	// subset) are blocked with a clear error message.
	void set_runtime_tools_enabled(const String &p_agent_token, bool p_enabled);
	bool get_runtime_tools_enabled(const String &p_agent_token) const;

	// Returns true if the tool requires a running game and should be gated
	// by the runtime_tools_enabled permission flag.
	static bool is_runtime_tool(const String &p_name);

	// Toggle editor control tool access for a specific agent (identified by token).
	// When disabled, tools in scene/* and editor/execute_script are blocked.
	void set_editor_controls_enabled(const String &p_agent_token, bool p_enabled);
	bool get_editor_controls_enabled(const String &p_agent_token) const;

	// Returns true if the tool modifies the editor's scene tree or navigates
	// editor UI and should be gated by the editor_controls_enabled flag.
	static bool is_editor_control_tool(const String &p_name);

	MCPToolRegistry *get_tool_registry() { return &tool_registry; }
	const MCPToolRegistry *get_tool_registry() const { return &tool_registry; }
	MCPResourceRegistry *get_resource_registry() { return &resource_registry; }
	const MCPResourceRegistry *get_resource_registry() const { return &resource_registry; }

	void set_debugger_bridge(MCPDebuggerBridge *p_bridge) { debugger_bridge = p_bridge; }
	MCPDebuggerBridge *get_debugger_bridge() const { return debugger_bridge; }

	// Queue a server-initiated notification to all active sessions.
	// The notification will be delivered to GET SSE streams on the next poll().
	// p_json_rpc_message is a complete JSON-RPC notification string (no "id").
	void queue_notification_all(const String &p_json_rpc_message);

	// Queue a notification to a specific session.
	void queue_notification(const String &p_session_id, const String &p_json_rpc_message);

	// Server push: notify clients that the tool list has changed (Phase D).
	void notify_tools_changed();

	// Server push: notify clients that the resource list has changed (Phase D).
	void notify_resources_list_changed();

	// Emit a log message to all GET SSE streams (Phase C).
	// Filtered by min_log_severity (set via logging/setLevel).
	void send_log(const String &p_level, const String &p_logger, const Variant &p_data);

	// --- Status panel API ---
	MCPEventBuffer *get_event_buffer() { return &event_buffer; }
	Vector<MCPClientSnapshot> get_client_snapshots() const;
	HashMap<String, MCPToolStats> get_tool_stats() const;
	bool is_running() const;
	String get_listen_address() const;
	int get_listen_port() const;
	uint64_t get_start_time_usec() const;
	int get_session_count() const;
	int get_client_count() const;
	void set_panel_visible(bool p_visible);
};
