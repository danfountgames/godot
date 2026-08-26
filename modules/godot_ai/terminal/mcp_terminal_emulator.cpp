/**************************************************************************/
/*  mcp_terminal_emulator.cpp                                             */
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

#include "mcp_terminal_emulator.h"

#include "core/error/error_macros.h"
#include "core/os/memory.h"

extern "C" {
#include <vterm.h>
}

#include <string.h>

// The public enums are ours so that this header can be included without libvterm on the
// include path. They are only useful if they still mean what libvterm means, so pin it
// here: a library update that renumbers a key fails the build instead of quietly
// turning Home into Insert.
static_assert((int)MCPTerminalEmulator::KEY_NONE == (int)VTERM_KEY_NONE, "VTermKey renumbered");
static_assert((int)MCPTerminalEmulator::KEY_ENTER == (int)VTERM_KEY_ENTER, "VTermKey renumbered");
static_assert((int)MCPTerminalEmulator::KEY_TAB == (int)VTERM_KEY_TAB, "VTermKey renumbered");
static_assert((int)MCPTerminalEmulator::KEY_BACKSPACE == (int)VTERM_KEY_BACKSPACE, "VTermKey renumbered");
static_assert((int)MCPTerminalEmulator::KEY_ESCAPE == (int)VTERM_KEY_ESCAPE, "VTermKey renumbered");
static_assert((int)MCPTerminalEmulator::KEY_UP == (int)VTERM_KEY_UP, "VTermKey renumbered");
static_assert((int)MCPTerminalEmulator::KEY_DOWN == (int)VTERM_KEY_DOWN, "VTermKey renumbered");
static_assert((int)MCPTerminalEmulator::KEY_LEFT == (int)VTERM_KEY_LEFT, "VTermKey renumbered");
static_assert((int)MCPTerminalEmulator::KEY_RIGHT == (int)VTERM_KEY_RIGHT, "VTermKey renumbered");
static_assert((int)MCPTerminalEmulator::KEY_INS == (int)VTERM_KEY_INS, "VTermKey renumbered");
static_assert((int)MCPTerminalEmulator::KEY_DEL == (int)VTERM_KEY_DEL, "VTermKey renumbered");
static_assert((int)MCPTerminalEmulator::KEY_HOME == (int)VTERM_KEY_HOME, "VTermKey renumbered");
static_assert((int)MCPTerminalEmulator::KEY_END == (int)VTERM_KEY_END, "VTermKey renumbered");
static_assert((int)MCPTerminalEmulator::KEY_PAGEUP == (int)VTERM_KEY_PAGEUP, "VTermKey renumbered");
static_assert((int)MCPTerminalEmulator::KEY_PAGEDOWN == (int)VTERM_KEY_PAGEDOWN, "VTermKey renumbered");
static_assert((int)MCPTerminalEmulator::KEY_FUNCTION_0 == (int)VTERM_KEY_FUNCTION_0, "VTermKey renumbered");
static_assert((int)MCPTerminalEmulator::MOD_NONE == (int)VTERM_MOD_NONE, "VTermModifier renumbered");
static_assert((int)MCPTerminalEmulator::MOD_SHIFT == (int)VTERM_MOD_SHIFT, "VTermModifier renumbered");
static_assert((int)MCPTerminalEmulator::MOD_ALT == (int)VTERM_MOD_ALT, "VTermModifier renumbered");
static_assert((int)MCPTerminalEmulator::MOD_CTRL == (int)VTERM_MOD_CTRL, "VTermModifier renumbered");

// ---------------------------------------------------------------------------
// The hidden state
// ---------------------------------------------------------------------------

struct MCPTerminalEmulatorImpl {
	static constexpr int MAX_SCROLLBACK = 2000;

	VTerm *vterm = nullptr;
	VTermScreen *vterm_screen = nullptr;
	int rows = 24;
	int cols = 80;
	bool dirty = true;
	MCPTerminalEmulator::CursorState cursor;
	String title;

	// Default and ANSI palette colors, set by the widget from the editor theme.
	Color default_fg = Color(0.9f, 0.9f, 0.9f);
	Color default_bg = Color(0.12f, 0.12f, 0.15f);
	Color ansi_colors[16] = {
		Color(0.0f, 0.0f, 0.0f), // 0  Black
		Color(0.67f, 0.0f, 0.0f), // 1  Red
		Color(0.0f, 0.67f, 0.0f), // 2  Green
		Color(0.67f, 0.67f, 0.0f), // 3  Yellow
		Color(0.0f, 0.0f, 0.67f), // 4  Blue
		Color(0.67f, 0.0f, 0.67f), // 5  Magenta
		Color(0.0f, 0.67f, 0.67f), // 6  Cyan
		Color(0.67f, 0.67f, 0.67f), // 7  White
		Color(0.33f, 0.33f, 0.33f), // 8  Bright Black
		Color(1.0f, 0.33f, 0.33f), // 9  Bright Red
		Color(0.33f, 1.0f, 0.33f), // 10 Bright Green
		Color(1.0f, 1.0f, 0.33f), // 11 Bright Yellow
		Color(0.33f, 0.33f, 1.0f), // 12 Bright Blue
		Color(1.0f, 0.33f, 1.0f), // 13 Bright Magenta
		Color(0.33f, 1.0f, 1.0f), // 14 Bright Cyan
		Color(1.0f, 1.0f, 1.0f), // 15 Bright White
	};

	// Data libvterm wants written to the pty (key responses, device reports).
	Vector<uint8_t> output_buffer;

	struct ScrollbackLine {
		Vector<VTermScreenCell> cells;
	};
	Vector<ScrollbackLine> scrollback;

	void free_vterm() {
		if (vterm) {
			vterm_free(vterm);
			vterm = nullptr;
		}
		vterm_screen = nullptr;
	}

	void push_palette() const {
		if (!vterm) {
			return;
		}
		VTermState *state = vterm_obtain_state(vterm);
		VTermColor vt_fg, vt_bg;
		vterm_color_rgb(&vt_fg, (uint8_t)(default_fg.r * 255), (uint8_t)(default_fg.g * 255), (uint8_t)(default_fg.b * 255));
		vterm_color_rgb(&vt_bg, (uint8_t)(default_bg.r * 255), (uint8_t)(default_bg.g * 255), (uint8_t)(default_bg.b * 255));
		vterm_state_set_default_colors(state, &vt_fg, &vt_bg);

		for (int i = 0; i < 16; i++) {
			VTermColor vc;
			vterm_color_rgb(&vc, (uint8_t)(ansi_colors[i].r * 255), (uint8_t)(ansi_colors[i].g * 255), (uint8_t)(ansi_colors[i].b * 255));
			vterm_state_set_palette_color(state, i, &vc);
		}
	}

	Color vterm_color_to_godot(VTermColor p_color) const;
	MCPTerminalEmulator::Cell to_cell(const VTermScreenCell &p_vcell) const;

	static Color extended_index_to_color(int p_index);
};

// ---------------------------------------------------------------------------
// libvterm callbacks
//
// File-static rather than members: nothing outside this file can name `Impl`, and the
// callbacks only ever need it.
// ---------------------------------------------------------------------------

namespace {

int damage_cb(VTermRect p_rect, void *p_user) {
	static_cast<MCPTerminalEmulatorImpl *>(p_user)->dirty = true;
	return 1;
}

int movecursor_cb(VTermPos p_pos, VTermPos p_oldpos, int p_visible, void *p_user) {
	MCPTerminalEmulatorImpl *impl = static_cast<MCPTerminalEmulatorImpl *>(p_user);
	impl->cursor.row = p_pos.row;
	impl->cursor.col = p_pos.col;
	impl->cursor.visible = p_visible != 0;
	impl->dirty = true;
	return 1;
}

int settermprop_cb(VTermProp p_prop, VTermValue *p_value, void *p_user) {
	MCPTerminalEmulatorImpl *impl = static_cast<MCPTerminalEmulatorImpl *>(p_user);

	switch (p_prop) {
		case VTERM_PROP_CURSORVISIBLE: {
			impl->cursor.visible = p_value->boolean;
		} break;
		case VTERM_PROP_CURSORSHAPE: {
			impl->cursor.shape = p_value->number;
		} break;
		case VTERM_PROP_TITLE: {
			impl->title = String::utf8(p_value->string.str, p_value->string.len);
		} break;
		default:
			break;
	}

	impl->dirty = true;
	return 1;
}

int bell_cb(void *p_user) {
	// Ignored: an editor panel that beeped at every tab-completion would not last a day.
	return 1;
}

int resize_cb(int p_rows, int p_cols, void *p_user) {
	MCPTerminalEmulatorImpl *impl = static_cast<MCPTerminalEmulatorImpl *>(p_user);
	impl->rows = p_rows;
	impl->cols = p_cols;
	impl->dirty = true;
	return 1;
}

int sb_pushline_cb(int p_cols, const VTermScreenCell *p_cells, void *p_user) {
	MCPTerminalEmulatorImpl *impl = static_cast<MCPTerminalEmulatorImpl *>(p_user);

	MCPTerminalEmulatorImpl::ScrollbackLine line;
	line.cells.resize(p_cols);
	memcpy(line.cells.ptrw(), p_cells, sizeof(VTermScreenCell) * p_cols);
	impl->scrollback.push_back(line);

	while (impl->scrollback.size() > MCPTerminalEmulatorImpl::MAX_SCROLLBACK) {
		impl->scrollback.remove_at(0);
	}

	return 1;
}

int sb_popline_cb(int p_cols, VTermScreenCell *p_cells, void *p_user) {
	MCPTerminalEmulatorImpl *impl = static_cast<MCPTerminalEmulatorImpl *>(p_user);

	if (impl->scrollback.is_empty()) {
		return 0;
	}

	const MCPTerminalEmulatorImpl::ScrollbackLine &line = impl->scrollback[impl->scrollback.size() - 1];
	const int copy_cols = MIN(p_cols, line.cells.size());
	memcpy(p_cells, line.cells.ptr(), sizeof(VTermScreenCell) * copy_cols);

	// Zero any remaining cells if the caller expects a wider line than we stored.
	for (int i = copy_cols; i < p_cols; i++) {
		memset(&p_cells[i], 0, sizeof(VTermScreenCell));
	}

	impl->scrollback.remove_at(impl->scrollback.size() - 1);
	return 1;
}

void output_cb(const char *p_data, size_t p_length, void *p_user) {
	MCPTerminalEmulatorImpl *impl = static_cast<MCPTerminalEmulatorImpl *>(p_user);
	const int old_size = impl->output_buffer.size();
	impl->output_buffer.resize(old_size + (int)p_length);
	memcpy(impl->output_buffer.ptrw() + old_size, p_data, p_length);
}

// One shared table, written once. The original rebuilt a mutable function-local static
// on every init(), which every emulator in the process then pointed at - harmless today
// because the contents are identical and this is single-threaded, but a mutable global
// several objects share is not a thing to leave lying around.
const VTermScreenCallbacks SCREEN_CALLBACKS = {
	/* damage      */ damage_cb,
	/* moverect    */ nullptr,
	/* movecursor  */ movecursor_cb,
	/* settermprop */ settermprop_cb,
	/* bell        */ bell_cb,
	/* resize      */ resize_cb,
	/* sb_pushline */ sb_pushline_cb,
	/* sb_popline  */ sb_popline_cb,
	/* sb_clear    */ nullptr,
};

} // namespace

// ---------------------------------------------------------------------------
// Impl helpers
// ---------------------------------------------------------------------------

Color MCPTerminalEmulatorImpl::vterm_color_to_godot(VTermColor p_color) const {
	if (VTERM_COLOR_IS_DEFAULT_FG(&p_color)) {
		return default_fg;
	}
	if (VTERM_COLOR_IS_DEFAULT_BG(&p_color)) {
		return default_bg;
	}
	if (VTERM_COLOR_IS_INDEXED(&p_color)) {
		const int index = p_color.indexed.idx;
		// The base 16 come from the editor theme, so the panel matches the editor.
		if (index < 16) {
			return ansi_colors[index];
		}
		VTermColor resolved = p_color;
		if (vterm_screen) {
			vterm_screen_convert_color_to_rgb(vterm_screen, &resolved);
			if (VTERM_COLOR_IS_RGB(&resolved)) {
				return Color(resolved.rgb.red / 255.0f, resolved.rgb.green / 255.0f, resolved.rgb.blue / 255.0f);
			}
		}
		return extended_index_to_color(index);
	}
	if (VTERM_COLOR_IS_RGB(&p_color)) {
		return Color(p_color.rgb.red / 255.0f, p_color.rgb.green / 255.0f, p_color.rgb.blue / 255.0f);
	}

	return default_fg;
}

Color MCPTerminalEmulatorImpl::extended_index_to_color(int p_index) {
	// Extended colors only (16-255); 0-15 come from the themed palette above.
	if (p_index < 16) {
		return Color(1.0f, 1.0f, 1.0f);
	}

	// 216-color cube (16-231): 6x6x6 RGB.
	if (p_index < 232) {
		int remaining = p_index - 16;
		const int blue = remaining % 6;
		remaining /= 6;
		const int green = remaining % 6;
		remaining /= 6;
		const int red = remaining;

		auto cube_component = [](int p_value) -> float {
			return p_value == 0 ? 0.0f : (55.0f + 40.0f * p_value) / 255.0f;
		};

		return Color(cube_component(red), cube_component(green), cube_component(blue));
	}

	// Grayscale ramp (232-255): 24 shades.
	if (p_index < 256) {
		const float value = (8.0f + 10.0f * (p_index - 232)) / 255.0f;
		return Color(value, value, value);
	}

	return Color(1.0f, 1.0f, 1.0f);
}

MCPTerminalEmulator::Cell MCPTerminalEmulatorImpl::to_cell(const VTermScreenCell &p_vcell) const {
	MCPTerminalEmulator::Cell cell;
	cell.fg = default_fg;
	cell.bg = default_bg;

	if (p_vcell.chars[0] != 0) {
		for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && p_vcell.chars[i] != 0; i++) {
			cell.text += String::chr((char32_t)p_vcell.chars[i]);
		}
	}

	cell.fg = vterm_color_to_godot(p_vcell.fg);
	cell.bg = vterm_color_to_godot(p_vcell.bg);
	cell.bold = p_vcell.attrs.bold != 0;
	cell.italic = p_vcell.attrs.italic != 0;
	cell.underline = p_vcell.attrs.underline != 0;
	cell.reverse = p_vcell.attrs.reverse != 0;
	cell.strike = p_vcell.attrs.strike != 0;
	cell.width = p_vcell.width;

	return cell;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MCPTerminalEmulator::MCPTerminalEmulator() {
	impl = memnew(MCPTerminalEmulatorImpl);
}

MCPTerminalEmulator::~MCPTerminalEmulator() {
	impl->free_vterm();
	memdelete(impl);
	impl = nullptr;
}

void MCPTerminalEmulator::init(int p_rows, int p_cols) {
	ERR_FAIL_COND_MSG(p_rows <= 0 || p_cols <= 0, "A terminal needs at least one row and one column.");

	// Re-initialising must not leave the previous session's text behind: the widget
	// calls this again when the pty restarts, and a new shell in an old grid reads as a
	// crash that scrolled.
	impl->free_vterm();
	impl->scrollback.clear();
	impl->output_buffer.clear();
	impl->cursor = MCPTerminalEmulator::CursorState();
	impl->title = String();

	impl->rows = p_rows;
	impl->cols = p_cols;

	impl->vterm = vterm_new(p_rows, p_cols);
	ERR_FAIL_NULL_MSG(impl->vterm, "libvterm refused to allocate a terminal.");
	vterm_set_utf8(impl->vterm, 1);
	vterm_output_set_callback(impl->vterm, output_cb, impl);

	impl->vterm_screen = vterm_obtain_screen(impl->vterm);
	vterm_screen_set_callbacks(impl->vterm_screen, &SCREEN_CALLBACKS, impl);
	vterm_screen_enable_altscreen(impl->vterm_screen, 1);
	vterm_screen_set_damage_merge(impl->vterm_screen, VTERM_DAMAGE_SCROLL);
	vterm_screen_reset(impl->vterm_screen, 1);

	impl->push_palette();
	impl->dirty = true;
}

bool MCPTerminalEmulator::is_initialized() const {
	return impl->vterm != nullptr;
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

void MCPTerminalEmulator::set_theme_colors(const Color &p_fg, const Color &p_bg, const Color *p_ansi16) {
	ERR_FAIL_NULL(p_ansi16);

	impl->default_fg = p_fg;
	impl->default_bg = p_bg;
	for (int i = 0; i < 16; i++) {
		impl->ansi_colors[i] = p_ansi16[i];
	}

	if (impl->vterm) {
		impl->push_palette();
		impl->dirty = true;
	}
}

Color MCPTerminalEmulator::get_default_fg() const {
	return impl->default_fg;
}

Color MCPTerminalEmulator::get_default_bg() const {
	return impl->default_bg;
}

// ---------------------------------------------------------------------------
// Input and output
// ---------------------------------------------------------------------------

void MCPTerminalEmulator::process_input(const uint8_t *p_data, int p_len) {
	if (!impl->vterm || !p_data || p_len <= 0) {
		return;
	}
	vterm_input_write(impl->vterm, (const char *)p_data, (size_t)p_len);
	vterm_screen_flush_damage(impl->vterm_screen);
}

Vector<uint8_t> MCPTerminalEmulator::consume_output() {
	Vector<uint8_t> out = impl->output_buffer;
	impl->output_buffer.clear();
	return out;
}

void MCPTerminalEmulator::input_key(Key p_key, int p_mod) {
	if (!impl->vterm) {
		return;
	}
	vterm_keyboard_key(impl->vterm, (VTermKey)p_key, (VTermModifier)p_mod);
}

void MCPTerminalEmulator::input_char(uint32_t p_char, int p_mod) {
	if (!impl->vterm) {
		return;
	}
	vterm_keyboard_unichar(impl->vterm, p_char, (VTermModifier)p_mod);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

void MCPTerminalEmulator::set_size(int p_rows, int p_cols) {
	if (!impl->vterm || p_rows <= 0 || p_cols <= 0) {
		return;
	}
	impl->rows = p_rows;
	impl->cols = p_cols;
	vterm_set_size(impl->vterm, p_rows, p_cols);
	vterm_screen_flush_damage(impl->vterm_screen);
}

int MCPTerminalEmulator::get_rows() const {
	return impl->rows;
}

int MCPTerminalEmulator::get_cols() const {
	return impl->cols;
}

MCPTerminalEmulator::CursorState MCPTerminalEmulator::get_cursor() const {
	return impl->cursor;
}

bool MCPTerminalEmulator::is_dirty() const {
	return impl->dirty;
}

void MCPTerminalEmulator::clear_dirty() {
	impl->dirty = false;
}

String MCPTerminalEmulator::get_title() const {
	return impl->title;
}

// ---------------------------------------------------------------------------
// Cell access
// ---------------------------------------------------------------------------

MCPTerminalEmulator::Cell MCPTerminalEmulator::get_cell(int p_row, int p_col) const {
	Cell cell;
	cell.fg = impl->default_fg;
	cell.bg = impl->default_bg;

	if (!impl->vterm_screen) {
		return cell;
	}
	if (p_col < 0 || p_col >= impl->cols || p_row < 0 || p_row >= impl->rows) {
		return cell;
	}

	VTermScreenCell vcell;
	memset(&vcell, 0, sizeof(vcell));

	VTermPos pos;
	pos.row = p_row;
	pos.col = p_col;
	vterm_screen_get_cell(impl->vterm_screen, pos, &vcell);

	return impl->to_cell(vcell);
}

int MCPTerminalEmulator::get_scrollback_length() const {
	return impl->scrollback.size();
}

MCPTerminalEmulator::Cell MCPTerminalEmulator::get_scrollback_cell(int p_scrollback_idx, int p_col) const {
	Cell cell;
	cell.fg = impl->default_fg;
	cell.bg = impl->default_bg;

	if (p_scrollback_idx < 0 || p_scrollback_idx >= impl->scrollback.size()) {
		return cell;
	}

	const MCPTerminalEmulatorImpl::ScrollbackLine &line = impl->scrollback[p_scrollback_idx];
	if (p_col < 0 || p_col >= line.cells.size()) {
		return cell;
	}

	return impl->to_cell(line.cells[p_col]);
}

#endif // MCP_TERMINAL_ENABLED
