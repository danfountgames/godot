/**************************************************************************/
/*  terminal_widget.cpp                                                   */
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

#include "terminal_widget.h"

#include "core/input/input_event.h"
#include "core/os/keyboard.h"
#include "scene/theme/theme_db.h"
#include "servers/display/display_server.h"

///////////////////////////////////////////////////////////////////////////////
// Constructor / Destructor
///////////////////////////////////////////////////////////////////////////////

TerminalWidget::TerminalWidget() {
	set_focus_mode(FOCUS_ALL);
	set_clip_contents(true);
	set_mouse_filter(MOUSE_FILTER_STOP);
	set_force_pass_scroll_events(false);
}

TerminalWidget::~TerminalWidget() {
	stop_process();
}

///////////////////////////////////////////////////////////////////////////////
// Bind methods
///////////////////////////////////////////////////////////////////////////////

void TerminalWidget::_bind_methods() {
	// Empty for now -- no GDScript-exposed methods needed.
}

///////////////////////////////////////////////////////////////////////////////
// Notification
///////////////////////////////////////////////////////////////////////////////

void TerminalWidget::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			// Load a monospace font from the editor theme.
			font = get_theme_font("source", "EditorFonts");
			if (font.is_null()) {
				font = ThemeDB::get_singleton()->get_fallback_font();
			}
			font_size = get_theme_font_size("source_size", "EditorFonts");
			if (font_size <= 0) {
				font_size = 14;
			}

			_calculate_cell_size();
			_recalculate_grid_size();

			int grid_cols = MAX(1, (int)(get_size().x / cell_width));
			int grid_rows = MAX(1, (int)(get_size().y / cell_height));
			emulator.init(grid_rows, grid_cols);

			set_process_internal(true);
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			if (running) {
				_poll_pty();
				_send_output_to_pty();
				if (!pty.is_running()) {
					running = false;
				}
			}

			// Update cursor blink.
			bool prev_blink = cursor_visible_blink;
			cursor_blink_timer += get_process_delta_time();
			if (cursor_blink_timer >= 0.5f) {
				cursor_blink_timer -= 0.5f;
				cursor_visible_blink = !cursor_visible_blink;
			}

			if (emulator.is_dirty() || cursor_visible_blink != prev_blink) {
				queue_redraw();
			}
		} break;

		case NOTIFICATION_DRAW: {
			_draw_terminal();
		} break;

		case NOTIFICATION_RESIZED: {
			_recalculate_grid_size();

			if (emulator.get_rows() > 0) {
				int new_cols = MAX(1, (int)(get_size().x / cell_width));
				int new_rows = MAX(1, (int)(get_size().y / cell_height));
				emulator.set_size(new_rows, new_cols);
				if (running) {
					pty.resize(new_rows, new_cols);
				}
			}
		} break;
	}
}

///////////////////////////////////////////////////////////////////////////////
// Cell size / grid size calculation
///////////////////////////////////////////////////////////////////////////////

void TerminalWidget::_calculate_cell_size() {
	cell_width = font->get_char_size('M', font_size).x;
	cell_height = font->get_height(font_size);

	if (cell_width <= 0) {
		cell_width = 8;
	}
	if (cell_height <= 0) {
		cell_height = 16;
	}
}

void TerminalWidget::_recalculate_grid_size() {
	if (cell_width <= 0 || cell_height <= 0) {
		return;
	}

	int new_cols = MAX(1, (int)(get_size().x / cell_width));
	int new_rows = MAX(1, (int)(get_size().y / cell_height));

	if (new_rows != emulator.get_rows() || new_cols != emulator.get_cols()) {
		// Sizes differ -- the NOTIFICATION_RESIZED handler will apply them.
	}
}

///////////////////////////////////////////////////////////////////////////////
// PTY polling / output
///////////////////////////////////////////////////////////////////////////////

void TerminalWidget::_poll_pty() {
	for (int i = 0; i < 10; i++) {
		int bytes = pty.read_pty(read_buffer, READ_BUFFER_SIZE);
		if (bytes > 0) {
			emulator.process_input(read_buffer, bytes);
		} else {
			break;
		}
	}
}

void TerminalWidget::_send_output_to_pty() {
	Vector<uint8_t> out = emulator.consume_output();
	if (out.size() > 0) {
		pty.write_pty(out.ptr(), out.size());
	}
}

///////////////////////////////////////////////////////////////////////////////
// Drawing
///////////////////////////////////////////////////////////////////////////////

void TerminalWidget::_draw_terminal() {
	// Clear background.
	Color default_bg(0.12f, 0.12f, 0.15f);
	draw_rect(Rect2(Vector2(), get_size()), default_bg);

	// Compute vertical offset to eliminate bottom whitespace gap.
	// Push the fractional cell remainder to the top (under the toolbar).
	int visible_rows = MAX(1, (int)(get_size().y / cell_height));
	y_offset = get_size().y - (visible_rows * cell_height);

	int rows = emulator.get_rows();
	int cols = emulator.get_cols();

	for (int row = 0; row < rows; row++) {
		for (int col = 0; col < cols; col++) {
			TerminalEmulator::Cell cell = emulator.get_cell(row, col);
			_draw_cell(row, col, cell);
		}
	}

	_draw_cursor();
	emulator.clear_dirty();
}

void TerminalWidget::_draw_cell(int p_row, int p_col, const TerminalEmulator::Cell &p_cell) {
	Vector2 pos(p_col * cell_width, p_row * cell_height + y_offset);
	Color default_bg(0.12f, 0.12f, 0.15f);

	Color fg = _effective_fg(p_cell);
	Color bg = _effective_bg(p_cell);

	// If cell is selected, invert fg/bg.
	bool selected = _is_cell_selected(p_row, p_col);
	if (selected) {
		Color sel_bg(0.35f, 0.55f, 0.85f);
		Color sel_fg(1.0f, 1.0f, 1.0f);
		draw_rect(Rect2(pos, Size2(cell_width * p_cell.width, cell_height)), sel_bg);
		fg = sel_fg;
	} else if (bg != default_bg) {
		draw_rect(Rect2(pos, Size2(cell_width * p_cell.width, cell_height)), bg);
	}

	if (!p_cell.text.is_empty()) {
		Vector2 text_pos(pos.x, pos.y + font->get_ascent(font_size));
		float text_width = (float)(cell_width * p_cell.width);

		draw_string(font, text_pos, p_cell.text, HORIZONTAL_ALIGNMENT_LEFT, text_width, font_size, fg);

		// Simple bold effect: draw the string again offset by 1 pixel to the right.
		if (p_cell.bold) {
			draw_string(font, text_pos + Vector2(1, 0), p_cell.text, HORIZONTAL_ALIGNMENT_LEFT, text_width, font_size, fg);
		}

		// Underline.
		if (p_cell.underline) {
			float line_y = pos.y + cell_height - 1;
			draw_line(Vector2(pos.x, line_y), Vector2(pos.x + cell_width * p_cell.width, line_y), fg);
		}

		// Strikethrough.
		if (p_cell.strike) {
			float line_y = pos.y + cell_height * 0.5f;
			draw_line(Vector2(pos.x, line_y), Vector2(pos.x + cell_width * p_cell.width, line_y), fg);
		}
	}
}

void TerminalWidget::_draw_cursor() {
	TerminalEmulator::CursorState cs = emulator.get_cursor();

	if (!cs.visible || !cursor_visible_blink) {
		return;
	}

	Vector2 pos(cs.col * cell_width, cs.row * cell_height + y_offset);
	Color cursor_color(0.8f, 0.8f, 0.8f, 0.7f);

	switch (cs.shape) {
		case 1: {
			// Block cursor.
			draw_rect(Rect2(pos, Size2(cell_width, cell_height)), cursor_color);
		} break;

		case 2: {
			// Underline cursor.
			draw_rect(Rect2(Vector2(pos.x, pos.y + cell_height - 2), Size2(cell_width, 2)), cursor_color);
		} break;

		case 3: {
			// Bar cursor.
			draw_rect(Rect2(pos, Size2(2, cell_height)), cursor_color);
		} break;
	}
}

Color TerminalWidget::_effective_fg(const TerminalEmulator::Cell &p_cell) const {
	if (p_cell.reverse) {
		return p_cell.bg;
	}
	return p_cell.fg;
}

Color TerminalWidget::_effective_bg(const TerminalEmulator::Cell &p_cell) const {
	if (p_cell.reverse) {
		return p_cell.fg;
	}
	return p_cell.bg;
}

///////////////////////////////////////////////////////////////////////////////
// Input handling
///////////////////////////////////////////////////////////////////////////////

void TerminalWidget::gui_input(const Ref<InputEvent> &p_event) {
	// --- Mouse button events (scroll + selection) ---
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid()) {
		// Scroll wheel.
		if (mb->is_pressed() && mb->get_button_index() == MouseButton::WHEEL_UP) {
			emulator.scroll_up(3);
			queue_redraw();
			accept_event();
			return;
		}
		if (mb->is_pressed() && mb->get_button_index() == MouseButton::WHEEL_DOWN) {
			emulator.scroll_down(3);
			queue_redraw();
			accept_event();
			return;
		}

		// Left click — start selection.
		if (mb->is_pressed() && mb->get_button_index() == MouseButton::LEFT) {
			grab_focus();
			selecting = true;
			sel_start = _pixel_to_cell(mb->get_position());
			sel_end = sel_start;
			has_selection = false;
			queue_redraw();
			accept_event();
			return;
		}
		if (!mb->is_pressed() && mb->get_button_index() == MouseButton::LEFT && selecting) {
			selecting = false;
			sel_end = _pixel_to_cell(mb->get_position());
			has_selection = (sel_start != sel_end);
			queue_redraw();
			accept_event();
			return;
		}
	}

	// --- Mouse motion events (drag selection) ---
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && selecting) {
		sel_end = _pixel_to_cell(mm->get_position());
		has_selection = (sel_start != sel_end);
		queue_redraw();
		accept_event();
		return;
	}

	// --- Key events ---
	Ref<InputEventKey> key_event = p_event;
	if (key_event.is_valid()) {
		if (!key_event->is_pressed() && !key_event->is_echo()) {
			return;
		}

		// Clipboard shortcuts via Godot input actions (handles Cmd/Ctrl correctly).
		if (key_event->is_action("ui_copy", true) && has_selection) {
			String text = _get_selected_text();
			DisplayServer::get_singleton()->clipboard_set(text);
			has_selection = false;
			queue_redraw();
			accept_event();
			return;
		}

		if (key_event->is_action("ui_paste", true)) {
			String clipboard = DisplayServer::get_singleton()->clipboard_get();
			if (!clipboard.is_empty() && running) {
				CharString utf8 = clipboard.utf8();
				pty.write_pty((const uint8_t *)utf8.get_data(), utf8.length());
			}
			accept_event();
			return;
		}

		// Don't clear selection on modifier-only keys (Cmd, Ctrl, Shift, Alt).
		Key keycode = key_event->get_keycode();
		if (has_selection && !_is_modifier_key(keycode)) {
			has_selection = false;
			queue_redraw();
		}

		accept_event();

		VTermModifier mod = _godot_mods_to_vterm(p_event);
		VTermKey vk = _godot_key_to_vterm(keycode);

		if (vk != VTERM_KEY_NONE) {
			emulator.input_key(vk, mod);
		} else if (key_event->get_unicode() > 0) {
			uint32_t ch = key_event->get_unicode();

			// Handle Ctrl+letter: map to control codes 1-26.
			if ((mod & VTERM_MOD_CTRL) && ch >= 'a' && ch <= 'z') {
				ch = ch - 'a' + 1;
			}

			emulator.input_char(ch, mod);
		}

		_send_output_to_pty();

		// Typing should scroll to bottom if scrolled up.
		if (emulator.get_scroll_offset() > 0) {
			emulator.scroll_to_bottom();
		}
	}
}

bool TerminalWidget::_is_modifier_key(Key p_keycode) const {
	return p_keycode == Key::SHIFT ||
			p_keycode == Key::CTRL ||
			p_keycode == Key::ALT ||
			p_keycode == Key::META ||
			p_keycode == Key::CAPSLOCK;
}

///////////////////////////////////////////////////////////////////////////////
// Selection helpers
///////////////////////////////////////////////////////////////////////////////

Vector2i TerminalWidget::_pixel_to_cell(Vector2 p_pos) const {
	int col = CLAMP((int)(p_pos.x / cell_width), 0, emulator.get_cols() - 1);
	int row = CLAMP((int)((p_pos.y - y_offset) / cell_height), 0, emulator.get_rows() - 1);
	return Vector2i(row, col);
}

bool TerminalWidget::_is_cell_selected(int p_row, int p_col) const {
	if (!has_selection) {
		return false;
	}

	// Normalize so start <= end in reading order.
	Vector2i start = sel_start;
	Vector2i end = sel_end;
	if (start.x > end.x || (start.x == end.x && start.y > end.y)) {
		SWAP(start, end);
	}

	if (p_row < start.x || p_row > end.x) {
		return false;
	}
	if (p_row == start.x && p_col < start.y) {
		return false;
	}
	if (p_row == end.x && p_col > end.y) {
		return false;
	}
	return true;
}

String TerminalWidget::_get_selected_text() const {
	if (!has_selection) {
		return String();
	}

	Vector2i start = sel_start;
	Vector2i end = sel_end;
	if (start.x > end.x || (start.x == end.x && start.y > end.y)) {
		SWAP(start, end);
	}

	String result;
	for (int row = start.x; row <= end.x; row++) {
		int col_begin = (row == start.x) ? start.y : 0;
		int col_end = (row == end.x) ? end.y : emulator.get_cols() - 1;

		String line;
		for (int col = col_begin; col <= col_end; col++) {
			TerminalEmulator::Cell cell = emulator.get_cell(row, col);
			if (!cell.text.is_empty()) {
				line += cell.text;
			} else {
				line += " ";
			}
		}

		// Trim trailing spaces from each line.
		line = line.rstrip(" ");
		result += line;

		if (row < end.x) {
			result += "\n";
		}
	}

	return result;
}

///////////////////////////////////////////////////////////////////////////////
// Key mapping
///////////////////////////////////////////////////////////////////////////////

VTermKey TerminalWidget::_godot_key_to_vterm(Key p_key) const {
	switch (p_key) {
		case Key::ENTER:
			return VTERM_KEY_ENTER;
		case Key::TAB:
			return VTERM_KEY_TAB;
		case Key::BACKSPACE:
			return VTERM_KEY_BACKSPACE;
		case Key::ESCAPE:
			return VTERM_KEY_ESCAPE;
		case Key::UP:
			return VTERM_KEY_UP;
		case Key::DOWN:
			return VTERM_KEY_DOWN;
		case Key::LEFT:
			return VTERM_KEY_LEFT;
		case Key::RIGHT:
			return VTERM_KEY_RIGHT;
		case Key::INSERT:
			return VTERM_KEY_INS;
		case Key::KEY_DELETE:
			return VTERM_KEY_DEL;
		case Key::HOME:
			return VTERM_KEY_HOME;
		case Key::END:
			return VTERM_KEY_END;
		case Key::PAGEUP:
			return VTERM_KEY_PAGEUP;
		case Key::PAGEDOWN:
			return VTERM_KEY_PAGEDOWN;
		case Key::F1:
			return (VTermKey)VTERM_KEY_FUNCTION(1);
		case Key::F2:
			return (VTermKey)VTERM_KEY_FUNCTION(2);
		case Key::F3:
			return (VTermKey)VTERM_KEY_FUNCTION(3);
		case Key::F4:
			return (VTermKey)VTERM_KEY_FUNCTION(4);
		case Key::F5:
			return (VTermKey)VTERM_KEY_FUNCTION(5);
		case Key::F6:
			return (VTermKey)VTERM_KEY_FUNCTION(6);
		case Key::F7:
			return (VTermKey)VTERM_KEY_FUNCTION(7);
		case Key::F8:
			return (VTermKey)VTERM_KEY_FUNCTION(8);
		case Key::F9:
			return (VTermKey)VTERM_KEY_FUNCTION(9);
		case Key::F10:
			return (VTermKey)VTERM_KEY_FUNCTION(10);
		case Key::F11:
			return (VTermKey)VTERM_KEY_FUNCTION(11);
		case Key::F12:
			return (VTermKey)VTERM_KEY_FUNCTION(12);
		case Key::KP_0:
			return VTERM_KEY_KP_0;
		case Key::KP_1:
			return VTERM_KEY_KP_1;
		case Key::KP_2:
			return VTERM_KEY_KP_2;
		case Key::KP_3:
			return VTERM_KEY_KP_3;
		case Key::KP_4:
			return VTERM_KEY_KP_4;
		case Key::KP_5:
			return VTERM_KEY_KP_5;
		case Key::KP_6:
			return VTERM_KEY_KP_6;
		case Key::KP_7:
			return VTERM_KEY_KP_7;
		case Key::KP_8:
			return VTERM_KEY_KP_8;
		case Key::KP_9:
			return VTERM_KEY_KP_9;
		case Key::KP_MULTIPLY:
			return VTERM_KEY_KP_MULT;
		case Key::KP_ADD:
			return VTERM_KEY_KP_PLUS;
		case Key::KP_SUBTRACT:
			return VTERM_KEY_KP_MINUS;
		case Key::KP_PERIOD:
			return VTERM_KEY_KP_PERIOD;
		case Key::KP_DIVIDE:
			return VTERM_KEY_KP_DIVIDE;
		case Key::KP_ENTER:
			return VTERM_KEY_KP_ENTER;
		default:
			return VTERM_KEY_NONE;
	}
}

VTermModifier TerminalWidget::_godot_mods_to_vterm(const Ref<InputEvent> &p_event) const {
	int mod = VTERM_MOD_NONE;

	Ref<InputEventWithModifiers> m = p_event;
	if (m.is_valid()) {
		if (m->is_shift_pressed()) {
			mod |= VTERM_MOD_SHIFT;
		}
		if (m->is_alt_pressed()) {
			mod |= VTERM_MOD_ALT;
		}
		if (m->is_ctrl_pressed()) {
			mod |= VTERM_MOD_CTRL;
		}
	}

	return (VTermModifier)mod;
}

///////////////////////////////////////////////////////////////////////////////
// Process management
///////////////////////////////////////////////////////////////////////////////

bool TerminalWidget::start_process(const String &p_command, const Vector<String> &p_args, const Vector<String> &p_env) {
	if (running) {
		stop_process();
	}

	running = pty.fork_and_exec(p_command, p_args, p_env);
	if (running) {
		pty.resize(emulator.get_rows(), emulator.get_cols());
	}

	grab_focus();
	return running;
}

void TerminalWidget::stop_process() {
	if (running) {
		pty.kill_child();
		pty.close_pty();
		running = false;
	}
}

bool TerminalWidget::is_process_running() const {
	return running && pty.is_running();
}

///////////////////////////////////////////////////////////////////////////////
// Minimum size
///////////////////////////////////////////////////////////////////////////////

Size2 TerminalWidget::get_minimum_size() const {
	return Size2(cell_width * 40, cell_height * 10);
}

#endif // MCP_TERMINAL_ENABLED
