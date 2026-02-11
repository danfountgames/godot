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

#ifdef TOOLS_ENABLED
#include "editor/mcp_status_panel.h"
#include "scene/gui/tab_container.h"
#ifdef MCP_TERMINAL_ENABLED
#include "terminal/agent_panel.h"
#endif
#endif

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

#ifdef TOOLS_ENABLED
	// Create a single "AI" bottom panel with tabs for Agent and MCP Status.
	ai_tab_container = memnew(TabContainer);
	ai_tab_container->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	ai_tab_container->set_h_size_flags(Control::SIZE_EXPAND_FILL);

#ifdef MCP_TERMINAL_ENABLED
	// "+" placeholder tab for creating new agents.
	new_tab_placeholder = memnew(Control);
	new_tab_placeholder->set_name("+");
	ai_tab_container->add_child(new_tab_placeholder);

	ai_tab_container->connect("tab_selected", callable_mp(this, &MCPServerPlugin::_on_tab_changed));
	ai_tab_container->connect("tab_button_pressed", callable_mp(this, &MCPServerPlugin::_on_tab_close_pressed));
#endif

	status_panel = memnew(MCPStatusPanel);
	status_panel->set_protocol(&protocol);
	status_panel->set_debugger_bridge(debugger_bridge.ptr());
	status_panel->set_server_plugin(this);
	status_panel->set_name("MCP Status");
	ai_tab_container->add_child(status_panel);

	panel_button = add_control_to_bottom_panel(ai_tab_container, "AI");
#endif

	set_process_internal(true);
}

MCPServerPlugin::~MCPServerPlugin() {
	if (started) {
		stop();
	}

#ifdef TOOLS_ENABLED
	if (ai_tab_container) {
		remove_control_from_bottom_panel(ai_tab_container);
		memdelete(ai_tab_container);
		ai_tab_container = nullptr;
		// Children (agent_panels, status_panel, new_tab_placeholder) are freed by the TabContainer.
#ifdef MCP_TERMINAL_ENABLED
		agent_panels.clear();
		new_tab_placeholder = nullptr;
#endif
		status_panel = nullptr;
	}
#endif

	// Remove and release the debugger bridge plugin.
	if (debugger_bridge.is_valid() && EditorDebuggerNode::get_singleton()) {
		EditorDebuggerNode::get_singleton()->remove_debugger_plugin(debugger_bridge);
		debugger_bridge.unref();
	}
}

// ---------------------------------------------------------------------------
// Multi-tab Agent Management
// ---------------------------------------------------------------------------

#ifdef MCP_TERMINAL_ENABLED

void MCPServerPlugin::_create_agent_tab() {
	agent_counter++;
	AgentPanel *panel = memnew(AgentPanel);
	panel->set_server_plugin(this);

	String default_name = "Agent " + itos(agent_counter);
	panel->set_name(default_name);

	panel->connect("title_changed", callable_mp(this, &MCPServerPlugin::_on_agent_title_changed));

	// Insert before the "+" tab if it exists, otherwise just add.
	if (new_tab_placeholder) {
		int plus_child_idx = new_tab_placeholder->get_index();
		ai_tab_container->add_child(panel);
		ai_tab_container->move_child(panel, plus_child_idx);
	} else {
		ai_tab_container->add_child(panel);
	}

	agent_panels.push_back(panel);
	_update_close_buttons();

	// Switch to the new tab.
	int new_tab = ai_tab_container->get_tab_idx_from_control(panel);
	ai_tab_container->set_current_tab(new_tab);

	// Auto-launch Claude.
	panel->launch();
}

void MCPServerPlugin::_on_tab_changed(int p_tab) {
	if (!new_tab_placeholder) {
		return;
	}
	int plus_tab = ai_tab_container->get_tab_idx_from_control(new_tab_placeholder);
	if (p_tab == plus_tab) {
		// Defer creation to avoid modifying the tab container during signal processing.
		callable_mp(this, &MCPServerPlugin::_create_agent_tab).call_deferred();
	}
}

void MCPServerPlugin::_on_tab_close_pressed(int p_tab) {
	Control *control = ai_tab_container->get_tab_control(p_tab);
	AgentPanel *panel = Object::cast_to<AgentPanel>(control);
	if (!panel) {
		return;
	}

	// Stop any running process.
	panel->stop();

	// Find and remove from our vector.
	int vec_idx = agent_panels.find(panel);
	if (vec_idx >= 0) {
		agent_panels.remove_at(vec_idx);
	}

	ai_tab_container->remove_child(panel);
	memdelete(panel);

	// If no agent tabs remain, create a fresh one.
	if (agent_panels.is_empty()) {
		_create_agent_tab();
	}

	_update_close_buttons();

	// Switch to the first agent tab.
	if (!agent_panels.is_empty()) {
		int tab_idx = ai_tab_container->get_tab_idx_from_control(agent_panels[0]);
		ai_tab_container->set_current_tab(tab_idx);
	}
}

void MCPServerPlugin::_on_agent_title_changed(const String &p_title) {
	for (int i = 0; i < agent_panels.size(); i++) {
		AgentPanel *panel = agent_panels[i];
		int tab_idx = ai_tab_container->get_tab_idx_from_control(panel);
		String raw_title = panel->get_current_title().strip_edges();

		// Claude Code title format: "Folder - {icon} Description - claude - 80x24"
		// Extract just the descriptive segment.
		String title;
		PackedStringArray parts = raw_title.split(" - ");
		if (parts.size() >= 3) {
			// Skip first (folder) and last segments ("claude", "NxM").
			// The descriptive part is typically the second segment.
			for (int p = 1; p < parts.size(); p++) {
				String seg = parts[p].strip_edges();
				// Skip "claude" and dimension strings like "80x24".
				if (seg.to_lower() == "claude") {
					continue;
				}
				if (seg.contains("x") && seg.is_valid_int() == false) {
					// Check if it looks like dimensions (digits x digits).
					PackedStringArray dims = seg.split("x");
					if (dims.size() == 2 && dims[0].is_valid_int() && dims[1].is_valid_int()) {
						continue;
					}
				}
				title = seg;
				break;
			}
		}
		if (title.is_empty()) {
			title = raw_title;
		}

		String default_name = "Agent " + itos(i + 1);
		ai_tab_container->set_tab_title(tab_idx, title.is_empty() ? default_name : title);
	}
}

void MCPServerPlugin::_update_close_buttons() {
	Ref<Texture2D> close_icon;
	if (agent_panels.size() > 1) {
		close_icon = ai_tab_container->get_theme_icon("close", "TabBar");
	}

	// Set or clear close buttons on agent tabs only.
	for (int i = 0; i < agent_panels.size(); i++) {
		int tab_idx = ai_tab_container->get_tab_idx_from_control(agent_panels[i]);
		ai_tab_container->set_tab_button_icon(tab_idx, close_icon);
	}

	// Ensure "+" and MCP Status never have close buttons.
	if (new_tab_placeholder) {
		int plus_idx = ai_tab_container->get_tab_idx_from_control(new_tab_placeholder);
		ai_tab_container->set_tab_button_icon(plus_idx, Ref<Texture2D>());
	}
	if (status_panel) {
		int status_idx = ai_tab_container->get_tab_idx_from_control(status_panel);
		ai_tab_container->set_tab_button_icon(status_idx, Ref<Texture2D>());
	}
}

#endif // MCP_TERMINAL_ENABLED

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

	int preferred_port = port;
	Error err = protocol.start(port, IPAddress(host));
	if (err != OK) {
		for (int try_port = preferred_port + 1; try_port <= preferred_port + MCP_PORT_RANGE; try_port++) {
			err = protocol.start(try_port, IPAddress(host));
			if (err == OK) {
				port = try_port;
				break;
			}
		}
	}
	if (err != OK) {
		ERR_PRINT("[MCP] All ports " + itos(preferred_port) + "-" + itos(preferred_port + MCP_PORT_RANGE) + " in use.");
		start_attempted = false;
		return;
	}

	print_line("[MCP] Server started on " + host + ":" + itos(port));

	cleanup_stale_discovery_files();
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
// Toggle Server (called from the status panel)
// ---------------------------------------------------------------------------

void MCPServerPlugin::toggle_server() {
	if (started) {
		stop();
	} else {
		start();
	}
}

// ---------------------------------------------------------------------------
// Discovery File
// ---------------------------------------------------------------------------

String MCPServerPlugin::get_discovery_file_path() const {
	return EditorPaths::get_singleton()->get_data_dir().path_join("mcp_server").path_join("discovery").path_join(itos(port) + ".json");
}

String MCPServerPlugin::get_legacy_discovery_file_path() const {
	return EditorPaths::get_singleton()->get_data_dir().path_join("mcp_server").path_join("discovery.json");
}

void MCPServerPlugin::cleanup_stale_discovery_files() {
	String discovery_dir = EditorPaths::get_singleton()->get_data_dir().path_join("mcp_server").path_join("discovery");

	Ref<DirAccess> da = DirAccess::open(discovery_dir);
	if (da.is_null()) {
		return; // Directory doesn't exist yet, nothing to clean.
	}

	da->list_dir_begin();
	String file_name = da->get_next();
	while (!file_name.is_empty()) {
		if (!da->current_is_dir() && file_name.ends_with(".json")) {
			String full_path = discovery_dir.path_join(file_name);
			Ref<FileAccess> f = FileAccess::open(full_path, FileAccess::READ);
			if (f.is_valid()) {
				String content = f->get_as_text();
				f.unref();

				JSON json;
				if (json.parse(content) == OK) {
					Dictionary data = json.get_data();
					if (data.has("pid")) {
						int file_pid = (int)data["pid"];
						if (!OS::get_singleton()->is_process_running(file_pid)) {
							da->remove(file_name);
							print_verbose("[MCP] Cleaned up stale discovery file: " + file_name);
						}
					}
				}
			}
		}
		file_name = da->get_next();
	}
	da->list_dir_end();
}

void MCPServerPlugin::write_discovery_file() {
	String discovery_dir = EditorPaths::get_singleton()->get_data_dir().path_join("mcp_server").path_join("discovery");

	Error dir_err = DirAccess::make_dir_recursive_absolute(discovery_dir);
	if (dir_err != OK) {
		ERR_PRINT("[MCP] Failed to create discovery directory: " + discovery_dir);
		return;
	}

	Dictionary discovery;
	discovery["endpoint"] = "http://" + host + ":" + itos(port) + "/mcp";
	discovery["token"] = auth_token;
	discovery["pid"] = OS::get_singleton()->get_process_id();
	discovery["godot_version"] = GODOT_VERSION_FULL_CONFIG;
	discovery["project_path"] = ProjectSettings::get_singleton()->get_resource_path();
	discovery["project_name"] = (String)ProjectSettings::get_singleton()->get_setting("application/config/name", "Unknown");

	String json_content = JSON::stringify(discovery, "\t");

	// Write per-instance discovery file: discovery/<port>.json
	String file_path = get_discovery_file_path();
	{
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
		FileAccess::set_hidden_attribute(file_path, true);
#endif
	}

	// Write legacy discovery.json for backward compatibility.
	String legacy_path = get_legacy_discovery_file_path();
	{
		Ref<FileAccess> f = FileAccess::open(legacy_path, FileAccess::WRITE);
		if (f.is_valid()) {
			f->store_string(json_content);
			f.unref();

#ifndef WINDOWS_ENABLED
			FileAccess::set_unix_permissions(legacy_path,
					FileAccess::UNIX_READ_OWNER | FileAccess::UNIX_WRITE_OWNER);
#else
			FileAccess::set_hidden_attribute(legacy_path, true);
#endif
		}
	}

	print_verbose("[MCP] Discovery files written: " + file_path + " + legacy");
}

void MCPServerPlugin::delete_discovery_file() {
	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (da.is_null()) {
		return;
	}

	// Remove per-instance discovery file.
	String file_path = get_discovery_file_path();
	if (FileAccess::exists(file_path)) {
		da->remove(file_path);
	}

	// Remove legacy discovery.json if it belongs to this instance.
	String legacy_path = get_legacy_discovery_file_path();
	if (FileAccess::exists(legacy_path)) {
		Ref<FileAccess> f = FileAccess::open(legacy_path, FileAccess::READ);
		if (f.is_valid()) {
			String content = f->get_as_text();
			f.unref();

			JSON json;
			if (json.parse(content) == OK) {
				Dictionary data = json.get_data();
				if (data.has("pid")) {
					int file_pid = (int)data["pid"];
					if (file_pid == OS::get_singleton()->get_process_id()) {
						da->remove(legacy_path);
					}
				}
			}
		}
	}
}
