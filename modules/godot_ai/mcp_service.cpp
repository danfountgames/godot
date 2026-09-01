/**************************************************************************/
/*  mcp_service.cpp                                                       */
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

#include "mcp_service.h"

#include "mcp_http.h"

#include "mcp_approvals_dialog.h"
#include "mcp_audit.h"
#include "mcp_activity_dock.h"
#ifdef MCP_TERMINAL_ENABLED
#include "terminal/mcp_agent_terminal_panel.h"
#endif
#include "mcp_runtime_bridge.h"
#include "mcp_deferred.h"
#include "mcp_tool_registry.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/version.h"
#include "editor/settings/editor_command_palette.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/settings/editor_settings.h"

// Ports are probed upward from the configured one so several editors can run at
// once; the relay learns the real port from the instance descriptor.
static const int PORT_PROBE_RANGE = 20;
static const int MAX_FRAME_CHARACTERS = 32 * 1024 * 1024;

// One per editor, set in the constructor. Not an Engine singleton: this is an
// EditorPlugin whose lifetime the editor owns, and registering it as one would outlive
// the editor node that frees it.
static MCPService *mcp_service_singleton = nullptr;

MCPService *MCPService::get_singleton() {
	return mcp_service_singleton;
}

MCPService::MCPService() {
	mcp_service_singleton = this;
	_EDITOR_DEF("network/godot_ai/enabled", true);
	_EDITOR_DEF("network/godot_ai/port", configured_port);
	_EDITOR_DEF("network/godot_ai/http_port", 6110);
	_EDITOR_DEF("network/godot_ai/auto_approve_clients", false);
	MCPPermissions::register_editor_settings();
}

MCPService::~MCPService() {
	stop();
	// Leaving this set hands every later get_singleton() a pointer to freed memory -
	// the same defect this fork found and fixed in EditorFileSystem.
	if (mcp_service_singleton == this) {
		mcp_service_singleton = nullptr;
	}
}

String MCPService::get_state_dir() {
	// Shared with godot-ai-relay, which resolves the same location without engine
	// APIs; keep both implementations in step.
	const String override_dir = OS::get_singleton()->get_environment("GODOT_AI_HOME");
	if (!override_dir.is_empty()) {
		return override_dir;
	}
	const String home = OS::get_singleton()->get_environment("HOME");
	if (!home.is_empty()) {
		return home.path_join(".godot-ai");
	}
	return OS::get_singleton()->get_user_data_dir().path_join("godot_ai");
}

String MCPService::get_instances_dir() {
	return get_state_dir().path_join("instances");
}

void MCPService::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_register_editor_commands();
			if (MCPToolRegistry::get_singleton()) {
				MCPToolRegistry::get_singleton()->connect("tools_changed", callable_mp(this, &MCPService::_on_tools_changed));
			}
			start();
		} break;

		case NOTIFICATION_EXIT_TREE: {
			if (MCPToolRegistry::get_singleton() &&
					MCPToolRegistry::get_singleton()->is_connected("tools_changed", callable_mp(this, &MCPService::_on_tools_changed))) {
				MCPToolRegistry::get_singleton()->disconnect("tools_changed", callable_mp(this, &MCPService::_on_tools_changed));
			}
			stop();
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			// Tools run editor operations that can pump the main loop, so re-entrant
			// polling must be prevented (the debug adapter guards the same way).
			if (started && !polling) {
				polling = true;
				_accept_new_peers();
				for (int i = peers.size() - 1; i >= 0; i--) {
					_poll_peer(peers[i]);
				}
				_poll_deferred();
				if (runtime_bridge.is_valid()) {
					runtime_bridge->poll();
				}
				if (profiler_recorder.is_valid()) {
					profiler_recorder->poll();
				}
				for (int i = peers.size() - 1; i >= 0; i--) {
					if (peers[i]->connection.is_null() || peers[i]->connection->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
						_drop_peer(i);
					}
				}
				polling = false;
			}
		} break;

		case EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED: {
			if (!EditorSettings::get_singleton()->check_changed_settings_in_group("network/godot_ai")) {
				break;
			}
			const bool enabled = EDITOR_GET("network/godot_ai/enabled");
			const int new_port = (int)_EDITOR_GET("network/godot_ai/port");
			if (!enabled && started) {
				stop();
			} else if (enabled && !started) {
				start();
			} else if (enabled && started && new_port != configured_port) {
				stop();
				start();
			}
		} break;
	}
}

void MCPService::start() {
	if (started) {
		return;
	}
	if (EditorSettings::get_singleton() && !(bool)EDITOR_GET("network/godot_ai/enabled")) {
		return;
	}

	configured_port = (int)_EDITOR_GET("network/godot_ai/port");
	server.instantiate();

	Error error = ERR_CANT_CREATE;
	for (int offset = 0; offset < PORT_PROBE_RANGE; offset++) {
		error = server->listen(configured_port + offset, IPAddress("127.0.0.1"));
		if (error == OK) {
			port = configured_port + offset;
			break;
		}
	}
	if (error != OK) {
		server.unref();
		ERR_PRINT(vformat("Godot AI: could not open a local port in the range %d-%d; the AI service is disabled.",
				configured_port, configured_port + PORT_PROBE_RANGE - 1));
		return;
	}

	// The Streamable HTTP endpoint, on its own port beside the bridge. This is the
	// transport that needs no relay process: the editor spawned the agent, so the
	// editor serves the agent (DEC-0014). Failure to open it disables HTTP, not the
	// service - the bridge above is already listening.
	const int configured_http_port = (int)_EDITOR_GET("network/godot_ai/http_port");
	http_server.instantiate();
	Error http_error = ERR_CANT_CREATE;
	for (int offset = 0; offset < PORT_PROBE_RANGE; offset++) {
		http_error = http_server->listen(configured_http_port + offset, IPAddress("127.0.0.1"));
		if (http_error == OK) {
			http_port = configured_http_port + offset;
			break;
		}
	}
	if (http_error == OK) {
		http_token = OS::get_singleton()->get_environment("GODOT_AI_HTTP_TOKEN");
		if (http_token.is_empty()) {
			http_token = mcp_http_random_id();
		}
	} else {
		http_server.unref();
		http_port = 0;
		http_token = String();
		WARN_PRINT(vformat("Godot AI: could not open an HTTP port in the range %d-%d; direct HTTP clients are disabled, the bridge still works.",
				configured_http_port, configured_http_port + PORT_PROBE_RANGE - 1));
	}

	started = true;
	set_process_internal(true);
	_write_instance_descriptor();

	if (EditorNode::get_log()) {
		EditorNode::get_log()->add_message(
				vformat("--- Godot AI service listening on 127.0.0.1:%d%s ---", port,
						http_port > 0 ? vformat(" (HTTP %d)", http_port) : String()),
				EditorLog::MSG_TYPE_EDITOR);
	}
}

void MCPService::stop() {
	if (!started && peers.is_empty() && server.is_null()) {
		return;
	}
	for (int i = peers.size() - 1; i >= 0; i--) {
		_drop_peer(i);
	}
	if (server.is_valid()) {
		server->stop();
		server.unref();
	}
	if (http_server.is_valid()) {
		http_server->stop();
		http_server.unref();
	}
	http_sessions.clear();
	http_port = 0;
	http_token = String();
	_remove_instance_descriptor();
	set_process_internal(false);

	if (started && EditorNode::get_log()) {
		EditorNode::get_log()->add_message("--- Godot AI service stopped ---", EditorLog::MSG_TYPE_EDITOR);
	}
	started = false;
	port = 0;
}

void MCPService::_accept_new_peers() {
	if (server.is_valid()) {
		while (server->is_connection_available()) {
			Ref<StreamPeerTCP> connection = server->take_connection();
			if (connection.is_null()) {
				break;
			}
			Peer *peer = memnew(Peer);
			peer->connection = connection;
			peer->address = String(connection->get_connected_host()) + ":" + itos(connection->get_connected_port());
			peers.push_back(peer);
		}
	}
	if (http_server.is_valid()) {
		while (http_server->is_connection_available()) {
			Ref<StreamPeerTCP> connection = http_server->take_connection();
			if (connection.is_null()) {
				break;
			}
			Peer *peer = memnew(Peer);
			peer->connection = connection;
			peer->http = true;
			peer->address = String(connection->get_connected_host()) + ":" + itos(connection->get_connected_port());
			peers.push_back(peer);
		}
	}
}

void MCPService::_poll_peer(Peer *p_peer) {
	Ref<StreamPeerTCP> connection = p_peer->connection;
	if (connection.is_null() || connection->poll() != OK) {
		return;
	}
	if (connection->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
		return;
	}
	if (p_peer->http) {
		_poll_http_peer(p_peer);
		return;
	}

	int available = connection->get_available_bytes();
	while (available > 0) {
		Vector<uint8_t> chunk;
		chunk.resize(available);
		int received = 0;
		if (connection->get_partial_data(chunk.ptrw(), available, received) != OK || received <= 0) {
			break;
		}
		p_peer->buffer += String::utf8((const char *)chunk.ptr(), received);

		int newline = p_peer->buffer.find("\n");
		while (newline >= 0) {
			String line = p_peer->buffer.substr(0, newline);
			p_peer->buffer = p_peer->buffer.substr(newline + 1);
			if (line.ends_with("\r")) {
				line = line.substr(0, line.length() - 1);
			}
			if (!line.strip_edges().is_empty()) {
				_handle_line(p_peer, line);
			}
			newline = p_peer->buffer.find("\n");
		}

		if (p_peer->buffer.length() > MAX_FRAME_CHARACTERS) {
			ERR_PRINT("Godot AI: dropping an oversized frame from a client.");
			p_peer->buffer = String();
		}

		available = connection->get_available_bytes();
	}
}

void MCPService::_handle_line(Peer *p_peer, const String &p_line) {
	const Variant parsed = JSON::parse_string(p_line);
	if (parsed.get_type() != Variant::DICTIONARY) {
		_send(p_peer, MCPProtocol::make_error(Variant(), MCPProtocol::ERROR_PARSE,
									 "the editor could not parse this frame as a JSON-RPC object"));
		return;
	}

	// This editor no longer sends requests to clients (the sampling round trip left
	// with the chat, DEC-0013), so every inbound frame is the client's own; a stray
	// response is handled by the protocol layer's no-method path.

	Dictionary response;
	current_peer = p_peer;
	const bool has_response = MCPProtocol::handle_message(parsed, p_peer->session, this, response);
	current_peer = nullptr;
	if (has_response) {
		_send(p_peer, response);
	}
}

void MCPService::_poll_http_peer(Peer *p_peer) {
	Ref<StreamPeerTCP> connection = p_peer->connection;
	int available = connection->get_available_bytes();
	while (available > 0) {
		Vector<uint8_t> chunk;
		chunk.resize(available);
		int received = 0;
		if (connection->get_partial_data(chunk.ptrw(), available, received) != OK || received <= 0) {
			break;
		}
		p_peer->buffer += String::utf8((const char *)chunk.ptr(), received);
		available = connection->get_available_bytes();
	}

	// One request at a time per connection. While a deferred tool owes this connection
	// its response, later requests wait in the buffer rather than interleave.
	while (!p_peer->http_awaiting_deferred) {
		MCPHttpRequest request;
		String parse_error;
		if (!mcp_http_parse_request(p_peer->buffer, request, parse_error)) {
			if (!parse_error.is_empty()) {
				const CharString bad = mcp_http_response_empty(400, "Bad Request").utf8();
				connection->put_data((const uint8_t *)bad.get_data(), bad.length());
				connection->disconnect_from_host();
			}
			return;
		}
		_handle_http_request(p_peer, request);
	}
}

void MCPService::_handle_http_request(Peer *p_peer, const MCPHttpRequest &p_request) {
	Ref<StreamPeerTCP> connection = p_peer->connection;
	auto respond = [&](const String &p_response) {
		const CharString utf8 = p_response.utf8();
		connection->put_data((const uint8_t *)utf8.get_data(), utf8.length());
	};

	if (!mcp_http_token_matches(p_request.header("authorization"), http_token)) {
		respond(mcp_http_response_empty(401, "Unauthorized"));
		return;
	}
	if (p_request.path != "/mcp") {
		respond(mcp_http_response_empty(404, "Not Found"));
		return;
	}

	const String session_id = p_request.header("mcp-session-id");

	if (p_request.method == "DELETE") {
		http_sessions.erase(session_id);
		respond(mcp_http_response_empty(200, "OK"));
		return;
	}
	if (p_request.method != "POST") {
		respond(mcp_http_response_empty(405, "Method Not Allowed"));
		return;
	}

	const Variant parsed = JSON::parse_string(p_request.body);
	if (parsed.get_type() != Variant::DICTIONARY) {
		respond(mcp_http_response_json(400,
				JSON::stringify(MCPProtocol::make_error(Variant(), MCPProtocol::ERROR_PARSE,
						"the editor could not parse this body as a JSON-RPC object"))));
		return;
	}

	// Sessions live in the store, keyed by header, because one MCP session spans many
	// HTTP connections. The peer's session is the working copy for this one dispatch.
	String effective_id = session_id;
	if (effective_id.is_empty()) {
		// Only an initialize may open a session; anything else without one is a client
		// that lost its id, and pretending otherwise would silently reset its approval.
		effective_id = mcp_http_random_id();
		MCPSession fresh;
		// The launch headers carry what the bridge's godot/hello used to: who this is,
		// and whether the session must refuse writes. Approval is decided here too -
		// same gate, same dialog, same settings as a bridge client - because the
		// protocol layer refuses every method on an unapproved session.
		fresh.client_name = p_request.header("x-godot-ai-client-name", "http-client");
		fresh.client_id = fresh.client_name;
		fresh.read_only = p_request.header("x-godot-ai-read-only") == "1";
		String approval_reason;
		fresh.client_approved = approve_client(fresh, approval_reason);
		if (!fresh.client_approved) {
			respond(mcp_http_response_json(403,
					JSON::stringify(MCPProtocol::make_error(Variant(), MCPProtocol::ERROR_CLIENT_NOT_APPROVED,
							approval_reason.is_empty() ? String("this client is not approved to use the editor") : approval_reason))));
			return;
		}
		http_sessions[effective_id] = fresh;
	} else if (!http_sessions.has(effective_id)) {
		respond(mcp_http_response_empty(404, "Unknown Session"));
		return;
	}

	p_peer->session = http_sessions[effective_id];
	p_peer->http_session_id = effective_id;

	Dictionary response;
	current_peer = p_peer;
	const int deferred_before = p_peer->deferred.size();
	const bool has_response = MCPProtocol::handle_message(parsed, p_peer->session, this, response);
	current_peer = nullptr;
	http_sessions[effective_id] = p_peer->session;

	if (has_response) {
		respond(mcp_http_response_json(200, JSON::stringify(response), session_id.is_empty() ? effective_id : String()));
	} else if (p_peer->deferred.size() > deferred_before) {
		// The tool deferred; the HTTP response is owed when it completes.
		p_peer->http_awaiting_deferred = true;
	} else {
		// A notification: accepted, nothing to say. 202 is Streamable HTTP's word for it.
		respond(mcp_http_response_empty(202, "Accepted"));
	}
}

void MCPService::_send_http_response(Peer *p_peer, const Dictionary &p_message) {
	if (p_peer->connection.is_null() || p_peer->connection->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
		return;
	}
	const CharString utf8 = mcp_http_response_json(200, JSON::stringify(p_message)).utf8();
	p_peer->connection->put_data((const uint8_t *)utf8.get_data(), utf8.length());
	p_peer->http_awaiting_deferred = false;
}

bool MCPService::defer_response(const Variant &p_id, int64_t p_token, const Ref<MCPTool> &p_tool, const String &p_checkpoint) {
	if (!current_peer) {
		return false;
	}
	DeferredCall call;
	call.id = p_id;
	call.token = p_token;
	call.tool = p_tool;
	call.checkpoint = p_checkpoint;
	current_peer->deferred.push_back(call);
	return true;
}

void MCPService::_poll_deferred() {
	MCPDeferred::update();

	for (Peer *peer : peers) {
		for (int i = peer->deferred.size() - 1; i >= 0; i--) {
			const DeferredCall &call = peer->deferred[i];
			MCPDeferred::Completion completion;
			if (!MCPDeferred::take(call.token, completion)) {
				continue;
			}

			Dictionary response;
			if (completion.error.has_error()) {
				response = MCPProtocol::make_result(call.id, MCPProtocol::make_tool_error_result(completion.error));
			} else {
				Dictionary tool_result = MCPProtocol::make_tool_result(completion.result,
						call.tool.is_valid() ? call.tool->get_output_schema() : Dictionary());
				if (!call.checkpoint.is_empty()) {
					Dictionary meta;
					meta["checkpoint"] = call.checkpoint;
					tool_result["_meta"] = meta;
				}
				response = MCPProtocol::make_result(call.id, tool_result);
			}
			_send(peer, response);
			peer->deferred.remove_at(i);
		}
	}
}

void MCPService::_send(Peer *p_peer, const Dictionary &p_message) {
	if (p_peer->connection.is_null() || p_peer->connection->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
		return;
	}
	if (p_peer->http) {
		// An HTTP peer is only ever answered - there is no channel for unsolicited
		// sends (no SSE, by design). The id check keeps a broadcast notification that
		// fires mid-defer from being written as the response the request is owed.
		if (p_peer->http_awaiting_deferred && p_message.has("id")) {
			_send_http_response(p_peer, p_message);
		}
		return;
	}
	// One JSON object per line; JSON::stringify never emits a raw newline.
	const CharString payload = (JSON::stringify(p_message) + "\n").utf8();
	p_peer->connection->put_data((const uint8_t *)payload.get_data(), payload.length());
}

void MCPService::_drop_peer(int p_index) {
	ERR_FAIL_INDEX(p_index, peers.size());
	Peer *peer = peers[p_index];
	// Nobody is left to hear the answer; abandoning also stops a dialog from
	// completing into a socket that no longer exists.
	for (const DeferredCall &call : peer->deferred) {
		MCPDeferred::abandon(call.token);
	}
	if (peer->connection.is_valid()) {
		peer->connection->disconnect_from_host();
	}
	peers.remove_at(p_index);
	memdelete(peer);
}

void MCPService::_on_tools_changed() {
	// Clients cache tool lists; tell every initialized session the set moved.
	const Dictionary notification = MCPProtocol::make_notification("notifications/tools/list_changed", Dictionary());
	for (Peer *peer : peers) {
		if (peer->session.initialized) {
			_send(peer, notification);
		}
	}
}

void MCPService::_write_instance_descriptor() {
	const String dir = get_instances_dir();
	Ref<DirAccess> access = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (access.is_null() || access->make_dir_recursive(dir) != OK) {
		WARN_PRINT(vformat("Godot AI: could not create the instance directory '%s'; clients will need --editor-socket %d.", dir, port));
		return;
	}

	Dictionary descriptor;
	descriptor["pid"] = OS::get_singleton()->get_process_id();
	descriptor["port"] = port;
	descriptor["project_path"] = get_project_path();
	descriptor["project_name"] = get_project_name();
	descriptor["editor_version"] = get_editor_version();
	descriptor["protocol_version"] = MCPProtocol::BRIDGE_VERSION;
	if (http_port > 0) {
		descriptor["http_port"] = http_port;
		// In the descriptor and nowhere else: this file is the user's own state dir and
		// is chmod 600 below, which is the same trust that protects their SSH keys. A
		// generated client configuration still references the env var, never the value.
		descriptor["http_token"] = http_token;
	}
	descriptor["started_at"] = Time::get_singleton()->get_unix_time_from_system();

	instance_descriptor_path = dir.path_join(itos(OS::get_singleton()->get_process_id()) + ".json");
	Ref<FileAccess> file = FileAccess::open(instance_descriptor_path, FileAccess::WRITE);
	if (file.is_null()) {
		WARN_PRINT(vformat("Godot AI: could not write the instance descriptor '%s'.", instance_descriptor_path));
		instance_descriptor_path = String();
		return;
	}
	file->store_string(JSON::stringify(descriptor));
	file.unref();
	// The descriptor now carries the HTTP token, so it is readable by its owner alone.
	// Platforms without unix permissions ignore this and rely on the user profile's own
	// isolation, which is what protects every other secret there too.
	FileAccess::set_unix_permissions(instance_descriptor_path,
			FileAccess::UNIX_READ_OWNER | FileAccess::UNIX_WRITE_OWNER);
}

void MCPService::_remove_instance_descriptor() {
	if (instance_descriptor_path.is_empty()) {
		return;
	}
	Ref<DirAccess> access = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (access.is_valid()) {
		access->remove(instance_descriptor_path);
	}
	instance_descriptor_path = String();
}

// -------------------------------------------------------------- editor entry ---

void MCPService::_register_editor_commands() {
	approvals_dialog = memnew(MCPApprovalsDialog(this));
	// Owned by the editor's window so it is cleaned up with the editor.
	EditorNode::get_singleton()->get_gui_base()->add_child(approvals_dialog);

	// The other end of the runtime channel. Registered here so it lives exactly as long
	// as the editor plugin does.
	runtime_bridge.instantiate();
	add_debugger_plugin(runtime_bridge);

	// Windowed profiler captures. Lives beside the bridge because it harvests the
	// same debugger session the bridge talks through.
	profiler_recorder.instantiate();

	// The bottom panel, not a side dock.
	//
	// It went in beside the chat first, and that shrank the Inspector enough that
	// Godot_CaptureInspectorProperty could no longer scroll a property into view - a
	// tool regression caused purely by where a panel was put. The bottom panel is also
	// where the workspace specification wants the evidence plane, so this is the smaller
	// change and the more correct one.
	//
	// It does mean the thin dock currently carries both the control plane (pause, stop)
	// and the evidence plane (records, diff, revert). Splitting them is worth doing once
	// there is a task hierarchy for the control side to show.
	activity_dock = memnew(MCPActivityDock);
	add_control_to_bottom_panel(activity_dock, TTR("Agent Activity"));

#ifdef MCP_TERMINAL_ENABLED
	// A terminal running a coding agent against this editor, beside the activity it
	// produces. Nothing starts it but the user pressing Start: an editor that spawns
	// programs on a tool's say-so is not what this module is.
	agent_terminal = memnew(MCPAgentTerminalPanel);
	add_control_to_bottom_panel(agent_terminal, TTR("Agent Terminal"));
#endif

	// The Tools menu is where a user goes looking; the command palette is where they
	// go when they already know what they want.
	add_tool_menu_item(TTR("Godot AI: Clients and Skills..."), callable_mp(this, &MCPService::_show_approvals));

	if (EditorCommandPalette::get_singleton()) {
		EditorCommandPalette::get_singleton()->add_command(
				TTR("Godot AI: Clients and Skills"), "godot_ai/approvals",
				callable_mp(this, &MCPService::_show_approvals), Ref<Shortcut>());
		EditorCommandPalette::get_singleton()->add_command(
				TTR("Godot AI: Show Service Status"), "godot_ai/status",
				callable_mp(this, &MCPService::_show_status), Ref<Shortcut>());
		EditorCommandPalette::get_singleton()->add_command(
				TTR("Godot AI: Restart Service"), "godot_ai/restart",
				callable_mp(this, &MCPService::restart), Ref<Shortcut>());
	}
}

void MCPService::_show_approvals() {
	if (approvals_dialog) {
		approvals_dialog->popup_centered(Size2(720, 460));
	}
}

void MCPService::_show_status() {
	if (!EditorNode::get_log()) {
		return;
	}
	if (!started) {
		EditorNode::get_log()->add_message(
				TTR("Godot AI: the service is not running. Check Editor Settings > Network > Godot AI."),
				EditorLog::MSG_TYPE_WARNING);
		return;
	}
	EditorNode::get_log()->add_message(
			vformat(TTR("Godot AI: listening on 127.0.0.1:%d, %d client(s) connected, %d awaiting approval, %d tool(s) registered."),
					port, peers.size(), pending_clients.size(),
					MCPToolRegistry::get_singleton() ? MCPToolRegistry::get_singleton()->get_tool_count() : 0),
			EditorLog::MSG_TYPE_EDITOR);
}

void MCPService::restart() {
	stop();
	start();
}

void MCPService::show_activity() {
	if (activity_dock) {
		make_bottom_panel_item_visible(activity_dock);
	}
}

// ----------------------------------------------------------------- approvals ---

static String approved_clients_setting() {
	return "network/godot_ai/approved_clients";
}

bool MCPService::is_client_approved(const String &p_client_name) {
	// An explicit opt-in for automation. Documented as CI/headless only: it bypasses
	// the first-connection approval that otherwise gates every client.
	if (OS::get_singleton()->get_environment("GODOT_AI_AUTO_APPROVE") == "1") {
		return true;
	}
	if (!EditorSettings::get_singleton()) {
		return false;
	}
	if ((bool)EDITOR_GET("network/godot_ai/auto_approve_clients")) {
		return true;
	}
	if (!EditorSettings::get_singleton()->has_setting(approved_clients_setting())) {
		return false;
	}
	const PackedStringArray approved = EditorSettings::get_singleton()->get_setting(approved_clients_setting());
	return approved.has(p_client_name);
}

void MCPService::approve_client_name(const String &p_client_name) {
	if (!EditorSettings::get_singleton()) {
		return;
	}
	PackedStringArray approved;
	if (EditorSettings::get_singleton()->has_setting(approved_clients_setting())) {
		approved = EditorSettings::get_singleton()->get_setting(approved_clients_setting());
	}
	if (!approved.has(p_client_name)) {
		approved.push_back(p_client_name);
		EditorSettings::get_singleton()->set_setting(approved_clients_setting(), approved);
	}
	pending_clients.erase(p_client_name);
}

void MCPService::revoke_client_name(const String &p_client_name) {
	if (!EditorSettings::get_singleton() || !EditorSettings::get_singleton()->has_setting(approved_clients_setting())) {
		return;
	}
	PackedStringArray approved = EditorSettings::get_singleton()->get_setting(approved_clients_setting());
	const int index = approved.find(p_client_name);
	if (index >= 0) {
		approved.remove_at(index);
		EditorSettings::get_singleton()->set_setting(approved_clients_setting(), approved);
	}
	// Revocation takes effect now, not at the next connection.
	for (int i = peers.size() - 1; i >= 0; i--) {
		if (peers[i]->session.client_name == p_client_name) {
			_drop_peer(i);
		}
	}
}

bool MCPService::approve_client(MCPSession &p_session, String &r_reason) {
	if (is_client_approved(p_session.client_name)) {
		return true;
	}
	if (!pending_clients.has(p_session.client_name)) {
		pending_clients.push_back(p_session.client_name);
		if (EditorNode::get_log()) {
			EditorNode::get_log()->add_message(
					vformat("Godot AI: client '%s' asked to connect and is waiting for approval "
							"(Editor Settings > Network > Godot AI).",
							p_session.client_name),
					EditorLog::MSG_TYPE_WARNING);
		}
	}
	r_reason = vformat("client '%s' is not approved for this editor; approve it in "
					   "Editor Settings > Network > Godot AI and reconnect",
			p_session.client_name);
	return false;
}

bool MCPService::prompt_for_tool(const MCPSession &p_session, const Ref<MCPTool> &p_tool, const Dictionary &p_arguments, String &r_reason) {
	if (OS::get_singleton()->get_environment("GODOT_AI_AUTO_APPROVE") == "1") {
		r_reason = "auto-approved (GODOT_AI_AUTO_APPROVE)";
		return true;
	}
	// Interactive per-invocation approval belongs to the settings UI; until the user
	// has made a standing decision, the safe answer is no.
	r_reason = vformat("'%s' needs approval for the '%s' capability. Set that capability to "
					   "'allow' in Editor Settings > Network > Godot AI, or start the relay with "
					   "--approval-mode allow, to permit it.",
			p_tool->get_tool_name(), mcp_capability_to_string(p_tool->get_capability()));
	return false;
}

void MCPService::record_invocation(const MCPSession &p_session, const String &p_tool_name, const String &p_summary, bool p_allowed, const String &p_reason) {
	MCPAudit::record(p_session.client_name, p_tool_name, p_summary, p_allowed, p_reason);
}

String MCPService::get_project_path() const {
	if (!ProjectSettings::get_singleton()) {
		return String();
	}
	return ProjectSettings::get_singleton()->get_resource_path();
}

String MCPService::get_project_name() const {
	if (!ProjectSettings::get_singleton()) {
		return String();
	}
	return ProjectSettings::get_singleton()->get("application/config/name");
}

String MCPService::get_editor_version() const {
	return String(VERSION_FULL_BUILD);
}
