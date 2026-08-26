/**************************************************************************/
/*  test_mcp_terminal_input.h                                             */
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

#ifndef TEST_MCP_TERMINAL_INPUT_H
#define TEST_MCP_TERMINAL_INPUT_H

#ifdef MCP_TERMINAL_ENABLED

#include "modules/godot_ai/terminal/mcp_terminal_keys.h"
#include "modules/godot_ai/terminal/mcp_terminal_selection.h"

#include "tests/test_macros.h"

namespace TestMCPTerminalInput {

static void feed(MCPTerminalEmulator &p_emulator, const String &p_text) {
	const CharString utf8 = p_text.utf8();
	p_emulator.process_input((const uint8_t *)utf8.get_data(), utf8.length());
}

// What the child actually receives for one key press, through the real emulator.
static Vector<uint8_t> bytes_for_key(Key p_keycode, uint32_t p_unicode, int p_mod) {
	MCPTerminalEmulator emulator;
	emulator.init(10, 40);
	emulator.consume_output();

	const MCPTerminalKeyPress press = mcp_terminal_translate_key(p_keycode, p_unicode, p_mod);
	if (press.sends_key()) {
		emulator.input_key(press.key, press.mod);
	} else if (press.sends_char()) {
		emulator.input_char(press.unicode, press.mod);
	}
	return emulator.consume_output();
}

TEST_CASE("[godot_ai] Ctrl-C reaches the child as 0x03 whichever form the platform reports") {
	// This is the test the branch needed. libvterm maps letter + MOD_CTRL to a control
	// code itself; handing it a control code that is *already* mapped makes it emit
	// `ESC [ 3;5 u` instead, which a shell ignores - so Ctrl-C would not interrupt
	// anything. Godot reports the key press differently per platform, so both forms have
	// to arrive at the same byte.
	const int ctrl = MCPTerminalEmulator::MOD_CTRL;

	SUBCASE("unicode is the letter") {
		const Vector<uint8_t> out = bytes_for_key(Key::C, 'c', ctrl);
		REQUIRE(out.size() == 1);
		CHECK(out[0] == 0x03);
	}

	SUBCASE("unicode is already the control code") {
		const Vector<uint8_t> out = bytes_for_key(Key::C, 0x03, ctrl);
		REQUIRE(out.size() == 1);
		CHECK(out[0] == 0x03);
	}

	SUBCASE("there is no unicode at all") {
		const Vector<uint8_t> out = bytes_for_key(Key::C, 0, ctrl);
		REQUIRE(out.size() == 1);
		CHECK(out[0] == 0x03);
	}
}

TEST_CASE("[godot_ai] Other control chords survive the same normalisation") {
	const int ctrl = MCPTerminalEmulator::MOD_CTRL;

	// Ctrl-D ends input, Ctrl-Z suspends, Ctrl-L clears. All are one byte.
	const Vector<uint8_t> ctrl_d = bytes_for_key(Key::D, 'd', ctrl);
	REQUIRE(ctrl_d.size() == 1);
	CHECK(ctrl_d[0] == 0x04);

	const Vector<uint8_t> ctrl_z = bytes_for_key(Key::Z, 0x1a, ctrl);
	REQUIRE(ctrl_z.size() == 1);
	CHECK(ctrl_z[0] == 0x1a);

	const Vector<uint8_t> ctrl_l = bytes_for_key(Key::L, 0, ctrl);
	REQUIRE(ctrl_l.size() == 1);
	CHECK(ctrl_l[0] == 0x0c);
}

TEST_CASE("[godot_ai] A plain letter is sent as itself") {
	const Vector<uint8_t> out = bytes_for_key(Key::A, 'a', MCPTerminalEmulator::MOD_NONE);
	REQUIRE(out.size() == 1);
	CHECK(out[0] == 'a');
}

TEST_CASE("[godot_ai] Named keys map to the terminal's own keys, not to characters") {
	CHECK(mcp_terminal_key_for_keycode(Key::ENTER) == MCPTerminalEmulator::KEY_ENTER);
	CHECK(mcp_terminal_key_for_keycode(Key::TAB) == MCPTerminalEmulator::KEY_TAB);
	CHECK(mcp_terminal_key_for_keycode(Key::BACKSPACE) == MCPTerminalEmulator::KEY_BACKSPACE);
	CHECK(mcp_terminal_key_for_keycode(Key::ESCAPE) == MCPTerminalEmulator::KEY_ESCAPE);
	CHECK(mcp_terminal_key_for_keycode(Key::UP) == MCPTerminalEmulator::KEY_UP);
	CHECK(mcp_terminal_key_for_keycode(Key::PAGEDOWN) == MCPTerminalEmulator::KEY_PAGEDOWN);
	CHECK(mcp_terminal_key_for_keycode(Key::KP_ENTER) == MCPTerminalEmulator::KEY_KP_ENTER);
	CHECK(mcp_terminal_key_for_keycode(Key::A) == MCPTerminalEmulator::KEY_NONE);
}

TEST_CASE("[godot_ai] Function keys are offset from KEY_FUNCTION_0, not off by one") {
	CHECK(mcp_terminal_key_for_keycode(Key::F1) == (MCPTerminalEmulator::Key)(MCPTerminalEmulator::KEY_FUNCTION_0 + 1));
	CHECK(mcp_terminal_key_for_keycode(Key::F5) == (MCPTerminalEmulator::Key)(MCPTerminalEmulator::KEY_FUNCTION_0 + 5));
	CHECK(mcp_terminal_key_for_keycode(Key::F12) == (MCPTerminalEmulator::Key)(MCPTerminalEmulator::KEY_FUNCTION_0 + 12));
}

TEST_CASE("[godot_ai] Arrow keys produce the escape sequence a shell expects") {
	const Vector<uint8_t> up = bytes_for_key(Key::UP, 0, MCPTerminalEmulator::MOD_NONE);
	REQUIRE(up.size() == 3);
	CHECK(up[0] == 0x1b);
	CHECK(up[1] == '[');
	CHECK(up[2] == 'A');
}

TEST_CASE("[godot_ai] A bare modifier sends nothing") {
	// Holding Ctrl on the way to Ctrl-C must not put a byte on the wire, and must not be
	// mistaken for a key press that clears the selection.
	for (const Key keycode : { Key::SHIFT, Key::CTRL, Key::ALT, Key::META, Key::CAPSLOCK }) {
		CHECK(mcp_terminal_is_modifier_keycode(keycode));
		const MCPTerminalKeyPress press = mcp_terminal_translate_key(keycode, 0, MCPTerminalEmulator::MOD_CTRL);
		CHECK(press.is_empty());
	}
	CHECK_FALSE(mcp_terminal_is_modifier_keycode(Key::A));
}

TEST_CASE("[godot_ai] The modifier bitmask is assembled from the event's flags") {
	CHECK(mcp_terminal_modifiers(false, false, false) == MCPTerminalEmulator::MOD_NONE);
	CHECK(mcp_terminal_modifiers(true, false, false) == MCPTerminalEmulator::MOD_SHIFT);
	CHECK(mcp_terminal_modifiers(false, true, false) == MCPTerminalEmulator::MOD_ALT);
	CHECK(mcp_terminal_modifiers(false, false, true) == MCPTerminalEmulator::MOD_CTRL);
	CHECK(mcp_terminal_modifiers(true, true, true) ==
			(MCPTerminalEmulator::MOD_SHIFT | MCPTerminalEmulator::MOD_ALT | MCPTerminalEmulator::MOD_CTRL));
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

TEST_CASE("[godot_ai] A click is not yet a selection") {
	MCPTerminalSelection selection;
	selection.begin(Vector2i(2, 3));
	CHECK_FALSE(selection.active);
	CHECK_FALSE(selection.contains(2, 3));

	// Only a drag away from the anchor selects anything.
	selection.extend(Vector2i(2, 3));
	CHECK_FALSE(selection.active);
	selection.extend(Vector2i(2, 5));
	CHECK(selection.active);
}

TEST_CASE("[godot_ai] A selection dragged backwards covers the same cells") {
	MCPTerminalSelection forwards;
	forwards.begin(Vector2i(1, 2));
	forwards.extend(Vector2i(3, 4));

	MCPTerminalSelection backwards;
	backwards.begin(Vector2i(3, 4));
	backwards.extend(Vector2i(1, 2));

	for (int row = 0; row < 5; row++) {
		for (int col = 0; col < 8; col++) {
			CHECK(forwards.contains(row, col) == backwards.contains(row, col));
		}
	}

	// Spot-check the boundaries: the first row starts at the anchor column, the last row
	// ends at the head column, and the rows between are whole.
	CHECK_FALSE(forwards.contains(1, 1));
	CHECK(forwards.contains(1, 2));
	CHECK(forwards.contains(2, 0));
	CHECK(forwards.contains(3, 4));
	CHECK_FALSE(forwards.contains(3, 5));
	CHECK_FALSE(forwards.contains(4, 0));
}

TEST_CASE("[godot_ai] Selected text comes back as it looks on screen") {
	MCPTerminalEmulator emulator;
	emulator.init(4, 20);
	feed(emulator, "alpha\r\nbeta\r\ngamma");

	MCPTerminalSelection selection;
	selection.begin(Vector2i(0, 0));
	selection.extend(Vector2i(2, 4));

	// Trailing blanks are trimmed per line, so a copied command pastes as a command.
	CHECK(mcp_terminal_selection_text(emulator, selection) == "alpha\nbeta\ngamma");
}

TEST_CASE("[godot_ai] A partial line selects only the part that was dragged over") {
	MCPTerminalEmulator emulator;
	emulator.init(4, 20);
	feed(emulator, "hello world");

	MCPTerminalSelection selection;
	selection.begin(Vector2i(0, 6));
	selection.extend(Vector2i(0, 10));
	CHECK(mcp_terminal_selection_text(emulator, selection) == "world");
}

TEST_CASE("[godot_ai] Selection reaches back into the scrollback") {
	MCPTerminalEmulator emulator;
	emulator.init(3, 20);
	for (int i = 0; i < 6; i++) {
		feed(emulator, vformat("line%d\r\n", i));
	}
	REQUIRE(emulator.get_scrollback_length() > 0);

	// Row 0 in combined coordinates is the oldest scrolled-off line, which is no longer
	// on screen at all.
	MCPTerminalSelection selection;
	selection.begin(Vector2i(0, 0));
	selection.extend(Vector2i(0, 4));
	CHECK(mcp_terminal_selection_text(emulator, selection) == "line0");
}

TEST_CASE("[godot_ai] An inactive selection yields no text") {
	MCPTerminalEmulator emulator;
	emulator.init(4, 20);
	feed(emulator, "something");

	MCPTerminalSelection selection;
	CHECK(mcp_terminal_selection_text(emulator, selection).is_empty());

	selection.begin(Vector2i(0, 0));
	CHECK(mcp_terminal_selection_text(emulator, selection).is_empty());
}

TEST_CASE("[godot_ai] A selection past the end of the grid is clamped, not read out of bounds") {
	MCPTerminalEmulator emulator;
	emulator.init(4, 20);
	feed(emulator, "short");

	MCPTerminalSelection selection;
	selection.begin(Vector2i(-5, -5));
	selection.extend(Vector2i(9999, 9999));
	// The whole grid: five characters of text and three blank lines, blanks trimmed.
	CHECK(mcp_terminal_selection_text(emulator, selection) == "short\n\n\n");
}

TEST_CASE("[godot_ai] A pixel maps to the cell under it, clamped to the grid") {
	MCPTerminalEmulator emulator;
	emulator.init(4, 20);

	CHECK(mcp_terminal_cell_at(emulator, Vector2(0, 0), 8, 16) == Vector2i(0, 0));
	CHECK(mcp_terminal_cell_at(emulator, Vector2(20, 33), 8, 16) == Vector2i(2, 2));

	// Past the right edge and below the last row: the nearest real cell, not a crash and
	// not a negative index.
	CHECK(mcp_terminal_cell_at(emulator, Vector2(100000, 100000), 8, 16) == Vector2i(3, 19));
	CHECK(mcp_terminal_cell_at(emulator, Vector2(-40, -40), 8, 16) == Vector2i(0, 0));

	// A zero cell size can happen for one frame before the theme arrives; dividing by it
	// would take the editor down.
	CHECK(mcp_terminal_cell_at(emulator, Vector2(10, 10), 0, 0) == Vector2i(0, 0));
}

} // namespace TestMCPTerminalInput

#endif // MCP_TERMINAL_ENABLED

#endif // TEST_MCP_TERMINAL_INPUT_H
