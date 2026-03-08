/**************************************************************************/
/*  sync_server.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           GODOT BEAM ENGINE                            */
/**************************************************************************/

#pragma once

#include "core/io/json.h"
#include "core/io/tcp_server.h"
#include "core/object/object.h"
#include "core/string/ustring.h"
#include "core/variant/typed_array.h"

class WebSocketPeer;

class SyncServer : public Object {
	GDCLASS(SyncServer, Object);

	static SyncServer *singleton;

public:
	enum ReloadTier {
		RELOAD_TIER_CONTENT = 1, // Textures, sounds, etc. -- hot-reload the resource.
		RELOAD_TIER_SESSION = 2, // Scripts changed -- stop_project + remount.
		RELOAD_TIER_FULL = 3, // project.godot changed -- full engine restart.
	};

private:
	// ---- Networking state ----
	Ref<TCPServer> tcp_server;

	struct ClientPeer {
		Ref<WebSocketPeer> ws;
		uint64_t connect_time = 0;
		bool handshake_complete = false;
	};

	HashMap<int, ClientPeer> pending_peers; // TCP accepted, WS handshake in progress.
	HashMap<int, Ref<WebSocketPeer>> connected_peers; // Fully connected WS clients.
	int next_peer_id = 1;

	int server_port = 6850;
	bool running = false;

	static constexpr uint64_t HANDSHAKE_TIMEOUT_MSEC = 5000;

	// ---- Protocol handling ----
	void _poll_new_connections();
	void _poll_pending_peers();
	void _poll_connected_peers();
	void _process_message(int p_peer_id, const String &p_text);

	// Message handlers.
	void _handle_hello(int p_peer_id, const Dictionary &p_msg);
	void _handle_manifest(int p_peer_id, const Dictionary &p_msg);
	void _handle_write_small_file(int p_peer_id, const Dictionary &p_msg);
	void _handle_reload_hint(int p_peer_id, const Dictionary &p_msg);
	void _handle_sync_complete(int p_peer_id, const Dictionary &p_msg);

	// Helpers.
	void _send_json(int p_peer_id, const Dictionary &p_msg);
	Error _write_file_to_project(const String &p_relative_path, const Vector<uint8_t> &p_data);
	String _get_project_root() const;

protected:
	static void _bind_methods();

public:
	static SyncServer *get_singleton();

	Error start_server(int p_port = 6850);
	void stop_server();
	bool is_running() const;
	int get_connected_client_count() const;
	int get_port() const;
	void poll();

	SyncServer();
	~SyncServer();
};

VARIANT_ENUM_CAST(SyncServer::ReloadTier);
