/**************************************************************************/
/*  terminal_emulator.cpp                                                 */
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

#include "terminal_emulator.h"

#include "core/string/ustring.h"

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

TerminalEmulator::TerminalEmulator() {
}

TerminalEmulator::~TerminalEmulator() {
	if (vterm) {
		vterm_free(vterm);
		vterm = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

void TerminalEmulator::init(int p_rows, int p_cols) {
	if (vterm) {
		vterm_free(vterm);
		vterm = nullptr;
	}

	rows = p_rows;
	cols = p_cols;

	vterm = vterm_new(p_rows, p_cols);
	vterm_set_utf8(vterm, 1);

	// Output callback -- libvterm calls this when it wants data sent to the PTY.
	vterm_output_set_callback(vterm, _output_cb, this);

	// Obtain the screen layer and wire up callbacks.
	vterm_screen = vterm_obtain_screen(vterm);

	static VTermScreenCallbacks screen_cbs;
	memset(&screen_cbs, 0, sizeof(screen_cbs));
	screen_cbs.damage = _damage_cb;
	screen_cbs.movecursor = _movecursor_cb;
	screen_cbs.settermprop = _settermprop_cb;
	screen_cbs.bell = _bell_cb;
	screen_cbs.resize = _resize_cb;
	screen_cbs.sb_pushline = _sb_pushline_cb;
	screen_cbs.sb_popline = _sb_popline_cb;

	vterm_screen_set_callbacks(vterm_screen, &screen_cbs, this);
	vterm_screen_enable_altscreen(vterm_screen, 1);
	vterm_screen_set_damage_merge(vterm_screen, VTERM_DAMAGE_SCROLL);
	vterm_screen_reset(vterm_screen, 1);

	// Set default foreground (light gray) and background (dark).
	VTermState *state = vterm_obtain_state(vterm);
	VTermColor fg, bg;
	vterm_color_rgb(&fg, 230, 230, 230);
	vterm_color_rgb(&bg, 30, 30, 38);
	vterm_state_set_default_colors(state, &fg, &bg);

	dirty = true;
}

// ---------------------------------------------------------------------------
// Input / Output
// ---------------------------------------------------------------------------

void TerminalEmulator::process_input(const uint8_t *p_data, int p_len) {
	if (!vterm || p_len <= 0) {
		return;
	}
	vterm_input_write(vterm, (const char *)p_data, (size_t)p_len);
	vterm_screen_flush_damage(vterm_screen);
}

Vector<uint8_t> TerminalEmulator::consume_output() {
	Vector<uint8_t> out = output_buffer;
	output_buffer.clear();
	return out;
}

void TerminalEmulator::input_key(VTermKey key, VTermModifier mod) {
	if (!vterm) {
		return;
	}
	vterm_keyboard_key(vterm, key, mod);
}

void TerminalEmulator::input_char(uint32_t ch, VTermModifier mod) {
	if (!vterm) {
		return;
	}
	vterm_keyboard_unichar(vterm, ch, mod);
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

void TerminalEmulator::set_size(int p_rows, int p_cols) {
	if (!vterm) {
		return;
	}
	rows = p_rows;
	cols = p_cols;
	vterm_set_size(vterm, p_rows, p_cols);
	vterm_screen_flush_damage(vterm_screen);
}

// ---------------------------------------------------------------------------
// Cell Access
// ---------------------------------------------------------------------------

TerminalEmulator::Cell TerminalEmulator::get_cell(int p_row, int p_col) const {
	Cell cell;

	if (!vterm_screen) {
		return cell;
	}

	if (p_col < 0 || p_col >= cols || p_row < 0 || p_row >= rows) {
		return cell;
	}

	VTermScreenCell vcell;
	memset(&vcell, 0, sizeof(vcell));

	VTermPos pos;
	pos.row = p_row;
	pos.col = p_col;
	vterm_screen_get_cell(vterm_screen, pos, &vcell);

	// Build the text string from the uint32_t chars array.
	if (vcell.chars[0] != 0) {
		int count = 0;
		while (count < VTERM_MAX_CHARS_PER_CELL && vcell.chars[count] != 0) {
			count++;
		}
		cell.text = String();
		for (int i = 0; i < count; i++) {
			char32_t c = (char32_t)vcell.chars[i];
			cell.text += String::chr(c);
		}
	}

	// Convert colors.
	cell.fg = _vterm_color_to_godot(vcell.fg);
	cell.bg = _vterm_color_to_godot(vcell.bg);

	// Attributes.
	cell.bold = vcell.attrs.bold != 0;
	cell.italic = vcell.attrs.italic != 0;
	cell.underline = vcell.attrs.underline != 0;
	cell.reverse = vcell.attrs.reverse != 0;
	cell.strike = vcell.attrs.strike != 0;
	cell.width = vcell.width;

	return cell;
}

TerminalEmulator::Cell TerminalEmulator::get_scrollback_cell(int p_scrollback_idx, int p_col) const {
	Cell cell;
	cell.fg = Color(0.9f, 0.9f, 0.9f);
	cell.bg = Color(0.12f, 0.12f, 0.15f);

	if (p_scrollback_idx < 0 || p_scrollback_idx >= scrollback.size()) {
		return cell;
	}

	const ScrollbackLine &line = scrollback[p_scrollback_idx];
	if (p_col < 0 || p_col >= line.cells.size()) {
		return cell;
	}

	const VTermScreenCell &vcell = line.cells[p_col];

	if (vcell.chars[0] != 0) {
		int count = 0;
		while (count < VTERM_MAX_CHARS_PER_CELL && vcell.chars[count] != 0) {
			count++;
		}
		cell.text = String();
		for (int i = 0; i < count; i++) {
			char32_t c = (char32_t)vcell.chars[i];
			cell.text += String::chr(c);
		}
	}

	cell.fg = _vterm_color_to_godot(vcell.fg);
	cell.bg = _vterm_color_to_godot(vcell.bg);
	cell.bold = vcell.attrs.bold != 0;
	cell.italic = vcell.attrs.italic != 0;
	cell.underline = vcell.attrs.underline != 0;
	cell.reverse = vcell.attrs.reverse != 0;
	cell.strike = vcell.attrs.strike != 0;
	cell.width = vcell.width;

	return cell;
}

// ---------------------------------------------------------------------------
// Color Conversion
// ---------------------------------------------------------------------------

Color TerminalEmulator::_vterm_color_to_godot(VTermColor col) const {
	if (VTERM_COLOR_IS_DEFAULT_FG(&col)) {
		return Color(0.9f, 0.9f, 0.9f);
	}
	if (VTERM_COLOR_IS_DEFAULT_BG(&col)) {
		return Color(0.12f, 0.12f, 0.15f);
	}
	if (VTERM_COLOR_IS_INDEXED(&col)) {
		// Try to resolve via libvterm first.
		VTermColor resolved = col;
		vterm_screen_convert_color_to_rgb(vterm_screen, &resolved);
		if (VTERM_COLOR_IS_RGB(&resolved)) {
			return Color(resolved.rgb.red / 255.0f, resolved.rgb.green / 255.0f, resolved.rgb.blue / 255.0f);
		}
		// Fallback to our own palette.
		return _index_to_color(col.indexed.idx);
	}
	if (VTERM_COLOR_IS_RGB(&col)) {
		return Color(col.rgb.red / 255.0f, col.rgb.green / 255.0f, col.rgb.blue / 255.0f);
	}

	// Fallback: white.
	return Color(1.0f, 1.0f, 1.0f);
}

Color TerminalEmulator::_index_to_color(int idx) {
	// Standard 16 ANSI colors.
	static const Color ansi16[16] = {
		Color(0.0f, 0.0f, 0.0f),       // 0  Black
		Color(0.67f, 0.0f, 0.0f),      // 1  Red
		Color(0.0f, 0.67f, 0.0f),      // 2  Green
		Color(0.67f, 0.67f, 0.0f),     // 3  Yellow
		Color(0.0f, 0.0f, 0.67f),      // 4  Blue
		Color(0.67f, 0.0f, 0.67f),     // 5  Magenta
		Color(0.0f, 0.67f, 0.67f),     // 6  Cyan
		Color(0.67f, 0.67f, 0.67f),    // 7  White
		Color(0.33f, 0.33f, 0.33f),    // 8  Bright Black
		Color(1.0f, 0.33f, 0.33f),     // 9  Bright Red
		Color(0.33f, 1.0f, 0.33f),     // 10 Bright Green
		Color(1.0f, 1.0f, 0.33f),      // 11 Bright Yellow
		Color(0.33f, 0.33f, 1.0f),     // 12 Bright Blue
		Color(1.0f, 0.33f, 1.0f),      // 13 Bright Magenta
		Color(0.33f, 1.0f, 1.0f),      // 14 Bright Cyan
		Color(1.0f, 1.0f, 1.0f),       // 15 Bright White
	};

	if (idx < 0) {
		return Color(1.0f, 1.0f, 1.0f);
	}

	if (idx < 16) {
		return ansi16[idx];
	}

	// 216-color cube (indices 16-231): 6x6x6 RGB.
	if (idx < 232) {
		int ci = idx - 16;
		int b_val = ci % 6;
		ci /= 6;
		int g_val = ci % 6;
		ci /= 6;
		int r_val = ci;

		auto cube_component = [](int val) -> float {
			return val == 0 ? 0.0f : (55.0f + 40.0f * val) / 255.0f;
		};

		return Color(cube_component(r_val), cube_component(g_val), cube_component(b_val));
	}

	// Grayscale ramp (indices 232-255): 24 shades.
	if (idx < 256) {
		float v = (8.0f + 10.0f * (idx - 232)) / 255.0f;
		return Color(v, v, v);
	}

	return Color(1.0f, 1.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// libvterm Callbacks
// ---------------------------------------------------------------------------

int TerminalEmulator::_damage_cb(VTermRect rect, void *user) {
	TerminalEmulator *self = static_cast<TerminalEmulator *>(user);
	self->dirty = true;
	return 1;
}

int TerminalEmulator::_movecursor_cb(VTermPos pos, VTermPos oldpos, int visible, void *user) {
	TerminalEmulator *self = static_cast<TerminalEmulator *>(user);
	self->cursor.row = pos.row;
	self->cursor.col = pos.col;
	self->cursor.visible = visible != 0;
	self->dirty = true;
	return 1;
}

int TerminalEmulator::_settermprop_cb(VTermProp prop, VTermValue *val, void *user) {
	TerminalEmulator *self = static_cast<TerminalEmulator *>(user);

	switch (prop) {
		case VTERM_PROP_CURSORVISIBLE: {
			self->cursor.visible = val->boolean;
		} break;
		case VTERM_PROP_CURSORSHAPE: {
			self->cursor.shape = val->number;
		} break;
		case VTERM_PROP_TITLE: {
			self->title = String::utf8(val->string.str, val->string.len);
		} break;
		case VTERM_PROP_ALTSCREEN: {
			// Alt screen change — dirty flag is set below.
		} break;
		default:
			break;
	}

	self->dirty = true;
	return 1;
}

int TerminalEmulator::_bell_cb(void *user) {
	// Ignored.
	return 1;
}

int TerminalEmulator::_resize_cb(int new_rows, int new_cols, void *user) {
	TerminalEmulator *self = static_cast<TerminalEmulator *>(user);
	self->rows = new_rows;
	self->cols = new_cols;
	self->dirty = true;
	return 1;
}

int TerminalEmulator::_sb_pushline_cb(int pushed_cols, const VTermScreenCell *cells, void *user) {
	TerminalEmulator *self = static_cast<TerminalEmulator *>(user);

	ScrollbackLine line;
	line.cells.resize(pushed_cols);
	memcpy(line.cells.ptrw(), cells, sizeof(VTermScreenCell) * pushed_cols);
	self->scrollback.push_back(line);

	// Cap scrollback size.
	while (self->scrollback.size() > MAX_SCROLLBACK) {
		self->scrollback.remove_at(0);
	}

	return 1;
}

int TerminalEmulator::_sb_popline_cb(int popped_cols, VTermScreenCell *cells, void *user) {
	TerminalEmulator *self = static_cast<TerminalEmulator *>(user);

	if (self->scrollback.is_empty()) {
		return 0;
	}

	const ScrollbackLine &line = self->scrollback[self->scrollback.size() - 1];
	int copy_cols = MIN(popped_cols, line.cells.size());
	memcpy(cells, line.cells.ptr(), sizeof(VTermScreenCell) * copy_cols);

	// Zero out any remaining cells if the caller expects more columns.
	for (int i = copy_cols; i < popped_cols; i++) {
		memset(&cells[i], 0, sizeof(VTermScreenCell));
	}

	self->scrollback.remove_at(self->scrollback.size() - 1);

	return 1;
}

void TerminalEmulator::_output_cb(const char *s, size_t len, void *user) {
	TerminalEmulator *self = static_cast<TerminalEmulator *>(user);
	int old_size = self->output_buffer.size();
	self->output_buffer.resize(old_size + (int)len);
	memcpy(self->output_buffer.ptrw() + old_size, s, len);
}

#endif // MCP_TERMINAL_ENABLED
