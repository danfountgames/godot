/**************************************************************************/
/*  mcp_agent_terminal_panel.h                                            */
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

// The editor panel that runs a coding agent in a terminal, pointed at this editor.
//
// Descended from the `GodotBeamDev` branch's `AgentPanel`, and this is the part the
// branch was warned about: it crashed. Its NOTIFICATION_PREDELETE handler is a list of
// defensive signal-disconnects and pointer-nullings, which is what a lifecycle looks
// like after it has been debugged from the outside. Two things caused it, and both are
// fixed at the source here rather than guarded against:
//
//   * The panel polled its own children every frame to notice a dead process and a
//     changed title. A deferred call from that poll could land after the children were
//     freed. Here the widget emits `process_exited` and `title_changed` and the panel
//     does not poll at all.
//   * `stop()` was never called on the way out. Its temporary MCP configuration file
//     stayed in the cache directory - one per launch, forever. Teardown is now a single
//     ordered path used by the button, PREDELETE, and the destructor alike.
//
// This is a user-facing panel, deliberately. No MCP tool starts it and no tool can:
// spawning a coding agent from inside a tool call would be an editor that runs arbitrary
// programs on request, which this module does not do.

#ifdef MCP_TERMINAL_ENABLED

#include "scene/gui/box_container.h"

class Button;
class CheckBox;
class Label;
class LineEdit;
class MCPTerminalWidget;
class ScrollContainer;

class MCPAgentTerminalPanel : public VBoxContainer {
	GDCLASS(MCPAgentTerminalPanel, VBoxContainer);

private:
	MCPTerminalWidget *terminal = nullptr;
	Control *terminal_frame = nullptr;
	ScrollContainer *scroll_container = nullptr;
	Button *to_bottom_button = nullptr;

	LineEdit *command_field = nullptr;
	CheckBox *read_only_check = nullptr;
	Button *start_button = nullptr;
	Button *stop_button = nullptr;
	Label *status_label = nullptr;

	String mcp_config_path;
	String last_error;

	void _build_ui();
	void _update_controls();
	void _set_status(const String &p_text);

	void _on_start_pressed();
	void _on_stop_pressed();
	void _on_to_bottom_pressed();
	void _on_process_exited(int p_exit_code);
	void _on_terminal_title_changed(const String &p_title);
	void _on_scroll_changed(double p_value);
	void _on_frame_resized();

	// Writes the temporary MCP configuration, owner-readable only. Returns false and
	// fills `r_error` if it could not be written.
	bool _write_mcp_config(const String &p_json, String &r_error);
	void _remove_mcp_config();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Starts the agent. Returns false and reports why in the status line when the relay
	// or the agent binary cannot be found, or the MCP service is not listening.
	bool launch();

	// Asks the agent to exit and cleans up after it. Safe to call when nothing is
	// running, and safe to call twice.
	void shutdown();

	bool is_running() const;
	String get_last_error() const { return last_error; }
	MCPTerminalWidget *get_terminal() const { return terminal; }

	MCPAgentTerminalPanel();
	~MCPAgentTerminalPanel();
};

#endif // MCP_TERMINAL_ENABLED
