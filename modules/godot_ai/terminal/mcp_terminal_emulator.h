/**************************************************************************/
/*  mcp_terminal_emulator.h                                               */
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

// The VT state machine behind the terminal, wrapping the vendored libvterm.
//
// Ported from the `GodotBeamDev` branch: unlike the pty layer this part was sound, and
// a VT parser is exactly the kind of code not to rewrite for taste. What changed is the
// naming, the build gate, a shared callback table that is now const rather than a
// mutable function-local static, and this header, which no longer exposes libvterm.
//
// Everything vterm-shaped now lives in an opaque `Impl` defined in the .cpp. That is
// not tidiness for its own sake: it is what lets a caller outside this module - the
// test suite, above all - include this header knowing only that the build has a
// terminal, without also needing the vendored library on its include path. The keys
// and modifiers below are ours, and the .cpp static_asserts that they still line up
// with libvterm's so the translation stays a cast.
//
// Compiled only where `MCP_TERMINAL_ENABLED` is defined - editor builds on platforms
// with a pty. See this module's SCsub.

#ifdef MCP_TERMINAL_ENABLED

#include "core/math/color.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

// Defined in the .cpp: everything libvterm-shaped lives in here.
struct MCPTerminalEmulatorImpl;

class MCPTerminalEmulator {
public:
	// A cell in the terminal grid, suitable for rendering.
	struct Cell {
		String text; // UTF-8 string for this cell (may be empty for wide-char continuations).
		Color fg; // Foreground color (Godot Color, 0-1 range).
		Color bg; // Background color.
		bool bold = false;
		bool italic = false;
		bool underline = false;
		bool reverse = false;
		bool strike = false;
		int width = 1; // 1 for normal, 2 for wide chars (the left cell of a wide char).
	};

	// Cursor info.
	struct CursorState {
		int row = 0;
		int col = 0;
		bool visible = true;
		int shape = 1; // 1=block, 2=underline, 3=bar.
	};

	// Non-character keys a caller can send. The values mirror libvterm's `VTermKey`;
	// the .cpp asserts that, so a mismatch after a library update is a build error
	// rather than a terminal where the arrow keys quietly do something else.
	enum Key {
		KEY_NONE = 0,
		KEY_ENTER = 1,
		KEY_TAB = 2,
		KEY_BACKSPACE = 3,
		KEY_ESCAPE = 4,
		KEY_UP = 5,
		KEY_DOWN = 6,
		KEY_LEFT = 7,
		KEY_RIGHT = 8,
		KEY_INS = 9,
		KEY_DEL = 10,
		KEY_HOME = 11,
		KEY_END = 12,
		KEY_PAGEUP = 13,
		KEY_PAGEDOWN = 14,
		KEY_FUNCTION_0 = 256, // Add n for Fn; KEY_FUNCTION_0 + 1 is F1.
		KEY_FUNCTION_MAX = 511,
		KEY_KP_0 = 512,
		KEY_KP_1 = 513,
		KEY_KP_2 = 514,
		KEY_KP_3 = 515,
		KEY_KP_4 = 516,
		KEY_KP_5 = 517,
		KEY_KP_6 = 518,
		KEY_KP_7 = 519,
		KEY_KP_8 = 520,
		KEY_KP_9 = 521,
		KEY_KP_MULT = 522,
		KEY_KP_PLUS = 523,
		KEY_KP_COMMA = 524,
		KEY_KP_MINUS = 525,
		KEY_KP_PERIOD = 526,
		KEY_KP_DIVIDE = 527,
		KEY_KP_ENTER = 528,
		KEY_KP_EQUAL = 529,
	};

	// Modifier bitmask, also mirroring libvterm's `VTermModifier`.
	enum Mod {
		MOD_NONE = 0x00,
		MOD_SHIFT = 0x01,
		MOD_ALT = 0x02,
		MOD_CTRL = 0x04,
	};

private:
	MCPTerminalEmulatorImpl *impl = nullptr;

public:
	MCPTerminalEmulator();
	~MCPTerminalEmulator();

	// One emulator owns one libvterm instance and hands it a `this` pointer as user
	// data, so copying one would give two objects the same callbacks.
	MCPTerminalEmulator(const MCPTerminalEmulator &) = delete;
	MCPTerminalEmulator &operator=(const MCPTerminalEmulator &) = delete;

	// Initialize with given size. Must be called before use; calling it again resets
	// the grid, the scrollback and the pending output.
	void init(int p_rows, int p_cols);
	bool is_initialized() const;

	// Set theme colors. May be called before or after init().
	// p_fg/p_bg are default foreground/background. p_ansi16 is an array of 16 ANSI colors.
	void set_theme_colors(const Color &p_fg, const Color &p_bg, const Color *p_ansi16);

	// Accessors for default colors (used by the widget for background fill, etc.).
	Color get_default_fg() const;
	Color get_default_bg() const;

	// Feed raw bytes from PTY into the terminal.
	void process_input(const uint8_t *p_data, int p_len);

	// Take the bytes libvterm wants sent to the PTY, emptying the buffer.
	Vector<uint8_t> consume_output();

	// Send a key press to libvterm (for keyboard input).
	void input_key(Key p_key, int p_mod);
	void input_char(uint32_t p_char, int p_mod);

	// Resize the terminal.
	void set_size(int p_rows, int p_cols);

	// Cell content at a position. Out of bounds yields a blank cell rather than a read
	// past the end: the widget draws from a rect it computed separately, and the two can
	// disagree for a frame after a resize.
	Cell get_cell(int p_row, int p_col) const;

	// Accessors.
	int get_rows() const;
	int get_cols() const;
	CursorState get_cursor() const;
	bool is_dirty() const;
	void clear_dirty();
	String get_title() const;

	// Scrollback, oldest first: index 0 is the oldest line still retained and
	// `get_scrollback_length() - 1` is the one that scrolled off the top most recently,
	// so the buffer reads continuously into row 0 of the live grid.
	int get_scrollback_length() const;
	Cell get_scrollback_cell(int p_scrollback_idx, int p_col) const;
};

#endif // MCP_TERMINAL_ENABLED
