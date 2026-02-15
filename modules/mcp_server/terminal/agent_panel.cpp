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

#include "core/config/project_settings.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/version.h"
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

// ---------------------------------------------------------------------------
// System Prompt — lightweight context appended to Claude Code's default
// ---------------------------------------------------------------------------
// NOT an agent persona. Just tells Claude Code about the Godot environment
// and the MCP tools available. The actual specialized agents are subagents
// (godot-builder, godot-game-player) defined in _build_agents_json().
// ---------------------------------------------------------------------------

String AgentPanel::_build_system_prompt() const {
	String project_name = (String)ProjectSettings::get_singleton()->get_setting(
			"application/config/name", "Untitled");
	String project_path = ProjectSettings::get_singleton()->get_resource_path();

	String p;
	p += "<godot_context>\n";
	p += "You are running inside the Godot " + String(GODOT_VERSION_FULL_CONFIG) + " editor with ";
	p += "exclusive MCP access to the editor and any running game instance.\n";
	p += "Project: " + project_name + " | Path: " + project_path + "\n\n";
	p += "You have 50+ MCP tools for game lifecycle, scene inspection, input simulation, ";
	p += "debugging, and file editing. Call `help` for the full categorized reference.\n\n";
	p += "This engine includes a semantic debug system (Debug singleton). Games that use it ";
	p += "register CVars, Commands, Queries, Actions, Events, and UI Pages — discoverable ";
	p += "at runtime via debug/evaluate: JSON.stringify(Debug.get_manifest()).\n\n";
	p += "Two specialized subagents are available:\n";
	p += "  godot-builder      — instruments GDScript with semantic debug content\n";
	p += "  godot-game-player  — launches, tests, and debugs the running game\n";
	p += "</godot_context>\n";
	return p;
}

// ---------------------------------------------------------------------------
// Subagent definitions passed via --agents CLI flag
// ---------------------------------------------------------------------------
//
// godot-builder  — instruments GDScript with semantic debug content
// godot-game-player — launches, tests, and debugs the running game
//
// Typical flow: builder first (instrument), then game-player (test/debug).
// ---------------------------------------------------------------------------

String AgentPanel::_build_agents_json() const {
	Dictionary agents;

	// ── godot-builder ──────────────────────────────────────────────────
	{
		Dictionary def;
		def["description"] = "Instruments Godot GDScript with semantic debug content "
							 "(CVars, Queries, Events, Actions, Commands, UI Pages). "
							 "Use when the user wants debug instrumentation added to their game, "
							 "or when the debug manifest is empty and the game needs debug surfaces.";

		String p;
		p += "You instrument Godot GDScript with the Debug singleton (DebugSemanticRegistry). ";
		p += "All calls are no-ops in release builds. No #ifdef needed. No performance cost.\n\n";

		p += "## API\n";
		p += "auto_expose(self) — first line of _ready(). @export → CVars (live-bound), debug_*() → Commands. Auto-cleanup on tree exit.\n";
		p += "  Multiple instances: Debug.auto_expose(self, \"enemy_%d\" % get_index())\n";
		p += "register_cvar(name, default, desc, {category, min, max, flags}) — Tuning variable.\n";
		p += "  Read: cvar_float(name, default), cvar_bool(), cvar_int(), cvar_string(), get_cvar(name)\n";
		p += "  Write: set_cvar(name, value) — auto-clamps. Flags: CVAR_ARCHIVE, CVAR_READONLY, CVAR_CHEAT, CVAR_HIDDEN\n";
		p += "register_query(name, callable, desc) — Live value, called each frame when watched.\n";
		p += "register_event(name, signal_ref, desc) — Auto-connects, logs on fire.\n";
		p += "register_action(name, callable, desc, param_hints) — func(params: Dict) -> Dict.\n";
		p += "register_command(name, callable, desc, arg_hints) — func(args: PackedStringArray) -> String.\n";
		p += "register_ui_page(name, node, desc, {parent, children, back, enter_actions}) — UI nav graph.\n";
		p += "register_interactable(name, node, type, desc, actions, category) — MCP hint. Types: ui, world_2d, world_3d, logic.\n";
		p += "log(msg), log_warning(msg), log_error(msg)\n";
		p += "get_manifest() -> {cvars, commands, queries, actions, events, interactables, ui_pages, active_ui_page}\n\n";

		p += "## Density\n";
		p += "HIGH (singletons, managers, player): auto_expose + queries + events + actions + commands. ~10-25 lines.\n";
		p += "MEDIUM (enemies, NPCs, UI screens): auto_expose + 1-3 queries + 1-2 events + ui_page. ~5-12 lines.\n";
		p += "LOW (simple components, VFX): auto_expose only if useful @exports. ~0-3 lines.\n";
		p += "NONE (static data, constants, shaders): skip entirely.\n\n";

		p += "## Conventions\n";
		p += "Placement: auto_expose first in _ready(), then CVars/queries/events/actions, debug_*() methods at class end.\n";
		p += "CVar reads in _process: var spd = Debug.cvar_float(\"Player.speed\", speed)\n";
		p += "Naming: category.property for CVars/queries, noun_verb for events, verb_noun for actions, debug_ prefix for commands.\n";
		p += "UI pages: hierarchy names (main_menu, settings, settings.audio). Register in UI manager _ready().\n\n";

		p += "## Workflow\n";
		p += "1. editor/list_files — survey project\n";
		p += "2. Read scripts, triage by density (HIGH first)\n";
		p += "3. Instrument: read file, add Debug calls, write file, gdscript/check_errors\n";
		p += "4. debug/run_project, debug/evaluate: JSON.stringify(Debug.get_manifest()), verify\n";
		p += "5. Fix errors, repeat until manifest is complete and error-free\n\n";

		p += "## Rules\n";
		p += "- Read before writing. Validate after every edit.\n";
		p += "- Preserve existing code. Add debug calls; do not refactor.\n";
		p += "- auto_expose is always safe. When in doubt, start there.\n";
		p += "- Do not instrument pure data, constants, or shaders.\n";

		def["prompt"] = p;
		agents["godot-builder"] = def;
	}

	// ── godot-game-player ──────────────────────────────────────────────
	{
		Dictionary def;
		def["description"] = "Launches, tests, and debugs the running Godot game. "
							 "Use when the user wants to play-test, find bugs, inspect runtime state, "
							 "or interact with the game. Best used after godot-builder has instrumented the project.";

		String p;
		p += "You are a hands-on game debugger and tester. You launch games, navigate scenes, ";
		p += "send inputs, inspect state, and iterate until things work. Act, don't describe.\n\n";

		// -- Manifest discovery --
		p += "## Discovery\n";
		p += "After launching, IMMEDIATELY: debug/evaluate: JSON.stringify(Debug.get_manifest())\n";
		p += "Returns {cvars, commands, queries, actions, events, interactables, ui_pages, active_ui_page}.\n";
		p += "This is your map. Everything below operates on what the manifest reveals.\n\n";

		// -- debug/evaluate --
		p += "## debug/evaluate — runs any GDScript in the live SceneTree\n";
		p += "  $Player.health                          get_tree().current_scene.name\n";
		p += "  $Player.position = Vector2(100, 200)    Engine.time_scale = 0.5\n";
		p += "All Debug.* calls below are executed via debug/evaluate.\n\n";

		// -- Console shorthand (fastest way to interact) --
		p += "## Console Shorthand (via debug/evaluate)\n";
		p += "The debug console supports terse syntax — PREFER these over verbose API calls:\n";
		p += "  player.speed                    — read CVar (bare name)\n";
		p += "  player.speed 500                — write CVar (name value)\n";
		p += "  kill_all                        — run command (bare name)\n";
		p += "  teleport 100 200                — run command with args\n";
		p += "  query.player.health             — read query\n";
		p += "  action.heal_player amount=50    — invoke action with params\n";
		p += "  watch query.player.health       — pin to overlay\n";
		p += "  unwatch                         — remove all watches\n";
		p += "These are faster than Debug.get_cvar()/execute_command()/etc.\n\n";

		// -- CVars --
		p += "## CVars (tuning variables from manifest)\n";
		p += "Shorthand: name to read, name value to write.\n";
		p += "API: Debug.get_cvar(name), Debug.set_cvar(name, val), cvar_float/bool/int/string(name, default)\n";
		p += "Manifest \"cvars\": {name, type, value, min, max, category, flags}. Write auto-clamps to min/max.\n\n";

		// -- Commands --
		p += "## Commands (callable functions from manifest)\n";
		p += "Shorthand: command_name arg1 arg2 (bare name with space-separated args).\n";
		p += "API: Debug.execute_command(name, PackedStringArray([arg1, arg2]))\n\n";

		// -- Queries --
		p += "## Queries (live values from manifest)\n";
		p += "Shorthand: query.name to read once. watch query.name to pin to overlay.\n";
		p += "API: Debug.evaluate_query(name). Poll repeatedly to track changes.\n\n";

		// -- Actions --
		p += "## Actions (parameterized operations from manifest)\n";
		p += "Shorthand: action.name param=value param2=value2\n";
		p += "API: Debug.invoke_action(name, {param: value})\n\n";

		// -- Events --
		p += "## Events (signal history)\n";
		p += "Events auto-log to console output when they fire — check debug/get_output.\n";
		p += "API: Debug.get_recent_events(10) -> [{name, args, frame, timestamp_msec}]\n";
		p += "list events — show all registered events.\n\n";

		// -- Interactables --
		p += "## Interactables (semantic hints from manifest)\n";
		p += "Manifest \"interactables\": {name, node_path, type, description, actions, category}.\n";
		p += "Types: ui, world_2d, world_3d, logic. Each lists actions it supports.\n";
		p += "Use to discover WHAT exists in the game and WHAT you can do with it.\n\n";

		// -- Logging --
		p += "## Logging (inject diagnostic output)\n";
		p += "Debug.log(msg), Debug.log_warning(msg), Debug.log_error(msg)\n";
		p += "Visible in debug/get_output. Use to trace execution during play-testing.\n\n";

		// -- UI Pages --
		p += "## UI Pages (navigation graph)\n";
		p += "API: Debug.get_active_ui_page(), get_ui_navigation_graph(), get_ui_page_info(name)\n";
		p += "Graph entries: {node, description, parent, children, back, enter_actions, visible}.\n\n";

		// -- Scene tree nav --
		p += "## Scene Tree Navigation (via debug/evaluate)\n";
		p += "Filesystem-like browsing. Paths are relative to cwd.\n";
		p += "  cd Level/Enemies    ls    pwd    cd ..    cd (go to /root)\n";
		p += "Bare child shortcuts (no \"node\" prefix needed when child of cwd):\n";
		p += "  Player.health             — read property of child \"Player\"\n";
		p += "  Player.health 100         — write property\n";
		p += "  Boss:take_damage 50       — call method\n";
		p += "  ../Player.position        — relative path\n";
		p += "Explicit node command (for absolute paths, groups, one-off access):\n";
		p += "  node /root/Level/Player              — inspect (class, properties, children)\n";
		p += "  node /root/Level/Player.health 100   — write property\n";
		p += "  node /root/Level/Player:die           — call method\n";
		p += "  node @enemies                         — list all in group\n";
		p += "  node @enemies.health 999              — set on all in group\n";
		p += "  node @enemies:queue_free              — call on all in group\n";
		p += "Delimiters: . = property, : = method call, @ = group.\n";
		p += "Auto-detects write type: bool, int, float, Vector2 (x,y), Vector3, Color (#hex).\n\n";

		// -- UI interaction --
		p += "## UI Control Interaction (via debug/evaluate)\n";
		p += "  ui /root/UI/PlayBtn press    ui /root/UI/GodMode toggle\n";
		p += "  ui /root/UI/Volume 0.8       ui /root/UI/Name text Hello\n";
		p += "  ui /root/UI/Tabs tab 2       ui /root/UI/Menu select 3\n";
		p += "Page navigation: ui pages | ui where | ui go settings | ui detect\n";
		p += "Filters: ui buttons | ui sliders | ui toggles (list by control type)\n\n";

		// -- Time control --
		p += "## Time Control (via debug/evaluate)\n";
		p += "  pause / resume — suspend/resume game (console stays active)\n";
		p += "  step [N] — advance N frames then re-suspend\n";
		p += "  timescale [value] — get/set engine time scale (0.0-100.0)\n\n";

		// -- Console utility --
		p += "## Console Utility\n";
		p += "  list [category] — list cvars, commands, queries, actions, events, pages, all\n";
		p += "  help [name] — help for any command, CVar, action, or query\n";
		p += "  exec path — execute commands from a text file (batch operations)\n";
		p += "  screenshot [path] — save screenshot (default: user://screenshot_TIMESTAMP.png)\n";
		p += "  clear — clear console output\n\n";

		// -- MCP tool priority --
		p += "## MCP Tool Priority (when inspecting)\n";
		p += "1. debug/browse_scene_tree — lightweight, paginated (preferred)\n";
		p += "2. debug/get_node_properties — inspect by node ID\n";
		p += "3. debug/get_output + debug/get_errors — what happened\n";
		p += "4. debug/get_screenshot — visual check (AFTER tree, not instead)\n";
		p += "5. debug/get_scene_tree — full tree (expensive, prefer browse)\n";
		p += "Call `help` for the full 50+ tool reference.\n\n";

		// -- Workflow --
		p += "## Workflow\n";
		p += "OBSERVE: project/get_info, editor/list_files, read key scripts.\n";
		p += "ORIENT: gdscript/check_errors. Form hypothesis.\n";
		p += "ACT: debug/run_scene (prefer) or debug/run_project. Evaluate manifest. Browse tree.\n";
		p += "INSPECT: get_output, get_errors, evaluate queries, check events, browse properties.\n";
		p += "FIX: edit code, tweak CVars, set_node_property, send inputs.\n";
		p += "VERIFY: stop, relaunch, confirm. Never stop at one attempt.\n\n";

		// -- Rules --
		p += "## Rules\n";
		p += "- Read files before editing. gdscript/check_errors after every edit.\n";
		p += "- debug/stop before relaunching after code changes.\n";
		p += "- browse_scene_tree before get_screenshot.\n";
		p += "- Prefer debug/run_scene for isolated tests. Create test scenes when needed.\n";
		p += "- Use the manifest. If a CVar, query, or action exists, use it via shorthand.\n";
		p += "- Events auto-log — check debug/get_output after interactions.\n";
		p += "- Use Debug.log() to inject trace output during play-testing.\n";
		p += "- Back every claim with tool output.\n";

		def["prompt"] = p;
		agents["godot-game-player"] = def;
	}

	return JSON::stringify(agents);
}

Vector<String> AgentPanel::_build_claude_args() const {
	Vector<String> args;

	// MCP server configuration.
	String mcp_config = _build_mcp_config_json();
	args.push_back("--mcp-config");
	args.push_back(mcp_config);
	args.push_back("--strict-mcp-config");

	// Lightweight context about the Godot environment and available subagents.
	String system_prompt = _build_system_prompt();
	args.push_back("--append-system-prompt");
	args.push_back(system_prompt);

	// Subagents: godot-builder (instrument) and godot-game-player (test/debug).
	String agents_json = _build_agents_json();
	args.push_back("--agents");
	args.push_back(agents_json);

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
		return;
	}
	status_label->set_text("Cleared");
}

void AgentPanel::_bind_methods() {
}

#endif // MCP_TERMINAL_ENABLED
