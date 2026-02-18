/**************************************************************************/
/*  terminal_emulator.h                                                   */
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

#include "core/math/color.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

extern "C" {
#include <vterm.h>
}

class TerminalEmulator {
public:
	// A cell in the terminal grid, suitable for rendering.
	struct Cell {
		String text;       // UTF-8 string for this cell (may be empty for wide-char continuations).
		Color fg;          // Foreground color (Godot Color, 0-1 range).
		Color bg;          // Background color.
		bool bold = false;
		bool italic = false;
		bool underline = false;
		bool reverse = false;
		bool strike = false;
		int width = 1;     // 1 for normal, 2 for wide chars (the left cell of a wide char).
	};

	// Cursor info.
	struct CursorState {
		int row = 0;
		int col = 0;
		bool visible = true;
		int shape = 1; // 1=block, 2=underline, 3=bar.
	};

private:
	VTerm *vterm = nullptr;
	VTermScreen *vterm_screen = nullptr;
	int rows = 24;
	int cols = 80;
	bool dirty = true;
	CursorState cursor;
	String title;

	// Output buffer -- data libvterm wants sent to the PTY (e.g. key responses).
	Vector<uint8_t> output_buffer;

	// Scrollback buffer.
	struct ScrollbackLine {
		Vector<VTermScreenCell> cells;
	};
	Vector<ScrollbackLine> scrollback;
	static const int MAX_SCROLLBACK = 2000;

	// libvterm callbacks.
	static int _damage_cb(VTermRect rect, void *user);
	static int _movecursor_cb(VTermPos pos, VTermPos oldpos, int visible, void *user);
	static int _settermprop_cb(VTermProp prop, VTermValue *val, void *user);
	static int _bell_cb(void *user);
	static int _resize_cb(int rows, int cols, void *user);
	static int _sb_pushline_cb(int cols, const VTermScreenCell *cells, void *user);
	static int _sb_popline_cb(int cols, VTermScreenCell *cells, void *user);

	// Output callback for libvterm -> PTY data.
	static void _output_cb(const char *s, size_t len, void *user);

	// Helper: convert VTermColor to Godot Color.
	Color _vterm_color_to_godot(VTermColor col) const;

	// 256-color palette (standard xterm).
	static Color _index_to_color(int idx);

public:
	TerminalEmulator();
	~TerminalEmulator();

	// Initialize with given size. Must be called before use.
	void init(int p_rows, int p_cols);

	// Feed raw bytes from PTY into the terminal.
	void process_input(const uint8_t *p_data, int p_len);

	// Get output that libvterm wants sent to the PTY.
	Vector<uint8_t> consume_output();

	// Send a key press to libvterm (for keyboard input).
	void input_key(VTermKey key, VTermModifier mod);
	void input_char(uint32_t ch, VTermModifier mod);

	// Resize the terminal.
	void set_size(int p_rows, int p_cols);

	// Get cell content at a position. Returns false if out of bounds.
	Cell get_cell(int p_row, int p_col) const;

	// Accessors.
	int get_rows() const { return rows; }
	int get_cols() const { return cols; }
	CursorState get_cursor() const { return cursor; }
	bool is_dirty() const { return dirty; }
	void clear_dirty() { dirty = false; }
	String get_title() const { return title; }

	// Scrollback.
	int get_scrollback_length() const { return scrollback.size(); }
	Cell get_scrollback_cell(int p_scrollback_idx, int p_col) const;
};

#endif // MCP_TERMINAL_ENABLED
