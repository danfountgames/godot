/**************************************************************************/
/*  debug_console_renderer.h                                              */
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

#ifdef DEBUG_ENABLED

#include "core/math/color.h"
#include "core/math/rect2.h"
#include "servers/rendering/rendering_server.h"

class DebugConsole;

class DebugConsoleRenderer {
	// RenderingServer RIDs for our canvas.
	RID canvas;
	RID canvas_item_bg;
	RID canvas_item_text;
	RID canvas_item_input;
	RID canvas_item_autocomplete;
	RID canvas_item_status;
	RID canvas_item_watches;
	RID canvas_item_quick_buttons;
	RID canvas_item_history_pills;
	RID canvas_item_popup; // Mini-badge popup shown when console is closed.
	RID viewport_texture; // Not a full viewport — we draw into the root viewport's canvas.

	// Font.
	RID font_rid;
	int font_size = 14;
	float line_height = 18.0f;

	// Theme colors.
	Color bg_color = Color(0.05, 0.05, 0.1, 0.92);
	Color input_bg_color = Color(0.1, 0.1, 0.15, 1.0);
	Color input_text_color = Color(0.9, 0.9, 0.9);
	Color cursor_color = Color(1.0, 1.0, 1.0, 0.8);
	Color status_bg_color = Color(0.08, 0.08, 0.12, 1.0);
	Color status_text_color = Color(0.6, 0.6, 0.7);
	Color autocomplete_bg_color = Color(0.12, 0.12, 0.18, 0.95);
	Color autocomplete_selected_color = Color(0.2, 0.2, 0.3, 1.0);
	Color watch_bg_color = Color(0.05, 0.05, 0.1, 0.85);
	Color watch_text_color = Color(0.8, 1.0, 0.8);

	// Margins.
	float margin = 8.0f;
	float input_height = 32.0f;
	float status_height = 24.0f;

	// --- Pill / chip rendering parameters ---
	float pill_height = 28.0f;
	float pill_padding_h = 12.0f;
	float pill_gap = 6.0f;
	float pill_row_height = 36.0f;

	// Pill colors by category.
	Color pill_quick_bg_color = Color(0.12, 0.18, 0.25, 1.0);
	Color pill_quick_text_color = Color(0.5, 0.8, 1.0);
	Color pill_history_bg_color = Color(0.18, 0.15, 0.12, 1.0);
	Color pill_history_text_color = Color(0.9, 0.8, 0.6);
	Color pill_autocomplete_bg_color = Color(0.1, 0.2, 0.15, 1.0);
	Color pill_autocomplete_text_color = Color(0.6, 1.0, 0.7);

	// Mini-badge popup colors.
	Color popup_bg_color = Color(0.1, 0.1, 0.15, 0.9);
	Color popup_info_color = Color(0.4, 0.7, 1.0);
	Color popup_warning_color = Color(1.0, 0.85, 0.3);
	Color popup_error_color = Color(1.0, 0.35, 0.35);
	float popup_width = 120.0f;
	float popup_height = 32.0f;

	// Filter button colors.
	Color filter_active_color = Color(0.3, 0.3, 0.4, 1.0);
	Color filter_inactive_color = Color(0.15, 0.15, 0.2, 0.6);

	bool _initialized = false;

	// --- Hit-test system for tappable UI elements ---
public:
	enum class HitAction {
		NONE,
		AUTOCOMPLETE_SELECT, // payload = completion index
		QUICK_COMMAND, // payload = index into quick_commands
		HISTORY_PILL, // payload = index into recent history (0 = most recent)
		POPUP_TAP, // Mini-badge tapped — open console.
		FILTER_INFO, // Toggle info log filter.
		FILTER_WARNING, // Toggle warning log filter.
		FILTER_ERROR, // Toggle error log filter.
		SNAP_TO_BOTTOM, // Scroll to bottom button.
	};

	struct QuickCommand {
		const char *label;
		const char *command;
	};

	static const int MAX_HISTORY_PILLS = 6;
	static const int QUICK_COMMAND_COUNT = 8;
	static const QuickCommand quick_commands[QUICK_COMMAND_COUNT];

private:
	struct HitRect {
		Rect2 rect;
		HitAction action = HitAction::NONE;
		int payload = 0;
	};

	Vector<HitRect> _hit_rects;

	void _ensure_initialized();
	void _draw_rect(RID p_canvas_item, const Rect2 &p_rect, const Color &p_color);
	void _draw_text(RID p_canvas_item, const Vector2 &p_pos, const String &p_text, const Color &p_color, int p_font_size = -1);
	float _get_text_width(const String &p_text, int p_font_size = -1) const;
	void _draw_pill(RID p_canvas_item, const Rect2 &p_rect, const Color &p_bg_color,
			const String &p_text, const Color &p_text_color, int p_font_size = -1);

	Rect2 _popup_rect; // Cached for drag hit-testing.

public:
	void draw(DebugConsole *p_console);
	void draw_watches(DebugConsole *p_console);
	void draw_popup(DebugConsole *p_console);

	// Hit-test: returns the action for the given screen position, or NONE.
	HitAction hit_test(const Vector2 &p_position, int &r_payload) const;

	// Popup hit-test (separate because popup is drawn when console is closed).
	bool popup_hit_test(const Vector2 &p_position) const;

	DebugConsoleRenderer();
	~DebugConsoleRenderer();
};

#endif // DEBUG_ENABLED
