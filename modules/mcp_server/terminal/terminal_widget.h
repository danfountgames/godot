/**************************************************************************/
/*  terminal_widget.h                                                     */
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

#include "terminal_emulator.h"
#include "pty_manager.h"

#include "scene/gui/control.h"
#include "scene/gui/scroll_container.h"
#include "scene/resources/font.h"

class TerminalWidget : public Control {
	GDCLASS(TerminalWidget, Control)

private:
	TerminalEmulator emulator;
	PTYManager pty;
	bool running = false;

	// Rendering config.
	Ref<Font> font;
	int font_size = 14;
	int cell_width = 0;   // Computed from font.
	int cell_height = 0;  // Computed from font.

	// Blink state for cursor.
	float cursor_blink_timer = 0.0f;
	bool cursor_visible_blink = true;

	// Selection state.
	bool selecting = false;
	Vector2i sel_start; // (row, col) in grid coordinates.
	Vector2i sel_end;
	bool has_selection = false;

	// ScrollContainer integration.
	ScrollContainer *scroll_container = nullptr;
	int last_scrollback_len = 0;
	bool stick_to_bottom = true;
	bool programmatic_scroll = false;

	// Read buffer for PTY polling.
	static const int READ_BUFFER_SIZE = 65536;
	uint8_t read_buffer[READ_BUFFER_SIZE];

	// Methods.
	void _calculate_cell_size();
	void _poll_pty();
	void _send_output_to_pty();
	void _draw_terminal();
	void _draw_cell(int p_row, int p_col, const TerminalEmulator::Cell &p_cell);
	void _draw_cursor();
	Color _effective_fg(const TerminalEmulator::Cell &p_cell) const;
	Color _effective_bg(const TerminalEmulator::Cell &p_cell) const;

	// Selection helpers.
	Vector2i _pixel_to_cell(Vector2 p_pos) const;
	String _get_selected_text() const;
	bool _is_cell_selected(int p_row, int p_col) const;

	// Scroll helpers.
	bool _is_at_bottom() const;
	void _do_scroll_to_bottom();

	void _notification(int p_what);
	virtual void gui_input(const Ref<InputEvent> &p_event) override;
	bool _is_modifier_key(Key p_keycode) const;

	// Keyboard mapping.
	VTermKey _godot_key_to_vterm(Key p_key) const;
	VTermModifier _godot_mods_to_vterm(const Ref<InputEvent> &p_event) const;

protected:
	static void _bind_methods();

public:
	// Start a process in the terminal.
	bool start_process(const String &p_command, const Vector<String> &p_args, const Vector<String> &p_env = Vector<String>());

	// Stop the running process.
	void stop_process();

	// Is a process running?
	bool is_process_running() const;

	// Access to sub-components.
	TerminalEmulator *get_emulator() { return &emulator; }
	PTYManager *get_pty() { return &pty; }

	void set_scroll_container(ScrollContainer *p_sc);
	void scroll_to_bottom();
	void unstick_from_bottom();
	bool is_programmatic_scroll() const { return programmatic_scroll; }
	void update_pty_size();

	virtual Size2 get_minimum_size() const override;

	TerminalWidget();
	~TerminalWidget();
};

#endif // MCP_TERMINAL_ENABLED
