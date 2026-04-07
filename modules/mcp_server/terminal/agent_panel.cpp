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
#include "core/crypto/crypto_core.h"
#include "core/input/input_map.h"
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

namespace {
String agent_toml_string(const String &p_value) {
	String out = "\"";
	for (int i = 0; i < p_value.length(); i++) {
		char32_t c = p_value[i];
		switch (c) {
			case '\\':
				out += "\\\\";
				break;
			case '"':
				out += "\\\"";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				out += String::chr(c);
				break;
		}
	}
	out += "\"";
	return out;
}
} // namespace

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

	editor_toggle = memnew(CheckBox);
	editor_toggle->set_text("Editor controls");
	editor_toggle->set_tooltip_text("Allow this agent to navigate and modify scenes in the editor.");
	editor_toggle->set_pressed(false);
	editor_toggle->connect("toggled", callable_mp(this, &AgentPanel::_on_editor_toggle_changed));
	toolbar->add_child(editor_toggle);

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
	// FOCUS_NONE prevents the button from stealing keyboard focus from the terminal.
	to_bottom_button = memnew(Button);
	to_bottom_button->set_text("To Bottom");
	to_bottom_button->set_visible(false);
	to_bottom_button->set_focus_mode(FOCUS_NONE);
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

		case NOTIFICATION_PREDELETE: {
			// Disconnect all signals that target this panel before children
			// are freed — prevents deferred callbacks from accessing freed
			// terminal/scroll_container pointers.
			set_process_internal(false);

			if (to_bottom_button && to_bottom_button->is_connected("pressed", callable_mp(this, &AgentPanel::_on_to_bottom_pressed))) {
				to_bottom_button->disconnect("pressed", callable_mp(this, &AgentPanel::_on_to_bottom_pressed));
			}
			if (scroll_container) {
				ScrollBar *vbar = scroll_container->get_v_scroll_bar();
				if (vbar && vbar->is_connected("value_changed", callable_mp(this, &AgentPanel::_on_scroll_changed))) {
					vbar->disconnect("value_changed", callable_mp(this, &AgentPanel::_on_scroll_changed));
				}
				if (scroll_container->is_connected("resized", callable_mp(this, &AgentPanel::_on_scroll_container_resized))) {
					scroll_container->disconnect("resized", callable_mp(this, &AgentPanel::_on_scroll_container_resized));
				}
			}
			if (runtime_toggle && runtime_toggle->is_connected("toggled", callable_mp(this, &AgentPanel::_on_runtime_toggle_changed))) {
				runtime_toggle->disconnect("toggled", callable_mp(this, &AgentPanel::_on_runtime_toggle_changed));
			}
			if (editor_toggle && editor_toggle->is_connected("toggled", callable_mp(this, &AgentPanel::_on_editor_toggle_changed))) {
				editor_toggle->disconnect("toggled", callable_mp(this, &AgentPanel::_on_editor_toggle_changed));
			}

			// Null out child pointers so any stray deferred calls bail out.
			terminal = nullptr;
			scroll_container = nullptr;
			to_bottom_button = nullptr;
			runtime_toggle = nullptr;
			editor_toggle = nullptr;
		} break;
	}
}

void AgentPanel::_update_status() {
	if (agent_running && terminal && !terminal->is_process_running()) {
		agent_running = false;
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

String AgentPanel::_find_codex_binary() const {
	return "codex";
}

String AgentPanel::_build_mcp_url() const {
	String host = server_plugin->get_host();
	int port = server_plugin->get_port();

	String url = "http://" + host + ":" + itos(port) + "/mcp";
	if (!agent_token.is_empty()) {
		url += "/" + agent_token;
	}
	return url;
}

String AgentPanel::_build_mcp_config_json() const {
	// Embed the agent token in the URL path instead of an Authorization
	// header. This avoids triggering OAuth discovery in Claude Code, which
	// interprets Authorization: Bearer as a signal to probe
	// /.well-known/oauth-* endpoints and attempt dynamic client registration.
	String url = _build_mcp_url();

	Dictionary godot_server;
	godot_server["type"] = "http";
	godot_server["url"] = url;

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

	// Inject game architecture docs if they exist in the project.
	String arch_path = "res://docs/architecture.md";
	String arch_content;
	if (FileAccess::exists(arch_path)) {
		arch_content = FileAccess::get_file_as_string(arch_path);
	} else {
		arch_content = "No architecture docs found. Use project/init_docs to create them when the game has enough structure.";
	}
	p = p.replace("{{PROJECT_ARCHITECTURE}}", arch_content);

	// Inject input map summary so all agents know the controls.
	String input_map_text = _build_input_map_summary();
	p = p.replace("{{INPUT_MAP}}", input_map_text);

	return p;
}

// ---------------------------------------------------------------------------
// Input Map Summary — compact text of project input actions for agent context
// ---------------------------------------------------------------------------

String AgentPanel::_build_input_map_summary() const {
	InputMap *im = InputMap::get_singleton();
	if (!im) {
		return "Input map not available.";
	}

	TypedArray<StringName> actions = im->get_actions();

	// Filter out built-in ui_* actions — keep only game actions.
	String text;
	int count = 0;
	for (int i = 0; i < actions.size(); i++) {
		StringName action = actions[i];
		String name = String(action);
		if (name.begins_with("ui_")) {
			continue; // Skip built-in UI actions (ui_accept, ui_cancel, etc.)
		}

		const List<Ref<InputEvent>> *events = im->action_get_events(action);
		if (!events) {
			continue;
		}

		String events_text;
		for (const Ref<InputEvent> &event : *events) {
			if (event.is_null()) {
				continue;
			}
			if (!events_text.is_empty()) {
				events_text += ", ";
			}
			events_text += event->as_text();
		}

		if (!events_text.is_empty()) {
			text += "  " + name + ": " + events_text + "\n";
			count++;
		}
	}

	if (count == 0) {
		return "No custom input actions defined. Use project/get_input_map after adding inputs.";
	}
	return text;
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

	// ── godot-designer ────────────────────────────────────────────────
	{
		Dictionary def;
		def["model"] = "sonnet";
		def["permissionMode"] = "acceptEdits";
		def["description"] = "Game UI and visual interface designer. "
							 "Use PROACTIVELY when the user asks to build menus, HUDs, "
							 "dialog systems, inventory screens, settings panels, or any "
							 "player-facing interface. Creates distinctive, production-grade "
							 ".tscn scenes with Theme resources, styled controls, shaders, "
							 "and animations that avoid default Godot gray-box aesthetics.";
		def["prompt"] = _agent_prompt_agent_designer;
		agents["godot-designer"] = def;
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

	// Debug mode — queried from the MCP Status panel (global setting).
	if (server_plugin && server_plugin->is_debug_mode_enabled()) {
		args.push_back("--debug");
	}

	// Skip permissions — runs Claude with --dangerously-skip-permissions.
	if (server_plugin && server_plugin->is_dangerously_mode_enabled()) {
		args.push_back("--dangerously-skip-permissions");
	}

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

Vector<String> AgentPanel::_build_codex_args() const {
	Vector<String> args;

	String project_dir = ProjectSettings::get_singleton()->get_resource_path();

	args.push_back("--no-alt-screen");
	if (!project_dir.is_empty()) {
		args.push_back("--cd");
		args.push_back(project_dir);
	}

	args.push_back("--enable");
	args.push_back("multi_agent");

	String mcp_url = "http://" + server_plugin->get_host() + ":" + itos(server_plugin->get_port()) + "/mcp";
	args.push_back("-c");
	args.push_back("mcp_servers.godot.url=" + agent_toml_string(mcp_url));
	args.push_back("-c");
	args.push_back("mcp_servers.godot.bearer_token_env_var=\"GODOT_MCP_AGENT_TOKEN\"");

	String codex_agents_dir = project_dir.path_join(".codex").path_join("agents");
	const char *agent_names[] = {
		"godot-planner",
		"godot-builder",
		"godot-designer",
		"godot-game-player",
		"godot-refactor",
		nullptr
	};
	for (int i = 0; agent_names[i] != nullptr; i++) {
		String name = agent_names[i];
		String agent_path = codex_agents_dir.path_join(name + ".toml");
		if (FileAccess::exists(agent_path)) {
			args.push_back("-c");
			args.push_back("agents." + name + ".config_file=" + agent_toml_string(agent_path));
		}
	}

	// Codex has no Claude-style --debug flag. The debug checkbox maps to
	// verbose logging in _build_codex_env().
	if (server_plugin && server_plugin->is_dangerously_mode_enabled()) {
		args.push_back("--dangerously-bypass-approvals-and-sandbox");
	} else {
		args.push_back("--full-auto");
	}

	return args;
}

Vector<String> AgentPanel::_build_agent_env() const {
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
		"CODEX_HOME",
		"OPENAI_API_KEY",
		"OPENAI_BASE_URL",
		"ANTHROPIC_API_KEY",
		"ANTHROPIC_BASE_URL",
		"HTTP_PROXY",
		"HTTPS_PROXY",
		"ALL_PROXY",
		"NO_PROXY",
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

Vector<String> AgentPanel::_build_claude_env() const {
	Vector<String> env = _build_agent_env();

	// Enable experimental agent teams (research preview).
	env.push_back("CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS=1");

	return env;
}

Vector<String> AgentPanel::_build_codex_env() const {
	Vector<String> env = _build_agent_env();

	if (!agent_token.is_empty()) {
		env.push_back("GODOT_MCP_AGENT_TOKEN=" + agent_token);
	}

	if (server_plugin && server_plugin->is_debug_mode_enabled()) {
		env.push_back("RUST_LOG=codex_core=debug,codex_tui=debug,codex_cli=debug,rmcp=debug");
		env.push_back("RUST_BACKTRACE=1");
		env.push_back("CODEX_LOG=debug");
	}

	return env;
}

void AgentPanel::launch() {
	if (!server_plugin || !server_plugin->is_started()) {
		return;
	}

	String agent_cli = server_plugin->get_agent_cli().to_lower();
	if (agent_cli == "codex") {
		server_plugin->sync_project_agent_files();
	}

	// Generate a unique per-agent token and register it with the MCP server.
	// This token is embedded in the MCP URL path so the server can identify
	// this agent's session and apply permissions.
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
		protocol->set_editor_controls_enabled(agent_token, editor_toggle->is_pressed());
	}

	// Claude uses --mcp-config, which expects a file path. Codex receives its
	// HTTP MCP server config via -c mcp_servers.godot.url=...
	if (agent_cli == "claude") {
		String mcp_json = _build_mcp_config_json();
		mcp_config_path = OS::get_singleton()->get_cache_path().path_join("godot_mcp_" + agent_token + ".json");
		Ref<FileAccess> f = FileAccess::open(mcp_config_path, FileAccess::WRITE);
		if (f.is_valid()) {
			f->store_string(mcp_json);
		} else {
			ERR_PRINT("[MCP] Failed to write MCP config to " + mcp_config_path);
			mcp_config_path = String();
		}
	} else {
		mcp_config_path = String();
	}

	String binary = agent_cli == "codex" ? _find_codex_binary() : _find_claude_binary();
	Vector<String> args = agent_cli == "codex" ? _build_codex_args() : _build_claude_args();
	Vector<String> env = agent_cli == "codex" ? _build_codex_env() : _build_claude_env();

	// Launch in the project directory so the selected CLI picks up project
	// instruction files and has the correct working directory.
	String project_dir = ProjectSettings::get_singleton()->get_resource_path();

	bool ok = terminal->start_process(binary, args, env, project_dir);
	if (ok) {
		agent_running = true;
		set_process_internal(true);
	}
}

void AgentPanel::stop() {
	// Stop internal processing first — prevents _update_status() from accessing
	// terminal/scroll_container after the PTY is torn down.
	set_process_internal(false);

	if (terminal) {
		// Stop the terminal's own internal processing too — it runs
		// independently of the AgentPanel's and would keep polling the
		// PTY / scrolling to bottom on a dead session.
		terminal->set_process_internal(false);
		terminal->stop_process();
	}
	agent_running = false;

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
		terminal->grab_focus();
	}
}

void AgentPanel::_on_scroll_changed(double p_value) {
	if (!terminal || !scroll_container || !to_bottom_button) {
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

void AgentPanel::_inject_pty_message(const String &p_message) {
	if (!terminal || !terminal->is_process_running()) {
		return;
	}
	CharString utf8 = (p_message + "\n").utf8();
	terminal->get_pty()->write_pty((const uint8_t *)utf8.get_data(), utf8.length());
}

void AgentPanel::_on_runtime_toggle_changed(bool p_enabled) {
	// Return focus to the terminal so the user can keep typing.
	if (terminal) {
		terminal->grab_focus();
	}
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
	// Inform the AI of the permission change.
	if (agent_running) {
		_inject_pty_message(p_enabled
				? "[Permission] Runtime tools enabled. You can now use runtime/*, debug/*, and automation/* tools."
				: "[Permission] Runtime tools disabled. runtime/*, debug/*, and automation/* tools are no longer available.");
	}
}

void AgentPanel::_on_editor_toggle_changed(bool p_enabled) {
	// Return focus to the terminal so the user can keep typing.
	if (terminal) {
		terminal->grab_focus();
	}
	if (agent_token.is_empty()) {
		return;
	}
	MCPProtocol *protocol = MCPProtocol::get_singleton();
	if (protocol) {
		protocol->set_editor_controls_enabled(agent_token, p_enabled);
	}
	if (server_plugin) {
		server_plugin->on_agent_editor_changed(this, p_enabled);
	}
	// Inform the AI of the permission change.
	if (agent_running) {
		_inject_pty_message(p_enabled
				? "[Permission] Editor controls enabled. You can now use scene/* tools."
				: "[Permission] Editor controls disabled. scene/* tools are no longer available.");
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

bool AgentPanel::is_editor_controls_enabled() const {
	return editor_toggle && editor_toggle->is_pressed();
}

void AgentPanel::set_editor_controls_enabled(bool p_enabled) {
	if (editor_toggle) {
		editor_toggle->set_pressed_no_signal(p_enabled);
	}
	if (!agent_token.is_empty()) {
		MCPProtocol *protocol = MCPProtocol::get_singleton();
		if (protocol) {
			protocol->set_editor_controls_enabled(agent_token, p_enabled);
		}
	}
}

void AgentPanel::_bind_methods() {
	ADD_SIGNAL(MethodInfo("title_changed", PropertyInfo(Variant::STRING, "title")));
}

#endif // MCP_TERMINAL_ENABLED
