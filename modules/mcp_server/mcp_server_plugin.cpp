/**************************************************************************/
/*  mcp_server_plugin.cpp                                                 */
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

#include "mcp_server_plugin.h"

#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/version.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_paths.h"
#include "editor/settings/editor_settings.h"

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

MCPServerPlugin::MCPServerPlugin() {
	// Register EditorSettings defaults.
	_EDITOR_DEF("network/mcp_server/enabled", true);
	_EDITOR_DEF("network/mcp_server/port", MCP_DEFAULT_PORT);
	_EDITOR_DEF("network/mcp_server/host", String(MCP_DEFAULT_HOST));
	_EDITOR_DEF("network/mcp_server/use_thread", true);
	_EDITOR_DEF("network/mcp_server/max_clients", MCP_MAX_CLIENTS);
	_EDITOR_DEF("network/mcp_server/session_timeout_sec", MCP_DEFAULT_SESSION_TIMEOUT_SEC);

	// Create and register the debugger bridge plugin.
	debugger_bridge.instantiate();
	if (EditorDebuggerNode::get_singleton()) {
		EditorDebuggerNode::get_singleton()->add_debugger_plugin(debugger_bridge);
	}

	// Give the protocol a pointer to the bridge for tool handlers to use.
	protocol.set_debugger_bridge(debugger_bridge.ptr());

	set_process_internal(true);
}

MCPServerPlugin::~MCPServerPlugin() {
	if (started) {
		stop();
	}

	// Remove and release the debugger bridge plugin.
	if (debugger_bridge.is_valid() && EditorDebuggerNode::get_singleton()) {
		EditorDebuggerNode::get_singleton()->remove_debugger_plugin(debugger_bridge);
		debugger_bridge.unref();
	}
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

void MCPServerPlugin::_bind_methods() {
	// No GDScript-exposed methods for now.
}

// ---------------------------------------------------------------------------
// Notification Handler
// ---------------------------------------------------------------------------

void MCPServerPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_EXIT_TREE: {
			stop();
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			if (!start_attempted && EditorNode::get_singleton()->is_editor_ready()) {
				start_attempted = true;
				bool enabled = (bool)_EDITOR_GET("network/mcp_server/enabled");
				if (enabled) {
					start();
				}
			}

			// If running without a thread, poll on the main thread.
			if (started && !use_thread) {
				protocol.poll();
			}
		} break;

		case EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED: {
			if (!EditorSettings::get_singleton()->check_changed_settings_in_group("network/mcp_server")) {
				break;
			}

			bool new_enabled = (bool)_EDITOR_GET("network/mcp_server/enabled");
			String new_host = String(_EDITOR_GET("network/mcp_server/host"));
			int new_port = (int)_EDITOR_GET("network/mcp_server/port");
			bool new_use_thread = (bool)_EDITOR_GET("network/mcp_server/use_thread");
			int new_max_clients = (int)_EDITOR_GET("network/mcp_server/max_clients");
			int new_session_timeout = (int)_EDITOR_GET("network/mcp_server/session_timeout_sec");

			if (!new_enabled && started) {
				stop();
			} else if (new_enabled && !started) {
				start();
			} else if (new_enabled && started) {
				if (new_host != host || new_port != port || new_use_thread != use_thread) {
					stop();
					start();
				} else {
					protocol.set_max_clients(new_max_clients);
					protocol.set_session_timeout(new_session_timeout);
				}
			}
		} break;
	}
}

// ---------------------------------------------------------------------------
// Thread Entry Point
// ---------------------------------------------------------------------------

void MCPServerPlugin::thread_main(void *p_userdata) {
	set_current_thread_safe_for_nodes(true);

	MCPServerPlugin *self = static_cast<MCPServerPlugin *>(p_userdata);
	while (self->thread_running.is_set()) {
		self->protocol.poll();
		OS::get_singleton()->delay_usec(50000); // 50ms -> ~20 Hz
	}
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

void MCPServerPlugin::start() {
	if (started) {
		return;
	}

	host = String(_EDITOR_GET("network/mcp_server/host"));
	port = (int)_EDITOR_GET("network/mcp_server/port");
	use_thread = (bool)_EDITOR_GET("network/mcp_server/use_thread");
	int max_clients = (int)_EDITOR_GET("network/mcp_server/max_clients");
	int session_timeout = (int)_EDITOR_GET("network/mcp_server/session_timeout_sec");

	protocol.set_max_clients(max_clients);
	protocol.set_session_timeout(session_timeout);

	// Generate a random bearer token for authentication (32 hex chars = 16 bytes).
	{
		uint8_t token_bytes[16];
		Error token_err = OS::get_singleton()->get_entropy(token_bytes, 16);
		if (token_err != OK) {
			CryptoCore::RandomGenerator rng;
			Error rng_err = rng.init();
			if (rng_err == OK) {
				rng_err = rng.get_random_bytes(token_bytes, 16);
			}
			ERR_FAIL_COND_MSG(rng_err != OK, "[MCP] Failed to generate auth token -- no CSPRNG available.");
		}
		auth_token = String();
		for (int i = 0; i < 16; i++) {
			auth_token += String::num_int64(token_bytes[i], 16).lpad(2, "0");
		}
		protocol.set_auth_token(auth_token);
	}

	Error err = protocol.start(port, IPAddress(host));
	if (err != OK) {
		ERR_PRINT("[MCP] Failed to start server on " + host + ":" + itos(port) + " (error: " + itos(err) + ")");
		start_attempted = false; // Allow retry on next process tick.
		return;
	}

	print_line("[MCP] Server started on " + host + ":" + itos(port));

	write_discovery_file();

	if (use_thread) {
		thread_running.set();
		thread.start(MCPServerPlugin::thread_main, this);
	}

	set_process_internal(!use_thread);
	started = true;
}

void MCPServerPlugin::stop() {
	if (!started) {
		return;
	}

	if (use_thread && thread.is_started()) {
		thread_running.clear();
		thread.wait_to_finish();
	}

	protocol.stop();
	started = false;

	delete_discovery_file();

	print_verbose("[MCP] Server stopped.");
}

// ---------------------------------------------------------------------------
// Discovery File
// ---------------------------------------------------------------------------

String MCPServerPlugin::get_discovery_file_path() const {
	return EditorPaths::get_singleton()->get_data_dir().path_join("mcp_server").path_join("discovery.json");
}

void MCPServerPlugin::write_discovery_file() {
	String dir_path = EditorPaths::get_singleton()->get_data_dir().path_join("mcp_server");

	Error dir_err = DirAccess::make_dir_recursive_absolute(dir_path);
	if (dir_err != OK) {
		ERR_PRINT("[MCP] Failed to create discovery directory: " + dir_path);
		return;
	}

	String file_path = get_discovery_file_path();

	Dictionary discovery;
	discovery["endpoint"] = "http://" + host + ":" + itos(port) + "/mcp";
	discovery["token"] = auth_token;
	discovery["pid"] = OS::get_singleton()->get_process_id();
	discovery["godot_version"] = GODOT_VERSION_FULL_CONFIG;

	String json_content = JSON::stringify(discovery, "\t");

	Ref<FileAccess> f = FileAccess::open(file_path, FileAccess::WRITE);
	if (f.is_null()) {
		ERR_PRINT("[MCP] Failed to write discovery file: " + file_path);
		return;
	}
	f->store_string(json_content);
	f.unref();

#ifndef WINDOWS_ENABLED
	FileAccess::set_unix_permissions(file_path,
			FileAccess::UNIX_READ_OWNER | FileAccess::UNIX_WRITE_OWNER);
#else
	// On Windows, set hidden attribute as a defense-in-depth measure.
	// The bearer token provides the primary security boundary.
	FileAccess::set_hidden_attribute(file_path, true);
#endif

	print_verbose("[MCP] Discovery file written: " + file_path);
}

void MCPServerPlugin::delete_discovery_file() {
	String file_path = get_discovery_file_path();
	if (FileAccess::exists(file_path)) {
		Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (da.is_valid()) {
			da->remove(file_path);
		}
	}
}
