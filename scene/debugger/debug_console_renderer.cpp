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

const DebugConsoleRenderer::QuickCommand DebugConsoleRenderer::quick_commands[QUICK_COMMAND_COUNT] = {
	{ "help", "help" },
	{ "ls", "ls" },
	{ "list all", "list all" },
	{ "clear", "clear" },
	{ "ui pages", "ui pages" },
	{ "ui where", "ui where" },
	{ "watch", "watch " },
	{ "unwatch", "unwatch" },
};

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
	if (canvas_item_quick_buttons.is_valid()) {
		rs->free_rid(canvas_item_quick_buttons);
	}
	if (canvas_item_history_pills.is_valid()) {
		rs->free_rid(canvas_item_history_pills);
	}
	if (canvas_item_discovery.is_valid()) {
		rs->free_rid(canvas_item_discovery);
	}
	if (canvas_item_popup.is_valid()) {
		rs->free_rid(canvas_item_popup);
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

	canvas_item_quick_buttons = rs->canvas_item_create();
	rs->canvas_item_set_parent(canvas_item_quick_buttons, canvas);

	canvas_item_history_pills = rs->canvas_item_create();
	rs->canvas_item_set_parent(canvas_item_history_pills, canvas);

	canvas_item_discovery = rs->canvas_item_create();
	rs->canvas_item_set_parent(canvas_item_discovery, canvas);

	canvas_item_popup = rs->canvas_item_create();
	rs->canvas_item_set_parent(canvas_item_popup, canvas);

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

void DebugConsoleRenderer::_draw_pill(RID p_canvas_item, const Rect2 &p_rect,
		const Color &p_bg_color, const String &p_text,
		const Color &p_text_color, int p_font_size) {
	_draw_rect(p_canvas_item, p_rect, p_bg_color);

	int size = (p_font_size > 0) ? p_font_size : font_size;
	float text_y = p_rect.position.y + p_rect.size.y - 7.0f;
	float text_x = p_rect.position.x + pill_padding_h;
	_draw_text(p_canvas_item, Vector2(text_x, text_y), p_text, p_text_color, size);
}

DebugConsoleRenderer::HitAction DebugConsoleRenderer::hit_test(
		const Vector2 &p_position, int &r_payload) const {
	// Iterate in reverse so elements drawn on top are tested first.
	for (int i = _hit_rects.size() - 1; i >= 0; i--) {
		if (_hit_rects[i].rect.has_point(p_position)) {
			r_payload = _hit_rects[i].payload;
			return _hit_rects[i].action;
		}
	}
	r_payload = 0;
	return HitAction::NONE;
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

	// Clear all canvas items and hit-test rects.
	rs->canvas_item_clear(canvas_item_bg);
	rs->canvas_item_clear(canvas_item_text);
	rs->canvas_item_clear(canvas_item_input);
	rs->canvas_item_clear(canvas_item_autocomplete);
	rs->canvas_item_clear(canvas_item_status);
	rs->canvas_item_clear(canvas_item_quick_buttons);
	rs->canvas_item_clear(canvas_item_history_pills);
	rs->canvas_item_clear(canvas_item_discovery);
	_hit_rects.clear();

	float progress = p_console->get_open_progress();
	if (progress <= 0.001f) {
		// Fully closed — hide everything.
		rs->canvas_item_set_visible(canvas_item_bg, false);
		rs->canvas_item_set_visible(canvas_item_text, false);
		rs->canvas_item_set_visible(canvas_item_input, false);
		rs->canvas_item_set_visible(canvas_item_autocomplete, false);
		rs->canvas_item_set_visible(canvas_item_status, false);
		rs->canvas_item_set_visible(canvas_item_quick_buttons, false);
		rs->canvas_item_set_visible(canvas_item_history_pills, false);
		rs->canvas_item_set_visible(canvas_item_discovery, false);
		return;
	}

	rs->canvas_item_set_visible(canvas_item_bg, true);
	rs->canvas_item_set_visible(canvas_item_text, true);
	rs->canvas_item_set_visible(canvas_item_input, true);
	rs->canvas_item_set_visible(canvas_item_autocomplete, true);
	rs->canvas_item_set_visible(canvas_item_status, true);
	rs->canvas_item_set_visible(canvas_item_quick_buttons, true);
	rs->canvas_item_set_visible(canvas_item_history_pills, true);
	rs->canvas_item_set_visible(canvas_item_discovery, true);

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

	// --- Determine which pill rows are active ---
	bool show_autocomplete_chips = p_console->is_completion_visible() &&
			!p_console->get_completions().is_empty();
	Vector<String> recent_history = p_console->get_recent_history(MAX_HISTORY_PILLS);
	bool show_history_pills = !recent_history.is_empty();
	bool show_quick_buttons = true; // Always show.
	bool show_discovery = p_console->is_discovery_visible();

	// --- Background ---
	Rect2 bg_rect(0, y_offset, screen_size.x, console_height);
	_draw_rect(canvas_item_bg, bg_rect, bg_color);

	// --- Status bar (bottom of console) — transport controls + filters ---
	float status_y = y_offset + console_height - status_height;
	Rect2 status_rect(0, status_y, screen_size.x, status_height);
	_draw_rect(canvas_item_status, status_rect, status_bg_color);

	int sfont = font_size - 2;
	float btn_h = status_height - 4.0f;
	float btn_y = status_y + 2.0f;
	float text_baseline = status_y + status_height - 6.0f;

	SceneTree *tree = SceneTree::get_singleton();
	bool suspended = tree && tree->is_suspended();
	bool paused = tree && tree->is_paused();

	// --- Left side: transport controls ---
	float lx = margin;

	// Play/Pause toggle button.
	{
		String label;
		Color bg, text_col;
		if (suspended || paused) {
			label = String::utf8("\u25b6"); // Play icon
			bg = Color(0.15, 0.25, 0.15, 1.0); // Green tint
			text_col = Color(0.4, 1.0, 0.4);
		} else {
			label = String::utf8("\u23f8"); // Pause icon
			bg = Color(0.25, 0.2, 0.1, 1.0); // Amber tint
			text_col = Color(1.0, 0.9, 0.3);
		}
		float bw = _get_text_width(label, sfont) + 14.0f;
		Rect2 r(lx, btn_y, bw, btn_h);
		_draw_rect(canvas_item_status, r, bg);
		_draw_text(canvas_item_status, Vector2(lx + 7.0f, text_baseline), label, text_col, sfont);
		HitRect hr;
		hr.rect = r;
		hr.action = HitAction::TRANSPORT;
		hr.payload = 0;
		_hit_rects.push_back(hr);
		lx += bw + 3.0f;
	}

	// Step buttons (always visible, but highlighted when suspended).
	{
		Color step_bg = suspended ? Color(0.15, 0.18, 0.28, 1.0) : Color(0.1, 0.1, 0.15, 0.6);
		Color step_text = suspended ? Color(0.6, 0.8, 1.0) : Color(0.35, 0.35, 0.4);

		// Step 1.
		String s1 = String::utf8("\u23ed\ufe0e1");
		float s1w = _get_text_width(s1, sfont) + 12.0f;
		Rect2 r1(lx, btn_y, s1w, btn_h);
		_draw_rect(canvas_item_status, r1, step_bg);
		_draw_text(canvas_item_status, Vector2(lx + 6.0f, text_baseline), s1, step_text, sfont);
		HitRect hr1;
		hr1.rect = r1;
		hr1.action = HitAction::TRANSPORT;
		hr1.payload = 1;
		_hit_rects.push_back(hr1);
		lx += s1w + 3.0f;

		// Step 10.
		String s10 = String::utf8("\u23ed\ufe0e10");
		float s10w = _get_text_width(s10, sfont) + 12.0f;
		Rect2 r10(lx, btn_y, s10w, btn_h);
		_draw_rect(canvas_item_status, r10, step_bg);
		_draw_text(canvas_item_status, Vector2(lx + 6.0f, text_baseline), s10, step_text, sfont);
		HitRect hr10;
		hr10.rect = r10;
		hr10.action = HitAction::TRANSPORT;
		hr10.payload = 2;
		_hit_rects.push_back(hr10);
		lx += s10w + 3.0f;
	}

	// Separator.
	lx += 4.0f;

	// Timescale button (tappable, cycles through presets).
	{
		double ts = Engine::get_singleton()->get_time_scale();
		String ts_label = vformat("x%s", String::num(ts, ts == (int)ts ? 0 : 2));
		float tsw = _get_text_width(ts_label, sfont) + 12.0f;
		Color ts_bg = (ts != 1.0) ? Color(0.25, 0.15, 0.1, 1.0) : Color(0.1, 0.1, 0.15, 0.6);
		Color ts_col = (ts != 1.0) ? Color(1.0, 0.7, 0.3) : Color(0.4, 0.4, 0.5);
		Rect2 r(lx, btn_y, tsw, btn_h);
		_draw_rect(canvas_item_status, r, ts_bg);
		_draw_text(canvas_item_status, Vector2(lx + 6.0f, text_baseline), ts_label, ts_col, sfont);
		HitRect hr;
		hr.rect = r;
		hr.action = HitAction::TIMESCALE;
		hr.payload = 0;
		_hit_rects.push_back(hr);
		lx += tsw + 8.0f;
	}

	// Frame count and FPS (non-interactive text).
	{
		String info = vformat("F:%d  FPS:%d", Engine::get_singleton()->get_process_frames(),
				Engine::get_singleton()->get_frames_per_second());
		_draw_text(canvas_item_status, Vector2(lx, text_baseline), info, status_text_color, sfont);
	}

	// --- Right side: watch toggle + filters ---
	{
		float rx = screen_size.x - margin;

		// Filter buttons: ERR, WARN, INFO.
		const char *filter_labels[] = { "ERR", "WARN", "INFO" };
		bool filter_states[] = { p_console->is_filter_error(), p_console->is_filter_warning(), p_console->is_filter_info() };
		Color filter_colors[] = { popup_error_color, popup_warning_color, popup_info_color };
		HitAction filter_actions[] = { HitAction::FILTER_ERROR, HitAction::FILTER_WARNING, HitAction::FILTER_INFO };

		for (int i = 0; i < 3; i++) {
			String label = filter_labels[i];
			float tw = _get_text_width(label, sfont);
			float bw = tw + 12.0f;
			rx -= bw + 4.0f;

			Rect2 r(rx, btn_y, bw, btn_h);
			Color bg = filter_states[i] ? filter_active_color : filter_inactive_color;
			_draw_rect(canvas_item_status, r, bg);
			_draw_text(canvas_item_status, Vector2(rx + 6.0f, text_baseline),
					label, filter_states[i] ? filter_colors[i] : Color(0.4, 0.4, 0.4), sfont);
			HitRect hr;
			hr.rect = r;
			hr.action = filter_actions[i];
			hr.payload = 0;
			_hit_rects.push_back(hr);
		}

		// Snap-to-bottom button (only when scrolled up).
		if (!p_console->is_auto_scroll()) {
			rx -= 4.0f;
			String snap_label = String::utf8("\u2193 BTM");
			float snap_w = _get_text_width(snap_label, sfont) + 12.0f;
			rx -= snap_w;

			Rect2 r(rx, btn_y, snap_w, btn_h);
			_draw_rect(canvas_item_status, r, Color(0.15, 0.25, 0.15, 1.0));
			_draw_text(canvas_item_status, Vector2(rx + 6.0f, text_baseline),
					snap_label, Color(0.5, 1.0, 0.5), sfont);
			HitRect hr;
			hr.rect = r;
			hr.action = HitAction::SNAP_TO_BOTTOM;
			hr.payload = 0;
			_hit_rects.push_back(hr);
		}

		// Watch overlay toggle.
		{
			rx -= 4.0f;
			bool has_w = p_console->has_watches();
			bool vis = p_console->is_watches_overlay_visible();
			String eye_label = has_w ? (vis ? String::utf8("\U0001F441") : String::utf8("\U0001F441\u0336")) : String::utf8("\U0001F441");
			// Simpler approach: use text labels.
			String w_label = vis ? "OVL" : "ovl";
			if (has_w) {
				w_label += vformat(":%d", p_console->get_watches().size());
			}
			float ww = _get_text_width(w_label, sfont) + 12.0f;
			rx -= ww;
			Color w_bg = vis ? Color(0.15, 0.22, 0.15, 1.0) : filter_inactive_color;
			Color w_col = vis ? Color(0.5, 1.0, 0.6) : Color(0.35, 0.35, 0.4);
			Rect2 r(rx, btn_y, ww, btn_h);
			_draw_rect(canvas_item_status, r, w_bg);
			_draw_text(canvas_item_status, Vector2(rx + 6.0f, text_baseline), w_label, w_col, sfont);
			HitRect hr;
			hr.rect = r;
			hr.action = HitAction::WATCH_TOGGLE;
			hr.payload = 0;
			_hit_rects.push_back(hr);
		}
	}

	// --- Input field (above status bar) ---
	float input_y = status_y - input_height;
	Rect2 input_rect(0, input_y, screen_size.x, input_height);
	_draw_rect(canvas_item_input, input_rect, input_bg_color);

	// Input prompt and text — show cwd as prompt.
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

	// --- Pill rows (stacked above input field, building from bottom up) ---
	float current_pill_y = input_y; // Start from top of input field.

	// Quick command buttons (always visible, bottom pill row).
	if (show_quick_buttons) {
		current_pill_y -= pill_row_height;
		float x = margin;

		_draw_rect(canvas_item_quick_buttons,
				Rect2(0, current_pill_y, screen_size.x, pill_row_height),
				Color(0.06, 0.06, 0.09, 0.8));

		for (int i = 0; i < QUICK_COMMAND_COUNT; i++) {
			String label = quick_commands[i].label;
			float text_w = _get_text_width(label, font_size - 1);
			float pill_w = text_w + pill_padding_h * 2;

			if (x + pill_w > screen_size.x - margin) {
				break;
			}

			Rect2 pill_rect(x, current_pill_y + (pill_row_height - pill_height) / 2.0f,
					pill_w, pill_height);
			_draw_pill(canvas_item_quick_buttons, pill_rect,
					pill_quick_bg_color, label, pill_quick_text_color, font_size - 1);

			HitRect hr;
			hr.rect = pill_rect;
			hr.action = HitAction::QUICK_COMMAND;
			hr.payload = i;
			_hit_rects.push_back(hr);

			x += pill_w + pill_gap;
		}
	}

	// History pills (recent commands, above quick buttons).
	if (show_history_pills) {
		current_pill_y -= pill_row_height;
		float x = margin;

		_draw_rect(canvas_item_history_pills,
				Rect2(0, current_pill_y, screen_size.x, pill_row_height),
				Color(0.07, 0.06, 0.04, 0.8));

		// "Recent:" label.
		_draw_text(canvas_item_history_pills,
				Vector2(x, current_pill_y + pill_row_height - 10.0f),
				"Recent:", Color(0.4, 0.4, 0.45), font_size - 2);
		x += _get_text_width("Recent:", font_size - 2) + 8.0f;

		for (int i = 0; i < recent_history.size(); i++) {
			String label = recent_history[i];
			if (label.length() > 20) {
				label = label.left(18) + "..";
			}
			float text_w = _get_text_width(label, font_size - 1);
			float pill_w = text_w + pill_padding_h * 2;

			if (x + pill_w > screen_size.x - margin) {
				break;
			}

			Rect2 pill_rect(x, current_pill_y + (pill_row_height - pill_height) / 2.0f,
					pill_w, pill_height);
			_draw_pill(canvas_item_history_pills, pill_rect,
					pill_history_bg_color, label, pill_history_text_color, font_size - 1);

			HitRect hr;
			hr.rect = pill_rect;
			hr.action = HitAction::HISTORY_PILL;
			hr.payload = i;
			_hit_rects.push_back(hr);

			x += pill_w + pill_gap;
		}
	}

	// Discovery pills (above history — tab headers + item pills).
	if (show_discovery) {
		const auto &items = p_console->get_discovery_items();
		int active_tab = p_console->get_discovery_active_tab();

		// Two rows: tab headers on top, item pills below.
		// Item pill row.
		current_pill_y -= pill_row_height;
		float pill_row_y = current_pill_y;

		_draw_rect(canvas_item_discovery,
				Rect2(0, pill_row_y, screen_size.x, pill_row_height),
				Color(0.04, 0.04, 0.07, 0.85));

		if (!items.is_empty()) {
			float x = margin;

			// Choose colors based on active tab type.
			Color item_bg, item_text;
			switch (active_tab) {
				case 0:
					item_bg = pill_action_bg_color;
					item_text = pill_action_text_color;
					break;
				case 1:
					item_bg = pill_query_bg_color;
					item_text = pill_query_text_color;
					break;
				case 2:
					item_bg = pill_cvar_bg_color;
					item_text = pill_cvar_text_color;
					break;
				case 3:
					item_bg = pill_command_bg_color;
					item_text = pill_command_text_color;
					break;
				default:
					item_bg = pill_quick_bg_color;
					item_text = pill_quick_text_color;
					break;
			}

			for (int i = 0; i < items.size(); i++) {
				String label = items[i].display;
				if (label.length() > 25) {
					label = label.left(23) + "..";
				}
				float text_w = _get_text_width(label, font_size - 2);
				float pw = text_w + pill_padding_h * 2;

				if (x + pw > screen_size.x - margin) {
					break; // No wrapping — just cut off.
				}

				Rect2 pill_rect(x, pill_row_y + (pill_row_height - pill_height) / 2.0f,
						pw, pill_height);
				_draw_pill(canvas_item_discovery, pill_rect, item_bg, label, item_text, font_size - 2);

				HitRect hr;
				hr.rect = pill_rect;
				hr.action = HitAction::DISCOVERY_PILL;
				hr.payload = i;
				_hit_rects.push_back(hr);

				x += pw + pill_gap;
			}
		} else {
			// Empty state message.
			_draw_text(canvas_item_discovery,
					Vector2(margin, pill_row_y + pill_row_height - 10.0f),
					"No items registered.", Color(0.4, 0.4, 0.45), font_size - 2);
		}

		// Tab header row (above item pills).
		current_pill_y -= pill_row_height;
		float tab_row_y = current_pill_y;

		_draw_rect(canvas_item_discovery,
				Rect2(0, tab_row_y, screen_size.x, pill_row_height),
				Color(0.05, 0.05, 0.08, 0.9));

		{
			float x = margin;

			// "Browse:" label.
			_draw_text(canvas_item_discovery,
					Vector2(x, tab_row_y + pill_row_height - 10.0f),
					"Browse:", Color(0.45, 0.45, 0.5), font_size - 2);
			x += _get_text_width("Browse:", font_size - 2) + 8.0f;

			for (int i = 0; i < DebugConsole::DISCOVERY_TAB_COUNT; i++) {
				String label = DebugConsole::discovery_tab_labels[i];
				float text_w = _get_text_width(label, font_size - 1);
				float pw = text_w + pill_padding_h * 2;

				if (x + pw > screen_size.x - margin) {
					break;
				}

				bool is_active = (i == active_tab);
				Color bg = is_active ? pill_tab_active_bg : pill_tab_inactive_bg;
				Color text_col = is_active ? Color(1.0, 1.0, 1.0) : pill_tab_text_color;

				Rect2 pill_rect(x, tab_row_y + (pill_row_height - pill_height) / 2.0f,
						pw, pill_height);
				_draw_pill(canvas_item_discovery, pill_rect, bg, label, text_col, font_size - 1);

				HitRect hr;
				hr.rect = pill_rect;
				hr.action = HitAction::DISCOVERY_TAB;
				hr.payload = i;
				_hit_rects.push_back(hr);

				x += pw + pill_gap;
			}
		}
	}

	// Autocomplete chips (above discovery/history, only when completions are visible).
	if (show_autocomplete_chips) {
		current_pill_y -= pill_row_height;
		float x = margin;

		const auto &completions = p_console->get_completions();

		_draw_rect(canvas_item_autocomplete,
				Rect2(0, current_pill_y, screen_size.x, pill_row_height),
				Color(0.05, 0.08, 0.06, 0.8));

		for (int i = 0; i < completions.size(); i++) {
			String label = completions[i]->name;
			float text_w = _get_text_width(label, font_size - 1);
			float pill_w = text_w + pill_padding_h * 2;

			if (x + pill_w > screen_size.x - margin) {
				break;
			}

			Color bg = (i == p_console->get_completion_selected())
					? autocomplete_selected_color
					: pill_autocomplete_bg_color;

			Rect2 pill_rect(x, current_pill_y + (pill_row_height - pill_height) / 2.0f,
					pill_w, pill_height);
			_draw_pill(canvas_item_autocomplete, pill_rect, bg, label,
					pill_autocomplete_text_color, font_size - 1);

			HitRect hr;
			hr.rect = pill_rect;
			hr.action = HitAction::AUTOCOMPLETE_SELECT;
			hr.payload = i;
			_hit_rects.push_back(hr);

			x += pill_w + pill_gap;
		}
	}

	// --- Output text (above pill rows) ---
	float output_y_end = current_pill_y;
	float output_y_start = y_offset;
	float output_height = output_y_end - output_y_start;
	int visible_lines = (int)(output_height / line_height);

	int total_entries = p_console->get_output_count();
	int scroll = p_console->get_scroll_offset();

	// Draw from bottom up, skipping filtered entries.
	int drawn = 0;
	int skipped_for_scroll = 0;
	for (int raw_i = 0; raw_i < total_entries; raw_i++) {
		int entry_idx = total_entries - 1 - raw_i;
		if (entry_idx < 0) {
			break;
		}

		const DebugConsole::OutputEntry &entry = p_console->get_output_entry(entry_idx);

		// Apply log type filter.
		if (!p_console->passes_filter(entry)) {
			continue;
		}

		// Apply scroll offset (skip entries for scrolling).
		if (skipped_for_scroll < scroll) {
			skipped_for_scroll++;
			continue;
		}

		float text_y = output_y_end - (drawn + 1) * line_height + line_height - 4.0f;
		if (text_y < output_y_start) {
			break;
		}

		_draw_text(canvas_item_text,
				Vector2(margin, text_y),
				entry.text, entry.color);
		drawn++;

		if (drawn >= visible_lines) {
			break;
		}
	}

	// Draw watches on top.
	draw_watches(p_console);
}

// =============================================================================
// Mini-badge popup (drawn when console is closed)
// =============================================================================

void DebugConsoleRenderer::draw_popup(DebugConsole *p_console) {
	_ensure_initialized();
	if (!_initialized) {
		return;
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	rs->canvas_item_clear(canvas_item_popup);

	if (!p_console->is_popup_visible()) {
		rs->canvas_item_set_visible(canvas_item_popup, false);
		_popup_rect = Rect2();
		return;
	}

	rs->canvas_item_set_visible(canvas_item_popup, true);

	Vector2 pos = p_console->get_popup_pos();
	_popup_rect = Rect2(pos.x, pos.y, popup_width, popup_height);

	// Background.
	_draw_rect(canvas_item_popup, _popup_rect, popup_bg_color);

	// Thin top border colored by severity.
	int errs = p_console->get_popup_error_count();
	int warns = p_console->get_popup_warning_count();
	Color border_color = popup_info_color;
	if (errs > 0) {
		border_color = popup_error_color;
	} else if (warns > 0) {
		border_color = popup_warning_color;
	}
	_draw_rect(canvas_item_popup, Rect2(pos.x, pos.y, popup_width, 3.0f), border_color);

	// Log count badges: I:n W:n E:n
	float text_y = pos.y + popup_height - 8.0f;
	float tx = pos.x + 8.0f;
	int pfont = font_size - 2;

	int infos = p_console->get_popup_info_count();
	String info_str = vformat("I:%d", infos);
	_draw_text(canvas_item_popup, Vector2(tx, text_y), info_str, popup_info_color, pfont);
	tx += _get_text_width(info_str, pfont) + 10.0f;

	String warn_str = vformat("W:%d", warns);
	_draw_text(canvas_item_popup, Vector2(tx, text_y), warn_str, popup_warning_color, pfont);
	tx += _get_text_width(warn_str, pfont) + 10.0f;

	String err_str = vformat("E:%d", errs);
	_draw_text(canvas_item_popup, Vector2(tx, text_y), err_str, popup_error_color, pfont);
}

bool DebugConsoleRenderer::popup_hit_test(const Vector2 &p_position) const {
	return _popup_rect.size.x > 0 && _popup_rect.has_point(p_position);
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
	if (watches.is_empty() || !p_console->is_watches_overlay_visible()) {
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
