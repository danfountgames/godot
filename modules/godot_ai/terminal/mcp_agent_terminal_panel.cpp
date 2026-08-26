/**************************************************************************/
/*  mcp_agent_terminal_panel.cpp                                          */
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

#include "mcp_agent_terminal_panel.h"

#include "mcp_agent_launch.h"
#include "mcp_terminal_widget.h"

#include "../mcp_service.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/scroll_bar.h"
#include "scene/gui/scroll_container.h"

MCPAgentTerminalPanel::MCPAgentTerminalPanel() {
	_build_ui();
}

MCPAgentTerminalPanel::~MCPAgentTerminalPanel() {
	// PREDELETE has normally run already, but a panel built and freed without ever
	// entering the tree never sees it, and that path must not leave a config file with a
	// live agent's configuration in the cache directory either.
	shutdown();
}

void MCPAgentTerminalPanel::_bind_methods() {
	ADD_SIGNAL(MethodInfo("agent_started"));
	ADD_SIGNAL(MethodInfo("agent_exited", PropertyInfo(Variant::INT, "exit_code")));
	ADD_SIGNAL(MethodInfo("title_changed", PropertyInfo(Variant::STRING, "title")));
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void MCPAgentTerminalPanel::_build_ui() {
	HBoxContainer *toolbar = memnew(HBoxContainer);
	add_child(toolbar);

	Label *command_label = memnew(Label);
	command_label->set_text(TTR("Agent:"));
	toolbar->add_child(command_label);

	command_field = memnew(LineEdit);
	command_field->set_text("claude");
	command_field->set_custom_minimum_size(Size2(160, 0));
	command_field->set_tooltip_text(TTR("The coding agent to run. Anything on your PATH, or an absolute path."));
	toolbar->add_child(command_field);

	read_only_check = memnew(CheckBox);
	read_only_check->set_text(TTR("Read-only"));
	read_only_check->set_tooltip_text(TTR("Start the agent's session in read-only mode: it can inspect the project and the running game, and every tool that would change something is refused."));
	toolbar->add_child(read_only_check);

	start_button = memnew(Button);
	start_button->set_text(TTR("Start"));
	start_button->connect(SceneStringName(pressed), callable_mp(this, &MCPAgentTerminalPanel::_on_start_pressed));
	toolbar->add_child(start_button);

	stop_button = memnew(Button);
	stop_button->set_text(TTR("Stop"));
	stop_button->connect(SceneStringName(pressed), callable_mp(this, &MCPAgentTerminalPanel::_on_stop_pressed));
	toolbar->add_child(stop_button);

	status_label = memnew(Label);
	status_label->set_h_size_flags(SIZE_EXPAND_FILL);
	status_label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	toolbar->add_child(status_label);

	terminal_frame = memnew(Control);
	terminal_frame->set_v_size_flags(SIZE_EXPAND_FILL);
	terminal_frame->set_h_size_flags(SIZE_EXPAND_FILL);
	// Without a minimum the bottom panel opens at the height of the toolbar alone: the
	// terminal is inside a ScrollContainer inside an anchored Control, none of which
	// report a height upwards, so the panel appears to have started an agent into
	// nothing. Deep enough for a screenful of an agent's output.
	terminal_frame->set_custom_minimum_size(Size2(0, 260 * EDSCALE));
	terminal_frame->connect(SceneStringName(resized), callable_mp(this, &MCPAgentTerminalPanel::_on_frame_resized));
	add_child(terminal_frame);

	scroll_container = memnew(ScrollContainer);
	scroll_container->set_anchors_preset(Control::PRESET_FULL_RECT);
	scroll_container->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	terminal_frame->add_child(scroll_container);

	terminal = memnew(MCPTerminalWidget);
	terminal->set_h_size_flags(SIZE_EXPAND_FILL);
	scroll_container->add_child(terminal);
	// The widget owns this connection, including disconnecting it if it is ever
	// re-parented. The branch wired the scrollbar from the panel and had to unpick it by
	// hand in PREDELETE.
	terminal->set_scroll_container(scroll_container);
	terminal->connect("process_exited", callable_mp(this, &MCPAgentTerminalPanel::_on_process_exited));
	terminal->connect("title_changed", callable_mp(this, &MCPAgentTerminalPanel::_on_terminal_title_changed));

	to_bottom_button = memnew(Button);
	to_bottom_button->set_text(TTR("To Bottom"));
	to_bottom_button->set_visible(false);
	to_bottom_button->set_anchors_preset(Control::PRESET_BOTTOM_RIGHT);
	to_bottom_button->set_grow_direction_preset(Control::PRESET_BOTTOM_RIGHT);
	to_bottom_button->set_offset(SIDE_LEFT, -120);
	to_bottom_button->set_offset(SIDE_TOP, -32);
	to_bottom_button->set_offset(SIDE_RIGHT, -8);
	to_bottom_button->set_offset(SIDE_BOTTOM, -8);
	to_bottom_button->connect(SceneStringName(pressed), callable_mp(this, &MCPAgentTerminalPanel::_on_to_bottom_pressed));
	terminal_frame->add_child(to_bottom_button);

	scroll_container->get_v_scroll_bar()->connect(SceneStringName(value_changed), callable_mp(this, &MCPAgentTerminalPanel::_on_scroll_changed));

	_set_status(TTR("Not started."));
	_update_controls();
}

void MCPAgentTerminalPanel::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_PREDELETE: {
			// One ordered teardown, shared with the Stop button. There is nothing to
			// disconnect by hand here: the panel never polls its children, so no deferred
			// call can arrive after they are freed, and every connection it did make is
			// to a child that is freed with it.
			shutdown();
		} break;
	}
}

// ---------------------------------------------------------------------------
// Launching
// ---------------------------------------------------------------------------

bool MCPAgentTerminalPanel::_write_mcp_config(const String &p_json, String &r_error) {
	const String directory = OS::get_singleton()->get_cache_path().path_join("godot_ai");
	Ref<DirAccess> cache_dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (cache_dir.is_valid() && !cache_dir->dir_exists(directory)) {
		cache_dir->make_dir_recursive(directory);
	}

	// Named for the editor and this panel, so several terminals in one editor - and
	// several editors on one machine - do not overwrite each other's configuration.
	const String path = directory.path_join(vformat("agent_mcp_%d_%d.json",
			(int64_t)OS::get_singleton()->get_process_id(), (int64_t)get_instance_id()));

	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_null()) {
		r_error = vformat(TTR("Could not write the agent's MCP configuration to %s."), path);
		return false;
	}
	file->store_string(p_json);
	file->close();

	// Owner only. The file names a relay that will drive this editor; on a shared machine
	// it is not everybody's business, and the cache directory is not private by default.
	FileAccess::set_unix_permissions(path, FileAccess::UNIX_READ_OWNER | FileAccess::UNIX_WRITE_OWNER);

	mcp_config_path = path;
	return true;
}

void MCPAgentTerminalPanel::_remove_mcp_config() {
	if (mcp_config_path.is_empty()) {
		return;
	}
	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (dir.is_valid()) {
		dir->remove(mcp_config_path);
	}
	mcp_config_path = String();
}

bool MCPAgentTerminalPanel::launch() {
	last_error = String();

	if (!terminal) {
		return false;
	}
	if (terminal->is_process_running()) {
		_set_status(TTR("Already running."));
		return false;
	}
	if (!MCPPty::is_supported()) {
		last_error = TTR("This platform has no pseudo-terminal support in this build, so the agent cannot be run in a panel.");
		_set_status(last_error);
		return false;
	}

	const MCPService *service = MCPService::get_singleton();
	if (!service || !service->is_running()) {
		// Worth stopping for. An agent started against a silent editor comes up with no
		// tools at all and cheerfully answers from the source files instead, which looks
		// like it is working.
		last_error = TTR("The GodotAI service is not listening, so the agent would start with no tools. Enable it in Editor Settings under Network > Godot AI.");
		_set_status(last_error);
		return false;
	}

	const String command = command_field->get_text().strip_edges();
	if (command.is_empty()) {
		last_error = TTR("Name an agent to run.");
		_set_status(last_error);
		return false;
	}

	// Find the relay. Report where we looked rather than only that it is missing: on a
	// development build the answer is almost always "you have not built it yet".
	const Vector<String> candidates = mcp_agent_relay_search_paths(
			OS::get_singleton()->get_executable_path().get_base_dir(),
			OS::get_singleton()->get_environment("GODOT_AI_RELAY"));

	String relay_path;
	for (int i = 0; i < candidates.size(); i++) {
		// The last candidate is the bare name, resolved through PATH by the exec; it has
		// no directory, so there is nothing to check here.
		if (!candidates[i].contains("/") && !candidates[i].contains("\\")) {
			relay_path = candidates[i];
			break;
		}
		if (FileAccess::exists(candidates[i])) {
			relay_path = candidates[i];
			break;
		}
	}
	if (relay_path.is_empty()) {
		last_error = vformat(TTR("Could not find godot-ai-relay. Looked in: %s. Build it with tools/relay/build.sh."), String(", ").join(candidates));
		_set_status(last_error);
		return false;
	}

	const String client_name = vformat("Godot Agent Terminal (%s)", command.get_file());
	const String config_json = mcp_agent_build_mcp_config(
			relay_path, OS::get_singleton()->get_process_id(), client_name, read_only_check->is_pressed());

	// A previous run's file, if the process died without going through shutdown().
	_remove_mcp_config();

	String write_error;
	if (!_write_mcp_config(config_json, write_error)) {
		last_error = write_error;
		_set_status(last_error);
		return false;
	}

	Vector<String> environment;
	const Vector<String> names = mcp_agent_inherited_variable_names();
	for (int i = 0; i < names.size(); i++) {
		const String value = OS::get_singleton()->get_environment(names[i]);
		if (!value.is_empty()) {
			environment.push_back(names[i] + "=" + value);
		}
	}
	environment = mcp_agent_build_environment(environment, mcp_config_path);

	const Vector<String> arguments = mcp_agent_build_arguments(command, mcp_config_path, String());
	const String working_directory = ProjectSettings::get_singleton()->globalize_path("res://");

	String start_error;
	if (!terminal->start_process(command, arguments, environment, working_directory, start_error)) {
		_remove_mcp_config();
		last_error = vformat(TTR("Could not start '%s': %s"), command, start_error);
		_set_status(last_error);
		_update_controls();
		return false;
	}

	if (mcp_agent_command_is_claude(command)) {
		_set_status(vformat(TTR("Running %s, connected through %s."), command, relay_path.get_file()));
	} else {
		// Say so rather than implying a connection this cannot make. The configuration is
		// there for the command to use; whether it does is up to it.
		_set_status(vformat(TTR("Running %s. Its MCP configuration is at $GODOT_AI_MCP_CONFIG."), command));
	}
	_update_controls();
	emit_signal(SNAME("agent_started"));
	return true;
}

void MCPAgentTerminalPanel::shutdown() {
	if (terminal) {
		// Ask first, then insist. The pty escalates to SIGKILL after its grace period, so
		// a well-behaved agent gets to save its transcript and a wedged one still dies.
		terminal->request_stop();
		terminal->stop_process();
	}
	_remove_mcp_config();
}

bool MCPAgentTerminalPanel::is_running() const {
	return terminal && terminal->is_process_running();
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

void MCPAgentTerminalPanel::_update_controls() {
	const bool running = is_running();
	if (start_button) {
		start_button->set_disabled(running);
	}
	if (stop_button) {
		stop_button->set_disabled(!running);
	}
	if (command_field) {
		command_field->set_editable(!running);
	}
	if (read_only_check) {
		// The session's mode is fixed when the relay connects, so offering to change it
		// mid-run would be a control that quietly does nothing.
		read_only_check->set_disabled(running);
	}
}

void MCPAgentTerminalPanel::_set_status(const String &p_text) {
	if (status_label) {
		status_label->set_text(p_text);
		status_label->set_tooltip_text(p_text);
	}
}

void MCPAgentTerminalPanel::_on_start_pressed() {
	launch();
}

void MCPAgentTerminalPanel::_on_stop_pressed() {
	shutdown();
	_set_status(TTR("Stopped."));
	_update_controls();
}

void MCPAgentTerminalPanel::_on_to_bottom_pressed() {
	if (terminal) {
		terminal->scroll_to_bottom();
	}
	if (to_bottom_button) {
		to_bottom_button->set_visible(false);
	}
}

void MCPAgentTerminalPanel::_on_process_exited(int p_exit_code) {
	// The configuration file is the agent's, and the agent is gone.
	_remove_mcp_config();

	if (p_exit_code == 0) {
		_set_status(TTR("The agent exited."));
	} else {
		_set_status(vformat(TTR("The agent exited with status %d."), p_exit_code));
	}
	_update_controls();
	emit_signal(SNAME("agent_exited"), p_exit_code);
}

void MCPAgentTerminalPanel::_on_terminal_title_changed(const String &p_title) {
	emit_signal(SNAME("title_changed"), p_title);
}

void MCPAgentTerminalPanel::_on_scroll_changed(double p_value) {
	if (!terminal || !to_bottom_button) {
		return;
	}
	// The widget decides whether it is following the tail; the button just reflects it,
	// so the two cannot disagree.
	to_bottom_button->set_visible(!terminal->is_stuck_to_bottom());
}

void MCPAgentTerminalPanel::_on_frame_resized() {
	if (terminal) {
		terminal->update_terminal_size();
	}
}

#endif // MCP_TERMINAL_ENABLED
