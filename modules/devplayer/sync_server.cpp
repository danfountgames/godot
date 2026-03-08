/**************************************************************************/
/*  sync_server.cpp                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           GODOT BEAM ENGINE                            */
/**************************************************************************/

#include "sync_server.h"

#include "launch_controller.h"
#include "project_domain_manager.h"

#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "modules/websocket/websocket_peer.h"

SyncServer *SyncServer::singleton = nullptr;

SyncServer *SyncServer::get_singleton() {
	return singleton;
}

// ---------------------------------------------------------------------------
// Bind methods
// ---------------------------------------------------------------------------

void SyncServer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start_server", "port"), &SyncServer::start_server, DEFVAL(6850));
	ClassDB::bind_method(D_METHOD("stop_server"), &SyncServer::stop_server);
	ClassDB::bind_method(D_METHOD("is_running"), &SyncServer::is_running);
	ClassDB::bind_method(D_METHOD("get_connected_client_count"), &SyncServer::get_connected_client_count);
	ClassDB::bind_method(D_METHOD("get_port"), &SyncServer::get_port);
	ClassDB::bind_method(D_METHOD("poll"), &SyncServer::poll);

	ADD_SIGNAL(MethodInfo("sync_file_received", PropertyInfo(Variant::STRING, "path")));
	ADD_SIGNAL(MethodInfo("reload_requested",
			PropertyInfo(Variant::INT, "tier"),
			PropertyInfo(Variant::PACKED_STRING_ARRAY, "changed_paths")));
	ADD_SIGNAL(MethodInfo("client_connected", PropertyInfo(Variant::INT, "peer_id")));
	ADD_SIGNAL(MethodInfo("client_disconnected", PropertyInfo(Variant::INT, "peer_id")));

	BIND_ENUM_CONSTANT(RELOAD_TIER_CONTENT);
	BIND_ENUM_CONSTANT(RELOAD_TIER_SESSION);
	BIND_ENUM_CONSTANT(RELOAD_TIER_FULL);
}

// ---------------------------------------------------------------------------
// Server lifecycle
// ---------------------------------------------------------------------------

Error SyncServer::start_server(int p_port) {
	if (running) {
		WARN_PRINT("[SyncServer] Server is already running. Call stop_server() first.");
		return ERR_ALREADY_IN_USE;
	}

	ERR_FAIL_COND_V_MSG(p_port < 1 || p_port > 65535, ERR_INVALID_PARAMETER,
			"[SyncServer] Invalid port number: " + itos(p_port));

	tcp_server.instantiate();
	// Bind to localhost only by default. The sync agent runs on the same
	// machine or connects via USB tunnel (iproxy), so network-wide binding
	// is unnecessary. This prevents exposing the unauthenticated file-write
	// endpoint to other machines on the local network.
	Error err = tcp_server->listen(p_port, IPAddress("127.0.0.1"));
	if (err != OK) {
		tcp_server.unref();
		ERR_FAIL_V_MSG(err, "[SyncServer] Failed to listen on port " + itos(p_port));
	}

	server_port = p_port;
	running = true;
	next_peer_id = 1;

	print_line("[SyncServer] WebSocket sync server started on port " + itos(p_port));
	return OK;
}

void SyncServer::stop_server() {
	if (!running) {
		return;
	}

	// Close all connected peers.
	for (KeyValue<int, Ref<WebSocketPeer>> &E : connected_peers) {
		E.value->close(1001, "Server shutting down");
	}
	connected_peers.clear();

	// Drop all pending peers.
	pending_peers.clear();

	// Stop the TCP server.
	if (tcp_server.is_valid()) {
		tcp_server->stop();
		tcp_server.unref();
	}

	running = false;
	print_line("[SyncServer] Server stopped.");
}

bool SyncServer::is_running() const {
	return running;
}

int SyncServer::get_connected_client_count() const {
	return connected_peers.size();
}

int SyncServer::get_port() const {
	return server_port;
}

// ---------------------------------------------------------------------------
// Polling -- must be called each frame
// ---------------------------------------------------------------------------

void SyncServer::poll() {
	if (!running || tcp_server.is_null()) {
		return;
	}

	_poll_new_connections();
	_poll_pending_peers();
	_poll_connected_peers();
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------

void SyncServer::_poll_new_connections() {
	while (tcp_server->is_connection_available()) {
		Ref<WebSocketPeer> ws = Ref<WebSocketPeer>(WebSocketPeer::create());
		ERR_CONTINUE_MSG(ws.is_null(), "[SyncServer] Failed to create WebSocketPeer.");

		Ref<StreamPeerTCP> tcp = tcp_server->take_connection();
		ERR_CONTINUE_MSG(tcp.is_null(), "[SyncServer] Failed to take TCP connection.");

		Error err = ws->accept_stream(tcp);
		if (err != OK) {
			WARN_PRINT("[SyncServer] Failed to begin WebSocket handshake.");
			continue;
		}

		int id = next_peer_id++;
		ClientPeer client;
		client.ws = ws;
		client.connect_time = OS::get_singleton()->get_ticks_msec();
		client.handshake_complete = false;
		pending_peers[id] = client;
	}
}

void SyncServer::_poll_pending_peers() {
	Vector<int> to_remove;
	uint64_t now = OS::get_singleton()->get_ticks_msec();

	for (KeyValue<int, ClientPeer> &E : pending_peers) {
		int id = E.key;
		ClientPeer &client = E.value;

		client.ws->poll();
		WebSocketPeer::State state = client.ws->get_ready_state();

		if (state == WebSocketPeer::STATE_OPEN) {
			// Handshake completed -- promote to connected.
			connected_peers[id] = client.ws;
			to_remove.push_back(id);
			print_line("[SyncServer] Client connected: peer " + itos(id) +
					" from " + client.ws->get_connected_host().operator String() +
					":" + itos(client.ws->get_connected_port()));
			emit_signal("client_connected", id);
			continue;
		}

		if (state == WebSocketPeer::STATE_CONNECTING) {
			// Check timeout.
			if (now - client.connect_time > HANDSHAKE_TIMEOUT_MSEC) {
				WARN_PRINT("[SyncServer] Handshake timeout for pending peer " + itos(id));
				to_remove.push_back(id);
			}
			continue;
		}

		// Closed or error.
		to_remove.push_back(id);
	}

	for (int id : to_remove) {
		pending_peers.erase(id);
	}
}

void SyncServer::_poll_connected_peers() {
	Vector<int> to_remove;

	for (KeyValue<int, Ref<WebSocketPeer>> &E : connected_peers) {
		int id = E.key;
		Ref<WebSocketPeer> ws = E.value;

		ws->poll();

		if (ws->get_ready_state() != WebSocketPeer::STATE_OPEN) {
			to_remove.push_back(id);
			continue;
		}

		// Process all available packets.
		while (ws->get_available_packet_count() > 0) {
			const uint8_t *buf;
			int buf_size;
			Error err = ws->get_packet(&buf, buf_size);
			if (err != OK) {
				WARN_PRINT("[SyncServer] Error reading packet from peer " + itos(id));
				break;
			}

			if (ws->was_string_packet()) {
				String text = String::utf8((const char *)buf, buf_size);
				_process_message(id, text);
			} else {
				WARN_PRINT("[SyncServer] Received binary packet from peer " + itos(id) + " -- expected text/JSON.");
			}
		}
	}

	for (int id : to_remove) {
		connected_peers.erase(id);
		print_line("[SyncServer] Client disconnected: peer " + itos(id));
		emit_signal("client_disconnected", id);
	}
}

// ---------------------------------------------------------------------------
// Protocol message dispatch
// ---------------------------------------------------------------------------

void SyncServer::_process_message(int p_peer_id, const String &p_text) {
	Variant parsed = JSON::parse_string(p_text);
	if (parsed.get_type() != Variant::DICTIONARY) {
		WARN_PRINT("[SyncServer] Received non-dictionary JSON from peer " + itos(p_peer_id));
		return;
	}

	Dictionary msg = parsed;
	if (!msg.has("type")) {
		WARN_PRINT("[SyncServer] Message missing 'type' field from peer " + itos(p_peer_id));
		return;
	}

	String type = msg["type"];

	if (type == "hello") {
		_handle_hello(p_peer_id, msg);
	} else if (type == "manifest") {
		_handle_manifest(p_peer_id, msg);
	} else if (type == "write_small_file") {
		_handle_write_small_file(p_peer_id, msg);
	} else if (type == "reload_hint") {
		_handle_reload_hint(p_peer_id, msg);
	} else if (type == "sync_complete") {
		_handle_sync_complete(p_peer_id, msg);
	} else {
		WARN_PRINT("[SyncServer] Unknown message type '" + type + "' from peer " + itos(p_peer_id));
	}
}

// ---------------------------------------------------------------------------
// Protocol handlers
// ---------------------------------------------------------------------------

void SyncServer::_handle_hello(int p_peer_id, const Dictionary &p_msg) {
	// Respond with device info for host discovery / handshake.
	print_line("[SyncServer] Received 'hello' from peer " + itos(p_peer_id));

	String project_root = _get_project_root();

	Dictionary response;
	response["type"] = "hello_ack";
	response["engine"] = "GodotBeam";
	response["device"] = OS::get_singleton()->get_name();
	response["project_mounted"] = !project_root.is_empty();
	response["project_root"] = project_root;

	_send_json(p_peer_id, response);
}

void SyncServer::_handle_manifest(int p_peer_id, const Dictionary &p_msg) {
	// The sync agent sends a file hash list. We compare against local state
	// and respond with which files need to be sent.
	print_line("[SyncServer] Received 'manifest' from peer " + itos(p_peer_id));

	String project_root = _get_project_root();
	if (project_root.is_empty()) {
		Dictionary response;
		response["type"] = "manifest_ack";
		response["status"] = "error";
		response["message"] = "No project mounted.";
		_send_json(p_peer_id, response);
		return;
	}

	// The manifest contains "files": { "relative/path": "sha256hex", ... }
	Dictionary files;
	if (p_msg.has("files")) {
		files = p_msg["files"];
	}

	// Compare against local files and build a list of paths that need updating.
	PackedStringArray needs_update;

	Array file_keys = files.keys();
	for (int i = 0; i < file_keys.size(); i++) {
		String rel_path = file_keys[i];

		// Security: skip paths with traversal attempts.
		if (rel_path.contains("..") || rel_path.begins_with("/")) {
			WARN_PRINT("[SyncServer] Skipping manifest entry with invalid path: " + rel_path);
			continue;
		}

		// The sync agent is authoritative: always accept all offered files.
		// A future optimization could compare hashes to skip unchanged files.
		needs_update.push_back(rel_path);
	}

	Dictionary response;
	response["type"] = "manifest_ack";
	response["status"] = "ok";
	response["needs_update"] = needs_update;

	_send_json(p_peer_id, response);
	print_line("[SyncServer] Manifest processed: " + itos(needs_update.size()) + " files need sync.");
}

void SyncServer::_handle_write_small_file(int p_peer_id, const Dictionary &p_msg) {
	// Receive a base64-encoded file and write it to the mounted project directory.
	if (!p_msg.has("path") || !p_msg.has("content")) {
		WARN_PRINT("[SyncServer] write_small_file missing 'path' or 'content' field.");
		return;
	}

	String rel_path = p_msg["path"];
	String content_b64 = p_msg["content"];

	// Decode base64 content.
	CharString cs = content_b64.ascii();
	size_t b64_len = cs.length();

	// Estimate decoded size (base64 expands ~4/3).
	size_t max_decoded = b64_len / 4 * 3 + 4;
	Vector<uint8_t> decoded;
	decoded.resize(max_decoded);

	size_t decoded_len = 0;
	Error b64_err = CryptoCore::b64_decode(
			decoded.ptrw(), decoded.size(), &decoded_len,
			(const uint8_t *)cs.get_data(), b64_len);

	if (b64_err != OK) {
		WARN_PRINT("[SyncServer] Failed to decode base64 content for: " + rel_path);
		return;
	}
	decoded.resize(decoded_len);

	// Write file.
	Error write_err = _write_file_to_project(rel_path, decoded);
	if (write_err != OK) {
		WARN_PRINT("[SyncServer] Failed to write file: " + rel_path);
		return;
	}

	print_line("[SyncServer] File written: " + rel_path + " (" + itos(decoded_len) + " bytes)");
	emit_signal("sync_file_received", rel_path);

	// Send acknowledgment.
	Dictionary response;
	response["type"] = "write_ack";
	response["path"] = rel_path;
	response["status"] = "ok";
	response["bytes"] = (int64_t)decoded_len;
	_send_json(p_peer_id, response);
}

void SyncServer::_handle_reload_hint(int p_peer_id, const Dictionary &p_msg) {
	// The sync agent tells us what reload tier is appropriate based on changed files.
	int tier = RELOAD_TIER_CONTENT;
	if (p_msg.has("tier")) {
		tier = p_msg["tier"];
	}

	PackedStringArray changed_paths;
	if (p_msg.has("changed_paths")) {
		Array arr = p_msg["changed_paths"];
		for (int i = 0; i < arr.size(); i++) {
			changed_paths.push_back(arr[i]);
		}
	}

	print_line("[SyncServer] Reload hint received: tier " + itos(tier) +
			", " + itos(changed_paths.size()) + " changed paths.");

	// Emit the signal so that the DevPlayerShell or LaunchController can react.
	emit_signal("reload_requested", tier, changed_paths);

	// Also trigger the reload directly based on tier.
	switch (tier) {
		case RELOAD_TIER_CONTENT: {
			// Tier 1: Hot-reload content resources (textures, sounds, etc.).
			// Godot doesn't have a public reload_changed_resources() in all builds,
			// so we notify via signal and let the shell handle it.
			print_line("[SyncServer] Tier 1: Content reload requested. Signal emitted.");
		} break;

		case RELOAD_TIER_SESSION: {
			// Tier 2: Scripts changed -- relaunch the project session.
			LaunchController *lc = LaunchController::get_singleton();
			if (lc) {
				print_line("[SyncServer] Tier 2: Session relaunch triggered.");
				lc->relaunch();
			} else {
				WARN_PRINT("[SyncServer] Tier 2 reload requested but LaunchController not available.");
			}
		} break;

		case RELOAD_TIER_FULL: {
			// Tier 3: project.godot changed -- full restart required.
			// We cannot safely automate a full engine restart from within the process.
			WARN_PRINT("[SyncServer] Tier 3: Full engine restart required. "
					   "project.godot has changed. Please restart the application manually.");
			print_line("[SyncServer] Tier 3: Full restart requested (not automated).");
		} break;

		default: {
			WARN_PRINT("[SyncServer] Unknown reload tier: " + itos(tier));
		} break;
	}

	// Acknowledge.
	Dictionary response;
	response["type"] = "reload_ack";
	response["tier"] = tier;
	response["status"] = "ok";
	_send_json(p_peer_id, response);
}

void SyncServer::_handle_sync_complete(int p_peer_id, const Dictionary &p_msg) {
	// The sync agent signals that a batch of file writes is complete.
	int files_synced = 0;
	if (p_msg.has("files_synced")) {
		files_synced = p_msg["files_synced"];
	}

	print_line("[SyncServer] Sync complete from peer " + itos(p_peer_id) +
			": " + itos(files_synced) + " files synced.");

	Dictionary response;
	response["type"] = "sync_complete_ack";
	response["status"] = "ok";
	_send_json(p_peer_id, response);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void SyncServer::_send_json(int p_peer_id, const Dictionary &p_msg) {
	if (!connected_peers.has(p_peer_id)) {
		return;
	}

	Ref<WebSocketPeer> ws = connected_peers[p_peer_id];
	if (ws.is_null() || ws->get_ready_state() != WebSocketPeer::STATE_OPEN) {
		return;
	}

	String json_text = JSON::stringify(p_msg);
	ws->send_text(json_text);
}

Error SyncServer::_write_file_to_project(const String &p_relative_path, const Vector<uint8_t> &p_data) {
	String project_root = _get_project_root();
	ERR_FAIL_COND_V_MSG(project_root.is_empty(), ERR_UNCONFIGURED,
			"[SyncServer] No project mounted; cannot write file.");

	// Security: reject path traversal attempts (e.g. "../../../etc/passwd").
	ERR_FAIL_COND_V_MSG(p_relative_path.contains(".."), ERR_INVALID_PARAMETER,
			"[SyncServer] Path traversal detected, rejecting: " + p_relative_path);
	ERR_FAIL_COND_V_MSG(p_relative_path.begins_with("/"), ERR_INVALID_PARAMETER,
			"[SyncServer] Absolute path rejected, must be relative: " + p_relative_path);

	String abs_path = project_root.path_join(p_relative_path);

	// Ensure parent directory exists.
	String dir_path = abs_path.get_base_dir();
	Error dir_err = DirAccess::make_dir_recursive_absolute(dir_path);
	if (dir_err != OK && dir_err != ERR_ALREADY_EXISTS) {
		ERR_FAIL_V_MSG(dir_err, "[SyncServer] Failed to create directory: " + dir_path);
	}

	// Write the file.
	Ref<FileAccess> fa = FileAccess::open(abs_path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(fa.is_null(), ERR_FILE_CANT_WRITE,
			"[SyncServer] Cannot open file for writing: " + abs_path);

	fa->store_buffer(p_data.ptr(), p_data.size());
	fa->flush();

	return OK;
}

String SyncServer::_get_project_root() const {
	ProjectDomainManager *pdm = ProjectDomainManager::get_singleton();
	if (pdm && pdm->is_project_mounted()) {
		return pdm->get_mounted_project_path();
	}
	return String();
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

SyncServer::SyncServer() {
	singleton = this;
}

SyncServer::~SyncServer() {
	stop_server();
	if (singleton == this) {
		singleton = nullptr;
	}
}
