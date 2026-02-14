/**************************************************************************/
/*  mcp_protocol.cpp                                                      */
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

#include "mcp_protocol.h"

#include "core/object/script_language.h"

#include "mcp_debugger_bridge.h"
#include "mcp_progress.h"
#include "mcp_tool_registry.h"
#include "mcp_types.h"
#include "tools/mcp_analysis_tools.h"
#include "tools/mcp_automation_tools.h"
#include "tools/mcp_breakpoint_tools.h"
#include "tools/mcp_debug_tools.h"
#include "tools/mcp_doc_tools.h"
#include "tools/mcp_editor_tools.h"
#include "tools/mcp_export_tools.h"
#include "tools/mcp_input_tools.h"
#include "tools/mcp_memory_tools.h"
#include "tools/mcp_scene_tools.h"
#include "tools/mcp_shader_tools.h"
#include "tools/mcp_signal_tools.h"
#include "tools/mcp_testing_tools.h"
#include "tools/mcp_timing_tools.h"
#include "tools/mcp_ui_tools.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/input/input_event.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/keyboard.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/version.h"

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

MCPProtocol *MCPProtocol::singleton = nullptr;

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

MCPProtocol::MCPProtocol() {
	singleton = this;
	server.instantiate();

	// Populate the origin allowlist.
	allowed_origin_prefixes.push_back("vscode-file://");
	allowed_origin_prefixes.push_back("file://");
	allowed_origin_prefixes.push_back("http://localhost");
	allowed_origin_prefixes.push_back("https://localhost");
	allowed_origin_prefixes.push_back("http://127.0.0.1");
	allowed_origin_prefixes.push_back("https://127.0.0.1");
	allowed_origin_prefixes.push_back("http://[::1]");
	allowed_origin_prefixes.push_back("https://[::1]");

	// Register tools/list + tools/call as JSON-RPC methods.
	// process_action() in the base JSONRPC class will dispatch to these
	// callables automatically.
	// We use wrapper methods on MCPProtocol because callable_mp requires
	// an Object-derived target (MCPToolRegistry is a plain class).
	set_method("tools/list", callable_mp(this, &MCPProtocol::_handle_tools_list));
	set_method("tools/call", callable_mp(this, &MCPProtocol::_handle_tools_call));

	// Register all tools into the registry.
	MCPEditorTools::register_tools(&tool_registry);
	MCPDebugTools::register_tools(&tool_registry);
	MCPAutomationTools::register_tools(&tool_registry);
	MCPInputTools::register_tools(&tool_registry);
	MCPUITools::register_tools(&tool_registry);
	MCPBreakpointTools::register_tools(&tool_registry);
	MCPSignalTools::register_tools(&tool_registry);
	MCPTestingTools::register_tools(&tool_registry);
	MCPDocTools::register_tools(&tool_registry);
	MCPSceneTools::register_tools(&tool_registry);
	MCPTimingTools::register_tools(&tool_registry);
	MCPExportTools::register_tools(&tool_registry);
	MCPMemoryTools::register_tools(&tool_registry);
	MCPAnalysisTools::register_tools(&tool_registry);
	MCPShaderTools::register_tools(&tool_registry);

	// Resource methods (Phase 5).
	set_method("resources/list",
			callable_mp(this, &MCPProtocol::handle_resources_list));
	set_method("resources/read",
			callable_mp(this, &MCPProtocol::handle_resources_read));
	set_method("resources/templates/list",
			callable_mp(this, &MCPProtocol::handle_resources_templates_list));
	set_method("resources/subscribe",
			callable_mp(this, &MCPProtocol::handle_resources_subscribe));
	set_method("resources/unsubscribe",
			callable_mp(this, &MCPProtocol::handle_resources_unsubscribe));

	// Logging (Phase C).
	set_method("logging/setLevel",
			callable_mp(this, &MCPProtocol::handle_logging_set_level));

	// Register all resources into the registry.
	_register_project_resources();
	_register_game_resources();
	_register_resource_templates();
}

MCPProtocol::~MCPProtocol() {
	singleton = nullptr;
	stop();
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

void MCPProtocol::_bind_methods() {
	// MCPProtocol is registered in ClassDB so GDCLASS works.
	// We don't expose methods to GDScript for now -- all interaction
	// is via the HTTP/JSON-RPC protocol.
}

// ---------------------------------------------------------------------------
// Server Lifecycle
// ---------------------------------------------------------------------------

Error MCPProtocol::start(int p_port, const IPAddress &p_bind_ip) {
	server_start_time_usec = OS::get_singleton()->get_ticks_usec();
	listen_address = String(p_bind_ip);
	listen_port = p_port;
	return server->listen(p_port, p_bind_ip);
}

void MCPProtocol::stop() {
	for (const KeyValue<int, Ref<MCPSession>> &E : clients) {
		Ref<MCPSession> session = E.value;
		if (session.is_valid() && session->connection.is_valid()) {
			session->connection->disconnect_from_host();
		}
	}
	clients.clear();
	sessions.clear();
	server->stop();
	server_start_time_usec = 0;
}

void MCPProtocol::set_session_timeout(int p_seconds) {
	session_timeout_sec = p_seconds;
}

void MCPProtocol::set_max_clients(int p_max) {
	max_clients = p_max;
}

// ---------------------------------------------------------------------------
// Session ID Generation (CSPRNG)
// ---------------------------------------------------------------------------

String MCPProtocol::generate_session_id() {
	uint8_t bytes[32];
	Error err = OS::get_singleton()->get_entropy(bytes, 32);

	if (err != OK) {
		CryptoCore::RandomGenerator rng;
		Error rng_err = rng.init();
		if (rng_err == OK) {
			rng_err = rng.get_random_bytes(bytes, 32);
		}
		ERR_FAIL_COND_V_MSG(rng_err != OK, String(),
				"MCP: Failed to generate session ID -- no CSPRNG available.");
	}

	String hex_id;
	for (int i = 0; i < 32; i++) {
		hex_id += String::num_int64(bytes[i], 16).lpad(2, "0");
	}

	return hex_id;
}

// ---------------------------------------------------------------------------
// Host / Origin Validation
// ---------------------------------------------------------------------------

bool MCPProtocol::validate_host(const String &p_host) {
	String host = p_host.get_slice(":", 0);
	return host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "[::1]";
}

bool MCPProtocol::validate_origin(const String &p_origin) {
	if (p_origin.is_empty()) {
		return true;
	}
	for (int i = 0; i < allowed_origin_prefixes.size(); i++) {
		if (p_origin.begins_with(allowed_origin_prefixes[i])) {
			// Prevent subdomain bypass: after the prefix match, the next
			// character must be end-of-string, ':' (port), or '/' (path).
			// This stops "http://localhost.evil.com" from matching "http://localhost".
			int prefix_len = allowed_origin_prefixes[i].length();
			if (p_origin.length() == prefix_len ||
					p_origin[prefix_len] == ':' ||
					p_origin[prefix_len] == '/') {
				return true;
			}
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// Response Helpers
// ---------------------------------------------------------------------------

String MCPProtocol::make_error_body(int p_code, const String &p_message, const Variant &p_id) {
	Dictionary err_dict = make_response_error(p_code, p_message, p_id);
	return JSON::stringify(err_dict);
}

String MCPProtocol::make_result_body(const Dictionary &p_result, const Variant &p_id) {
	Dictionary resp = make_response(p_result, p_id);
	return JSON::stringify(resp);
}

// ---------------------------------------------------------------------------
// Client Lifecycle
// ---------------------------------------------------------------------------

Error MCPProtocol::on_client_connected() {
	Ref<StreamPeerTCP> tcp_peer = server->take_connection();
	ERR_FAIL_COND_V_MSG((int)clients.size() >= max_clients, FAILED,
			"MCP: Max client limit reached (" + itos(max_clients) + ").");

	Ref<MCPSession> session;
	session.instantiate();
	session->connection = tcp_peer;

	int client_id = next_client_id++;
	if (next_client_id < 0) {
		next_client_id = 0; // Handle overflow: wrap back to zero.
	}
	// Skip IDs that are already in use (prevents silent overwrite on wrap-around).
	while (clients.has(client_id)) {
		client_id = next_client_id++;
		if (next_client_id < 0) {
			next_client_id = 0;
		}
	}
	clients.insert(client_id, session);

	print_verbose("[MCP] Client connected (id: " + itos(client_id) + ")");
	return OK;
}

void MCPProtocol::on_client_disconnected(int p_client_id) {
	if (clients.has(p_client_id)) {
		// NOTE: We do NOT remove from `sessions` here.
		// MCP Streamable HTTP sessions persist across TCP connections.
		// A client may open a new TCP connection and reuse the same session ID.
		print_verbose("[MCP] Client disconnected (id: " + itos(p_client_id) + ")");
		clients.erase(p_client_id);
	}
}

// ---------------------------------------------------------------------------
// Session Garbage Collection
// ---------------------------------------------------------------------------

void MCPProtocol::gc_stale_sessions() {
	uint64_t now = OS::get_singleton()->get_ticks_usec();

	// 1. GC idle TCP connections (no activity for connection idle timeout).
	//    SSE streams are exempt -- they stay open as long as their session is alive.
	Vector<int> connections_to_remove;
	for (const KeyValue<int, Ref<MCPSession>> &E : clients) {
		Ref<MCPSession> session = E.value;
		if (!session.is_valid()) {
			connections_to_remove.push_back(E.key);
			continue;
		}
		// SSE streams are long-lived but still need cleanup:
		// - Orphaned streams (session expired/terminated)
		// - Stale streams (TCP went quiet for > 5 minutes, likely dead)
		if (session->is_get_sse_stream()) {
			if (!sessions.has(session->sse_session_id)) {
				connections_to_remove.push_back(E.key);
			} else {
				// Check if the underlying TCP connection is still alive.
				StreamPeerTCP::Status tcp_status = session->connection->get_status();
				if (tcp_status == StreamPeerTCP::STATUS_NONE ||
						tcp_status == StreamPeerTCP::STATUS_ERROR) {
					connections_to_remove.push_back(E.key);
				}
			}
			continue;
		}
		uint64_t elapsed_sec = (now - session->last_activity) / 1000000;
		if (elapsed_sec > MCP_DEFAULT_CONNECTION_IDLE_TIMEOUT_SEC) {
			connections_to_remove.push_back(E.key);
		}
	}
	for (int i = 0; i < connections_to_remove.size(); i++) {
		on_client_disconnected(connections_to_remove[i]);
	}

	// 2. GC stale MCP sessions (no activity for session timeout).
	Vector<String> sessions_to_remove;
	for (const KeyValue<String, MCPSessionState> &E : sessions) {
		uint64_t elapsed_sec = (now - E.value.last_activity) / 1000000;
		if ((int)elapsed_sec > session_timeout_sec) {
			sessions_to_remove.push_back(E.key);
		}
	}
	for (int i = 0; i < sessions_to_remove.size(); i++) {
		print_verbose("[MCP] Session expired: " + sessions_to_remove[i].substr(0, 8) + "...");
		resource_registry.unsubscribe_all(sessions_to_remove[i]);
		sessions.erase(sessions_to_remove[i]);
	}
}

// ---------------------------------------------------------------------------
// Main Poll Loop
// ---------------------------------------------------------------------------

void MCPProtocol::poll() {
	// Accept new connections.
	// Stop accepting when at capacity — leave pending connections in the OS
	// TCP backlog so clients block instead of getting rapid accept-then-reset.
	while (server->is_connection_available() && (int)clients.size() < max_clients) {
		on_client_connected();
	}

	// Process existing clients.
	Vector<int> to_disconnect;

	for (KeyValue<int, Ref<MCPSession>> &E : clients) {
		int client_id = E.key;
		Ref<MCPSession> session = E.value;

		session->connection->poll();
		StreamPeerTCP::Status status = session->connection->get_status();

		if (status == StreamPeerTCP::STATUS_NONE || status == StreamPeerTCP::STATUS_ERROR) {
			to_disconnect.push_back(client_id);
			continue;
		}

		// ── SSE streams (GET or POST): skip request reading, flush events ──
		if (session->is_get_sse_stream() ||
				session->response_mode == MCPSession::RESPONSE_SSE_POST) {
			// GET SSE: keep the parent MCP session alive.
			if (session->is_get_sse_stream() && sessions.has(session->sse_session_id)) {
				sessions[session->sse_session_id].last_activity = OS::get_singleton()->get_ticks_usec();
			}
			// POST SSE: flush thread-safe event queue to res_queue.
			if (session->response_mode == MCPSession::RESPONSE_SSE_POST) {
				session->flush_sse_events();
				session->last_activity = OS::get_singleton()->get_ticks_usec();
			}
			Error err = session->send_data();
			if (err != OK && err != ERR_BUSY) {
				to_disconnect.push_back(client_id);
			}
			continue;
		}

		// ── Standard request processing ──
		Error err = OK;
		while (session->connection->get_available_bytes() > 0) {
			err = session->handle_data();
			if (err != OK && err != ERR_BUSY) {
				break;
			}
			if (session->parse_state == MCPSession::REQUEST_COMPLETE) {
				process_request(client_id);
				// Only reset if we're NOT in SSE mode after dispatch.
				// SSE mode means process_request() started an SSE stream
				// and will reset later via end_sse_stream().
				if (session->response_mode == MCPSession::RESPONSE_NONE) {
					session->reset_request();
				}
			}
			if (err == ERR_BUSY) {
				break;
			}
		}

		if (err != OK && err != ERR_BUSY) {
			to_disconnect.push_back(client_id);
			continue;
		}

		// Send queued responses.
		err = session->send_data();
		if (err != OK && err != ERR_BUSY) {
			to_disconnect.push_back(client_id);
			continue;
		}
		// Close the connection immediately after draining the response queue
		// when Connection: close was sent (standard POST responses).
		if (err == OK && session->close_after_send) {
			to_disconnect.push_back(client_id);
			continue;
		}
	}

	// Deliver queued notifications to SSE streams.
	flush_sse_notifications();

	// Disconnect failed clients.
	for (int i = 0; i < to_disconnect.size(); i++) {
		on_client_disconnected(to_disconnect[i]);
	}

	// Garbage-collect stale sessions.
	gc_stale_sessions();
}

// ---------------------------------------------------------------------------
// HTTP Request Processing
// ---------------------------------------------------------------------------

void MCPProtocol::process_request(int p_client_id) {
	ERR_FAIL_COND(!clients.has(p_client_id));
	Ref<MCPSession> session = clients[p_client_id];
	ERR_FAIL_COND(!session.is_valid());

	uint64_t request_start_usec = OS::get_singleton()->get_ticks_usec();

	// ── Step 1: Extract request data ─────────────────────────────────────

	String host_header = session->headers.has("host") ? session->headers["host"] : String();
	String origin_header = session->headers.has("origin") ? session->headers["origin"] : String();
	String content_type = session->headers.has("content-type") ? session->headers["content-type"] : String();
	String session_id_header = session->headers.has(MCP_SESSION_HEADER) ? session->headers[MCP_SESSION_HEADER] : String();

	// Peer address for event logging.
	String client_ip;
	if (session->connection.is_valid()) {
		client_ip = String(session->connection->get_connected_host());
	}

	// ── Step 2: Validate host and origin ─────────────────────────────────

	// Host header validation (DNS rebinding protection).
	if (host_header.is_empty()) {
		session->queue_response(MCP_HTTP_400,
				make_error_body(JSONRPC::INVALID_REQUEST, "Missing Host header"),
				origin_header);
		return;
	}
	if (!validate_host(host_header)) {
		session->queue_response(MCP_HTTP_403, "", origin_header);
		return;
	}

	// Origin validation (CORS).
	if (!validate_origin(origin_header)) {
		session->queue_response(MCP_HTTP_403, "", String());
		return;
	}

	// ── Step 3: Validate path ────────────────────────────────────────────
	if (session->http_path != "/mcp") {
		session->queue_response(MCP_HTTP_404, "", origin_header);
		return;
	}

	// ── Step 4: Route by HTTP method (OPTIONS / DELETE / GET / POST) ─────
	if (session->http_method == "OPTIONS") {
		session->queue_response(MCP_HTTP_204, "", origin_header);
		return;
	}

	// ── Step 4b: Bearer token authentication ─────────────────────────────
	if (!auth_token.is_empty()) {
		String auth_header = session->headers.has("authorization")
				? session->headers["authorization"]
				: String();
		// Constant-time comparison to prevent timing side-channel attacks.
		String expected = "Bearer " + auth_token;
		bool token_valid = (auth_header.length() == expected.length());
		int diff = 0;
		int len = MIN(auth_header.length(), expected.length());
		for (int i = 0; i < len; i++) {
			diff |= auth_header[i] ^ expected[i];
		}
		token_valid = token_valid && (diff == 0);
		if (auth_header.is_empty() || !token_valid) {
			session->queue_response(MCP_HTTP_401,
					make_error_body(JSONRPC::INVALID_REQUEST, "Unauthorized: invalid or missing Bearer token"),
					origin_header);
			return;
		}
	}

	if (session->http_method == "DELETE") {
		terminate_session(p_client_id, origin_header);
		return;
	}

	if (session->http_method == "GET") {
		// GET /mcp opens an SSE stream for server-initiated notifications.
		handle_get_sse(p_client_id, origin_header);
		return;
	}

	if (session->http_method != "POST") {
		session->queue_response(MCP_HTTP_405, "", origin_header);
		return;
	}

	// ── Step 5: Validate Content-Type (POST only) ────────────────────────
	if (!content_type.begins_with("application/json")) {
		String err_body = make_error_body(JSONRPC::INVALID_REQUEST,
				"Content-Type must be application/json");
		session->queue_response(MCP_HTTP_400, err_body, origin_header);
		return;
	}

	// ── Step 6: Parse JSON body ──────────────────────────────────────────
	JSON json;
	Error json_err = json.parse(session->request_body);

	if (json_err != OK) {
		String err_body = make_error_body(JSONRPC::PARSE_ERROR,
				"JSON parse error: " + json.get_error_message() +
						" at line " + itos(json.get_error_line()));
		session->queue_response(MCP_HTTP_400, err_body, origin_header);
		return;
	}

	Variant json_parsed = json.get_data();

	// Reject batch requests (JSON arrays).
	if (json_parsed.get_type() == Variant::ARRAY) {
		session->queue_response(MCP_HTTP_400,
				make_error_body(JSONRPC::INVALID_REQUEST,
						"Batch requests (JSON arrays) are not supported. "
						"Send one JSON-RPC message per HTTP POST request."),
				origin_header);
		return;
	}

	if (json_parsed.get_type() != Variant::DICTIONARY) {
		String err_body = make_error_body(JSONRPC::PARSE_ERROR,
				"Expected JSON object, got " + Variant::get_type_name(json_parsed.get_type()));
		session->queue_response(MCP_HTTP_400, err_body, origin_header);
		return;
	}

	Dictionary json_request = json_parsed;
	String method = json_request.get("method", "");
	Variant request_id = json_request.get("id", Variant());

	// ── Step 7: Validate MCP-Protocol-Version header ─────────────────────
	if (method != "initialize") {
		String protocol_version_header = session->headers.has("mcp-protocol-version")
				? session->headers["mcp-protocol-version"]
				: String();
		if (!protocol_version_header.is_empty() && protocol_version_header != MCP_PROTOCOL_VERSION) {
			session->queue_response(MCP_HTTP_400,
					make_error_body(-32600,
							"Unsupported MCP-Protocol-Version: " + protocol_version_header +
									". Supported: " + MCP_PROTOCOL_VERSION,
							request_id),
					origin_header);
			return;
		}
	}

	// ── Step 8: Session enforcement (initialize / lookup / validate) ─────
	if (method == "initialize") {
		// Reject if the client already sent a session ID header (re-initialize attempt).
		if (!session_id_header.is_empty()) {
			String err_body = make_error_body(JSONRPC::INVALID_REQUEST,
					"Cannot re-initialize an existing session", request_id);
			session->queue_response(MCP_HTTP_400, err_body, origin_header);
			return;
		}

		Dictionary result = handle_initialize(json_request.get("params", Dictionary()));

		if (result.is_empty() || !result.has("_mcp_session_id")) {
			String err_body = make_error_body(JSONRPC::INTERNAL_ERROR,
					"Failed to generate session ID", request_id);
			session->queue_response(MCP_HTTP_500, err_body, origin_header);
			return;
		}

		String new_session_id = result["_mcp_session_id"];
		result.erase("_mcp_session_id");

		// Create a new MCPSessionState (independent of this TCP connection).
		MCPSessionState new_state;
		new_state.session_id = new_session_id;
		new_state.init_response_sent = true;
		new_state.initialized = false;
		new_state.last_activity = OS::get_singleton()->get_ticks_usec();
		sessions[new_session_id] = new_state;

		HashMap<String, String> extra_headers;
		extra_headers["Mcp-Session-Id"] = new_session_id;

		String body = make_result_body(result, request_id);
		session->queue_response(MCP_HTTP_200, body, origin_header, extra_headers);

		uint64_t duration_usec = OS::get_singleton()->get_ticks_usec() - request_start_usec;
		_emit_request_event("initialize", new_session_id, client_ip, 200,
				duration_usec, session->request_body, body, "");
		return;
	}

	// All other methods require a session ID header.
	if (session_id_header.is_empty()) {
		String err_body = make_error_body(JSONRPC::INVALID_REQUEST,
				"Missing Mcp-Session-Id header", request_id);
		session->queue_response(MCP_HTTP_400, err_body, origin_header);
		return;
	}

	// Look up the session in the protocol-level sessions map.
	if (!sessions.has(session_id_header)) {
		String err_body = make_error_body(JSONRPC::INVALID_REQUEST,
				"Invalid or expired session ID", request_id);
		session->queue_response(MCP_HTTP_404, err_body, origin_header);
		return;
	}

	MCPSessionState &mcp_state = sessions[session_id_header];
	mcp_state.last_activity = OS::get_singleton()->get_ticks_usec();

	// Handle notifications/initialized.
	if (method == "notifications/initialized") {
		if (!mcp_state.init_response_sent) {
			String err_body = make_error_body(JSONRPC::INVALID_REQUEST,
					"Cannot send notifications/initialized before initialize", request_id);
			session->queue_response(MCP_HTTP_400, err_body, origin_header);
			return;
		}
		handle_notifications_initialized(session_id_header);
		session->queue_response(MCP_HTTP_202, "", origin_header);

		uint64_t duration_usec = OS::get_singleton()->get_ticks_usec() - request_start_usec;
		_emit_request_event("notifications/initialized", session_id_header, client_ip, 202,
				duration_usec, session->request_body, "", "");
		return;
	}

	// Handle ping (allowed before initialized).
	if (method == "ping") {
		Dictionary ping_result = handle_ping();
		String body = make_result_body(ping_result, request_id);
		session->queue_response(MCP_HTTP_200, body, origin_header);

		uint64_t duration_usec = OS::get_singleton()->get_ticks_usec() - request_start_usec;
		_emit_request_event("ping", session_id_header, client_ip, 200,
				duration_usec, session->request_body, body, "");
		return;
	}

	// All other methods require initialized session.
	if (!mcp_state.initialized) {
		String err_body = make_error_body(JSONRPC::INVALID_REQUEST,
				"Session not yet initialized. Send notifications/initialized first.", request_id);
		session->queue_response(MCP_HTTP_400, err_body, origin_header);
		return;
	}

	// ── Step 9: JSON-RPC dispatch ────────────────────────────────────────
	// Inject the validated MCP session ID into request params so method
	// handlers (e.g., resources/subscribe) can access the caller's session.
	// The "_mcp_session_id" key is stripped from client-visible schemas.
	if (json_request.has("params") && json_request["params"].get_type() == Variant::DICTIONARY) {
		Dictionary params = json_request["params"];
		params["_mcp_session_id"] = session_id_header;
		json_request["params"] = params;
	} else {
		Dictionary params;
		params["_mcp_session_id"] = session_id_header;
		json_request["params"] = params;
	}

	// ── Step 9a: Handle notifications/cancelled specially ────────────────
	// This is a notification (no "id") that must set the cancelled flag on
	// the in-flight request identified by params.requestId.
	if (method == "notifications/cancelled") {
		Dictionary params = json_request.get("params", Dictionary());
		handle_cancelled(params);
		session->queue_response(MCP_HTTP_202, "", origin_header);
		return;
	}

	// ── Step 9b: SSE negotiation for tools/call ─────────────────────────
	// If the client accepts SSE and the request has a progressToken or the
	// tool is known to be long-running, use POST SSE streaming.
	bool is_notification = (request_id.get_type() == Variant::NIL);
	if (method == "tools/call" && !is_notification) {
		bool accepts_sse = false;
		String accept = session->headers.has("accept") ? session->headers["accept"] : String();
		if (accept.find("text/event-stream") != -1) {
			accepts_sse = true;
		}

		bool has_progress_token = false;
		String progress_token;
		Dictionary params = json_request.get("params", Dictionary());
		if (params.has("_meta")) {
			Variant meta_v = params["_meta"];
			if (meta_v.get_type() == Variant::DICTIONARY) {
				Dictionary meta = meta_v;
				if (meta.has("progressToken")) {
					has_progress_token = true;
					progress_token = String(meta["progressToken"]);
				}
			}
		}

		String tool_name = params.get("name", "");
		bool tool_is_long_running = tool_registry.is_long_running_tool(tool_name);

		if (accepts_sse && (has_progress_token || tool_is_long_running)) {
			// SSE path: stream progress + final result.
			session->begin_sse_response(session_id_header, origin_header);

			dispatch_tool_with_progress(session, json_request, progress_token,
					request_id, origin_header);

			// DO NOT call reset_request() here. end_sse_stream() handles it.
			return;
		}
	}

	// ── Step 9c: Standard discrete dispatch ─────────────────────────────
	// The base JSONRPC class looks up 'method' in the registered callables map.
	// Returns a Dictionary: either {"jsonrpc":"2.0","result":...,"id":...}
	// or {"jsonrpc":"2.0","error":...,"id":...}.
	// Returns Variant::NIL for notifications (no "id").
	Variant result = process_action(json_request);

	// Extract tool name for tools/call events.
	String event_tool_name;
	if (method == "tools/call") {
		Dictionary tc_params = json_request.get("params", Dictionary());
		event_tool_name = tc_params.get("name", "");
	}

	if (result.get_type() == Variant::DICTIONARY) {
		Dictionary result_dict = result;

		// Fix up nested errors: process_action() always wraps handler return
		// values with make_response(), producing {"result": <handler_return>}.
		// When a handler returns {"error": {code, message}} to signal an error,
		// it ends up as {"result": {"error": {...}}} — a successful response
		// with an error buried inside "result". MCP clients won't detect this.
		// Detect this case and convert to a proper JSON-RPC error response.
		bool is_error_response = false;
		if (result_dict.has("result") && result_dict["result"].get_type() == Variant::DICTIONARY) {
			Dictionary inner_result = result_dict["result"];
			if (inner_result.has("error") && inner_result["error"].get_type() == Variant::DICTIONARY) {
				Dictionary inner_error = inner_result["error"];
				int error_code = inner_error.get("code", JSONRPC::INTERNAL_ERROR);
				String error_message = inner_error.get("message", "Unknown error");
				result_dict = make_response_error(error_code, error_message, request_id);
				is_error_response = true;
			}
		}

		String body = JSON::stringify(result_dict);
		session->queue_response(MCP_HTTP_200, body, origin_header);

		// Emit event to status panel.
		uint64_t duration_usec = OS::get_singleton()->get_ticks_usec() - request_start_usec;
		int event_status = is_error_response ? 500 : 200;
		_emit_request_event(method, session_id_header, client_ip, event_status,
				duration_usec, session->request_body, body, event_tool_name);

		// Update tool stats for tools/call.
		if (method == "tools/call" && !event_tool_name.is_empty()) {
			_update_tool_stats(event_tool_name, duration_usec, is_error_response);
		}
	} else if (result.get_type() == Variant::NIL) {
		// Notification — no response expected per JSON-RPC spec.
		// Send 202 Accepted per MCP Streamable HTTP transport.
		session->queue_response(MCP_HTTP_202, "", origin_header);

		uint64_t duration_usec = OS::get_singleton()->get_ticks_usec() - request_start_usec;
		_emit_request_event(method, session_id_header, client_ip, 202,
				duration_usec, session->request_body, "", event_tool_name);
	} else {
		String err_body = make_error_body(JSONRPC::METHOD_NOT_FOUND,
				"Method not found: " + method, request_id);
		session->queue_response(MCP_HTTP_404, err_body, origin_header);

		uint64_t duration_usec = OS::get_singleton()->get_ticks_usec() - request_start_usec;
		_emit_request_event(method, session_id_header, client_ip, 404,
				duration_usec, session->request_body, err_body, event_tool_name);
	}
}

// ---------------------------------------------------------------------------
// MCP Method Handlers
// ---------------------------------------------------------------------------

Dictionary MCPProtocol::handle_initialize(const Dictionary &p_params) {
	String new_session_id = generate_session_id();
	if (new_session_id.is_empty()) {
		return Dictionary();
	}

	// Version negotiation.
	String client_version = p_params.get("protocolVersion", "");
	if (!client_version.is_empty() && client_version != MCP_PROTOCOL_VERSION) {
		print_verbose("[MCP] Client requested protocol version " + client_version +
				", responding with " + MCP_PROTOCOL_VERSION);
	}

	Dictionary result;
	result["protocolVersion"] = MCP_PROTOCOL_VERSION;

	Dictionary capabilities;
	Dictionary tools_cap;
	tools_cap["listChanged"] = true;
	capabilities["tools"] = tools_cap;
	Dictionary resources_cap;
	resources_cap["subscribe"] = true;
	resources_cap["listChanged"] = true;
	capabilities["resources"] = resources_cap;
	Dictionary logging_cap;
	capabilities["logging"] = logging_cap;
	result["capabilities"] = capabilities;

	Dictionary server_info;
	server_info["name"] = "godot-mcp";
	server_info["title"] = "Godot Engine MCP Server";
	server_info["version"] = GODOT_VERSION_FULL_CONFIG;
	server_info["description"] = "Built-in MCP server for the Godot game engine editor. "
								 "Provides tools for project context, file operations, "
								 "GDScript validation, game lifecycle, and live inspection.";
	result["serverInfo"] = server_info;

	result["instructions"] = "Godot Engine MCP server. Provides tools for "
							 "project context, file operations, GDScript validation, game lifecycle, "
							 "live inspection, game automation, and session summaries. "
							 "All file paths use res:// format (Godot's virtual filesystem). "
							 "Tools in runtime/* require a running game (use runtime/run_project first). "
							 "Call tools/list to discover available tools.";

	// Stash session_id in result; process_request() extracts and removes it.
	result["_mcp_session_id"] = new_session_id;

	return result;
}

void MCPProtocol::handle_notifications_initialized(const String &p_session_id) {
	ERR_FAIL_COND(!sessions.has(p_session_id));
	sessions[p_session_id].initialized = true;
	print_verbose("[MCP] Session initialized: " + p_session_id.substr(0, 8) + "...");
}

Dictionary MCPProtocol::handle_ping() {
	return Dictionary();
}

// ---------------------------------------------------------------------------
// Tool Dispatch Wrappers
// ---------------------------------------------------------------------------

Dictionary MCPProtocol::_handle_tools_list(const Dictionary &p_params) {
	return tool_registry.list_tools(p_params);
}

Dictionary MCPProtocol::_handle_tools_call(const Dictionary &p_params) {
	return tool_registry.call_tool(p_params);
}

// ---------------------------------------------------------------------------
// Session Termination (DELETE /mcp)
// ---------------------------------------------------------------------------

void MCPProtocol::terminate_session(int p_client_id, const String &p_origin) {
	ERR_FAIL_COND(!clients.has(p_client_id));
	Ref<MCPSession> session = clients[p_client_id];

	String session_id_header = session->headers.has(MCP_SESSION_HEADER)
			? session->headers[MCP_SESSION_HEADER]
			: String();

	if (session_id_header.is_empty()) {
		session->queue_response(MCP_HTTP_400, "", p_origin);
		return;
	}

	if (!sessions.has(session_id_header)) {
		session->queue_response(MCP_HTTP_404, "", p_origin);
		return;
	}

	// Close any GET SSE streams associated with this session.
	for (KeyValue<int, Ref<MCPSession>> &E : clients) {
		if (E.value.is_valid() && E.value->is_get_sse_stream() &&
				E.value->sse_session_id == session_id_header) {
			E.value->response_mode = MCPSession::RESPONSE_NONE;
			// The SSE stream will be disconnected on next poll cycle
			// when it fails to send or the client detects closure.
		}
	}

	// Clean up resource subscriptions for this session.
	resource_registry.unsubscribe_all(session_id_header);

	// Remove the session from the protocol-level sessions map.
	sessions.erase(session_id_header);

	session->queue_response(MCP_HTTP_204, "", p_origin);

	print_verbose("[MCP] Session terminated: " + session_id_header.substr(0, 8) + "...");
}

// ---------------------------------------------------------------------------
// GET /mcp -- SSE Stream for Server-Initiated Notifications
// ---------------------------------------------------------------------------

void MCPProtocol::handle_get_sse(int p_client_id, const String &p_origin) {
	ERR_FAIL_COND(!clients.has(p_client_id));
	Ref<MCPSession> session = clients[p_client_id];

	String session_id_header = session->headers.has(MCP_SESSION_HEADER)
			? session->headers[MCP_SESSION_HEADER]
			: String();

	// Session ID is required for GET /mcp.
	if (session_id_header.is_empty()) {
		session->queue_response(MCP_HTTP_400,
				make_error_body(JSONRPC::INVALID_REQUEST,
						"Missing Mcp-Session-Id header for GET /mcp"),
				p_origin);
		return;
	}

	// Session must exist and be initialized.
	if (!sessions.has(session_id_header)) {
		session->queue_response(MCP_HTTP_404,
				make_error_body(JSONRPC::INVALID_REQUEST,
						"Invalid or expired session ID"),
				p_origin);
		return;
	}

	MCPSessionState &mcp_state = sessions[session_id_header];
	if (!mcp_state.initialized) {
		session->queue_response(MCP_HTTP_400,
				make_error_body(JSONRPC::INVALID_REQUEST,
						"Session not yet initialized. Send notifications/initialized first."),
				p_origin);
		return;
	}

	// Validate Accept header (should be text/event-stream).
	String accept = session->headers.has("accept") ? session->headers["accept"] : String();
	if (!accept.is_empty() && accept.find("text/event-stream") == -1) {
		session->queue_response(MCP_HTTP_400,
				make_error_body(JSONRPC::INVALID_REQUEST,
						"GET /mcp requires Accept: text/event-stream"),
				p_origin);
		return;
	}

	// Close any existing GET SSE streams for this session before opening a new
	// one.  Without this, reconnecting clients accumulate orphaned streams that
	// hold client slots indefinitely (SSE streams are exempt from idle GC).
	Vector<int> old_streams;
	for (const KeyValue<int, Ref<MCPSession>> &E : clients) {
		if (E.key != p_client_id && E.value.is_valid() &&
				E.value->is_get_sse_stream() &&
				E.value->sse_session_id == session_id_header) {
			old_streams.push_back(E.key);
		}
	}
	for (int i = 0; i < old_streams.size(); i++) {
		print_verbose("[MCP] Closing superseded SSE stream (client " + itos(old_streams[i]) + ") for session: " + session_id_header.substr(0, 8) + "...");
		on_client_disconnected(old_streams[i]);
	}

	// Begin the SSE stream. This sends the HTTP response headers and marks
	// this TCP connection as a long-lived SSE stream.
	session->begin_sse_stream(session_id_header, p_origin);
	mcp_state.last_activity = OS::get_singleton()->get_ticks_usec();

	print_verbose("[MCP] SSE stream opened for session: " + session_id_header.substr(0, 8) + "...");
}

// ---------------------------------------------------------------------------
// SSE Notification Delivery
// ---------------------------------------------------------------------------

void MCPProtocol::flush_sse_notifications() {
	MutexLock lock(notification_mutex);
	// Step 1: Pull resource change notifications from the registry and convert
	// them to JSON-RPC "notifications/resources/updated" messages, queued into
	// each session's notification_queue.
	for (KeyValue<String, MCPSessionState> &E : sessions) {
		MCPSessionState &state = E.value;
		if (!state.initialized) {
			continue;
		}

		Vector<String> changed_uris = resource_registry.flush_notifications(state.session_id);
		for (int i = 0; i < changed_uris.size(); i++) {
			// MCP spec: notifications/resources/updated
			Dictionary notification;
			notification["jsonrpc"] = "2.0";
			notification["method"] = "notifications/resources/updated";
			Dictionary params;
			params["uri"] = changed_uris[i];
			notification["params"] = params;

			String json_str = JSON::stringify(notification);
			if (state.notification_queue.size() < MCP_MAX_EVENT_QUEUE_SIZE) {
				state.notification_queue.push_back(json_str);
			}
		}
	}

	// Step 2: Deliver all queued notifications to ONE SSE stream per session.
	// MCP Streamable HTTP spec: server MUST send each notification on only
	// one SSE stream per session. We pick the first matching GET SSE stream.
	for (KeyValue<String, MCPSessionState> &E : sessions) {
		MCPSessionState &state = E.value;
		if (state.notification_queue.is_empty()) {
			continue;
		}

		// Find the first GET SSE stream client for this session.
		for (KeyValue<int, Ref<MCPSession>> &C : clients) {
			Ref<MCPSession> client = C.value;
			if (!client.is_valid() || !client->is_get_sse_stream()) {
				continue;
			}
			if (client->sse_session_id != state.session_id) {
				continue;
			}

			// Deliver all queued notifications to this SSE stream.
			for (int i = 0; i < state.notification_queue.size(); i++) {
				client->queue_sse_event(state.notification_queue[i]);
			}
			break; // One stream per session (spec requirement).
		}

		// Clear the queue after delivery (even if no stream was found,
		// to prevent unbounded accumulation).
		state.notification_queue.clear();
	}
}

void MCPProtocol::queue_notification_all(const String &p_json_rpc_message) {
	MutexLock lock(notification_mutex);
	for (KeyValue<String, MCPSessionState> &E : sessions) {
		if (E.value.initialized) {
			if (E.value.notification_queue.size() < MCP_MAX_EVENT_QUEUE_SIZE) {
				E.value.notification_queue.push_back(p_json_rpc_message);
			}
		}
	}
}

void MCPProtocol::queue_notification(const String &p_session_id, const String &p_json_rpc_message) {
	MutexLock lock(notification_mutex);
	if (sessions.has(p_session_id) && sessions[p_session_id].initialized) {
		if (sessions[p_session_id].notification_queue.size() < MCP_MAX_EVENT_QUEUE_SIZE) {
			sessions[p_session_id].notification_queue.push_back(p_json_rpc_message);
		}
	}
}

// ---------------------------------------------------------------------------
// Resource Dispatch Wrappers (Phase 5)
// ---------------------------------------------------------------------------

Dictionary MCPProtocol::handle_resources_list(const Dictionary &p_params) {
	bool game_running = debugger_bridge && debugger_bridge->is_game_running();
	return resource_registry.handle_list(game_running);
}

Dictionary MCPProtocol::handle_resources_read(const Dictionary &p_params) {
	String uri = p_params.get("uri", "");
	if (uri.is_empty()) {
		Dictionary err;
		err["code"] = INVALID_PARAMS;
		err["message"] = "Missing required parameter: uri";
		Dictionary response;
		response["error"] = err;
		return response;
	}
	bool game_running = debugger_bridge && debugger_bridge->is_game_running();
	return resource_registry.handle_read(uri, game_running);
}

Dictionary MCPProtocol::handle_resources_templates_list(const Dictionary &p_params) {
	return resource_registry.handle_templates_list();
}

Dictionary MCPProtocol::handle_resources_subscribe(const Dictionary &p_params) {
	String uri = p_params.get("uri", "");
	String session_id = p_params.get("_mcp_session_id", "");

	if (uri.is_empty()) {
		Dictionary err;
		err["code"] = INVALID_PARAMS;
		err["message"] = "Missing required parameter: uri";
		Dictionary response;
		response["error"] = err;
		return response;
	}
	if (session_id.is_empty()) {
		Dictionary err;
		err["code"] = INTERNAL_ERROR;
		err["message"] = "Internal error: missing session context";
		Dictionary response;
		response["error"] = err;
		return response;
	}

	return resource_registry.handle_subscribe(uri, session_id);
}

Dictionary MCPProtocol::handle_resources_unsubscribe(const Dictionary &p_params) {
	String uri = p_params.get("uri", "");
	String session_id = p_params.get("_mcp_session_id", "");

	if (uri.is_empty()) {
		Dictionary err;
		err["code"] = INVALID_PARAMS;
		err["message"] = "Missing required parameter: uri";
		Dictionary response;
		response["error"] = err;
		return response;
	}
	if (session_id.is_empty()) {
		Dictionary err;
		err["code"] = INTERNAL_ERROR;
		err["message"] = "Internal error: missing session context";
		Dictionary response;
		response["error"] = err;
		return response;
	}

	return resource_registry.handle_unsubscribe(uri, session_id);
}

// ---------------------------------------------------------------------------
// Resource Registration (Phase 5)
// ---------------------------------------------------------------------------

void MCPProtocol::_register_project_resources() {
	MCPResourceRegistry::ResourceDef def;

	def.uri = "godot://project/info";
	def.name = "Project Info";
	def.description = "Project name, Godot version, main scene, renderer, autoloads";
	def.mime_type = "application/json";
	def.handler = callable_mp(this, &MCPProtocol::_read_project_info);
	def.requires_game = false;
	def.subscribable = false;
	resource_registry.register_resource(def);

	def.uri = "godot://project/settings";
	def.name = "Project Settings";
	def.description = "Full project.godot file contents as INI-style text";
	def.mime_type = "text/plain";
	def.handler = callable_mp(this, &MCPProtocol::_read_project_settings);
	def.requires_game = false;
	def.subscribable = false;
	resource_registry.register_resource(def);

	def.uri = "godot://project/file-tree";
	def.name = "Project File Tree";
	def.description = "Recursive directory listing of the project with file sizes";
	def.mime_type = "application/json";
	def.handler = callable_mp(this, &MCPProtocol::_read_file_tree);
	def.requires_game = false;
	def.subscribable = false;
	resource_registry.register_resource(def);

	def.uri = "godot://project/input-map";
	def.name = "Input Map";
	def.description = "All input actions with their event bindings (keys, mouse, joypad)";
	def.mime_type = "application/json";
	def.handler = callable_mp(this, &MCPProtocol::_read_input_map);
	def.requires_game = false;
	def.subscribable = false;
	resource_registry.register_resource(def);
}

void MCPProtocol::_register_game_resources() {
	MCPResourceRegistry::ResourceDef def;

	def.uri = "godot://game/status";
	def.name = "Game Status";
	def.description = "Current game state: stopped, launching, running, or paused";
	def.mime_type = "application/json";
	def.handler = callable_mp(this, &MCPProtocol::_read_game_status);
	def.requires_game = false; // Always available (returns "stopped" when not running).
	def.subscribable = true;
	resource_registry.register_resource(def);

	def.uri = "godot://game/scene-tree";
	def.name = "Game Scene Tree";
	def.description = "Full scene tree hierarchy from the running game with node names, types, and IDs";
	def.mime_type = "application/json";
	def.handler = callable_mp(this, &MCPProtocol::_read_game_scene_tree);
	def.requires_game = true;
	def.subscribable = true;
	resource_registry.register_resource(def);

	def.uri = "godot://game/output";
	def.name = "Game Output";
	def.description = "Latest 200 lines of print/log output from the running game";
	def.mime_type = "text/plain";
	def.handler = callable_mp(this, &MCPProtocol::_read_game_output);
	def.requires_game = true;
	def.subscribable = true;
	resource_registry.register_resource(def);

	def.uri = "godot://game/errors";
	def.name = "Runtime Errors";
	def.description = "Latest 50 runtime errors with source file, line, message, and stack trace";
	def.mime_type = "application/json";
	def.handler = callable_mp(this, &MCPProtocol::_read_game_errors);
	def.requires_game = true;
	def.subscribable = true;
	resource_registry.register_resource(def);
}

void MCPProtocol::_register_resource_templates() {
	MCPResourceRegistry::ResourceTemplateDef tmpl;

	tmpl.uri_template = "godot://file/{path}";
	tmpl.name = "Project File";
	tmpl.description = "Read any file from the project by its path (without res:// prefix). "
					   "Example: godot://file/scripts/player.gd reads res://scripts/player.gd";
	tmpl.mime_type = "text/plain";
	tmpl.handler = callable_mp(this, &MCPProtocol::_read_file_resource);
	resource_registry.register_template(tmpl);

	tmpl.uri_template = "godot://game/node/{node_id}/properties";
	tmpl.name = "Node Properties";
	tmpl.description = "Get all properties of a scene tree node by its object ID. "
					   "The node_id comes from the scene tree resource or the runtime/get_scene_tree tool. "
					   "Requires a running game.";
	tmpl.mime_type = "application/json";
	tmpl.handler = callable_mp(this, &MCPProtocol::_read_node_properties_resource);
	resource_registry.register_template(tmpl);
}

// ---------------------------------------------------------------------------
// Resource Content Handlers (Phase 5)
// ---------------------------------------------------------------------------

Dictionary MCPProtocol::_read_project_info() {
	Dictionary info;
	info["project_name"] = ProjectSettings::get_singleton()->get_setting(
			"application/config/name");
	info["godot_version"] = Engine::get_singleton()->get_version_info()["string"];
	info["main_scene"] = ProjectSettings::get_singleton()->get_setting(
			"application/run/main_scene");

	// Renderer.
	String renderer = ProjectSettings::get_singleton()->get_setting(
			"rendering/renderer/rendering_method");
	info["renderer"] = renderer;

	// Autoloads.
	Array autoloads;
	HashMap<StringName, ProjectSettings::AutoloadInfo> autoload_map =
			ProjectSettings::get_singleton()->get_autoload_list();
	for (const KeyValue<StringName, ProjectSettings::AutoloadInfo> &E : autoload_map) {
		Dictionary al;
		al["name"] = String(E.key);
		al["path"] = E.value.path;
		autoloads.push_back(al);
	}
	info["autoloads"] = autoloads;

	// Features.
	PackedStringArray features = ProjectSettings::get_singleton()->get_setting(
			"application/config/features");
	Array features_arr;
	for (int i = 0; i < features.size(); i++) {
		features_arr.push_back(features[i]);
	}
	info["features"] = features_arr;

	Dictionary item;
	item["text"] = JSON::stringify(info);
	return item;
}

Dictionary MCPProtocol::_read_project_settings() {
	String content = FileAccess::get_file_as_string("res://project.godot");

	Dictionary item;
	item["text"] = content;
	return item;
}

Dictionary MCPProtocol::_read_file_tree() {
	Dictionary tree;
	tree["root"] = "res://";
	Array directories;
	int total_files = 0;
	int total_dirs = 0;

	_walk_directory("res://", directories, total_files, total_dirs);

	tree["directories"] = directories;
	tree["total_files"] = total_files;
	tree["total_directories"] = total_dirs;

	Dictionary item;
	item["text"] = JSON::stringify(tree);
	return item;
}

void MCPProtocol::_walk_directory(const String &p_path, Array &r_dirs,
		int &r_total_files, int &r_total_dirs, int p_depth) {
	if (p_depth > 20) {
		return; // Max depth.
	}
	if (r_total_files > 10000) {
		return; // Max files.
	}

	Ref<DirAccess> dir = DirAccess::open(p_path);
	ERR_FAIL_COND(dir.is_null());

	Dictionary dir_entry;
	dir_entry["path"] = p_path;
	Array files;

	dir->list_dir_begin();
	String item_name = dir->get_next();
	Vector<String> subdirs;

	while (!item_name.is_empty()) {
		// Skip hidden/generated directories.
		if (item_name == "." || item_name == ".." || is_skip_directory(item_name)) {
			item_name = dir->get_next();
			continue;
		}

		String full_path = p_path.path_join(item_name);

		if (dir->current_is_dir()) {
			subdirs.push_back(full_path);
		} else {
			Dictionary file_entry;
			file_entry["name"] = item_name;
			Ref<FileAccess> fa = FileAccess::open(full_path, FileAccess::READ);
			file_entry["size"] = fa.is_valid() ? (int64_t)fa->get_length() : 0;
			files.push_back(file_entry);
			r_total_files++;
		}

		item_name = dir->get_next();
	}
	dir->list_dir_end();

	dir_entry["files"] = files;
	r_dirs.push_back(dir_entry);
	r_total_dirs++;

	// Recurse into subdirectories (sorted for deterministic output).
	subdirs.sort();
	for (const String &subdir : subdirs) {
		_walk_directory(subdir, r_dirs, r_total_files, r_total_dirs, p_depth + 1);
	}
}

Dictionary MCPProtocol::_read_input_map() {
	Array actions;
	List<PropertyInfo> plist;
	ProjectSettings::get_singleton()->get_property_list(&plist);

	for (const PropertyInfo &pi : plist) {
		if (!pi.name.begins_with("input/")) {
			continue;
		}

		String action_name = pi.name.substr(6); // Strip "input/".
		Dictionary action;
		action["name"] = action_name;

		Dictionary action_data = ProjectSettings::get_singleton()->get_setting(pi.name);
		action["deadzone"] = action_data.get("deadzone", 0.5);

		Array events_out;
		Array events_in = action_data.get("events", Array());
		for (int i = 0; i < events_in.size(); i++) {
			Ref<InputEvent> event = events_in[i];
			if (event.is_null()) {
				continue;
			}

			Dictionary ev;

			Ref<InputEventKey> key = event;
			if (key.is_valid()) {
				ev["type"] = "key";
				ev["physical_keycode"] = keycode_get_string(
						key->get_physical_keycode());
				events_out.push_back(ev);
				continue;
			}

			Ref<InputEventJoypadButton> btn = event;
			if (btn.is_valid()) {
				ev["type"] = "joypad_button";
				ev["button_index"] = (int)btn->get_button_index();
				events_out.push_back(ev);
				continue;
			}

			Ref<InputEventJoypadMotion> motion = event;
			if (motion.is_valid()) {
				ev["type"] = "joypad_motion";
				ev["axis"] = (int)motion->get_axis();
				ev["axis_value"] = motion->get_axis_value();
				events_out.push_back(ev);
				continue;
			}

			Ref<InputEventMouseButton> mouse = event;
			if (mouse.is_valid()) {
				ev["type"] = "mouse_button";
				ev["button_index"] = (int)mouse->get_button_index();
				events_out.push_back(ev);
				continue;
			}
		}

		action["events"] = events_out;
		actions.push_back(action);
	}

	Dictionary result;
	result["actions"] = actions;

	Dictionary item;
	item["text"] = JSON::stringify(result);
	return item;
}

Dictionary MCPProtocol::_read_game_status() {
	Dictionary status;

	if (debugger_bridge) {
		if (debugger_bridge->is_game_launching()) {
			status["state"] = "launching";
		} else if (debugger_bridge->is_game_running()) {
			if (debugger_bridge->is_game_paused()) {
				status["state"] = "paused";
			} else {
				status["state"] = "running";
			}
			status["uptime_seconds"] = debugger_bridge->get_game_uptime_seconds();
			status["frame_count"] = debugger_bridge->get_game_frame_count();

			// Try to get scene path from cached tree.
			Dictionary cached_tree = debugger_bridge->get_cached_scene_tree();
			if (!cached_tree.is_empty()) {
				Array children = cached_tree.get("children", Array());
				if (children.size() > 0) {
					Dictionary first_child = children[0];
					status["scene"] = first_child.get("scene_file_path", "");
				}
			}
		} else {
			status["state"] = "stopped";
			status["stop_reason"] = debugger_bridge->get_last_stop_reason();
		}
	} else {
		status["state"] = "stopped";
	}

	if (!status.has("uptime_seconds")) {
		status["uptime_seconds"] = 0;
	}
	if (!status.has("scene")) {
		status["scene"] = "";
	}

	Dictionary item;
	item["text"] = JSON::stringify(status);
	return item;
}

Dictionary MCPProtocol::_read_game_scene_tree() {
	ERR_FAIL_NULL_V(debugger_bridge, Dictionary());

	// Prefer cached tree for fast reads (non-blocking).
	// The resource is for passive context. Clients that need a guaranteed
	// fresh tree should use the runtime/get_scene_tree tool instead.
	Dictionary tree = debugger_bridge->get_cached_scene_tree();
	if (tree.is_empty() || !tree.has("node_count")) {
		// No cached tree -- request a fresh one (blocks up to 5s).
		tree = debugger_bridge->request_scene_tree(5000);
	}

	Dictionary item;
	item["text"] = JSON::stringify(tree);
	return item;
}

Dictionary MCPProtocol::_read_game_output() {
	ERR_FAIL_NULL_V(debugger_bridge, Dictionary());

	// Read the latest 200 lines WITHOUT advancing the cursor.
	// Passing cursor=0 returns the most recent entries.
	Vector<OutputEntry> entries = debugger_bridge->get_output_since(0, 200);

	String text;
	for (const OutputEntry &e : entries) {
		text += e.text;
		if (!e.text.ends_with("\n")) {
			text += "\n";
		}
	}

	Dictionary item;
	item["text"] = text;
	return item;
}

Dictionary MCPProtocol::_read_game_errors() {
	ERR_FAIL_NULL_V(debugger_bridge, Dictionary());

	// Read the latest 50 errors WITHOUT advancing the cursor.
	Vector<OutputEntry> entries = debugger_bridge->get_errors_since(0, 50);

	Array errors_arr;
	for (const OutputEntry &e : entries) {
		Dictionary err;
		err["seq"] = (int64_t)e.seq;
		err["text"] = e.text;
		err["is_warning"] = (e.type == 1);
		err["timestamp_msec"] = (int64_t)e.timestamp_msec;
		errors_arr.push_back(err);
	}

	Dictionary result;
	result["errors"] = errors_arr;
	result["count"] = errors_arr.size();

	Dictionary item;
	item["text"] = JSON::stringify(result);
	return item;
}

Dictionary MCPProtocol::_read_file_resource(const Dictionary &p_params) {
	String path = p_params.get("path", "");

	// Path traversal protection.

	// Rule 1: Reject empty paths.
	if (path.is_empty()) {
		return make_resource_error(-32602, "Invalid path: path is empty");
	}

	// Rule 2: Reject directory traversal sequences.
	if (path.find("..") != -1) {
		return make_resource_error(-32602,
				"Invalid path: path traversal detected in '" + path + "'");
	}

	// Rule 3: Reject null bytes (path truncation attack).
	// Also reject U+FFFD — Godot's JSON parser converts raw \x00 to U+FFFD.
	for (int i = 0; i < path.length(); i++) {
		if (path[i] == '\0' || path[i] == 0xFFFD) {
			return make_resource_error(-32602, "Invalid path: null byte in path");
		}
	}

	// Rule 4: Reject URL-encoded traversal sequences.
	String path_lower = path.to_lower();
	if (path_lower.find("%2e") != -1 || path_lower.find("%2f") != -1 || path_lower.find("%00") != -1) {
		return make_resource_error(-32602,
				"Invalid path: URL-encoded traversal detected in '" + path + "'");
	}

	// Rule 5: Reject absolute paths (must be relative within project).
	if (path.begins_with("/") || path.find("://") != -1) {
		return make_resource_error(-32602,
				"Invalid path: must be a relative project path, got '" + path + "'");
	}

	// Rule 6: Construct res:// path and validate via validate_path().
	String res_path = "res://" + path;
	if (!validate_path(res_path)) {
		return make_resource_error(-32602,
				"Invalid path: '" + path + "' is outside the project or targets a restricted directory");
	}

	// Read the file.
	if (!FileAccess::exists(res_path)) {
		return make_resource_error(-32602, "File not found: " + res_path);
	}

	String content = FileAccess::get_file_as_string(res_path);

	Dictionary item;
	item["text"] = content;
	return item;
}

Dictionary MCPProtocol::_read_node_properties_resource(const Dictionary &p_params) {
	String node_id_str = p_params.get("node_id", "");

	if (node_id_str.is_empty() || !node_id_str.is_valid_int()) {
		return make_resource_error(-32602, "Invalid node_id: must be a numeric object ID");
	}

	if (!debugger_bridge || !debugger_bridge->is_game_running()) {
		return make_resource_error(MCP_ERROR_RESOURCE_UNAVAILABLE,
				"Game is not running. Start the game first with runtime/run_project.");
	}

	// Use send_evaluate() to get node properties via an expression.
	// This constructs an expression that gets the node by object ID and
	// retrieves its property list.
	// Sanitize: convert to int64 and back to string to guarantee only digits,
	// preventing any expression injection even if is_valid_int() were bypassed.
	int64_t id_val = node_id_str.to_int();
	String safe_id = itos(id_val);
	String expression = "var n = instance_from_id(" + safe_id + "); "
						"n.get_class() if n != null else \"<not found>\"";

	Dictionary eval_result = debugger_bridge->send_evaluate(expression);

	if (!(bool)eval_result.get("success", false)) {
		return make_resource_error(MCP_ERROR_RESOURCE_UNAVAILABLE,
				"Failed to inspect node " + node_id_str + ": " +
						String(eval_result.get("value", eval_result.get("error", "Unknown error"))));
	}

	// Build a simple properties response.
	Dictionary props;
	props["object_id"] = node_id_str.to_int();
	props["class"] = eval_result.get("value", "Unknown");
	props["note"] = "Use runtime/get_node_properties tool for full property inspection.";

	Dictionary item;
	item["text"] = JSON::stringify(props);
	return item;
}

// ---------------------------------------------------------------------------
// SSE-Aware Tool Dispatch (Phase A)
// ---------------------------------------------------------------------------

// RAII guard to ensure active_requests cleanup on all code paths
// (including ERR_FAIL early returns from tool dispatch).
struct ActiveRequestGuard {
	Mutex &mutex;
	HashMap<String, ProgressContext *> &map;
	String key;

	ActiveRequestGuard(Mutex &p_mutex, HashMap<String, ProgressContext *> &p_map,
			const String &p_key, ProgressContext *p_ctx) :
			mutex(p_mutex), map(p_map), key(p_key) {
		MutexLock lock(mutex);
		map[key] = p_ctx;
	}

	~ActiveRequestGuard() {
		MutexLock lock(mutex);
		map.erase(key);
	}
};

void MCPProtocol::dispatch_tool_with_progress(
		Ref<MCPSession> p_session,
		const Dictionary &p_msg,
		const String &p_progress_token,
		const Variant &p_request_id,
		const String &p_origin) {
	// NOTE: This executes synchronously on the poll thread, blocking other
	// clients for the duration of tool execution. This is acceptable for the
	// single-threaded model. A future phase will move tool execution to a
	// worker thread, activating the existing synchronization primitives.

	Dictionary params = p_msg.get("params", Dictionary());
	String tool_name = params.get("name", "");
	Dictionary arguments;
	if (params.has("arguments")) {
		arguments = params["arguments"];
	}

	// Create progress context (stack-allocated; valid for tool execution duration).
	ProgressContext ctx;
	ctx.token = p_progress_token;
	ctx.session = p_session.ptr();

	// Register in active requests map (for cancellation lookup).
	// RAII guard ensures cleanup even if an ERR_FAIL macro triggers early return.
	String request_key = String(p_request_id);
	ActiveRequestGuard guard(active_requests_mutex, active_requests, request_key, &ctx);

	// Log the tool invocation.
	send_log("notice", "mcp.tools", "Calling " + tool_name);

	// Execute the tool (blocks until complete or cancelled).
	// The tool calls ctx.report_progress() to send SSE progress events.
	// The tool checks ctx.is_cancelled() at natural loop boundaries.
	Dictionary tool_result = tool_registry.call_tool_with_progress(
			tool_name, arguments, &ctx);

	// Build the final JSON-RPC response.
	// Check if tool_result is actually an error from the registry (e.g., unknown
	// tool, missing params). These return {"error": {"code":..., "message":...}}
	// which must become a top-level JSON-RPC error, not be wrapped under "result".
	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = p_request_id;

	if (tool_result.has("error") && tool_result["error"].get_type() == Variant::DICTIONARY) {
		Dictionary inner_error = tool_result["error"];
		if (inner_error.has("code") && inner_error.has("message")) {
			response["error"] = inner_error;
		} else {
			response["result"] = tool_result;
		}
	} else {
		response["result"] = tool_result;
	}

	String response_body = JSON::stringify(response);

	// Send as the last SSE event (via thread-safe queue).
	p_session->queue_sse_event_threadsafe(response_body);

	// Flush all remaining events to the TCP send buffer, then close the stream.
	p_session->end_sse_stream();

	// Emit event for the status panel.
	{
		uint64_t now_usec = OS::get_singleton()->get_ticks_usec();
		// We don't have direct access to request_start_usec here (it's in process_request),
		// so compute duration from the start of dispatch_tool_with_progress.
		// For a more accurate duration, compute from the session's request_start_time.
		uint64_t duration_usec = (p_session->request_start_time > 0)
				? (now_usec - p_session->request_start_time)
				: 0;

		bool is_error = response.has("error");
		int status_code = is_error ? 500 : 200;

		String session_id_short;
		if (!p_session->sse_session_id.is_empty()) {
			session_id_short = p_session->sse_session_id.substr(0, 8);
		}

		String peer_ip;
		if (p_session->connection.is_valid()) {
			peer_ip = String(p_session->connection->get_connected_host());
		}

		bool capture_json = panel_visible.is_set();
		String req_json = capture_json ? p_session->request_body.left(4096) : String();
		String res_json = capture_json ? response_body.left(4096) : String();

		_emit_request_event("tools/call", session_id_short, peer_ip, status_code,
				duration_usec, req_json, res_json, tool_name);
		_update_tool_stats(tool_name, duration_usec, is_error);
	}
}

// ---------------------------------------------------------------------------
// Cancellation Handler (Phase B)
// ---------------------------------------------------------------------------

void MCPProtocol::handle_cancelled(const Dictionary &p_params) {
	Variant request_id = p_params.get("requestId", Variant());
	String reason = p_params.get("reason", "");

	if (request_id.get_type() == Variant::NIL) {
		// Invalid notification; ignore per JSON-RPC spec (no error response
		// for notifications).
		return;
	}

	String request_key = String(request_id);
	MutexLock lock(active_requests_mutex);
	ProgressContext **ctx_ptr = active_requests.getptr(request_key);
	if (ctx_ptr && *ctx_ptr) {
		(*ctx_ptr)->cancelled.set();
		print_verbose("[MCP] Request " + request_key +
				" cancelled" + (reason.is_empty() ? "" : ": " + reason));
	}
	// If request_id is not found, the request may have already completed.
	// Silently ignore (per MCP spec: cancellation is best-effort).
}

// ---------------------------------------------------------------------------
// Logging (Phase C)
// ---------------------------------------------------------------------------

int MCPProtocol::severity_from_string(const String &p_level) {
	if (p_level == "emergency") {
		return 0;
	}
	if (p_level == "alert") {
		return 1;
	}
	if (p_level == "critical") {
		return 2;
	}
	if (p_level == "error") {
		return 3;
	}
	if (p_level == "warning") {
		return 4;
	}
	if (p_level == "notice") {
		return 5;
	}
	if (p_level == "info") {
		return 6;
	}
	if (p_level == "debug") {
		return 7;
	}
	return 6; // Default to info for unknown levels.
}

Dictionary MCPProtocol::handle_logging_set_level(const Dictionary &p_params) {
	String level = p_params.get("level", "info");
	int severity = severity_from_string(level);

	// Validate that the level string is recognized.
	if (level != "emergency" && level != "alert" && level != "critical" &&
			level != "error" && level != "warning" && level != "notice" &&
			level != "info" && level != "debug") {
		severity = 6;
		print_verbose("[MCP] Unknown log level '" + level + "', defaulting to 'info'");
	}

	min_log_severity.set(severity);
	print_verbose("[MCP] Log level set to '" + level + "' (severity " + itos(severity) + ")");

	// logging/setLevel is a JSON-RPC request (has an id), not a notification.
	// Return an empty result to acknowledge success.
	return Dictionary();
}

void MCPProtocol::send_log(const String &p_level, const String &p_logger,
		const Variant &p_data) {
	int severity = severity_from_string(p_level);

	// Filter: only send if severity <= min_log_severity.
	// Lower number = higher severity. debug=7, info=6, error=3, etc.
	if (severity > min_log_severity.get()) {
		return;
	}

	Dictionary params;
	params["level"] = p_level;
	params["logger"] = p_logger;
	params["data"] = p_data;

	Dictionary notification;
	notification["jsonrpc"] = "2.0";
	notification["method"] = "notifications/message";
	notification["params"] = params;

	String json = JSON::stringify(notification);

	// Broadcast to all sessions via the notification queue.
	// This is delivered to GET SSE streams on the next flush.
	queue_notification_all(json);
}

// ---------------------------------------------------------------------------
// Server Push Notifications (Phase D)
// ---------------------------------------------------------------------------

void MCPProtocol::notify_tools_changed() {
	Dictionary notification;
	notification["jsonrpc"] = "2.0";
	notification["method"] = "notifications/tools/list_changed";

	String json = JSON::stringify(notification);
	queue_notification_all(json);
}

void MCPProtocol::notify_resources_list_changed() {
	Dictionary notification;
	notification["jsonrpc"] = "2.0";
	notification["method"] = "notifications/resources/list_changed";

	String json = JSON::stringify(notification);
	queue_notification_all(json);
}

// ---------------------------------------------------------------------------
// Status Panel Data Feed
// ---------------------------------------------------------------------------

void MCPProtocol::_emit_request_event(const String &p_method, const String &p_session_id,
		const String &p_client_ip, int p_http_status, uint64_t p_duration_usec,
		const String &p_request_json, const String &p_response_json,
		const String &p_tool_name) {
	MCPRequestEvent evt;
	evt.timestamp_usec = OS::get_singleton()->get_ticks_usec();
	evt.method = p_method;
	evt.session_id = p_session_id.is_empty() ? "-" : p_session_id.substr(0, 8);
	evt.client_ip = p_client_ip;
	evt.http_status = p_http_status;
	evt.is_error = (p_http_status >= 400);
	evt.duration_usec = p_duration_usec;
	evt.tool_name = p_tool_name;

	// Conditional JSON capture: full JSON only when the panel is visible.
	bool capture_json = panel_visible.is_set();
	evt.request_json = capture_json ? p_request_json.left(4096) : String();
	evt.response_json = capture_json ? p_response_json.left(4096) : String();

	event_buffer.push(evt);
}

void MCPProtocol::_update_tool_stats(const String &p_tool_name, uint64_t p_duration_usec, bool p_is_error) {
	MutexLock lock(tool_stats_mutex);

	MCPToolStats &stats = tool_stats[p_tool_name];
	stats.call_count++;
	stats.total_duration_usec += p_duration_usec;
	stats.last_call_time_usec = OS::get_singleton()->get_ticks_usec();
	stats.last_was_error = p_is_error;
}

Vector<MCPClientSnapshot> MCPProtocol::get_client_snapshots() const {
	// NOTE: This is called from the main (UI) thread. The clients HashMap is
	// only modified on the poll thread. We snapshot under no lock because the
	// worst case is a slightly stale read (the panel updates every 500ms anyway).
	// In threaded mode, there is a small race window, but the data is non-critical
	// display data and the HashMap iteration is safe as long as no mutations happen
	// concurrently. This is acceptable per the design doc's threading model.
	Vector<MCPClientSnapshot> result;
	uint64_t now_usec = OS::get_singleton()->get_ticks_usec();

	for (const KeyValue<int, Ref<MCPSession>> &E : clients) {
		Ref<MCPSession> session = E.value;
		if (!session.is_valid()) {
			continue;
		}

		MCPClientSnapshot snap;
		if (!session->sse_session_id.is_empty()) {
			snap.session_id = session->sse_session_id.substr(0, 8) + "...";
		} else {
			// Check the headers for the session ID.
			if (session->headers.has(MCP_SESSION_HEADER)) {
				String sid = session->headers[MCP_SESSION_HEADER];
				snap.session_id = sid.substr(0, 8) + "...";
			} else {
				snap.session_id = "-";
			}
		}

		if (session->connection.is_valid()) {
			snap.peer_address = String(session->connection->get_connected_host()) + ":" +
					itos(session->connection->get_connected_port());
		}

		snap.connected_since = session->request_start_time > 0
				? session->request_start_time
				: now_usec;
		snap.last_activity = session->last_activity;
		snap.is_sse_stream = session->is_get_sse_stream();

		result.push_back(snap);
	}

	return result;
}

HashMap<String, MCPToolStats> MCPProtocol::get_tool_stats() const {
	MutexLock lock(tool_stats_mutex);
	return tool_stats;
}

bool MCPProtocol::is_running() const {
	return server.is_valid() && server->is_listening();
}

String MCPProtocol::get_listen_address() const {
	return listen_address;
}

int MCPProtocol::get_listen_port() const {
	return listen_port;
}

uint64_t MCPProtocol::get_start_time_usec() const {
	return server_start_time_usec;
}

int MCPProtocol::get_session_count() const {
	return sessions.size();
}

int MCPProtocol::get_client_count() const {
	return clients.size();
}

void MCPProtocol::set_panel_visible(bool p_visible) {
	if (p_visible) {
		panel_visible.set();
	} else {
		panel_visible.clear();
	}
}
