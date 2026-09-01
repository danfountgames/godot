/**************************************************************************/
/*  mcp_agent_setup_dialog.h                                              */
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

#ifdef MCP_TERMINAL_ENABLED

#include "mcp_agent_launch.h"

#include "../mcp_types.h"
#include "scene/gui/dialogs.h"

class CheckBox;
class Label;
class LineEdit;

// The explicit boundary between pressing Start and giving an agent access. It gathers
// the editor's capability decisions, MCP client approval, and the coding agent's own
// workspace policy in one place so the user does not have to chase three settings UIs.
class MCPAgentSetupDialog : public ConfirmationDialog {
	GDCLASS(MCPAgentSetupDialog, ConfirmationDialog);

	LineEdit *command_edit = nullptr;
	Label *mcp_summary = nullptr;
	Label *session_summary = nullptr;
	Label *host_access_summary = nullptr;
	CheckBox *edit_files_check = nullptr;
	CheckBox *edit_scene_check = nullptr;
	CheckBox *run_project_check = nullptr;
	CheckBox *simulate_input_check = nullptr;
	CheckBox *read_user_data_check = nullptr;
	CheckBox *edit_user_data_check = nullptr;
	CheckBox *host_approval_check = nullptr;
	CheckBox *codex_mcp_approval_check = nullptr;

	MCPAgentKind agent_kind = MCP_AGENT_CODEX;
	bool read_only = false;

	void _configure_capability(CheckBox *p_check, MCPCapability p_capability);

public:
	MCPAgentSetupDialog();

	void configure(MCPAgentKind p_kind, const String &p_command, bool p_read_only, int p_http_port);
	String get_command() const;
	bool is_capability_allowed(MCPCapability p_capability) const;
	bool should_update_capability(MCPCapability p_capability) const;
	bool is_host_approval_allowed() const;
};

#endif // MCP_TERMINAL_ENABLED
