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

#include "agent_prompts.gen.h"

#ifdef TOOLS_ENABLED
#include "editor/mcp_status_panel.h"
#include "editor/mcp_test_panel.h"
#include "scene/gui/tab_container.h"
#ifdef MCP_TERMINAL_ENABLED
#include "terminal/agent_panel.h"
#endif
#endif

#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/input/input_event.h"
#include "core/input/input_map.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/version.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/editor_interface.h"
#include "editor/editor_main_screen.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_paths.h"
#include "editor/script/script_editor_plugin.h"
#include "editor/settings/editor_settings.h"

namespace {
static const char *GODOT_MCP_MANAGED_BEGIN = "<!-- BEGIN GODOT MCP MANAGED CONTEXT -->";
static const char *GODOT_MCP_MANAGED_END = "<!-- END GODOT MCP MANAGED CONTEXT -->";

String mcp_toml_string(const String &p_value) {
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

bool mcp_write_if_changed(const String &p_path, const String &p_content) {
	if (FileAccess::exists(p_path)) {
		String existing = FileAccess::get_file_as_string(p_path);
		if (existing == p_content) {
			return false;
		}
	}

	String dir = p_path.get_base_dir();
	if (!dir.is_empty()) {
		Error dir_err = DirAccess::make_dir_recursive_absolute(dir);
		if (dir_err != OK) {
			WARN_PRINT("[MCP] Failed to create directory for managed agent file: " + dir);
			return false;
		}
	}

	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	if (f.is_null()) {
		WARN_PRINT("[MCP] Failed to write managed agent file: " + p_path);
		return false;
	}

	f->store_string(p_content);
	return true;
}

String mcp_replace_managed_block(const String &p_existing, const String &p_block) {
	String block = String(GODOT_MCP_MANAGED_BEGIN) + "\n" + p_block.strip_edges() + "\n" + GODOT_MCP_MANAGED_END + "\n";

	int begin = p_existing.find(GODOT_MCP_MANAGED_BEGIN);
	int end = p_existing.find(GODOT_MCP_MANAGED_END);
	if (begin >= 0 && end >= begin) {
		end += String(GODOT_MCP_MANAGED_END).length();
		String prefix = p_existing.substr(0, begin).rstrip("\n");
		String suffix = p_existing.substr(end).strip_edges(true, false);
		String result = prefix;
		if (!result.is_empty()) {
			result += "\n\n";
		}
		result += block;
		if (!suffix.is_empty()) {
			result += "\n" + suffix;
		}
		return result;
	}

	if (p_existing.strip_edges().is_empty()) {
		return block;
	}

	return p_existing.rstrip("\n") + "\n\n" + block;
}

String mcp_build_input_map_summary() {
	InputMap *input_map = InputMap::get_singleton();
	if (!input_map) {
		return "Input map not available.";
	}

	TypedArray<StringName> actions = input_map->get_actions();
	String text;
	int count = 0;
	for (int i = 0; i < actions.size(); i++) {
		StringName action = actions[i];
		String name = String(action);
		if (name.begins_with("ui_")) {
			continue;
		}

		const List<Ref<InputEvent>> *events = input_map->action_get_events(action);
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

String mcp_build_system_prompt() {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	if (!project_settings) {
		return String(_agent_prompt_system_prompt);
	}

	String project_name = (String)project_settings->get_setting("application/config/name", "Untitled");
	String project_path = project_settings->get_resource_path();

	String prompt = String(_agent_prompt_system_prompt);
	prompt = prompt.replace("{{GODOT_VERSION}}", GODOT_VERSION_FULL_CONFIG);
	prompt = prompt.replace("{{PROJECT_NAME}}", project_name);
	prompt = prompt.replace("{{PROJECT_PATH}}", project_path);

	String arch_path = "res://docs/architecture.md";
	String arch_content;
	if (FileAccess::exists(arch_path)) {
		arch_content = FileAccess::get_file_as_string(arch_path);
	} else {
		arch_content = "No architecture docs found. Use project/init_docs to create them when the game has enough structure.";
	}
	prompt = prompt.replace("{{PROJECT_ARCHITECTURE}}", arch_content);
	prompt = prompt.replace("{{INPUT_MAP}}", mcp_build_input_map_summary());

	return prompt;
}

String mcp_make_codex_agent_toml(const String &p_name, const String &p_description, const String &p_prompt, const Vector<String> &p_nicknames) {
	String toml;
	toml += "name = " + mcp_toml_string(p_name) + "\n";
	toml += "description = " + mcp_toml_string(p_description) + "\n";
	toml += "developer_instructions = " + mcp_toml_string(p_prompt) + "\n";
	toml += "nickname_candidates = [";
	for (int i = 0; i < p_nicknames.size(); i++) {
		if (i > 0) {
			toml += ", ";
		}
		toml += mcp_toml_string(p_nicknames[i]);
	}
	toml += "]\n";
	return toml;
}
} // namespace

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
	_EDITOR_DEF("network/mcp_server/test_root", String("res://tests/"));

	// Register keyboard shortcuts for test operations.
	ED_SHORTCUT("mcp_server/run_all_tests", TTRC("Run All Tests"), KeyModifierMask::ALT | KeyModifierMask::SHIFT | Key::T);
	ED_SHORTCUT("mcp_server/run_current_test", TTRC("Run Current Test File"), KeyModifierMask::ALT | Key::T);
	ED_SHORTCUT("mcp_server/rerun_failed", TTRC("Rerun Failed Tests"), KeyModifierMask::ALT | KeyModifierMask::SHIFT | Key::R);

	// Heap-allocate the protocol (Object-derived, must use memnew).
	protocol = memnew(MCPProtocol);

	// Create and register the debugger bridge plugin.
	debugger_bridge.instantiate();
	if (EditorDebuggerNode::get_singleton()) {
		EditorDebuggerNode::get_singleton()->add_debugger_plugin(debugger_bridge);
	}

	// Give the protocol a pointer to the bridge for tool handlers to use.
	protocol->set_debugger_bridge(debugger_bridge.ptr());

#ifdef TOOLS_ENABLED
	// Create the "AI" main screen with tabs for Agent and MCP Status.
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
	status_panel->set_protocol(protocol);
	status_panel->set_debugger_bridge(debugger_bridge.ptr());
	status_panel->set_server_plugin(this);
	status_panel->set_name("MCP Status");
	ai_tab_container->add_child(status_panel);

	// Add as a main screen plugin (top row alongside 2D, 3D, Script).
	EditorNode::get_singleton()->get_editor_main_screen()->get_control()->add_child(ai_tab_container);
	ai_tab_container->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	ai_tab_container->hide();

	// Create the Test panel and add it as an editor bottom panel.
	test_panel = memnew(MCPTestPanel);
	test_panel->set_debugger_bridge(debugger_bridge.ptr());

	// Apply test root from settings.
	String test_root = String(_EDITOR_GET("network/mcp_server/test_root"));
	if (!test_root.is_empty()) {
		test_panel->set_test_root(test_root);
	}

	add_control_to_bottom_panel(test_panel, TTR("Tests"));

	// Connect to script editor for "Run Current Test" integration.
	ScriptEditor *script_editor = ScriptEditor::get_singleton();
	if (script_editor) {
		script_editor->connect("editor_script_changed",
				callable_mp(this, &MCPServerPlugin::_on_script_changed));
	}
#endif

	set_process_internal(true);
}

MCPServerPlugin::~MCPServerPlugin() {
	if (started) {
		stop();
	}

#ifdef TOOLS_ENABLED
	if (test_panel) {
		remove_control_from_bottom_panel(test_panel);
		memdelete(test_panel);
		test_panel = nullptr;
	}

	if (ai_tab_container) {
#ifdef MCP_TERMINAL_ENABLED
		// Stop all agent panels before destruction — unregisters MCP tokens,
		// cleans up temp config files, and kills child processes gracefully.
		for (AgentPanel *panel : agent_panels) {
			panel->stop();
		}
#endif
		if (ai_tab_container->get_parent()) {
			ai_tab_container->get_parent()->remove_child(ai_tab_container);
		}
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

	if (protocol) {
		memdelete(protocol);
		protocol = nullptr;
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

	// Mutual exclusivity: runtime tools default to off; if any existing panel
	// already has them enabled, ensure this new tab stays off.
	for (AgentPanel *existing : agent_panels) {
		if (existing->is_runtime_tools_enabled()) {
			panel->set_runtime_tools_enabled(false);
			break;
		}
	}

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
	_update_tab_icons();

	// Switch to the new tab.
	int new_tab = ai_tab_container->get_tab_idx_from_control(panel);
	ai_tab_container->set_current_tab(new_tab);

	// Auto-launch the selected agent CLI.
	panel->launch();
}

void MCPServerPlugin::_on_tab_changed(int p_tab) {
	if (!new_tab_placeholder || _closing_tab) {
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

	// Guard: prevent _on_tab_changed from creating new tabs while we're
	// tearing down this one (remove_child / set_current_tab fire signals).
	_closing_tab = true;

	// Stop any running process and halt all processing on the panel +
	// its TerminalWidget so nothing ticks between now and the deferred free.
	panel->stop();

	// Disconnect signals targeting us before removal — prevents deferred
	// title_changed callbacks from iterating a stale agent_panels vector.
	if (panel->is_connected("title_changed", callable_mp(this, &MCPServerPlugin::_on_agent_title_changed))) {
		panel->disconnect("title_changed", callable_mp(this, &MCPServerPlugin::_on_agent_title_changed));
	}

	// Find and remove from our vector.
	int vec_idx = agent_panels.find(panel);
	if (vec_idx >= 0) {
		agent_panels.remove_at(vec_idx);
	}

	// Remove from the tree immediately (stops processing, hides the tab)
	// but defer the actual memory deallocation — we're inside TabContainer's
	// tab_button_pressed signal, and synchronous memdelete here corrupts
	// its internal state when control returns to the TabContainer.
	ai_tab_container->remove_child(panel);
	callable_mp(this, &MCPServerPlugin::_deferred_free_panel).call_deferred(panel->get_instance_id());

	// If no agent tabs remain, create a fresh one.
	// Keep _closing_tab = true through this — _create_agent_tab calls
	// set_current_tab which fires tab_selected, and we must prevent
	// _on_tab_changed from interpreting that as a "+" click.
	if (agent_panels.is_empty()) {
		_create_agent_tab();
	}

	_closing_tab = false;

	_update_close_buttons();
	_update_tab_icons();

	// Switch to the first agent tab.
	if (!agent_panels.is_empty()) {
		int tab_idx = ai_tab_container->get_tab_idx_from_control(agent_panels[0]);
		ai_tab_container->set_current_tab(tab_idx);
	}
}

void MCPServerPlugin::_deferred_free_panel(ObjectID p_id) {
	Object *obj = ObjectDB::get_instance(p_id);
	if (obj) {
		memdelete(obj);
	}
}

void MCPServerPlugin::_on_agent_title_changed(const String &p_title) {
	for (int i = 0; i < agent_panels.size(); i++) {
		AgentPanel *panel = agent_panels[i];
		int tab_idx = ai_tab_container->get_tab_idx_from_control(panel);
		if (tab_idx < 0) {
			continue; // Panel removed from tree but not yet freed.
		}
		String raw_title = panel->get_current_title().strip_edges();

		print_verbose(vformat("[MCP] Agent %d raw title: \"%s\"", i + 1, raw_title));

		// Normalize em-dash (U+2014) and en-dash (U+2013) separators to
		// plain " - " so we can split uniformly.
		raw_title = raw_title.replace(String::chr(0x2014), "-");
		raw_title = raw_title.replace(String::chr(0x2013), "-");
		raw_title = raw_title.replace(" -- ", " - ");

		// Agent CLI title format varies by version. Claude examples:
		//   "Folder - {emoji} Description - Claude Code - 80x24"
		//   "Claude Code - {emoji} Description - 80x24"
		//   "{emoji} Description - Claude Code - 80x24"
		// Strategy: split on " - ", skip known non-descriptive segments
		// (folder at index 0, "claude"/"claude code", dimension strings),
		// take the first remaining segment which includes the emoji.
		String title;
		PackedStringArray parts = raw_title.split(" - ");
		if (parts.size() >= 2) {
			print_verbose(vformat("[MCP] Agent %d title parts (%d):", i + 1, parts.size()));
			for (int p = 0; p < parts.size(); p++) {
				print_verbose(vformat("[MCP]   [%d] \"%s\"", p, parts[p].strip_edges()));
			}
			for (int p = 1; p < parts.size(); p++) {
				String seg = parts[p].strip_edges();
				if (seg.is_empty()) {
					continue;
				}
				// Skip agent name segments.
				String lower = seg.to_lower();
				if (lower == "claude" || lower == "claude code" || lower == "codex" || lower == "codex cli") {
					continue;
				}
				// Skip terminal dimension strings like "80x24".
				if (seg.contains("x")) {
					PackedStringArray dims = seg.split("x");
					if (dims.size() == 2 && dims[0].strip_edges().is_valid_int() && dims[1].strip_edges().is_valid_int()) {
						continue;
					}
				}
				title = seg;
				break;
			}
		}

		String default_name = "Agent " + itos(i + 1);
		String final_title = title.is_empty() ? default_name : title;
		print_verbose(vformat("[MCP] Agent %d parsed title: \"%s\"", i + 1, final_title));
		ai_tab_container->set_tab_title(tab_idx, final_title);
	}
}

void MCPServerPlugin::on_agent_runtime_changed(AgentPanel *p_panel, bool p_enabled) {
	if (p_enabled) {
		// Mutual exclusivity: disable runtime tools on all other panels.
		for (AgentPanel *panel : agent_panels) {
			if (panel != p_panel && panel->is_runtime_tools_enabled()) {
				panel->set_runtime_tools_enabled(false);
			}
		}
	}
	_update_tab_icons();
}

void MCPServerPlugin::on_agent_editor_changed(AgentPanel *p_panel, bool p_enabled) {
	if (p_enabled) {
		// Mutual exclusivity: disable editor controls on all other panels.
		for (AgentPanel *panel : agent_panels) {
			if (panel != p_panel && panel->is_editor_controls_enabled()) {
				panel->set_editor_controls_enabled(false);
			}
		}
	}
	_update_tab_icons();
}

void MCPServerPlugin::_update_tab_icons() {
	Ref<Texture2D> play_icon = ai_tab_container->get_theme_icon("MainPlay", "EditorIcons");
	Ref<Texture2D> edit_icon = ai_tab_container->get_theme_icon("Edit", "EditorIcons");
	for (int i = 0; i < agent_panels.size(); i++) {
		int tab_idx = ai_tab_container->get_tab_idx_from_control(agent_panels[i]);
		if (agent_panels[i]->is_runtime_tools_enabled()) {
			ai_tab_container->set_tab_icon(tab_idx, play_icon);
		} else if (agent_panels[i]->is_editor_controls_enabled()) {
			ai_tab_container->set_tab_icon(tab_idx, edit_icon);
		} else {
			ai_tab_container->set_tab_icon(tab_idx, Ref<Texture2D>());
		}
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
// Main Screen Plugin
// ---------------------------------------------------------------------------

void MCPServerPlugin::make_visible(bool p_visible) {
	if (ai_tab_container) {
		if (p_visible) {
			ai_tab_container->show();
		} else {
			ai_tab_container->hide();
		}
	}
}

// ---------------------------------------------------------------------------
// Script Editor Integration
// ---------------------------------------------------------------------------

#ifdef TOOLS_ENABLED
void MCPServerPlugin::_on_script_changed(const Ref<Script> &p_script) {
	if (p_script.is_null()) {
		current_script_path = "";
		return;
	}

	current_script_path = p_script->get_path();

	if (!test_panel) {
		return;
	}

	String test_root = test_panel->get_test_root();

	// Auto-show the Tests bottom panel when a test file is opened.
	if (current_script_path.begins_with(test_root) && current_script_path.get_file().begins_with("test_")) {
		make_bottom_panel_item_visible(test_panel);
	}
}

void MCPServerPlugin::_on_run_current_test() {
	if (!test_panel || current_script_path.is_empty()) {
		return;
	}

	if (current_script_path.get_file().begins_with("test_") && current_script_path.ends_with(".gd")) {
		make_bottom_panel_item_visible(test_panel);
		test_panel->run_file(current_script_path);
	}
}

void MCPServerPlugin::_on_run_all_tests() {
	if (!test_panel) {
		return;
	}
	make_bottom_panel_item_visible(test_panel);
	test_panel->run_all_tests();
}

void MCPServerPlugin::_on_rerun_failed_tests() {
	if (!test_panel) {
		return;
	}
	make_bottom_panel_item_visible(test_panel);
	test_panel->rerun_failed_tests();
}
#endif // TOOLS_ENABLED

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

			if (!agent_files_synced && EditorNode::get_singleton()->is_editor_ready()) {
				agent_files_synced = true;
				sync_project_agent_files();
			}

			// If running without a thread, poll on the main thread.
			if (started && !use_thread) {
				protocol->poll();
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
					protocol->set_max_clients(new_max_clients);
					protocol->set_session_timeout(new_session_timeout);
				}
			}

#ifdef TOOLS_ENABLED
			// Update test root if changed.
			if (test_panel) {
				String new_test_root = String(_EDITOR_GET("network/mcp_server/test_root"));
				test_panel->set_test_root(new_test_root);
			}
#endif
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
		self->protocol->poll();
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

	protocol->set_max_clients(max_clients);
	protocol->set_session_timeout(session_timeout);

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
		protocol->set_auth_token(auth_token);
	}

	int preferred_port = port;
	Error err = protocol->start(port, IPAddress(host));
	if (err != OK) {
		for (int try_port = preferred_port + 1; try_port <= preferred_port + MCP_PORT_RANGE; try_port++) {
			err = protocol->start(try_port, IPAddress(host));
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

	protocol->stop();
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

void MCPServerPlugin::sync_project_agent_files() {
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	if (!project_settings) {
		return;
	}

	String project_dir = project_settings->get_resource_path();
	if (project_dir.is_empty()) {
		return;
	}

	String system_prompt = mcp_build_system_prompt();
	String agents_md_block;
	agents_md_block += "# Godot MCP Agent Context\n\n";
	agents_md_block += "This project is opened in a Godot editor fork with a built-in MCP server.\n";
	agents_md_block += "Use the configured `godot` MCP server for project, editor, scene, runtime, debug, docs, testing, and analysis tools.\n\n";
	agents_md_block += system_prompt.strip_edges();
	agents_md_block += "\n\n## Codex Agent Roles\n";
	agents_md_block += "Specialized Codex agent role files are managed in `.codex/agents/` for planner, builder, designer, game-player, and refactor workflows.\n";

	String agents_md_path = project_dir.path_join("AGENTS.md");
	String existing_agents_md;
	if (FileAccess::exists(agents_md_path)) {
		existing_agents_md = FileAccess::get_file_as_string(agents_md_path);
	}
	mcp_write_if_changed(agents_md_path, mcp_replace_managed_block(existing_agents_md, agents_md_block));

	String codex_agents_dir = project_dir.path_join(".codex").path_join("agents");

	Vector<String> planner_nicknames;
	planner_nicknames.push_back("planner");
	planner_nicknames.push_back("godot planner");
	mcp_write_if_changed(codex_agents_dir.path_join("godot-planner.toml"),
			mcp_make_codex_agent_toml(
					"godot-planner",
					"Godot project architect and implementation planner. Use before non-trivial features, refactors, scene reorganization, or multi-file changes.",
					_agent_prompt_agent_planner,
					planner_nicknames));

	Vector<String> builder_nicknames;
	builder_nicknames.push_back("builder");
	builder_nicknames.push_back("godot builder");
	mcp_write_if_changed(codex_agents_dir.path_join("godot-builder.toml"),
			mcp_make_codex_agent_toml(
					"godot-builder",
					"Builds Godot features, scripts, scenes, UI, gameplay systems, bug fixes, and validation loops.",
					_agent_prompt_agent_builder,
					builder_nicknames));

	Vector<String> designer_nicknames;
	designer_nicknames.push_back("designer");
	designer_nicknames.push_back("godot designer");
	mcp_write_if_changed(codex_agents_dir.path_join("godot-designer.toml"),
			mcp_make_codex_agent_toml(
					"godot-designer",
					"Builds production-grade Godot UI and visual interfaces: menus, HUDs, dialogs, inventory, themes, and styled scenes.",
					_agent_prompt_agent_designer,
					designer_nicknames));

	Vector<String> game_player_nicknames;
	game_player_nicknames.push_back("game player");
	game_player_nicknames.push_back("runtime");
	mcp_write_if_changed(codex_agents_dir.path_join("godot-game-player.toml"),
			mcp_make_codex_agent_toml(
					"godot-game-player",
					"Owns running-game investigation: launches, tests, debugs, plays, gathers evidence, and verifies runtime behavior.",
					_agent_prompt_agent_game_player,
					game_player_nicknames));

	Vector<String> refactor_nicknames;
	refactor_nicknames.push_back("refactor");
	refactor_nicknames.push_back("godot refactor");
	mcp_write_if_changed(codex_agents_dir.path_join("godot-refactor.toml"),
			mcp_make_codex_agent_toml(
					"godot-refactor",
					"Code health guardian for Godot projects. Splits monoliths, extracts duplication, and preserves behavior while validating changes.",
					_agent_prompt_agent_refactor,
					refactor_nicknames));
}

String MCPServerPlugin::get_agent_cli() const {
	return status_panel ? status_panel->get_agent_cli() : "claude";
}

bool MCPServerPlugin::is_debug_mode_enabled() const {
	return status_panel && status_panel->is_debug_mode_enabled();
}

bool MCPServerPlugin::is_dangerously_mode_enabled() const {
	return status_panel && status_panel->is_dangerously_mode_enabled();
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
