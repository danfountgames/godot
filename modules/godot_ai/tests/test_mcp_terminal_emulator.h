/**************************************************************************/
/*  test_mcp_terminal_emulator.h                                          */
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

#ifndef TEST_MCP_TERMINAL_EMULATOR_H
#define TEST_MCP_TERMINAL_EMULATOR_H

#ifdef MCP_TERMINAL_ENABLED

#include "modules/godot_ai/terminal/mcp_terminal_emulator.h"

#include "tests/test_macros.h"

namespace TestMCPTerminalEmulator {

static void feed(MCPTerminalEmulator &p_emulator, const String &p_text) {
	const CharString utf8 = p_text.utf8();
	p_emulator.process_input((const uint8_t *)utf8.get_data(), utf8.length());
}

// The visible text of one row, trailing blanks trimmed.
static String row_text(const MCPTerminalEmulator &p_emulator, int p_row) {
	String out;
	for (int column = 0; column < p_emulator.get_cols(); column++) {
		out += p_emulator.get_cell(p_row, column).text;
	}
	return out.strip_edges(false, true);
}

TEST_CASE("[godot_ai] The emulator lays plain text into the grid") {
	MCPTerminalEmulator emulator;
	emulator.init(10, 40);

	feed(emulator, "hello");
	CHECK(row_text(emulator, 0) == "hello");
	CHECK(emulator.get_cursor().row == 0);
	CHECK(emulator.get_cursor().col == 5);
}

TEST_CASE("[godot_ai] Carriage return and line feed move the cursor as a terminal does") {
	MCPTerminalEmulator emulator;
	emulator.init(10, 40);

	feed(emulator, "first\r\nsecond");
	CHECK(row_text(emulator, 0) == "first");
	CHECK(row_text(emulator, 1) == "second");
	CHECK(emulator.get_cursor().row == 1);
}

TEST_CASE("[godot_ai] A carriage return alone overwrites the line in place") {
	MCPTerminalEmulator emulator;
	emulator.init(10, 40);

	// The progress-bar idiom: return to column zero and redraw. A terminal that treated
	// this as a newline would turn one spinner into a thousand lines of scrollback.
	feed(emulator, "aaaaa\rbb");
	CHECK(row_text(emulator, 0) == "bbaaa");
	CHECK(emulator.get_cursor().row == 0);
}

TEST_CASE("[godot_ai] SGR escape sequences colour the cells they cover") {
	MCPTerminalEmulator emulator;

	Color palette[16];
	for (int i = 0; i < 16; i++) {
		palette[i] = Color(0, 0, 0);
	}
	palette[1] = Color(1, 0, 0); // ANSI red.

	// Colours set before init(), the order the widget uses: it reads the editor theme in
	// NOTIFICATION_THEME_CHANGED, which can arrive before the pty is up.
	emulator.set_theme_colors(Color(1, 1, 1), Color(0, 0, 0), palette);
	emulator.init(10, 40);

	feed(emulator, "\x1b[31mred\x1b[0mplain");

	const MCPTerminalEmulator::Cell coloured = emulator.get_cell(0, 0);
	CHECK(coloured.text == "r");
	CHECK(coloured.fg.is_equal_approx(Color(1, 0, 0)));

	// After the reset the cell takes the default foreground again.
	const MCPTerminalEmulator::Cell plain = emulator.get_cell(0, 3);
	CHECK(plain.text == "p");
	CHECK(plain.fg.is_equal_approx(Color(1, 1, 1)));
}

TEST_CASE("[godot_ai] Bold and underline survive into the cell attributes") {
	MCPTerminalEmulator emulator;
	emulator.init(10, 40);

	feed(emulator, "\x1b[1mB\x1b[0m\x1b[4mU\x1b[0m");
	CHECK(emulator.get_cell(0, 0).bold);
	CHECK_FALSE(emulator.get_cell(0, 0).underline);
	CHECK(emulator.get_cell(0, 1).underline);
	CHECK_FALSE(emulator.get_cell(0, 1).bold);
}

TEST_CASE("[godot_ai] Clearing the screen empties it") {
	MCPTerminalEmulator emulator;
	emulator.init(10, 40);

	feed(emulator, "some text here");
	REQUIRE(row_text(emulator, 0) == "some text here");

	feed(emulator, "\x1b[2J\x1b[H");
	CHECK(row_text(emulator, 0).is_empty());
	CHECK(emulator.get_cursor().row == 0);
	CHECK(emulator.get_cursor().col == 0);
}

TEST_CASE("[godot_ai] Cursor positioning is one-based on the wire and zero-based here") {
	MCPTerminalEmulator emulator;
	emulator.init(10, 40);

	// CUP row 3, column 5, in the terminal's own one-based counting.
	feed(emulator, "\x1b[3;5Hx");
	CHECK(emulator.get_cell(2, 4).text == "x");
}

TEST_CASE("[godot_ai] Lines scrolled off the top land in the scrollback") {
	MCPTerminalEmulator emulator;
	emulator.init(4, 20);

	for (int i = 0; i < 10; i++) {
		feed(emulator, vformat("line%d\r\n", i));
	}
	// Four rows on screen, so the earlier lines must have gone somewhere rather than
	// being dropped - a terminal that loses them loses the start of every build log.
	CHECK(emulator.get_scrollback_length() > 0);

	// Oldest first: index 0 is "line0", the first line pushed off the top.
	String oldest;
	for (int column = 0; column < emulator.get_cols(); column++) {
		oldest += emulator.get_scrollback_cell(0, column).text;
	}
	CHECK(oldest.strip_edges() == "line0");

	// And the last entry is the line that scrolled off most recently, so the buffer
	// reads continuously into row 0 of the live grid rather than jumping.
	String newest;
	for (int column = 0; column < emulator.get_cols(); column++) {
		newest += emulator.get_scrollback_cell(emulator.get_scrollback_length() - 1, column).text;
	}
	CHECK(newest.strip_edges() != "line0");
	CHECK(newest.strip_edges().begins_with("line"));
}

TEST_CASE("[godot_ai] The emulator reports a title set through OSC") {
	MCPTerminalEmulator emulator;
	emulator.init(10, 40);

	feed(emulator, "\x1b]0;my-title\x07");
	CHECK(emulator.get_title() == "my-title");
}

TEST_CASE("[godot_ai] Resizing keeps the emulator usable") {
	MCPTerminalEmulator emulator;
	emulator.init(10, 40);
	feed(emulator, "before");

	emulator.set_size(20, 60);
	CHECK(emulator.get_rows() == 20);
	CHECK(emulator.get_cols() == 60);

	feed(emulator, "\r\nafter");
	CHECK(row_text(emulator, 1) == "after");
}

TEST_CASE("[godot_ai] Reading outside the grid is refused rather than read out of bounds") {
	MCPTerminalEmulator emulator;
	emulator.init(5, 10);

	// An empty cell rather than a crash: the widget draws from a rect it computed
	// separately, and the two can disagree for a frame after a resize.
	CHECK(emulator.get_cell(-1, 0).text.is_empty());
	CHECK(emulator.get_cell(0, -1).text.is_empty());
	CHECK(emulator.get_cell(5, 0).text.is_empty());
	CHECK(emulator.get_cell(0, 10).text.is_empty());
	CHECK(emulator.get_scrollback_cell(-1, 0).text.is_empty());
	CHECK(emulator.get_scrollback_cell(9999, 0).text.is_empty());
}

TEST_CASE("[godot_ai] Keys typed at the emulator become bytes for the child") {
	MCPTerminalEmulator emulator;
	emulator.init(10, 40);
	emulator.consume_output(); // Drop anything init produced.

	emulator.input_char('a', MCPTerminalEmulator::MOD_NONE);
	Vector<uint8_t> out = emulator.consume_output();
	REQUIRE(out.size() == 1);
	CHECK(out[0] == 'a');

	// Consuming empties the buffer: the widget writes what it takes, and taking the same
	// bytes twice would type everything twice.
	CHECK(emulator.consume_output().is_empty());

	emulator.input_key(MCPTerminalEmulator::KEY_ENTER, MCPTerminalEmulator::MOD_NONE);
	out = emulator.consume_output();
	CHECK(out.size() >= 1);
}

TEST_CASE("[godot_ai] Ctrl-C reaches the child as an interrupt byte") {
	MCPTerminalEmulator emulator;
	emulator.init(10, 40);
	emulator.consume_output();

	// The single most important key in a terminal running an agent.
	emulator.input_char('c', MCPTerminalEmulator::MOD_CTRL);
	const Vector<uint8_t> out = emulator.consume_output();
	REQUIRE(out.size() >= 1);
	CHECK(out[0] == 0x03);
}

TEST_CASE("[godot_ai] Re-initialising an emulator does not leak its previous state") {
	MCPTerminalEmulator emulator;
	emulator.init(10, 40);
	feed(emulator, "old content");
	REQUIRE(row_text(emulator, 0) == "old content");

	emulator.init(10, 40);
	CHECK(row_text(emulator, 0).is_empty());
	CHECK(emulator.get_scrollback_length() == 0);
}

TEST_CASE("[godot_ai] An emulator used before init does not crash") {
	// The widget calls init() from NOTIFICATION_ENTER_TREE, and something may read the
	// grid before that - a theme change, or a draw on the same frame it was added.
	MCPTerminalEmulator emulator;
	CHECK_FALSE(emulator.is_initialized());
	CHECK(emulator.get_cell(0, 0).text.is_empty());
	CHECK(emulator.get_scrollback_length() == 0);
	CHECK(emulator.consume_output().is_empty());
	const uint8_t byte = 'x';
	emulator.process_input(&byte, 1);
	emulator.set_size(12, 30);
}

} // namespace TestMCPTerminalEmulator

#endif // MCP_TERMINAL_ENABLED

#endif // TEST_MCP_TERMINAL_EMULATOR_H
