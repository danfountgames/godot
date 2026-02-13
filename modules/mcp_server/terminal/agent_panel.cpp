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
#include "scene/gui/button.h"
#include "scene/gui/scroll_bar.h"
#include "scene/gui/scroll_container.h"

AgentPanel::AgentPanel() {
	_build_ui();
}

AgentPanel::~AgentPanel() {
}

void AgentPanel::_build_ui() {
	// Terminal container (holds scroll + to-bottom button).
	terminal_container = memnew(Control);
	terminal_container->set_v_size_flags(SIZE_EXPAND_FILL);
	terminal_container->set_h_size_flags(SIZE_EXPAND_FILL);
	add_child(terminal_container);

	// ScrollContainer fills the terminal_container.
	scroll_container = memnew(ScrollContainer);
	scroll_container->set_anchors_preset(Control::PRESET_FULL_RECT);
	scroll_container->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	terminal_container->add_child(scroll_container);

	// Terminal widget inside the scroll container.
	terminal = memnew(TerminalWidget);
	terminal->set_h_size_flags(SIZE_EXPAND_FILL);
	scroll_container->add_child(terminal);
	terminal->set_scroll_container(scroll_container);

	// "To Bottom" button anchored bottom-right, initially hidden.
	to_bottom_button = memnew(Button);
	to_bottom_button->set_text("To Bottom");
	to_bottom_button->set_visible(false);
	to_bottom_button->set_anchors_preset(Control::PRESET_BOTTOM_RIGHT);
	to_bottom_button->set_grow_direction_preset(Control::PRESET_BOTTOM_RIGHT);
	to_bottom_button->set_offset(SIDE_LEFT, -120);
	to_bottom_button->set_offset(SIDE_TOP, -32);
	to_bottom_button->set_offset(SIDE_RIGHT, -8);
	to_bottom_button->set_offset(SIDE_BOTTOM, -8);
	terminal_container->add_child(to_bottom_button);

	// Connect signals.
	to_bottom_button->connect("pressed", callable_mp(this, &AgentPanel::_on_to_bottom_pressed));
	scroll_container->get_v_scroll_bar()->connect("value_changed", callable_mp(this, &AgentPanel::_on_scroll_changed));
	scroll_container->connect("resized", callable_mp(this, &AgentPanel::_on_scroll_container_resized));
}

void AgentPanel::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_INTERNAL_PROCESS: {
			_update_status();
		} break;
	}
}

void AgentPanel::_update_status() {
	if (claude_running && terminal && !terminal->is_process_running()) {
		claude_running = false;
	}

	// Poll terminal title and emit signal on change.
	if (terminal) {
		String title = terminal->get_emulator()->get_title();
		if (title != current_title) {
			current_title = title;
			emit_signal("title_changed", current_title);
		}
	}
}

String AgentPanel::_find_claude_binary() const {
	return "claude";
}

String AgentPanel::_build_mcp_config_json() const {
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

	// Pre-authorize all tools from the Godot MCP server.
	args.push_back("--allowedTools");
	args.push_back("mcp__godot__*");

	return args;
}

Vector<String> AgentPanel::_build_claude_env() const {
	Vector<String> env;

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

	env.push_back("TERM=xterm-256color");

	return env;
}

void AgentPanel::launch() {
	if (!server_plugin || !server_plugin->is_started()) {
		return;
	}

	String binary = _find_claude_binary();
	Vector<String> args = _build_claude_args();
	Vector<String> env = _build_claude_env();

	bool ok = terminal->start_process(binary, args, env);
	if (ok) {
		claude_running = true;
		set_process_internal(true);
	}
}

void AgentPanel::stop() {
	if (terminal) {
		terminal->stop_process();
	}
	claude_running = false;
}

void AgentPanel::_on_to_bottom_pressed() {
	if (terminal) {
		terminal->scroll_to_bottom();
	}
}

void AgentPanel::_on_scroll_changed(double p_value) {
	if (!terminal || !scroll_container) {
		return;
	}
	ScrollBar *vbar = scroll_container->get_v_scroll_bar();
	bool at_bottom = vbar->get_value() >= vbar->get_max() - scroll_container->get_size().y - 1;
	to_bottom_button->set_visible(!at_bottom);

	// Only unstick on genuine user scrolling, not our own programmatic scrolls.
	if (!at_bottom && !terminal->is_programmatic_scroll()) {
		terminal->unstick_from_bottom();
	}
}

void AgentPanel::_on_scroll_container_resized() {
	if (terminal) {
		terminal->update_pty_size();
	}
}

void AgentPanel::_bind_methods() {
	ADD_SIGNAL(MethodInfo("title_changed", PropertyInfo(Variant::STRING, "title")));
}

#endif // MCP_TERMINAL_ENABLED
