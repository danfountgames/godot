/**************************************************************************/
/*  mcp_terminal_keys.cpp                                                 */
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

#include "mcp_terminal_keys.h"

int mcp_terminal_modifiers(bool p_shift, bool p_alt, bool p_ctrl) {
	int mod = MCPTerminalEmulator::MOD_NONE;
	if (p_shift) {
		mod |= MCPTerminalEmulator::MOD_SHIFT;
	}
	if (p_alt) {
		mod |= MCPTerminalEmulator::MOD_ALT;
	}
	if (p_ctrl) {
		mod |= MCPTerminalEmulator::MOD_CTRL;
	}
	return mod;
}

MCPTerminalEmulator::Key mcp_terminal_key_for_keycode(Key p_keycode) {
	switch (p_keycode) {
		case Key::ENTER:
			return MCPTerminalEmulator::KEY_ENTER;
		case Key::TAB:
			return MCPTerminalEmulator::KEY_TAB;
		case Key::BACKSPACE:
			return MCPTerminalEmulator::KEY_BACKSPACE;
		case Key::ESCAPE:
			return MCPTerminalEmulator::KEY_ESCAPE;
		case Key::UP:
			return MCPTerminalEmulator::KEY_UP;
		case Key::DOWN:
			return MCPTerminalEmulator::KEY_DOWN;
		case Key::LEFT:
			return MCPTerminalEmulator::KEY_LEFT;
		case Key::RIGHT:
			return MCPTerminalEmulator::KEY_RIGHT;
		case Key::INSERT:
			return MCPTerminalEmulator::KEY_INS;
		case Key::KEY_DELETE:
			return MCPTerminalEmulator::KEY_DEL;
		case Key::HOME:
			return MCPTerminalEmulator::KEY_HOME;
		case Key::END:
			return MCPTerminalEmulator::KEY_END;
		case Key::PAGEUP:
			return MCPTerminalEmulator::KEY_PAGEUP;
		case Key::PAGEDOWN:
			return MCPTerminalEmulator::KEY_PAGEDOWN;
		case Key::KP_0:
			return MCPTerminalEmulator::KEY_KP_0;
		case Key::KP_1:
			return MCPTerminalEmulator::KEY_KP_1;
		case Key::KP_2:
			return MCPTerminalEmulator::KEY_KP_2;
		case Key::KP_3:
			return MCPTerminalEmulator::KEY_KP_3;
		case Key::KP_4:
			return MCPTerminalEmulator::KEY_KP_4;
		case Key::KP_5:
			return MCPTerminalEmulator::KEY_KP_5;
		case Key::KP_6:
			return MCPTerminalEmulator::KEY_KP_6;
		case Key::KP_7:
			return MCPTerminalEmulator::KEY_KP_7;
		case Key::KP_8:
			return MCPTerminalEmulator::KEY_KP_8;
		case Key::KP_9:
			return MCPTerminalEmulator::KEY_KP_9;
		case Key::KP_MULTIPLY:
			return MCPTerminalEmulator::KEY_KP_MULT;
		case Key::KP_ADD:
			return MCPTerminalEmulator::KEY_KP_PLUS;
		case Key::KP_SUBTRACT:
			return MCPTerminalEmulator::KEY_KP_MINUS;
		case Key::KP_PERIOD:
			return MCPTerminalEmulator::KEY_KP_PERIOD;
		case Key::KP_DIVIDE:
			return MCPTerminalEmulator::KEY_KP_DIVIDE;
		case Key::KP_ENTER:
			return MCPTerminalEmulator::KEY_KP_ENTER;
		default:
			break;
	}

	// F1 through F12 are contiguous in both enumerations.
	if (p_keycode >= Key::F1 && p_keycode <= Key::F12) {
		const int index = 1 + (int)(p_keycode - Key::F1);
		return (MCPTerminalEmulator::Key)(MCPTerminalEmulator::KEY_FUNCTION_0 + index);
	}

	return MCPTerminalEmulator::KEY_NONE;
}

bool mcp_terminal_is_modifier_keycode(Key p_keycode) {
	return p_keycode == Key::SHIFT ||
			p_keycode == Key::CTRL ||
			p_keycode == Key::ALT ||
			p_keycode == Key::META ||
			p_keycode == Key::CAPSLOCK;
}

MCPTerminalKeyPress mcp_terminal_translate_key(Key p_keycode, uint32_t p_unicode, int p_mod) {
	MCPTerminalKeyPress press;
	press.mod = p_mod;

	if (mcp_terminal_is_modifier_keycode(p_keycode)) {
		return press;
	}

	press.key = mcp_terminal_key_for_keycode(p_keycode);
	if (press.key != MCPTerminalEmulator::KEY_NONE) {
		return press;
	}

	press.unicode = p_unicode;

	if (press.mod & MCPTerminalEmulator::MOD_CTRL) {
		// Ctrl-letter is the one case where being careless costs the user their only way
		// to interrupt a runaway agent, so it is worth spelling out.
		//
		// libvterm does the control mapping itself: given the letter and MOD_CTRL it
		// masks with 0x1f and writes 0x03 for Ctrl-C. What it does with a character that
		// is *already* a control code is emit a CSI-u sequence instead - `ESC [ 3;5 u` -
		// which a shell reads as an unknown escape and drops on the floor.
		//
		// Godot hands us either form depending on the platform: X11 fills `unicode` with
		// the control code, and elsewhere it can be zero. Both are normalised back to the
		// plain letter here, so libvterm always gets the one input it maps correctly.
		if (press.unicode >= 1 && press.unicode <= 26) {
			press.unicode = press.unicode + 'a' - 1;
		} else if (press.unicode == 0 && p_keycode >= Key::A && p_keycode <= Key::Z) {
			press.unicode = (uint32_t)('a' + (int)(p_keycode - Key::A));
		}
	}

	return press;
}

#endif // MCP_TERMINAL_ENABLED
