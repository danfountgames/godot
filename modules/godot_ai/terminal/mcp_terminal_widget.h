/**************************************************************************/
/*  mcp_terminal_widget.h                                                 */
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

// A terminal you can see and type into: the pty, the VT emulator and a Control that
// draws the grid.
//
// Ported from the `GodotBeamDev` branch's `TerminalWidget`. The drawing and the theme
// derivation came across close to unchanged - they were fine. What was rebuilt is
// everything around the edges, because that is where the branch's terminal misbehaved:
//
//   * The font was read once, in NOTIFICATION_ENTER_TREE, and never again. Changing the
//     editor theme left the widget measuring cells with the old font, so the grid and
//     the text drifted apart. Theme work now happens in NOTIFICATION_THEME_CHANGED,
//     which fires on entering the tree as well.
//   * Cell size could be zero if the theme had no font, and three code paths then
//     divided by it.
//   * The process was polled but its death was only noticed as a flag flip. It now
//     emits `process_exited`, so a panel can say so instead of showing a dead prompt.
//   * Ctrl-letter was mapped twice; see mcp_terminal_keys.cpp for what that cost.
//
// Compiled only where `MCP_TERMINAL_ENABLED` is defined. See this module's SCsub.

#ifdef MCP_TERMINAL_ENABLED

#include "mcp_pty.h"
#include "mcp_terminal_emulator.h"
#include "mcp_terminal_selection.h"

#include "scene/gui/control.h"
#include "scene/gui/scroll_container.h"
#include "scene/resources/font.h"

class MCPTerminalWidget : public Control {
	GDCLASS(MCPTerminalWidget, Control);

private:
	// Read in one go per poll. Sized so a chatty build log arrives in a frame or two
	// rather than a hundred.
	static constexpr int READ_BUFFER_SIZE = 65536;
	static constexpr int MAX_READS_PER_FRAME = 16;
	static constexpr float CURSOR_BLINK_SECONDS = 0.5f;
	static constexpr int FALLBACK_CELL_WIDTH = 8;
	static constexpr int FALLBACK_CELL_HEIGHT = 16;

	MCPTerminalEmulator emulator;
	MCPPty pty;
	bool running = false;

	Ref<Font> font;
	int font_size = 14;
	// Never zero: three call sites divide by these, and a theme with no monospace font
	// would otherwise take the editor down rather than look wrong.
	int cell_width = FALLBACK_CELL_WIDTH;
	int cell_height = FALLBACK_CELL_HEIGHT;

	Color theme_bg = Color(0.12f, 0.12f, 0.15f);
	Color theme_selection_bg = Color(0.35f, 0.55f, 0.85f);
	Color theme_selection_fg = Color(1.0f, 1.0f, 1.0f);
	Color theme_cursor_color = Color(0.8f, 0.8f, 0.8f, 0.7f);

	float cursor_blink_timer = 0.0f;
	bool cursor_blink_on = true;

	bool dragging = false;
	MCPTerminalSelection selection;

	ScrollContainer *scroll_container = nullptr;
	int last_scrollback_length = 0;
	bool stick_to_bottom = true;
	bool programmatic_scroll = false;

	uint8_t read_buffer[READ_BUFFER_SIZE];

	void _update_font();
	void _update_theme_colors();
	void _poll_pty();
	void _flush_emulator_output();
	void _draw_terminal();
	void _draw_cell(int p_row, int p_col, const MCPTerminalEmulator::Cell &p_cell);
	void _draw_cursor();
	Color _effective_fg(const MCPTerminalEmulator::Cell &p_cell) const;
	Color _effective_bg(const MCPTerminalEmulator::Cell &p_cell) const;
	void _scroll_to_bottom_now();
	void _on_scroll_changed(double p_value);

protected:
	void _notification(int p_what);
	virtual void gui_input(const Ref<InputEvent> &p_event) override;
	static void _bind_methods();

public:
	// Starts `p_command` under a new pty. `p_env` entries are "NAME=value". Returns false
	// and fills `r_error` when the process could not be started.
	bool start_process(const String &p_command, const Vector<String> &p_args,
			const Vector<String> &p_env, const String &p_working_dir, String &r_error);

	// Asks the child to exit, escalating to SIGKILL after a grace period. Returns at once.
	void request_stop();

	// Stops the child and closes the pty, blocking only as long as the child takes to die.
	void stop_process();

	bool is_process_running() const;
	int get_exit_code() const;

	// Types text into the terminal as though the user had, honouring the running child's
	// line discipline. Used by paste, and by anything that drives the terminal.
	void send_text(const String &p_text);

	MCPTerminalEmulator *get_emulator() { return &emulator; }
	MCPPty *get_pty() { return &pty; }

	String get_title() const { return emulator.get_title(); }

	void set_scroll_container(ScrollContainer *p_scroll_container);
	void scroll_to_bottom();
	bool is_stuck_to_bottom() const { return stick_to_bottom; }

	// Matches the emulator's grid to the visible area and tells the child about it.
	void update_terminal_size();

	virtual Size2 get_minimum_size() const override;

	MCPTerminalWidget();
	~MCPTerminalWidget();
};

#endif // MCP_TERMINAL_ENABLED
