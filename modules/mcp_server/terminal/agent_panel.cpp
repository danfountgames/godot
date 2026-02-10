/**************************************************************************/
/*  agent_panel.cpp                                                       */
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

#ifdef MCP_TERMINAL_ENABLED

#include "agent_panel.h"

#include "terminal_widget.h"
#include "../mcp_server_plugin.h"

#include "core/io/json.h"
#include "core/os/os.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"

AgentPanel::AgentPanel() {
	_build_ui();
}

AgentPanel::~AgentPanel() {
}

void AgentPanel::_build_ui() {
	// --- Toolbar ---
	HBoxContainer *toolbar = memnew(HBoxContainer);
	add_child(toolbar);

	Label *title_label = memnew(Label);
	title_label->set_text("Agent: ");
	toolbar->add_child(title_label);

	launch_button = memnew(Button);
	launch_button->set_text("Launch Claude");
	launch_button->connect("pressed", callable_mp(this, &AgentPanel::_on_launch_pressed));
	toolbar->add_child(launch_button);

	stop_button = memnew(Button);
	stop_button->set_text("Stop");
	stop_button->set_disabled(true);
	stop_button->connect("pressed", callable_mp(this, &AgentPanel::_on_stop_pressed));
	toolbar->add_child(stop_button);

	clear_button = memnew(Button);
	clear_button->set_text("Clear");
	clear_button->connect("pressed", callable_mp(this, &AgentPanel::_on_clear_pressed));
	toolbar->add_child(clear_button);

	status_label = memnew(Label);
	status_label->set_text("Not running");
	status_label->set_h_size_flags(SIZE_EXPAND_FILL);
	status_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	toolbar->add_child(status_label);

	// --- Terminal ---
	terminal = memnew(TerminalWidget);
	terminal->set_v_size_flags(SIZE_EXPAND_FILL);
	terminal->set_h_size_flags(SIZE_EXPAND_FILL);
	add_child(terminal);
}

void AgentPanel::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_INTERNAL_PROCESS: {
			_update_status();
		} break;
	}
}

void AgentPanel::_update_status() {
	if (claude_running) {
		if (terminal && terminal->is_process_running()) {
			status_label->set_text("Claude is running");
			launch_button->set_disabled(true);
			stop_button->set_disabled(false);
		} else {
			claude_running = false;
			status_label->set_text("Claude exited");
			launch_button->set_disabled(false);
			stop_button->set_disabled(true);
		}
	} else {
		launch_button->set_disabled(false);
		stop_button->set_disabled(true);
	}
}

String AgentPanel::_find_claude_binary() const {
	return "claude";
}

String AgentPanel::_build_mcp_config_json() const {
	// Build:
	// {
	//   "mcpServers": {
	//     "godot": {
	//       "type": "http",
	//       "url": "http://<host>:<port>/mcp",
	//       "headers": {
	//         "Authorization": "Bearer <token>"
	//       }
	//     }
	//   }
	// }

	String host = server_plugin->get_host();
	int port = server_plugin->get_port();
	String token = server_plugin->get_auth_token();

	Dictionary headers;
	if (!token.is_empty()) {
		headers["Authorization"] = "Bearer " + token;
	}

	Dictionary godot_server;
	godot_server["type"] = "http";
	godot_server["url"] = "http://" + host + ":" + itos(port) + "/mcp";
	if (!headers.is_empty()) {
		godot_server["headers"] = headers;
	}

	Dictionary mcp_servers;
	mcp_servers["godot"] = godot_server;

	Dictionary config;
	config["mcpServers"] = mcp_servers;

	return JSON::stringify(config);
}

Vector<String> AgentPanel::_build_claude_args() const {
	Vector<String> args;

	String mcp_config = _build_mcp_config_json();
	args.push_back("--mcp-config");
	args.push_back(mcp_config);
	args.push_back("--strict-mcp-config");

	return args;
}

Vector<String> AgentPanel::_build_claude_env() const {
	Vector<String> env;

	// Inherit important environment variables from the current process.
	const char *inherit_vars[] = {
		"PATH",
		"HOME",
		"USER",
		"SHELL",
		"LANG",
		"LC_ALL",
		"LC_CTYPE",
		"XDG_RUNTIME_DIR",
		"XDG_DATA_HOME",
		"XDG_CONFIG_HOME",
		"DISPLAY",
		"WAYLAND_DISPLAY",
		"SSH_AUTH_SOCK",
		nullptr
	};

	for (int i = 0; inherit_vars[i] != nullptr; i++) {
		String key = inherit_vars[i];
		String val = OS::get_singleton()->get_environment(key);
		if (!val.is_empty()) {
			env.push_back(key + "=" + val);
		}
	}

	// Always set TERM so the terminal emulator works correctly.
	env.push_back("TERM=xterm-256color");

	return env;
}

void AgentPanel::_on_launch_pressed() {
	if (!server_plugin || !server_plugin->is_started()) {
		status_label->set_text("MCP server not running!");
		return;
	}

	String binary = _find_claude_binary();
	Vector<String> args = _build_claude_args();
	Vector<String> env = _build_claude_env();

	bool ok = terminal->start_process(binary, args, env);
	if (ok) {
		claude_running = true;
		status_label->set_text("Claude is starting...");
		set_process_internal(true);
	} else {
		status_label->set_text("Failed to launch Claude");
	}
}

void AgentPanel::_on_stop_pressed() {
	terminal->stop_process();
	claude_running = false;
	status_label->set_text("Stopped");
}

void AgentPanel::_on_clear_pressed() {
	if (claude_running) {
		return; // Don't clear while a process is running.
	}
	status_label->set_text("Cleared");
}

void AgentPanel::_bind_methods() {
}

#endif // MCP_TERMINAL_ENABLED
