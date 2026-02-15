/**************************************************************************/
/*  agent_panel.h                                                         */
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

#include "scene/gui/box_container.h"

class Button;
class Label;
class TerminalWidget;
class MCPServerPlugin;

class AgentPanel : public VBoxContainer {
	GDCLASS(AgentPanel, VBoxContainer)

private:
	MCPServerPlugin *server_plugin = nullptr;
	TerminalWidget *terminal = nullptr;

	// Toolbar widgets.
	Button *launch_button = nullptr;
	Button *stop_button = nullptr;
	Button *clear_button = nullptr;
	Label *status_label = nullptr;

	bool claude_running = false;

	void _build_ui();
	void _on_launch_pressed();
	void _on_stop_pressed();
	void _on_clear_pressed();
	void _update_status();

	String _find_claude_binary() const;
	String _build_mcp_config_json() const;
	String _build_system_prompt() const;
	String _build_agents_json() const;
	Vector<String> _build_claude_args() const;
	Vector<String> _build_claude_env() const;

	void _notification(int p_what);

protected:
	static void _bind_methods();

public:
	void set_server_plugin(MCPServerPlugin *p_plugin) { server_plugin = p_plugin; }

	AgentPanel();
	~AgentPanel();
};

#endif // MCP_TERMINAL_ENABLED
