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
#include "../mcp_protocol.h"
#include "../mcp_server_plugin.h"
#include "../agent_prompts.gen.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/version.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/scroll_bar.h"
#include "scene/gui/scroll_container.h"

AgentPanel::AgentPanel() {
	_build_ui();
}

AgentPanel::~AgentPanel() {
}

void AgentPanel::_build_ui() {
	// Toolbar row with runtime toggle.
	HBoxContainer *toolbar = memnew(HBoxContainer);
	add_child(toolbar);

	runtime_toggle = memnew(CheckBox);
	runtime_toggle->set_text("Runtime tools");
	runtime_toggle->set_tooltip_text("Allow this agent to start/stop and control the running game.");
	runtime_toggle->set_pressed(false);
	runtime_toggle->connect("toggled", callable_mp(this, &AgentPanel::_on_runtime_toggle_changed));
	toolbar->add_child(runtime_toggle);

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

	// Use the per-agent token (registered with MCPProtocol) instead of the
	// global auth token. This lets the server identify which agent panel
	// owns each MCP session and apply per-agent permissions.
	Dictionary headers;
	if (!agent_token.is_empty()) {
		headers["Authorization"] = "Bearer " + agent_token;
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

// ---------------------------------------------------------------------------
// System Prompt — lightweight context appended to Claude Code's default
// ---------------------------------------------------------------------------
// NOT an agent persona. Just tells Claude Code about the Godot environment
// and the MCP tools available. The actual specialized agents are subagents
// (godot-builder, godot-game-player) defined in _build_agents_json().
//
// The prompt text lives in prompts/system_prompt.txt and is compiled into
// _agent_prompt_system_prompt via agent_prompts.gen.h at build time.
// Three template markers are replaced at runtime:
//   {{GODOT_VERSION}}  — full engine version string
//   {{PROJECT_NAME}}   — application/config/name from project settings
//   {{PROJECT_PATH}}   — res:// resource path
// ---------------------------------------------------------------------------

String AgentPanel::_build_system_prompt() const {
	String project_name = (String)ProjectSettings::get_singleton()->get_setting(
			"application/config/name", "Untitled");
	String project_path = ProjectSettings::get_singleton()->get_resource_path();

	String p = String(_agent_prompt_system_prompt);
	p = p.replace("{{GODOT_VERSION}}", GODOT_VERSION_FULL_CONFIG);
	p = p.replace("{{PROJECT_NAME}}", project_name);
	p = p.replace("{{PROJECT_PATH}}", project_path);
	return p;
}

// ---------------------------------------------------------------------------
// Subagent definitions passed via --agents CLI flag
// ---------------------------------------------------------------------------
//
// Prompt text for each agent lives in prompts/agent_*.txt, compiled into
// _agent_prompt_agent_* constants via agent_prompts.gen.h at build time.
//
// godot-planner      — plans architecture, scene/node structure, implementation
// godot-builder      — builds game features: scripts, scenes, UI, gameplay
// godot-game-player  — launches, tests, and debugs the running game
// godot-refactor     — code health guardian: splits monoliths, enforces KISS
//
// Typical flow: planner → builder → game-player (test).
// Periodic:     refactor (every 3 cycles or when files are bloated).
// ---------------------------------------------------------------------------

String AgentPanel::_build_agents_json() const {
	Dictionary agents;

	// ── godot-planner ─────────────────────────────────────────────────
	{
		Dictionary def;
		def["description"] = "Godot project architect and implementation planner. "
							 "Use PROACTIVELY before building anything non-trivial: new features, "
							 "refactors, scene reorganization, or multi-file changes. "
							 "MUST be used when the user describes a feature, asks 'how should I...', "
							 "or when the scope of work spans more than two files. "
							 "Returns a concrete plan with scene tree layout, script responsibilities, "
							 "signal wiring, and @export surface before any code is written.";
		def["prompt"] = _agent_prompt_agent_planner;
		agents["godot-planner"] = def;
	}

	// ── godot-builder ──────────────────────────────────────────────────
	{
		Dictionary def;
		def["model"] = "sonnet";
		def["permissionMode"] = "acceptEdits";
		def["description"] = "Builds game features: scripts, scenes, UI, gameplay logic. "
							 "Use PROACTIVELY when the user wants new game functionality, "
							 "bug fixes, UI improvements, or gameplay features implemented. "
							 "Follows scene-first architecture — creates .tscn files, uses "
							 "@export properties, and wires everything up. "
							 "Debug introspection is automatic — no game-side code needed.";
		def["prompt"] = _agent_prompt_agent_builder;
		agents["godot-builder"] = def;
	}

	// ── godot-game-player ──────────────────────────────────────────────
	{
		Dictionary def;
		def["permissionMode"] = "acceptEdits";
		def["description"] = "Owns the running Godot game. Use PROACTIVELY for ANY runtime question: "
							 "play-testing, bug hunting, performance, UI flow, balance tuning, "
							 "or answering 'what happens when...'. MUST be used when the user asks "
							 "about game behavior, reports a bug, or wants something tested. "
							 "Can inject debug logging, create test scenes, and iterate independently until solved.";
		def["prompt"] = _agent_prompt_agent_game_player;
		agents["godot-game-player"] = def;
	}

	// ── godot-refactor ────────────────────────────────────────────────
	{
		Dictionary def;
		def["model"] = "sonnet";
		def["permissionMode"] = "acceptEdits";
		def["description"] = "Code health guardian. Splits monolithic files, extracts "
							 "duplicated patterns, enforces single-responsibility. "
							 "Use after every 3 build cycles, at the end of a session, "
							 "or when any file feels too large or has mixed responsibilities. "
							 "NEVER changes behavior — only structure. Validates everything.";
		def["prompt"] = _agent_prompt_agent_refactor;
		agents["godot-refactor"] = def;
	}

	return JSON::stringify(agents);
}

Vector<String> AgentPanel::_build_claude_args() const {
	Vector<String> args;

	// MCP server configuration written to a temp file.
	if (!mcp_config_path.is_empty()) {
		args.push_back("--mcp-config");
		args.push_back(mcp_config_path);
		args.push_back("--strict-mcp-config");
	}

	// Pre-authorize all tools from the Godot MCP server.
	args.push_back("--allowedTools");
	args.push_back("mcp__godot__*");

	// Lightweight context about the Godot environment and available subagents.
	String system_prompt = _build_system_prompt();
	args.push_back("--append-system-prompt");
	args.push_back(system_prompt);

	// Subagents: planner, builder, game-player, refactor.
	String agents_json = _build_agents_json();
	args.push_back("--agents");
	args.push_back(agents_json);

	// Experimental agent teams: in-process mode for embedded terminal.
	// Allows multiple Claude Code instances to coordinate via shared task lists.
	args.push_back("--teammate-mode");
	args.push_back("in-process");

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

	// Enable experimental agent teams (research preview).
	env.push_back("CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS=1");

	return env;
}

void AgentPanel::launch() {
	if (!server_plugin || !server_plugin->is_started()) {
		return;
	}

	// Generate a unique per-agent token and register it with the MCP server.
	// This token is used as the Bearer auth token in the MCP config so the
	// server can identify this agent's session and apply permissions.
	{
		agent_token = "agent_";
		for (int i = 0; i < 16; i++) {
			agent_token += String::num_int64(Math::random(0, 255), 16).lpad(2, "0");
		}
	}

	MCPProtocol *protocol = MCPProtocol::get_singleton();
	if (protocol) {
		protocol->register_agent_token(agent_token);
		// Apply the current toggle state (in case it was changed before launch).
		protocol->set_runtime_tools_enabled(agent_token, runtime_toggle->is_pressed());
	}

	// Write MCP config to a temp file (--mcp-config expects a file path).
	{
		String mcp_json = _build_mcp_config_json();
		mcp_config_path = OS::get_singleton()->get_cache_path().path_join("godot_mcp_" + agent_token + ".json");
		Ref<FileAccess> f = FileAccess::open(mcp_config_path, FileAccess::WRITE);
		if (f.is_valid()) {
			f->store_string(mcp_json);
		} else {
			ERR_PRINT("[MCP] Failed to write MCP config to " + mcp_config_path);
			mcp_config_path = String();
		}
	}

	String binary = _find_claude_binary();
	Vector<String> args = _build_claude_args();
	Vector<String> env = _build_claude_env();

	// Launch Claude Code in the project directory so it picks up CLAUDE.md
	// and has the correct working directory for file operations.
	String project_dir = ProjectSettings::get_singleton()->get_resource_path();

	bool ok = terminal->start_process(binary, args, env, project_dir);
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

	// Unregister the agent token so stale sessions don't accumulate.
	if (!agent_token.is_empty()) {
		MCPProtocol *protocol = MCPProtocol::get_singleton();
		if (protocol) {
			protocol->unregister_agent_token(agent_token);
		}
		agent_token = String();
	}

	// Remove the temp MCP config file.
	if (!mcp_config_path.is_empty()) {
		Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (da.is_valid()) {
			da->remove(mcp_config_path);
		}
		mcp_config_path = String();
	}
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

void AgentPanel::_on_runtime_toggle_changed(bool p_enabled) {
	if (agent_token.is_empty()) {
		return; // No agent launched yet.
	}
	MCPProtocol *protocol = MCPProtocol::get_singleton();
	if (protocol) {
		protocol->set_runtime_tools_enabled(agent_token, p_enabled);
	}
	// Notify server plugin for mutual exclusivity across agent tabs.
	if (server_plugin) {
		server_plugin->on_agent_runtime_changed(this, p_enabled);
	}
}

bool AgentPanel::is_runtime_tools_enabled() const {
	return runtime_toggle && runtime_toggle->is_pressed();
}

void AgentPanel::set_runtime_tools_enabled(bool p_enabled) {
	if (runtime_toggle) {
		// Use set_pressed_no_signal to avoid re-triggering _on_runtime_toggle_changed.
		runtime_toggle->set_pressed_no_signal(p_enabled);
	}
	// Apply to MCPProtocol if agent is running.
	if (!agent_token.is_empty()) {
		MCPProtocol *protocol = MCPProtocol::get_singleton();
		if (protocol) {
			protocol->set_runtime_tools_enabled(agent_token, p_enabled);
		}
	}
}

void AgentPanel::_bind_methods() {
	ADD_SIGNAL(MethodInfo("title_changed", PropertyInfo(Variant::STRING, "title")));
}

#endif // MCP_TERMINAL_ENABLED
