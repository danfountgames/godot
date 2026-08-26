/**************************************************************************/
/*  mcp_terminal_widget.cpp                                               */
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

#include "mcp_terminal_widget.h"

#include "mcp_terminal_keys.h"

#include "core/input/input_event.h"
#include "core/object/class_db.h"
#include "core/os/keyboard.h"
#include "core/object/callable_mp.h"
#include "editor/editor_string_names.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_theme_manager.h"
#include "scene/theme/theme_db.h"
#include "servers/display/display_server.h"

MCPTerminalWidget::MCPTerminalWidget() {
	set_focus_mode(FOCUS_ALL);
	set_mouse_filter(MOUSE_FILTER_STOP);
	set_force_pass_scroll_events(false);
	memset(read_buffer, 0, sizeof(read_buffer));
}

MCPTerminalWidget::~MCPTerminalWidget() {
	// MCPPty's destructor closes too, so this is belt and braces - but a terminal that
	// leaks a `claude` process every time a tab closes is exactly the failure this port
	// exists to avoid, and saying so twice costs nothing.
	pty.close();
	running = false;
}

void MCPTerminalWidget::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_scroll_changed", "value"), &MCPTerminalWidget::_on_scroll_changed);

	ADD_SIGNAL(MethodInfo("process_exited", PropertyInfo(Variant::INT, "exit_code")));
	ADD_SIGNAL(MethodInfo("title_changed", PropertyInfo(Variant::STRING, "title")));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void MCPTerminalWidget::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (!emulator.is_initialized()) {
				// A starting size, replaced by update_terminal_size() as soon as the
				// widget has one. A child started before then still gets a sane TERM
				// geometry rather than 0x0.
				emulator.init(24, 80);
			}
			set_process_internal(true);
		} break;

		case NOTIFICATION_THEME_CHANGED: {
			// Both the font and the colours, together. The branch read the font once on
			// entering the tree and the colours on every theme change, so switching to a
			// theme with a different monospace font left the cell size measuring the old
			// one and every glyph landed slightly wrong.
			_update_font();
			_update_theme_colors();
			update_minimum_size();
			update_terminal_size();
			queue_redraw();
		} break;

		case NOTIFICATION_EXIT_TREE: {
			// Stop polling, but leave the child alone: a bottom panel leaves the tree when
			// the editor shuts down, and also when a dock is rearranged. Killing an agent
			// mid-answer because someone dragged a panel would be its own bug. The owner
			// decides when the process dies; the destructor is the backstop.
			set_process_internal(false);
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			if (running) {
				_poll_pty();
				_flush_emulator_output();
			}

			const int scrollback_length = emulator.get_scrollback_length();
			if (scrollback_length != last_scrollback_length) {
				last_scrollback_length = scrollback_length;
				update_minimum_size();
				if (stick_to_bottom) {
					// Deferred: the scroll range only grows once the container has
					// re-laid out, so scrolling now would land short.
					callable_mp(this, &MCPTerminalWidget::_scroll_to_bottom_now).call_deferred();
				}
			}

			if (stick_to_bottom) {
				_scroll_to_bottom_now();
			}

			const bool was_on = cursor_blink_on;
			cursor_blink_timer += get_process_delta_time();
			if (cursor_blink_timer >= CURSOR_BLINK_SECONDS) {
				cursor_blink_timer -= CURSOR_BLINK_SECONDS;
				cursor_blink_on = !cursor_blink_on;
			}

			if (emulator.is_dirty() || cursor_blink_on != was_on) {
				queue_redraw();
			}
		} break;

		case NOTIFICATION_RESIZED: {
			update_terminal_size();
		} break;

		case NOTIFICATION_DRAW: {
			_draw_terminal();
		} break;
	}
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

void MCPTerminalWidget::_update_font() {
	font = get_theme_font(SNAME("source"), EditorStringName(EditorFonts));
	if (font.is_null()) {
		font = ThemeDB::get_singleton()->get_fallback_font();
	}

	font_size = get_theme_font_size(SNAME("source_size"), EditorStringName(EditorFonts));
	if (font_size <= 0) {
		font_size = 14;
	}

	// Fall back rather than divide by zero later. A theme can legitimately have no
	// monospace font; a terminal drawn at the wrong pitch is recoverable, a crash is not.
	cell_width = FALLBACK_CELL_WIDTH;
	cell_height = FALLBACK_CELL_HEIGHT;
	if (font.is_valid()) {
		const int measured_width = (int)font->get_char_size('M', font_size).x;
		const int measured_height = (int)font->get_height(font_size);
		if (measured_width > 0) {
			cell_width = measured_width;
		}
		if (measured_height > 0) {
			cell_height = measured_height;
		}
	}
}

void MCPTerminalWidget::_update_theme_colors() {
	if (!EditorSettings::get_singleton()) {
		return;
	}

	const Color bg_color = EDITOR_GET("text_editor/theme/highlighting/background_color");
	const Color text_color = EDITOR_GET("text_editor/theme/highlighting/text_color");
	const Color caret_color = EDITOR_GET("text_editor/theme/highlighting/caret_color");
	const Color selection_color = EDITOR_GET("text_editor/theme/highlighting/selection_color");

	const Color keyword_color = EDITOR_GET("text_editor/theme/highlighting/keyword_color");
	const Color control_flow_color = EDITOR_GET("text_editor/theme/highlighting/control_flow_keyword_color");
	const Color base_type_color = EDITOR_GET("text_editor/theme/highlighting/base_type_color");
	const Color engine_type_color = EDITOR_GET("text_editor/theme/highlighting/engine_type_color");
	const Color string_color = EDITOR_GET("text_editor/theme/highlighting/string_color");
	const Color function_color = EDITOR_GET("text_editor/theme/highlighting/function_color");
	const Color number_color = EDITOR_GET("text_editor/theme/highlighting/number_color");
	const Color symbol_color = EDITOR_GET("text_editor/theme/highlighting/symbol_color");
	const Color comment_color = EDITOR_GET("text_editor/theme/highlighting/comment_color");

	const bool dark = EditorThemeManager::is_dark_theme();

	theme_bg = bg_color;
	theme_cursor_color = Color(caret_color.r, caret_color.g, caret_color.b, 0.7f);

	// The editor's selection colour, forced opaque enough to read as a filled background.
	theme_selection_bg = Color(selection_color.r, selection_color.g, selection_color.b, MAX(selection_color.a, 0.45f));
	theme_selection_fg = dark ? Color(1.0f, 1.0f, 1.0f) : Color(0.0f, 0.0f, 0.0f);

	// The ANSI palette, drawn from the editor's syntax colours so the terminal looks like
	// part of the editor rather than a window someone pasted in:
	//   Red     <- keyword          (errors)
	//   Green   <- number           (success, paths)
	//   Yellow  <- string           (warnings)
	//   Blue    <- function         (links, directories, tool names)
	//   Magenta <- control flow     (emphasis)
	//   Cyan    <- engine type      (info, headers)
	Color ansi16[16];

	if (dark) {
		ansi16[0] = bg_color.lerp(Color(0, 0, 0), 0.35f);
		ansi16[1] = keyword_color;
		ansi16[2] = number_color;
		ansi16[3] = Color(string_color.r, string_color.g, string_color.b * 0.7f);
		ansi16[4] = function_color;
		ansi16[5] = control_flow_color;
		ansi16[6] = engine_type_color;
		ansi16[7] = Color(text_color.r, text_color.g, text_color.b, 1.0f).lerp(Color(0.75f, 0.75f, 0.75f), 0.5f);

		ansi16[8] = Color(comment_color.r, comment_color.g, comment_color.b, 1.0f);
		ansi16[9] = Color(1.0f, 0.47f, 0.42f);
		ansi16[10] = base_type_color;
		ansi16[11] = string_color;
		ansi16[12] = symbol_color;
		ansi16[13] = Color(0.64f, 0.64f, 0.96f);
		ansi16[14] = Color(0.4f, 0.9f, 1.0f);
		ansi16[15] = Color(1.0f, 1.0f, 1.0f);
	} else {
		ansi16[0] = Color(0.0f, 0.0f, 0.0f);
		ansi16[1] = keyword_color;
		ansi16[2] = number_color;
		ansi16[3] = Color(0.6f, 0.42f, 0.0f);
		ansi16[4] = function_color;
		ansi16[5] = control_flow_color;
		ansi16[6] = engine_type_color;
		ansi16[7] = Color(0.35f, 0.35f, 0.35f);

		ansi16[8] = Color(comment_color.r, comment_color.g, comment_color.b, 1.0f);
		ansi16[9] = Color(0.8f, 0.22f, 0.22f);
		ansi16[10] = base_type_color;
		ansi16[11] = string_color;
		ansi16[12] = symbol_color;
		ansi16[13] = Color(0.36f, 0.18f, 0.72f);
		ansi16[14] = Color(0.0f, 0.6f, 0.6f);
		ansi16[15] = Color(text_color.r, text_color.g, text_color.b, 1.0f);
	}

	emulator.set_theme_colors(text_color, bg_color, ansi16);
}

// ---------------------------------------------------------------------------
// The pty
// ---------------------------------------------------------------------------

void MCPTerminalWidget::_poll_pty() {
	const String title_before = emulator.get_title();

	// Drain before polling. A child that has just exited may still have output sitting in
	// the pty buffer, and declaring it dead first would throw away its last words - which
	// on a failed command are the only interesting ones.
	for (int i = 0; i < MAX_READS_PER_FRAME; i++) {
		const int count = pty.read(read_buffer, READ_BUFFER_SIZE);
		if (count <= 0) {
			break;
		}
		emulator.process_input(read_buffer, count);
	}

	const bool still_running = pty.poll();
	if (!still_running && running) {
		running = false;
		emit_signal(SNAME("process_exited"), pty.get_exit_code());
	}

	if (emulator.get_title() != title_before) {
		emit_signal(SNAME("title_changed"), emulator.get_title());
	}
}

void MCPTerminalWidget::_flush_emulator_output() {
	const Vector<uint8_t> out = emulator.consume_output();
	if (!out.is_empty()) {
		pty.write(out.ptr(), out.size());
	}
}

bool MCPTerminalWidget::start_process(const String &p_command, const Vector<String> &p_args,
		const Vector<String> &p_env, const String &p_working_dir, String &r_error) {
	if (running) {
		stop_process();
	}

	if (!emulator.is_initialized()) {
		emulator.init(24, 80);
	}

	// Size the pty before the fork so the child's first draw is at the right width; a
	// shell that starts at 80 columns in a 200-column panel wraps its banner and never
	// redraws it.
	pty.resize(emulator.get_rows(), emulator.get_cols());

	if (!pty.start(p_command, p_args, p_env, p_working_dir, r_error)) {
		running = false;
		return false;
	}

	running = true;
	stick_to_bottom = true;
	grab_focus();
	return true;
}

void MCPTerminalWidget::request_stop() {
	pty.request_stop();
}

void MCPTerminalWidget::stop_process() {
	pty.close();
	running = false;
}

bool MCPTerminalWidget::is_process_running() const {
	return running && pty.is_running();
}

int MCPTerminalWidget::get_exit_code() const {
	return pty.get_exit_code();
}

void MCPTerminalWidget::send_text(const String &p_text) {
	if (!running || p_text.is_empty()) {
		return;
	}
	const CharString utf8 = p_text.utf8();
	pty.write((const uint8_t *)utf8.get_data(), utf8.length());
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void MCPTerminalWidget::_draw_terminal() {
	draw_rect(Rect2(Vector2(), get_size()), theme_bg);

	if (cell_width <= 0 || cell_height <= 0) {
		return;
	}

	const int scrollback_length = emulator.get_scrollback_length();
	const int total_rows = scrollback_length + emulator.get_rows();
	const int cols = emulator.get_cols();

	// Cull to what the scroll container is showing. A terminal with 2000 lines of
	// scrollback would otherwise draw 160,000 cells a frame.
	int first_row = 0;
	int last_row = total_rows;
	if (scroll_container) {
		const float scroll_y = scroll_container->get_v_scroll_bar()->get_value();
		const float viewport_height = scroll_container->get_size().y;
		first_row = MAX(0, (int)(scroll_y / cell_height));
		last_row = MIN(total_rows, (int)((scroll_y + viewport_height) / cell_height) + 1);
	}

	for (int row = first_row; row < last_row; row++) {
		for (int col = 0; col < cols; col++) {
			_draw_cell(row, col, mcp_terminal_combined_cell(emulator, row, col));
		}
	}

	_draw_cursor();
	emulator.clear_dirty();
}

void MCPTerminalWidget::_draw_cell(int p_row, int p_col, const MCPTerminalEmulator::Cell &p_cell) {
	const Vector2 pos(p_col * cell_width, p_row * cell_height);
	const float width = (float)(cell_width * MAX(1, p_cell.width));

	Color fg = _effective_fg(p_cell);
	const Color bg = _effective_bg(p_cell);

	if (selection.contains(p_row, p_col)) {
		draw_rect(Rect2(pos, Size2(width, cell_height)), theme_selection_bg);
		fg = theme_selection_fg;
	} else if (bg != theme_bg) {
		draw_rect(Rect2(pos, Size2(width, cell_height)), bg);
	}

	if (p_cell.text.is_empty() || font.is_null()) {
		return;
	}

	const Vector2 text_pos(pos.x, pos.y + font->get_ascent(font_size));
	draw_string(font, text_pos, p_cell.text, HORIZONTAL_ALIGNMENT_LEFT, width, font_size, fg);

	// Faux bold: the same glyph one pixel over. Real bold would need a second font face
	// with the same advance, and a monospace grid cannot tolerate one that has not.
	if (p_cell.bold) {
		draw_string(font, text_pos + Vector2(1, 0), p_cell.text, HORIZONTAL_ALIGNMENT_LEFT, width, font_size, fg);
	}

	if (p_cell.underline) {
		const float line_y = pos.y + cell_height - 1;
		draw_line(Vector2(pos.x, line_y), Vector2(pos.x + width, line_y), fg);
	}

	if (p_cell.strike) {
		const float line_y = pos.y + cell_height * 0.5f;
		draw_line(Vector2(pos.x, line_y), Vector2(pos.x + width, line_y), fg);
	}
}

void MCPTerminalWidget::_draw_cursor() {
	const MCPTerminalEmulator::CursorState cursor = emulator.get_cursor();
	if (!cursor.visible || !cursor_blink_on) {
		return;
	}

	const int scrollback_length = emulator.get_scrollback_length();
	const Vector2 pos(cursor.col * cell_width, (scrollback_length + cursor.row) * cell_height);

	switch (cursor.shape) {
		case 2: { // Underline.
			draw_rect(Rect2(Vector2(pos.x, pos.y + cell_height - 2), Size2(cell_width, 2)), theme_cursor_color);
		} break;
		case 3: { // Bar.
			draw_rect(Rect2(pos, Size2(2, cell_height)), theme_cursor_color);
		} break;
		default: { // Block.
			draw_rect(Rect2(pos, Size2(cell_width, cell_height)), theme_cursor_color);
		} break;
	}
}

Color MCPTerminalWidget::_effective_fg(const MCPTerminalEmulator::Cell &p_cell) const {
	return p_cell.reverse ? p_cell.bg : p_cell.fg;
}

Color MCPTerminalWidget::_effective_bg(const MCPTerminalEmulator::Cell &p_cell) const {
	return p_cell.reverse ? p_cell.fg : p_cell.bg;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void MCPTerminalWidget::gui_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventMouseButton> mouse_button = p_event;
	if (mouse_button.is_valid() && mouse_button->get_button_index() == MouseButton::LEFT) {
		if (mouse_button->is_pressed()) {
			grab_focus();
			dragging = true;
			selection.begin(mcp_terminal_cell_at(emulator, mouse_button->get_position(), cell_width, cell_height));
			queue_redraw();
			accept_event();
		} else if (dragging) {
			dragging = false;
			selection.extend(mcp_terminal_cell_at(emulator, mouse_button->get_position(), cell_width, cell_height));
			queue_redraw();
			accept_event();
		}
		return;
	}

	const Ref<InputEventMouseMotion> mouse_motion = p_event;
	if (mouse_motion.is_valid() && dragging) {
		selection.extend(mcp_terminal_cell_at(emulator, mouse_motion->get_position(), cell_width, cell_height));
		queue_redraw();
		accept_event();
		return;
	}

	const Ref<InputEventKey> key_event = p_event;
	if (key_event.is_null()) {
		return;
	}
	if (!key_event->is_pressed()) {
		return;
	}

	// Copy and paste go through the editor's own actions so Cmd works on macOS and Ctrl
	// everywhere else without this widget deciding which platform it is on.
	if (key_event->is_action("ui_copy", true) && selection.active) {
		DisplayServer::get_singleton()->clipboard_set(mcp_terminal_selection_text(emulator, selection));
		selection.clear();
		queue_redraw();
		accept_event();
		return;
	}

	if (key_event->is_action("ui_paste", true)) {
		send_text(DisplayServer::get_singleton()->clipboard_get());
		accept_event();
		return;
	}

	const Key keycode = key_event->get_keycode();

	// Typing replaces a selection everywhere else, so clear it - but not for the modifier
	// half of a chord, or holding Ctrl on the way to Ctrl-C would wipe what is being copied.
	if (selection.active && !mcp_terminal_is_modifier_keycode(keycode)) {
		selection.clear();
		queue_redraw();
	}

	accept_event();

	const int mod = mcp_terminal_modifiers(key_event->is_shift_pressed(), key_event->is_alt_pressed(), key_event->is_ctrl_pressed());
	const MCPTerminalKeyPress press = mcp_terminal_translate_key(keycode, key_event->get_unicode(), mod);

	if (press.sends_key()) {
		emulator.input_key(press.key, press.mod);
	} else if (press.sends_char()) {
		emulator.input_char(press.unicode, press.mod);
	} else {
		return;
	}

	_flush_emulator_output();

	// Typing means you want to see what you typed.
	if (!stick_to_bottom) {
		stick_to_bottom = true;
		_scroll_to_bottom_now();
	}
}

// ---------------------------------------------------------------------------
// Scrolling and size
// ---------------------------------------------------------------------------

Size2 MCPTerminalWidget::get_minimum_size() const {
	const int total_rows = emulator.get_scrollback_length() + emulator.get_rows();
	return Size2(cell_width * 40, total_rows * cell_height);
}

void MCPTerminalWidget::set_scroll_container(ScrollContainer *p_scroll_container) {
	if (scroll_container == p_scroll_container) {
		return;
	}

	// Disconnect the old one first. Re-parenting a terminal without this leaves the
	// previous container's scrollbar holding a callable into this widget, and it fires
	// long after the container stopped being ours.
	if (scroll_container && scroll_container->get_v_scroll_bar()) {
		scroll_container->get_v_scroll_bar()->disconnect(SceneStringName(value_changed), callable_mp(this, &MCPTerminalWidget::_on_scroll_changed));
	}

	scroll_container = p_scroll_container;

	if (scroll_container && scroll_container->get_v_scroll_bar()) {
		scroll_container->get_v_scroll_bar()->connect(SceneStringName(value_changed), callable_mp(this, &MCPTerminalWidget::_on_scroll_changed));
	}

	update_terminal_size();
}

void MCPTerminalWidget::_on_scroll_changed(double p_value) {
	if (programmatic_scroll || !scroll_container) {
		return;
	}

	// The user moved the bar. Follow the tail only while they are looking at it - this is
	// what makes reading back through a build log possible while it is still being written.
	ScrollBar *bar = scroll_container->get_v_scroll_bar();
	const double bottom = bar->get_max() - bar->get_page();
	stick_to_bottom = p_value >= bottom - cell_height;
}

void MCPTerminalWidget::scroll_to_bottom() {
	stick_to_bottom = true;
	_scroll_to_bottom_now();
}

void MCPTerminalWidget::_scroll_to_bottom_now() {
	if (!scroll_container) {
		return;
	}
	programmatic_scroll = true;
	scroll_container->set_v_scroll(INT_MAX);
	programmatic_scroll = false;
}

void MCPTerminalWidget::update_terminal_size() {
	if (cell_width <= 0 || cell_height <= 0) {
		return;
	}

	// The visible area, not this control's size: the control is as tall as all its
	// scrollback, and a child told it has 2000 rows would draw its status line off-screen.
	Size2 visible = scroll_container ? scroll_container->get_size() : get_size();
	if (visible.x <= 0 || visible.y <= 0) {
		return;
	}

	const int rows = MAX(1, (int)(visible.y / cell_height));
	const int cols = MAX(1, (int)(visible.x / cell_width));
	if (rows == emulator.get_rows() && cols == emulator.get_cols()) {
		return;
	}

	emulator.set_size(rows, cols);
	update_minimum_size();
	if (running) {
		pty.resize(rows, cols);
	}
}

#endif // MCP_TERMINAL_ENABLED
