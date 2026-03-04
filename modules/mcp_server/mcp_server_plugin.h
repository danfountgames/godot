/**************************************************************************/
/*  mcp_server_plugin.h                                                   */
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

#include "mcp_debugger_bridge.h"
#include "mcp_protocol.h"

#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "editor/plugins/editor_plugin.h"

class Script;
class TabContainer;

#ifdef TOOLS_ENABLED
class MCPStatusPanel;
class MCPTestPanel;
#ifdef MCP_TERMINAL_ENABLED
class AgentPanel;
#endif
#endif

class MCPServerPlugin : public EditorPlugin {
	GDCLASS(MCPServerPlugin, EditorPlugin)

private:
	MCPProtocol *protocol = nullptr;
	Ref<MCPDebuggerBridge> debugger_bridge;

	Thread thread;
	SafeFlag thread_running;
	bool start_attempted = false;
	bool started = false;
	bool use_thread = true;

	String host = MCP_DEFAULT_HOST;
	int port = MCP_DEFAULT_PORT;
	String auth_token;

#ifdef TOOLS_ENABLED
	TabContainer *ai_tab_container = nullptr;
	MCPStatusPanel *status_panel = nullptr;
	MCPTestPanel *test_panel = nullptr;

	// Phase 4: Editor integration.
	String current_script_path; // Tracked via script editor signal.
	void _on_script_changed(const Ref<Script> &p_script);
	void _on_run_current_test();
	void _on_run_all_tests();
	void _on_rerun_failed_tests();

#ifdef MCP_TERMINAL_ENABLED
	Vector<AgentPanel *> agent_panels;
	Control *new_tab_placeholder = nullptr;
	int agent_counter = 0;
	bool _closing_tab = false; // Guard: prevents _on_tab_changed from creating tabs during close.

	void _create_agent_tab();
	void _on_tab_changed(int p_tab);
	void _on_tab_close_pressed(int p_tab);
	void _deferred_free_panel(ObjectID p_id);
	void _on_agent_title_changed(const String &p_title);
	void _update_close_buttons();
	void _update_tab_icons();
#endif
#endif

	static void thread_main(void *p_userdata);

	void start();
	void stop();

	// Discovery file for MCP clients to auto-detect the server.
	void write_discovery_file();
	void delete_discovery_file();
	String get_discovery_file_path() const;
	String get_legacy_discovery_file_path() const;
	void cleanup_stale_discovery_files();

	void _notification(int p_what);

protected:
	static void _bind_methods();

public:
	MCPServerPlugin();
	~MCPServerPlugin();

	// Main screen plugin overrides.
	bool has_main_screen() const override { return true; }
	virtual String get_plugin_name() const override { return "AI"; }
	virtual void make_visible(bool p_visible) override;

	MCPProtocol *get_protocol() { return protocol; }
	Ref<MCPDebuggerBridge> get_debugger_bridge() { return debugger_bridge; }

#ifdef MCP_TERMINAL_ENABLED
	// Called by AgentPanel when its runtime toggle changes.
	// Enforces mutual exclusivity: only one agent tab may have runtime tools
	// enabled at a time. Updates tab icons to reflect the active agent.
	void on_agent_runtime_changed(AgentPanel *p_panel, bool p_enabled);

	// Called by AgentPanel when its editor controls toggle changes.
	// Enforces mutual exclusivity: only one agent tab may have editor controls
	// enabled at a time. Updates tab icons.
	void on_agent_editor_changed(AgentPanel *p_panel, bool p_enabled);
#endif

	// Called by the panel's Start/Stop button.
	void toggle_server();

	// Debug mode — queried by AgentPanel when building CLI args.
	bool is_debug_mode_enabled() const;

	// Expose host/port/token for the panels.
	String get_host() const { return host; }
	int get_port() const { return port; }
	String get_auth_token() const { return auth_token; }
	bool is_started() const { return started; }
};
