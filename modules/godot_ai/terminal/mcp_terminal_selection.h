/**************************************************************************/
/*  mcp_terminal_selection.h                                              */
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

// Selecting text in the terminal.
//
// Kept out of the widget for the same reason as the key mapping: copying the wrong
// region, or dropping the last character of a line, is a silent defect that only shows
// up when someone pastes a command into a bug report. All of it is a pure function of
// the grid, so all of it can be tested without a scene.
//
// Rows are addressed in *combined* coordinates: rows [0, scrollback_length) are
// scrollback, oldest first, and the live grid follows. That is the order the widget
// draws in, so a click maps to one number rather than two cases.

#ifdef MCP_TERMINAL_ENABLED

#include "mcp_terminal_emulator.h"

#include "core/math/vector2.h"
#include "core/math/vector2i.h"

struct MCPTerminalSelection {
	bool active = false;
	Vector2i anchor; // (row, col) where the drag started.
	Vector2i head; // (row, col) where it is now.

	void begin(const Vector2i &p_cell);
	void extend(const Vector2i &p_cell);
	void clear();

	// Start and end in reading order, whichever way the user dragged.
	void ordered(Vector2i &r_start, Vector2i &r_end) const;
	bool contains(int p_row, int p_col) const;
};

// The row and column under a pixel, clamped to the grid.
Vector2i mcp_terminal_cell_at(const MCPTerminalEmulator &p_emulator, const Vector2 &p_pixel, int p_cell_width, int p_cell_height);

// Total addressable rows: scrollback plus the live grid.
int mcp_terminal_total_rows(const MCPTerminalEmulator &p_emulator);

// One cell in combined coordinates.
MCPTerminalEmulator::Cell mcp_terminal_combined_cell(const MCPTerminalEmulator &p_emulator, int p_row, int p_col);

// The selected text, with trailing blanks trimmed per line and lines joined by "\n".
String mcp_terminal_selection_text(const MCPTerminalEmulator &p_emulator, const MCPTerminalSelection &p_selection);

#endif // MCP_TERMINAL_ENABLED
