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
class CheckBox;
class ScrollContainer;
class TerminalWidget;
class MCPServerPlugin;

class AgentPanel : public VBoxContainer {
	GDCLASS(AgentPanel, VBoxContainer)

private:
	MCPServerPlugin *server_plugin = nullptr;
	TerminalWidget *terminal = nullptr;

	// ScrollContainer + "To Bottom" button.
	Control *terminal_container = nullptr;
	ScrollContainer *scroll_container = nullptr;
	Button *to_bottom_button = nullptr;

	// Runtime tools toggle.
	CheckBox *runtime_toggle = nullptr;

	// Per-agent identity token (used as Bearer token and agent_id in MCPProtocol).
	String agent_token;

	// Title tracking.
	String current_title;

	bool claude_running = false;

	void _build_ui();
	void _on_to_bottom_pressed();
	void _on_scroll_changed(double p_value);
	void _on_scroll_container_resized();
	void _on_runtime_toggle_changed(bool p_enabled);
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
	String get_current_title() const { return current_title; }
	void launch();
	void stop();

	// Runtime tools toggle — called by MCPServerPlugin to enforce mutual exclusivity.
	bool is_runtime_tools_enabled() const;
	void set_runtime_tools_enabled(bool p_enabled);
	String get_agent_token() const { return agent_token; }

	AgentPanel();
	~AgentPanel();
};

#endif // MCP_TERMINAL_ENABLED
