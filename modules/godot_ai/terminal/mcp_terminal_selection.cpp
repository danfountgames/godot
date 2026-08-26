/**************************************************************************/
/*  mcp_terminal_selection.cpp                                            */
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

#include "mcp_terminal_selection.h"

void MCPTerminalSelection::begin(const Vector2i &p_cell) {
	anchor = p_cell;
	head = p_cell;
	// A press is not yet a selection: a plain click should place focus, not copy a cell.
	active = false;
}

void MCPTerminalSelection::extend(const Vector2i &p_cell) {
	head = p_cell;
	active = anchor != head;
}

void MCPTerminalSelection::clear() {
	active = false;
	anchor = Vector2i();
	head = Vector2i();
}

void MCPTerminalSelection::ordered(Vector2i &r_start, Vector2i &r_end) const {
	r_start = anchor;
	r_end = head;
	if (r_start.x > r_end.x || (r_start.x == r_end.x && r_start.y > r_end.y)) {
		SWAP(r_start, r_end);
	}
}

bool MCPTerminalSelection::contains(int p_row, int p_col) const {
	if (!active) {
		return false;
	}

	Vector2i start, end;
	ordered(start, end);

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

int mcp_terminal_total_rows(const MCPTerminalEmulator &p_emulator) {
	return p_emulator.get_scrollback_length() + p_emulator.get_rows();
}

Vector2i mcp_terminal_cell_at(const MCPTerminalEmulator &p_emulator, const Vector2 &p_pixel, int p_cell_width, int p_cell_height) {
	if (p_cell_width <= 0 || p_cell_height <= 0) {
		return Vector2i();
	}

	const int total_rows = mcp_terminal_total_rows(p_emulator);
	const int col = CLAMP((int)(p_pixel.x / p_cell_width), 0, MAX(0, p_emulator.get_cols() - 1));
	const int row = CLAMP((int)(p_pixel.y / p_cell_height), 0, MAX(0, total_rows - 1));
	return Vector2i(row, col);
}

MCPTerminalEmulator::Cell mcp_terminal_combined_cell(const MCPTerminalEmulator &p_emulator, int p_row, int p_col) {
	const int scrollback_length = p_emulator.get_scrollback_length();
	if (p_row < scrollback_length) {
		return p_emulator.get_scrollback_cell(p_row, p_col);
	}
	return p_emulator.get_cell(p_row - scrollback_length, p_col);
}

String mcp_terminal_selection_text(const MCPTerminalEmulator &p_emulator, const MCPTerminalSelection &p_selection) {
	if (!p_selection.active) {
		return String();
	}

	Vector2i start, end;
	p_selection.ordered(start, end);

	const int total_rows = mcp_terminal_total_rows(p_emulator);
	const int last_col = p_emulator.get_cols() - 1;
	if (total_rows <= 0 || last_col < 0) {
		return String();
	}

	start.x = CLAMP(start.x, 0, total_rows - 1);
	end.x = CLAMP(end.x, 0, total_rows - 1);
	start.y = CLAMP(start.y, 0, last_col);
	end.y = CLAMP(end.y, 0, last_col);

	String result;
	for (int row = start.x; row <= end.x; row++) {
		const int first = (row == start.x) ? start.y : 0;
		const int last = (row == end.x) ? end.y : last_col;

		String line;
		for (int col = first; col <= last; col++) {
			const MCPTerminalEmulator::Cell cell = mcp_terminal_combined_cell(p_emulator, row, col);
			// An empty cell is a space, not nothing: columns must line up in the paste,
			// or a copied table arrives as one run-on word.
			line += cell.text.is_empty() ? String(" ") : cell.text;
		}

		result += line.rstrip(" ");
		if (row < end.x) {
			result += "\n";
		}
	}

	return result;
}

#endif // MCP_TERMINAL_ENABLED
