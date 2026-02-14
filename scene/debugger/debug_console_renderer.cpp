/**************************************************************************/
/*  debug_console_renderer.cpp                                            */
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

#ifdef DEBUG_ENABLED

#include "debug_console_renderer.h"

#include "core/config/engine.h"
#include "debug_console.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/theme/theme_db.h"
#include "servers/rendering/rendering_server.h"

DebugConsoleRenderer::DebugConsoleRenderer() {
}

DebugConsoleRenderer::~DebugConsoleRenderer() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs) {
		return;
	}

	if (canvas.is_valid()) {
		rs->free_rid(canvas);
	}
	if (canvas_item_bg.is_valid()) {
		rs->free_rid(canvas_item_bg);
	}
	if (canvas_item_text.is_valid()) {
		rs->free_rid(canvas_item_text);
	}
	if (canvas_item_input.is_valid()) {
		rs->free_rid(canvas_item_input);
	}
	if (canvas_item_autocomplete.is_valid()) {
		rs->free_rid(canvas_item_autocomplete);
	}
	if (canvas_item_status.is_valid()) {
		rs->free_rid(canvas_item_status);
	}
	if (canvas_item_watches.is_valid()) {
		rs->free_rid(canvas_item_watches);
	}
}

void DebugConsoleRenderer::_ensure_initialized() {
	if (_initialized) {
		return;
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs) {
		return;
	}

	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		return;
	}

	// Create our canvas.
	canvas = rs->canvas_create();

	// Attach to root viewport at a very high z-index so we're on top of everything.
	RID root_viewport = tree->get_root()->get_viewport_rid();
	rs->viewport_attach_canvas(root_viewport, canvas);
	// Set canvas layer to a very high value.
	rs->viewport_set_canvas_stacking(root_viewport, canvas, 1000, 0); // layer 1000, sublayer 0

	// Create canvas items, parented to our canvas.
	canvas_item_bg = rs->canvas_item_create();
	rs->canvas_item_set_parent(canvas_item_bg, canvas);

	canvas_item_text = rs->canvas_item_create();
	rs->canvas_item_set_parent(canvas_item_text, canvas);

	canvas_item_input = rs->canvas_item_create();
	rs->canvas_item_set_parent(canvas_item_input, canvas);

	canvas_item_autocomplete = rs->canvas_item_create();
	rs->canvas_item_set_parent(canvas_item_autocomplete, canvas);

	canvas_item_status = rs->canvas_item_create();
	rs->canvas_item_set_parent(canvas_item_status, canvas);

	canvas_item_watches = rs->canvas_item_create();
	rs->canvas_item_set_parent(canvas_item_watches, canvas);

	_initialized = true;
}

void DebugConsoleRenderer::_draw_rect(RID p_canvas_item, const Rect2 &p_rect, const Color &p_color) {
	RenderingServer::get_singleton()->canvas_item_add_rect(p_canvas_item, p_rect, p_color);
}

void DebugConsoleRenderer::_draw_text(RID p_canvas_item, const Vector2 &p_pos, const String &p_text, const Color &p_color, int p_font_size) {
	Ref<Font> font = ThemeDB::get_singleton()->get_fallback_font();
	if (font.is_null()) {
		return;
	}
	int size = (p_font_size > 0) ? p_font_size : font_size;
	font->draw_string(p_canvas_item, p_pos, p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, size, p_color);
}

float DebugConsoleRenderer::_get_text_width(const String &p_text, int p_font_size) const {
	Ref<Font> font = ThemeDB::get_singleton()->get_fallback_font();
	if (font.is_null()) {
		return 0.0f;
	}
	int size = (p_font_size > 0) ? p_font_size : font_size;
	return font->get_string_size(p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, size).x;
}

// =============================================================================
// Main draw
// =============================================================================

void DebugConsoleRenderer::draw(DebugConsole *p_console) {
	_ensure_initialized();
	if (!_initialized) {
		return;
	}

	RenderingServer *rs = RenderingServer::get_singleton();

	// Clear all canvas items.
	rs->canvas_item_clear(canvas_item_bg);
	rs->canvas_item_clear(canvas_item_text);
	rs->canvas_item_clear(canvas_item_input);
	rs->canvas_item_clear(canvas_item_autocomplete);
	rs->canvas_item_clear(canvas_item_status);

	float progress = p_console->get_open_progress();
	if (progress <= 0.001f) {
		// Fully closed — hide everything.
		rs->canvas_item_set_visible(canvas_item_bg, false);
		rs->canvas_item_set_visible(canvas_item_text, false);
		rs->canvas_item_set_visible(canvas_item_input, false);
		rs->canvas_item_set_visible(canvas_item_autocomplete, false);
		rs->canvas_item_set_visible(canvas_item_status, false);
		return;
	}

	rs->canvas_item_set_visible(canvas_item_bg, true);
	rs->canvas_item_set_visible(canvas_item_text, true);
	rs->canvas_item_set_visible(canvas_item_input, true);
	rs->canvas_item_set_visible(canvas_item_autocomplete, true);
	rs->canvas_item_set_visible(canvas_item_status, true);

	// Get screen size.
	Window *root = SceneTree::get_singleton()->get_root();
	Size2 screen_size = root->get_size();

	float console_height = screen_size.y * p_console->get_target_height_ratio();
	float y_offset = -console_height * (1.0f - progress); // Slide in from top.

	// Calculate font metrics.
	Ref<Font> font = ThemeDB::get_singleton()->get_fallback_font();
	if (font.is_null()) {
		return;
	}
	line_height = font->get_height(font_size) + 2.0f;

	// --- Background ---
	Rect2 bg_rect(0, y_offset, screen_size.x, console_height);
	_draw_rect(canvas_item_bg, bg_rect, bg_color);

	// --- Status bar (bottom of console) ---
	float status_y = y_offset + console_height - status_height;
	Rect2 status_rect(0, status_y, screen_size.x, status_height);
	_draw_rect(canvas_item_status, status_rect, status_bg_color);

	// Status bar text.
	SceneTree *tree = SceneTree::get_singleton();
	String status_text;
	if (tree) {
		if (tree->is_suspended()) {
			status_text += String::utf8("\u23f8 SUSPENDED");
		} else if (tree->is_paused()) {
			status_text += String::utf8("\u23f8 PAUSED");
		} else {
			status_text += String::utf8("\u25b6 RUNNING");
		}
	}
	status_text += vformat("  |  F:%d", Engine::get_singleton()->get_process_frames());
	status_text += vformat("  |  FPS:%d", Engine::get_singleton()->get_frames_per_second());
	double ts = Engine::get_singleton()->get_time_scale();
	if (ts != 1.0) {
		status_text += vformat("  |  x%s", String::num(ts, 2));
	}

	_draw_text(canvas_item_status,
			Vector2(margin, status_y + status_height - 6.0f),
			status_text, status_text_color, font_size - 2);

	// --- Input field (above status bar) ---
	float input_y = status_y - input_height;
	Rect2 input_rect(0, input_y, screen_size.x, input_height);
	_draw_rect(canvas_item_input, input_rect, input_bg_color);

	// Input prompt and text — show cwd as prompt.
	// Shorten the cwd: if it starts with "/root", display from the child.
	// /root → ~    /root/Level → ~/Level    /root/Level/Player → ~/Level/Player
	String cwd = p_console->get_cwd();
	String prompt_path;
	if (cwd == "/root") {
		prompt_path = "~";
	} else if (cwd.begins_with("/root/")) {
		prompt_path = "~/" + cwd.substr(6);
	} else {
		prompt_path = cwd;
	}
	String prompt = prompt_path + "> ";
	String full_input = prompt + p_console->get_input_text();

	// Draw cwd part in a dimmer color, then the ">" and input in the normal color.
	_draw_text(canvas_item_input,
			Vector2(margin, input_y + input_height - 8.0f),
			prompt_path, Color(0.45, 0.55, 0.65));
	float path_width = _get_text_width(prompt_path);
	_draw_text(canvas_item_input,
			Vector2(margin + path_width, input_y + input_height - 8.0f),
			"> " + p_console->get_input_text(), input_text_color);

	// Cursor.
	if (p_console->is_cursor_visible() && p_console->is_open()) {
		String before_cursor = prompt + p_console->get_input_text().left(p_console->get_cursor_pos());
		float cursor_x = margin + _get_text_width(before_cursor);
		Rect2 cursor_rect(cursor_x, input_y + 4.0f, 2.0f, input_height - 8.0f);
		_draw_rect(canvas_item_input, cursor_rect, cursor_color);
	}

	// --- Output text (above input field) ---
	float output_y_end = input_y;
	float output_y_start = y_offset;
	float output_height = output_y_end - output_y_start;
	int visible_lines = (int)(output_height / line_height);

	int total_entries = p_console->get_output_count();
	int scroll = p_console->get_scroll_offset();

	// Draw from bottom up.
	for (int i = 0; i < visible_lines && i < total_entries; i++) {
		int entry_idx = total_entries - 1 - i - scroll;
		if (entry_idx < 0) {
			break;
		}

		const DebugConsole::OutputEntry &entry = p_console->get_output_entry(entry_idx);
		float text_y = output_y_end - (i + 1) * line_height + line_height - 4.0f;

		if (text_y < output_y_start) {
			break;
		}

		_draw_text(canvas_item_text,
				Vector2(margin, text_y),
				entry.text, entry.color);
	}

	// --- Autocomplete popup ---
	if (p_console->is_completion_visible() && !p_console->get_completions().is_empty()) {
		const auto &completions = p_console->get_completions();
		int count = completions.size();
		float popup_height = count * line_height + 8.0f;
		float popup_y = input_y - popup_height;
		float popup_width = 0.0f;

		// Calculate max width.
		for (int i = 0; i < count; i++) {
			float w = _get_text_width(completions[i]->name + "  " + completions[i]->type_label + "  " + completions[i]->description);
			popup_width = MAX(popup_width, w);
		}
		popup_width = MIN(popup_width + margin * 4, screen_size.x - margin * 2);

		Rect2 popup_rect(margin, popup_y, popup_width, popup_height);
		_draw_rect(canvas_item_autocomplete, popup_rect, autocomplete_bg_color);

		for (int i = 0; i < count; i++) {
			float entry_y = popup_y + i * line_height;

			// Highlight selected.
			if (i == p_console->get_completion_selected()) {
				_draw_rect(canvas_item_autocomplete,
						Rect2(margin, entry_y, popup_width, line_height),
						autocomplete_selected_color);
			}

			// Type label.
			float text_x = margin + 4.0f;
			_draw_text(canvas_item_autocomplete,
					Vector2(text_x, entry_y + line_height - 4.0f),
					completions[i]->type_label,
					completions[i]->type_color, font_size - 2);
			text_x += _get_text_width(completions[i]->type_label, font_size - 2) + 8.0f;

			// Name.
			_draw_text(canvas_item_autocomplete,
					Vector2(text_x, entry_y + line_height - 4.0f),
					completions[i]->name,
					Color(1, 1, 1));
			text_x += _get_text_width(completions[i]->name) + 12.0f;

			// Description.
			_draw_text(canvas_item_autocomplete,
					Vector2(text_x, entry_y + line_height - 4.0f),
					completions[i]->description,
					Color(0.5, 0.5, 0.6), font_size - 2);
		}
	}

	// Draw watches on top.
	draw_watches(p_console);
}

// =============================================================================
// Watch overlay (drawn even when console is closed)
// =============================================================================

void DebugConsoleRenderer::draw_watches(DebugConsole *p_console) {
	_ensure_initialized();
	if (!_initialized) {
		return;
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	rs->canvas_item_clear(canvas_item_watches);

	const Vector<DebugConsole::WatchEntry> &watches = p_console->get_watches();
	if (watches.is_empty()) {
		rs->canvas_item_set_visible(canvas_item_watches, false);
		return;
	}

	rs->canvas_item_set_visible(canvas_item_watches, true);

	Window *root_window = SceneTree::get_singleton()->get_root();
	if (!root_window) {
		return;
	}
	Size2 screen_size = root_window->get_size();

	float watch_x = screen_size.x - margin - 250.0f;
	float watch_y = margin;

	// Background.
	float watch_height = watches.size() * line_height + margin * 2;
	Rect2 watch_bg_rect(watch_x - margin, watch_y - margin / 2.0f, 250.0f + margin * 2, watch_height);
	_draw_rect(canvas_item_watches, watch_bg_rect, watch_bg_color);

	for (int i = 0; i < watches.size(); i++) {
		_draw_text(canvas_item_watches,
				Vector2(watch_x, watch_y + (i + 1) * line_height - 4.0f),
				watches[i].display_text,
				watch_text_color);
	}
}

#endif // DEBUG_ENABLED
