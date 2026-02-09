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
#include "mcp_tool_registry.h"
#include "mcp_types.h"
#include "tools/mcp_automation_tools.h"
#include "tools/mcp_debug_tools.h"
#include "tools/mcp_editor_tools.h"
#include "tools/mcp_gdscript_tools.h"

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

	// Create the tool registry and register tools/list + tools/call as
	// JSON-RPC methods. process_action() in the base JSONRPC class will
	// dispatch to these callables automatically.
	// We use wrapper methods on MCPProtocol because callable_mp requires
	// an Object-derived target (MCPToolRegistry is a plain class).
	tool_registry = memnew(MCPToolRegistry);

	set_method("tools/list", callable_mp(this, &MCPProtocol::_handle_tools_list));
	set_method("tools/call", callable_mp(this, &MCPProtocol::_handle_tools_call));

	// Register all tools into the registry.
	MCPEditorTools::register_tools(tool_registry);
	MCPGDScriptTools::register_tools(tool_registry);
	MCPDebugTools::register_tools(tool_registry);
	MCPAutomationTools::register_tools(tool_registry);

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

	// Register all resources into the registry.
	_register_project_resources();
	_register_game_resources();
	_register_resource_templates();
}

MCPProtocol::~MCPProtocol() {
	singleton = nullptr;
	stop();

	if (tool_registry) {
		memdelete(tool_registry);
		tool_registry = nullptr;
	}
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
			return true;
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
		// SSE streams are long-lived; they are only GC'd when their session expires.
		if (session->is_sse_stream) {
			if (!sessions.has(session->sse_session_id)) {
				// Orphaned SSE stream -- session was terminated or expired.
				connections_to_remove.push_back(E.key);
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
		sessions.erase(sessions_to_remove[i]);
	}
}

// ---------------------------------------------------------------------------
// Main Poll Loop
// ---------------------------------------------------------------------------

void MCPProtocol::poll() {
	// Accept new connections.
	while (server->is_connection_available()) {
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

		if (session->is_sse_stream) {
			// SSE streams are long-lived. Don't parse new HTTP requests --
			// just keep sending queued SSE events and check for disconnection.
			// Keep the parent MCP session alive while SSE is connected.
			if (sessions.has(session->sse_session_id)) {
				sessions[session->sse_session_id].last_activity = OS::get_singleton()->get_ticks_usec();
			}
			Error err = session->send_data();
			if (err != OK && err != ERR_BUSY) {
				to_disconnect.push_back(client_id);
			}
			continue;
		}

		// Read incoming data.
		Error err = OK;
		while (session->connection->get_available_bytes() > 0) {
			err = session->handle_data();
			if (err != OK && err != ERR_BUSY) {
				break;
			}
			if (session->parse_state == MCPSession::REQUEST_COMPLETE) {
				process_request(client_id);
				session->reset_request();
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

	String host_header = session->headers.has("host") ? session->headers["host"] : String();
	String origin_header = session->headers.has("origin") ? session->headers["origin"] : String();
	String content_type = session->headers.has("content-type") ? session->headers["content-type"] : String();
	String session_id_header = session->headers.has(MCP_SESSION_HEADER) ? session->headers[MCP_SESSION_HEADER] : String();

	// Step 1: Host header validation (DNS rebinding protection).
	if (!host_header.is_empty() && !validate_host(host_header)) {
		session->queue_response(MCP_HTTP_403, "", origin_header);
		return;
	}

	// Step 2: Origin validation (CORS).
	if (!validate_origin(origin_header)) {
		session->queue_response(MCP_HTTP_403, "", String());
		return;
	}

	// Step 3: Path validation.
	if (session->http_path != "/mcp") {
		session->queue_response(MCP_HTTP_404, "", origin_header);
		return;
	}

	// Step 4: Method routing.
	if (session->http_method == "OPTIONS") {
		session->queue_response(MCP_HTTP_204, "", origin_header);
		return;
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

	// Step 5: Content-Type validation (POST only).
	if (!content_type.begins_with("application/json")) {
		String err_body = make_error_body(JSONRPC::INVALID_REQUEST,
				"Content-Type must be application/json");
		session->queue_response(MCP_HTTP_400, err_body, origin_header);
		return;
	}

	// Step 6: JSON parse.
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

	// Step 6b: MCP-Protocol-Version header validation.
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

	// Step 7: Session enforcement.
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
		return;
	}

	// Handle ping (allowed before initialized).
	if (method == "ping") {
		Dictionary ping_result = handle_ping();
		String body = make_result_body(ping_result, request_id);
		session->queue_response(MCP_HTTP_200, body, origin_header);
		return;
	}

	// All other methods require initialized session.
	if (!mcp_state.initialized) {
		String err_body = make_error_body(JSONRPC::INVALID_REQUEST,
				"Session not yet initialized. Send notifications/initialized first.", request_id);
		session->queue_response(MCP_HTTP_400, err_body, origin_header);
		return;
	}

	// Step 8: JSON-RPC dispatch via process_action().
	// The base JSONRPC class looks up 'method' in the registered callables map.
	// Returns a Dictionary: either {"jsonrpc":"2.0","result":...,"id":...}
	// or {"jsonrpc":"2.0","error":...,"id":...}.
	// Returns Variant::NIL for notifications (no "id").
	Variant result = process_action(json_request);

	if (result.get_type() == Variant::DICTIONARY) {
		Dictionary result_dict = result;
		String body = JSON::stringify(result_dict);
		session->queue_response(MCP_HTTP_200, body, origin_header);
	} else if (result.get_type() == Variant::NIL) {
		// Notification — no response expected per JSON-RPC spec.
		// Send 202 Accepted per MCP Streamable HTTP transport.
		session->queue_response(MCP_HTTP_202, "", origin_header);
	} else {
		String err_body = make_error_body(JSONRPC::METHOD_NOT_FOUND,
				"Method not found: " + method, request_id);
		session->queue_response(MCP_HTTP_404, err_body, origin_header);
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
							 "Tools in debug/* require a running game (use debug/run_project first). "
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
	ERR_FAIL_NULL_V(tool_registry, Dictionary());
	return tool_registry->list_tools(p_params);
}

Dictionary MCPProtocol::_handle_tools_call(const Dictionary &p_params) {
	ERR_FAIL_NULL_V(tool_registry, Dictionary());
	return tool_registry->call_tool(p_params);
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

	// Close any SSE streams associated with this session.
	for (KeyValue<int, Ref<MCPSession>> &E : clients) {
		if (E.value.is_valid() && E.value->is_sse_stream &&
				E.value->sse_session_id == session_id_header) {
			E.value->is_sse_stream = false;
			// The SSE stream will be disconnected on next poll cycle
			// when it fails to send or the client detects closure.
		}
	}

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
	// For each session that has queued notifications, deliver them
	// to all connected SSE streams for that session.
	for (KeyValue<String, MCPSessionState> &E : sessions) {
		MCPSessionState &state = E.value;
		if (state.notification_queue.is_empty()) {
			continue;
		}

		// Find SSE stream clients for this session.
		for (KeyValue<int, Ref<MCPSession>> &C : clients) {
			Ref<MCPSession> client = C.value;
			if (!client.is_valid() || !client->is_sse_stream) {
				continue;
			}
			if (client->sse_session_id != state.session_id) {
				continue;
			}

			// Deliver all queued notifications to this SSE stream.
			for (int i = 0; i < state.notification_queue.size(); i++) {
				client->queue_sse_event(state.notification_queue[i]);
			}
		}

		// Clear the queue after delivery.
		state.notification_queue.clear();
	}
}

void MCPProtocol::queue_notification_all(const String &p_json_rpc_message) {
	for (KeyValue<String, MCPSessionState> &E : sessions) {
		if (E.value.initialized) {
			E.value.notification_queue.push_back(p_json_rpc_message);
		}
	}
}

void MCPProtocol::queue_notification(const String &p_session_id, const String &p_json_rpc_message) {
	if (sessions.has(p_session_id) && sessions[p_session_id].initialized) {
		sessions[p_session_id].notification_queue.push_back(p_json_rpc_message);
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
		return make_response_error(INVALID_PARAMS, "Missing required parameter: uri");
	}
	bool game_running = debugger_bridge && debugger_bridge->is_game_running();
	return resource_registry.handle_read(uri, game_running);
}

Dictionary MCPProtocol::handle_resources_templates_list(const Dictionary &p_params) {
	return resource_registry.handle_templates_list();
}

Dictionary MCPProtocol::handle_resources_subscribe(const Dictionary &p_params) {
	String uri = p_params.get("uri", "");
	if (uri.is_empty()) {
		return make_response_error(INVALID_PARAMS, "Missing required parameter: uri");
	}
	// Subscription tracking is a stub. Pass a placeholder session ID.
	// Full session-aware subscription delivery will be implemented in AGENT_06.
	return resource_registry.handle_subscribe(uri, "stub_session");
}

Dictionary MCPProtocol::handle_resources_unsubscribe(const Dictionary &p_params) {
	String uri = p_params.get("uri", "");
	if (uri.is_empty()) {
		return make_response_error(INVALID_PARAMS, "Missing required parameter: uri");
	}
	return resource_registry.handle_unsubscribe(uri, "stub_session");
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

	def.uri = "godot://game/performance";
	def.name = "Performance Metrics";
	def.description = "FPS, frame time, idle/physics time, memory usage, object counts";
	def.mime_type = "application/json";
	def.handler = callable_mp(this, &MCPProtocol::_read_game_performance);
	def.requires_game = true;
	def.subscribable = false; // Changes every frame, too noisy for subscriptions.
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
					   "The node_id comes from the scene tree resource or the debug/get_scene_tree tool. "
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
		int &r_total_files, int &r_total_dirs) {
	Ref<DirAccess> dir = DirAccess::open(p_path);
	ERR_FAIL_COND(dir.is_null());

	Dictionary dir_entry;
	dir_entry["path"] = p_path;
	Array files;

	dir->list_dir_begin();
	String item_name = dir->get_next();
	Vector<String> subdirs;

	while (!item_name.is_empty()) {
		// Skip hidden directories and Godot internal cache.
		if (item_name == "." || item_name == ".." || item_name.begins_with(".godot")) {
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
		_walk_directory(subdir, r_dirs, r_total_files, r_total_dirs);
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
	// fresh tree should use the debug/get_scene_tree tool instead.
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

Dictionary MCPProtocol::_read_game_performance() {
	ERR_FAIL_NULL_V(debugger_bridge, Dictionary());

	// send_get_performance() is an async round-trip to the running game.
	// It blocks up to 5s.
	Dictionary perf = debugger_bridge->send_get_performance();

	Dictionary item;
	item["text"] = JSON::stringify(perf);
	return item;
}

Dictionary MCPProtocol::_read_file_resource(const Dictionary &p_params) {
	String path = p_params.get("path", "");

	// Path traversal protection.

	// Rule 1: Reject empty paths.
	if (path.is_empty()) {
		Dictionary error;
		error["code"] = -32602;
		error["message"] = "Invalid path: path is empty";
		Dictionary response;
		response["error"] = error;
		return response;
	}

	// Rule 2: Reject directory traversal sequences.
	if (path.find("..") != -1) {
		Dictionary error;
		error["code"] = -32602;
		error["message"] = "Invalid path: path traversal detected in '" + path + "'";
		Dictionary response;
		response["error"] = error;
		return response;
	}

	// Rule 3: Reject null bytes (path truncation attack).
	if (path.find_char('\0') != -1) {
		Dictionary error;
		error["code"] = -32602;
		error["message"] = "Invalid path: null byte in path";
		Dictionary response;
		response["error"] = error;
		return response;
	}

	// Rule 4: Reject absolute paths (must be relative within project).
	if (path.begins_with("/") || path.find("://") != -1) {
		Dictionary error;
		error["code"] = -32602;
		error["message"] = "Invalid path: must be a relative project path, got '" + path + "'";
		Dictionary response;
		response["error"] = error;
		return response;
	}

	// Rule 5: Construct res:// path and validate via validate_path().
	String res_path = "res://" + path;
	if (!validate_path(res_path)) {
		Dictionary error;
		error["code"] = -32602;
		error["message"] = "Invalid path: '" + path + "' is outside the project or targets a restricted directory";
		Dictionary response;
		response["error"] = error;
		return response;
	}

	// Read the file.
	if (!FileAccess::exists(res_path)) {
		Dictionary error;
		error["code"] = -32602;
		error["message"] = "File not found: " + res_path;
		Dictionary response;
		response["error"] = error;
		return response;
	}

	String content = FileAccess::get_file_as_string(res_path);

	Dictionary item;
	item["text"] = content;
	return item;
}

Dictionary MCPProtocol::_read_node_properties_resource(const Dictionary &p_params) {
	String node_id_str = p_params.get("node_id", "");

	if (node_id_str.is_empty() || !node_id_str.is_valid_int()) {
		Dictionary error;
		error["code"] = -32602;
		error["message"] = "Invalid node_id: must be a numeric object ID";
		Dictionary response;
		response["error"] = error;
		return response;
	}

	if (!debugger_bridge || !debugger_bridge->is_game_running()) {
		Dictionary error;
		error["code"] = -32002;
		error["message"] = "Game is not running. Start the game first with debug/run_project.";
		Dictionary response;
		response["error"] = error;
		return response;
	}

	// Use send_evaluate() to get node properties via an expression.
	// This constructs an expression that gets the node by object ID and
	// retrieves its property list.
	String object_id = node_id_str;
	String expression = "var n = instance_from_id(" + object_id + "); "
						"n.get_class() if n != null else \"<not found>\"";

	Dictionary eval_result = debugger_bridge->send_evaluate(expression);

	if (!(bool)eval_result.get("success", false)) {
		Dictionary error;
		error["code"] = -32002;
		error["message"] = "Failed to inspect node " + node_id_str + ": " +
				String(eval_result.get("value", eval_result.get("error", "Unknown error")));
		Dictionary response;
		response["error"] = error;
		return response;
	}

	// Build a simple properties response.
	Dictionary props;
	props["object_id"] = node_id_str.to_int();
	props["class"] = eval_result.get("value", "Unknown");
	props["note"] = "Use debug/get_node_properties tool for full property inspection.";

	Dictionary item;
	item["text"] = JSON::stringify(props);
	return item;
}
