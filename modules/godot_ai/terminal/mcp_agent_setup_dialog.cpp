/**************************************************************************/
/*  mcp_agent_setup_dialog.cpp                                            */
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

#include "mcp_agent_setup_dialog.h"

#include "../mcp_permissions.h"

#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/check_box.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/separator.h"

static Label *setup_section_label(const String &p_text) {
	Label *label = memnew(Label);
	label->set_text(p_text);
	label->add_theme_font_size_override("font_size", 16 * EDSCALE);
	return label;
}

static CheckBox *setup_capability_check(VBoxContainer *p_parent, const String &p_text, const String &p_tooltip) {
	CheckBox *check = memnew(CheckBox);
	check->set_text(p_text);
	check->set_tooltip_text(p_tooltip);
	p_parent->add_child(check);
	return check;
}

MCPAgentSetupDialog::MCPAgentSetupDialog() {
	set_title(TTR("Agent Setup"));
	set_min_size(Size2(620, 620) * EDSCALE);
	set_ok_button_text(TTR("Start Agent"));

	VBoxContainer *layout = memnew(VBoxContainer);
	add_child(layout);

	Label *introduction = memnew(Label);
	introduction->set_text(TTR("Confirm the editor, MCP, and workspace access this agent will receive before it starts."));
	introduction->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	layout->add_child(introduction);

	HBoxContainer *command_row = memnew(HBoxContainer);
	layout->add_child(command_row);
	Label *command_label = memnew(Label);
	command_label->set_text(TTR("Executable:"));
	command_row->add_child(command_label);
	command_edit = memnew(LineEdit);
	command_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	command_edit->set_tooltip_text(TTR("A command on the editor's PATH or an absolute path. On macOS, an absolute path is useful when the app was opened from Finder."));
	command_row->add_child(command_edit);

	layout->add_child(memnew(HSeparator));
	layout->add_child(setup_section_label(TTR("MCP connection")));
	mcp_summary = memnew(Label);
	mcp_summary->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	layout->add_child(mcp_summary);
	CheckBox *client_approval = memnew(CheckBox);
	client_approval->set_text(TTR("Approve this Agent Terminal as an MCP client"));
	client_approval->set_pressed(true);
	client_approval->set_disabled(true);
	client_approval->set_tooltip_text(TTR("Starting is the approval. The approval is stored in Editor Settings and can be revoked from Godot AI: Clients and Skills."));
	layout->add_child(client_approval);

	layout->add_child(memnew(HSeparator));
	layout->add_child(setup_section_label(TTR("Godot editor tools")));
	session_summary = memnew(Label);
	session_summary->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	layout->add_child(session_summary);

	CheckBox *read_project_check = setup_capability_check(layout, TTR("Read project files and edited scenes"), TTR("Always available to an approved client."));
	read_project_check->set_pressed(true);
	read_project_check->set_disabled(true);
	CheckBox *read_runtime_check = setup_capability_check(layout, TTR("Inspect the running game"), TTR("Always available to an approved client."));
	read_runtime_check->set_pressed(true);
	read_runtime_check->set_disabled(true);
	edit_files_check = setup_capability_check(layout, TTR("Edit project files"), TTR("Allows structured file-writing tools inside the project. Checkpoints are taken first."));
	edit_scene_check = setup_capability_check(layout, TTR("Edit scenes and resources"), TTR("Allows structured editor mutations. Checkpoints and editor undo are used."));
	run_project_check = setup_capability_check(layout, TTR("Run, pause, and stop the project"), TTR("Allows play mode and agent-owned game instances."));
	simulate_input_check = setup_capability_check(layout, TTR("Send input to the game and editor"), TTR("Allows pointer, keyboard, and action input for playtesting."));
	read_user_data_check = setup_capability_check(layout, TTR("Read game saves and user data"), TTR("Allows access to user://, which is outside version-controlled project files."));
	edit_user_data_check = setup_capability_check(layout, TTR("Edit game saves and user data"), TTR("Allows changes under user://. This can affect player saves and settings."));
	CheckBox *dangerous_check = setup_capability_check(layout, TTR("Run arbitrary shell commands through Godot (always denied)"), TTR("Godot AI tools never expose arbitrary command execution."));
	dangerous_check->set_pressed(false);
	dangerous_check->set_disabled(true);

	layout->add_child(memnew(HSeparator));
	layout->add_child(setup_section_label(TTR("Coding agent access")));
	host_access_summary = memnew(Label);
	host_access_summary->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	layout->add_child(host_access_summary);
	host_approval_check = memnew(CheckBox);
	host_approval_check->set_text(TTR("Let Codex request approval for Git, network, and protected paths"));
	host_approval_check->set_pressed(true);
	host_approval_check->set_tooltip_text(TTR("Codex starts in workspace-write mode. Commits, pushes, network access, and protected paths such as .git remain approval-gated."));
	layout->add_child(host_approval_check);
	codex_mcp_approval_check = memnew(CheckBox);
	codex_mcp_approval_check->set_text(TTR("Use the Godot permissions above for MCP tools (no duplicate Codex prompt per call)"));
	codex_mcp_approval_check->set_pressed(true);
	codex_mcp_approval_check->set_disabled(true);
	layout->add_child(codex_mcp_approval_check);
}

void MCPAgentSetupDialog::_configure_capability(CheckBox *p_check, MCPCapability p_capability) {
	const bool session_denies = read_only && mcp_capability_is_mutating(p_capability);
	// "ask" is intentionally shown checked: this dialog is the missing interactive ask.
	// Confirming converts the visible choice to a standing allow/deny policy.
	p_check->set_pressed(!session_denies && MCPPermissions::get_policy(p_capability) != MCP_POLICY_DENY);
	p_check->set_disabled(session_denies);
}

void MCPAgentSetupDialog::configure(MCPAgentKind p_kind, const String &p_command, bool p_read_only, int p_http_port) {
	agent_kind = p_kind;
	read_only = p_read_only;
	command_edit->set_text(p_command);

	mcp_summary->set_text(vformat(TTR("Direct Streamable HTTP to this editor on port %d. The bearer token is passed only in the child process environment; no global client configuration is changed."), p_http_port));
	session_summary->set_text(read_only ?
			TTR("This launch is read-only. Mutating permissions are disabled for the session and their stored editor policies are left unchanged.") :
			TTR("Checked capabilities will be set to Allow in Editor Settings; unchecked capabilities will be set to Deny. These choices persist and can be changed later."));

	_configure_capability(edit_files_check, MCP_CAP_EDIT_FILES);
	_configure_capability(edit_scene_check, MCP_CAP_EDIT_SCENE);
	_configure_capability(run_project_check, MCP_CAP_RUN_PROJECT);
	_configure_capability(simulate_input_check, MCP_CAP_SIMULATE_INPUT);
	_configure_capability(read_user_data_check, MCP_CAP_READ_USER_DATA);
	_configure_capability(edit_user_data_check, MCP_CAP_EDIT_USER_DATA);

	const bool is_codex = agent_kind == MCP_AGENT_CODEX;
	host_approval_check->set_visible(is_codex);
	codex_mcp_approval_check->set_visible(is_codex);
	if (is_codex) {
		host_access_summary->set_text(read_only ?
				TTR("Codex will use a read-only sandbox. If enabled below, it can still ask before a protected operation; Godot's read-only MCP session remains absolute.") :
				TTR("Codex will use workspace-write for this project. Project edits run directly; .git, network access, and paths outside the project stay protected and require a Codex approval request."));
	} else {
		host_access_summary->set_text(TTR("Claude Code controls its own filesystem, Git, and network permission prompts. This setup still configures and approves its direct Godot MCP connection."));
	}
}

String MCPAgentSetupDialog::get_command() const {
	return command_edit->get_text().strip_edges();
}

bool MCPAgentSetupDialog::is_capability_allowed(MCPCapability p_capability) const {
	switch (p_capability) {
		case MCP_CAP_READ_PROJECT:
		case MCP_CAP_READ_RUNTIME:
			return true;
		case MCP_CAP_EDIT_FILES:
			return edit_files_check->is_pressed();
		case MCP_CAP_EDIT_SCENE:
			return edit_scene_check->is_pressed();
		case MCP_CAP_RUN_PROJECT:
			return run_project_check->is_pressed();
		case MCP_CAP_SIMULATE_INPUT:
			return simulate_input_check->is_pressed();
		case MCP_CAP_READ_USER_DATA:
			return read_user_data_check->is_pressed();
		case MCP_CAP_EDIT_USER_DATA:
			return edit_user_data_check->is_pressed();
		case MCP_CAP_DANGEROUS_EXEC:
		case MCP_CAP_MAX:
		default:
			return false;
	}
}

bool MCPAgentSetupDialog::should_update_capability(MCPCapability p_capability) const {
	return p_capability != MCP_CAP_DANGEROUS_EXEC &&
			!(read_only && mcp_capability_is_mutating(p_capability));
}

bool MCPAgentSetupDialog::is_host_approval_allowed() const {
	return agent_kind == MCP_AGENT_CODEX && host_approval_check->is_pressed();
}

#endif // MCP_TERMINAL_ENABLED
