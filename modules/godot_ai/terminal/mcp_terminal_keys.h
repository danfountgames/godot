/**************************************************************************/
/*  mcp_terminal_keys.h                                                   */
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

// Turning a Godot key event into something a terminal understands.
//
// This is a free function rather than a method on the widget because it is where the
// branch's terminal was actually wrong, and a bug in it is invisible until someone
// presses Ctrl-C on a runaway agent and nothing happens. Pulled out here, it is a pure
// function of (keycode, unicode, modifiers) and the tests can press every key.

#ifdef MCP_TERMINAL_ENABLED

#include "mcp_terminal_emulator.h"

#include "core/os/keyboard.h"

// What one key press means to a terminal: either a named key, or a character, or
// nothing at all (a bare modifier).
struct MCPTerminalKeyPress {
	MCPTerminalEmulator::Key key = MCPTerminalEmulator::KEY_NONE;
	uint32_t unicode = 0;
	int mod = MCPTerminalEmulator::MOD_NONE;

	bool sends_key() const { return key != MCPTerminalEmulator::KEY_NONE; }
	bool sends_char() const { return key == MCPTerminalEmulator::KEY_NONE && unicode != 0; }
	bool is_empty() const { return !sends_key() && !sends_char(); }
};

// The modifier bitmask for a key event.
int mcp_terminal_modifiers(bool p_shift, bool p_alt, bool p_ctrl);

// The named key a Godot keycode maps to, or KEY_NONE for an ordinary character key.
MCPTerminalEmulator::Key mcp_terminal_key_for_keycode(Key p_keycode);

// True for keys that only ever modify another one. Pressing one must not clear a
// selection: holding Ctrl on the way to Ctrl-C would otherwise wipe what you are copying.
bool mcp_terminal_is_modifier_keycode(Key p_keycode);

// The whole translation.
MCPTerminalKeyPress mcp_terminal_translate_key(Key p_keycode, uint32_t p_unicode, int p_mod);

#endif // MCP_TERMINAL_ENABLED
