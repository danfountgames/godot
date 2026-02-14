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

	bool _initialized = false;

	void _ensure_initialized();
	void _draw_rect(RID p_canvas_item, const Rect2 &p_rect, const Color &p_color);
	void _draw_text(RID p_canvas_item, const Vector2 &p_pos, const String &p_text, const Color &p_color, int p_font_size = -1);
	float _get_text_width(const String &p_text, int p_font_size = -1) const;

public:
	void draw(DebugConsole *p_console);
	void draw_watches(DebugConsole *p_console);

	DebugConsoleRenderer();
	~DebugConsoleRenderer();
};

#endif // DEBUG_ENABLED
