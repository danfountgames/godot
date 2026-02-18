/**************************************************************************/
/*  debug_console.h                                                       */
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

#include "core/input/input_event.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "debug_console_autocomplete.h"

class DebugConsoleRenderer;
class Node;

class DebugConsole {
	static DebugConsole *singleton;

public:
	static DebugConsole *get_singleton() { return singleton; }

	// Console output entry (stored in ring buffer for display).
	struct OutputEntry {
		String text;
		Color color;
		uint64_t timestamp_msec = 0;
	};

	// Watch entry (pinned queries that update each frame).
	struct WatchEntry {
		String query_name;
		String display_text;
		Variant last_value;
	};

private:
	// === State ===
	bool _open = false;
	float _open_progress = 0.0f; // 0.0 = closed, 1.0 = open. Animates.
	float _target_height_ratio = 0.4f; // How much of screen to cover.

	// === Input ===
	String _input_text;
	int _cursor_pos = 0;
	bool _cursor_visible = true;
	float _cursor_blink_timer = 0.0f;

	// === History ===
	static const int HISTORY_CAPACITY = 100;
	Vector<String> _history;
	int _history_pos = -1; // -1 = not browsing history.
	String _saved_input; // Input saved when entering history mode.

	// === Output ===
	static const int OUTPUT_CAPACITY = 1000;
	Vector<OutputEntry> _output;
	int _output_write_pos = 0;
	int _output_count = 0;
	int _scroll_offset = 0; // Lines scrolled up from bottom.
	bool _auto_scroll = true;

	// === Autocomplete ===
	DebugConsoleAutocomplete _autocomplete;
	Vector<const DebugConsoleAutocomplete::CompletionEntry *> _current_completions;
	int _completion_selected = 0;
	bool _completion_visible = false;

	// === Watches ===
	Vector<WatchEntry> _watches;
	bool _watches_overlay_visible = false; // Toggle for persistent on-screen overlay. Default off.

	// === Discovery pills ===
public:
	enum DiscoveryType {
		DISCOVERY_ACTION,
		DISCOVERY_QUERY,
		DISCOVERY_CVAR,
		DISCOVERY_COMMAND,
		DISCOVERY_TYPE_COUNT,
	};

	struct DiscoveryItem {
		String name; // e.g. "add_score", "score", "paddle.speed"
		String display; // Label shown on pill (may include live value).
		String command; // Command string to populate input with on tap.
		DiscoveryType type = DISCOVERY_ACTION;
	};

	static const int DISCOVERY_TAB_COUNT = 4;
	static const char *discovery_tab_labels[DISCOVERY_TAB_COUNT];
	static const int MAX_DISCOVERY_PILLS = 20; // Max pills per tab visible.

private:
	int _discovery_active_tab = 1; // Default to queries (most useful to see live).
	Vector<DiscoveryItem> _discovery_items; // Current filtered items for active tab.
	bool _discovery_visible = true; // Show discovery row when registry has items.
	uint64_t _discovery_last_update_frame = 0;
	void _update_discovery_items();

	// === Rendering ===
	DebugConsoleRenderer *_renderer = nullptr;

	// === Mobile gesture tracking ===
	int _touch_count = 0;
	Vector2 _swipe_start;
	bool _swipe_tracking = false;
	static const int SWIPE_FINGER_COUNT = 2; // Two-finger swipe down to open.

	// === Mini-badge popup (visible when console is closed) ===
	bool _popup_visible = true; // Show popup when console closes.
	Vector2 _popup_pos; // Anchored position (screen coords).
	bool _popup_initialized = false;
	bool _popup_dragging = false;
	Vector2 _popup_drag_offset;
	int _popup_new_info = 0;
	int _popup_new_warning = 0;
	int _popup_new_error = 0;
	void _reset_popup_counts();
	void _snap_popup_to_edge(const Size2 &p_screen_size);

	// === Log type filter ===
	bool _filter_info = true;
	bool _filter_warning = true;
	bool _filter_error = true;

	// === Toggle key ===
	Key _toggle_key = Key::QUOTELEFT; // Backtick `

	// === Scene tree navigation (cwd) ===
	String _cwd = "/root"; // Current working directory in scene tree.

	// === Frame stepping ===
	int _step_frames_remaining = 0;
	void _step_one_frame();

	// === Console log sync ===
	int _last_console_log_sync = 0; // Last synced count from the registry.

	// === Internal methods ===
	void _execute_input();
	String _execute_command_string(const String &p_input);
	PackedStringArray _tokenize(const String &p_input) const;
	void _push_output(const String &p_text, const Color &p_color = Color(1, 1, 1));
	void _update_completions();
	void _update_watches();

	// Built-in command handlers.
	String _cmd_help(const PackedStringArray &p_args);
	String _cmd_clear(const PackedStringArray &p_args);
	String _cmd_list(const PackedStringArray &p_args);
	String _cmd_watch(const PackedStringArray &p_args);
	String _cmd_unwatch(const PackedStringArray &p_args);
	String _cmd_pause(const PackedStringArray &p_args);
	String _cmd_resume(const PackedStringArray &p_args);
	String _cmd_step(const PackedStringArray &p_args);
	String _cmd_step_instant(const PackedStringArray &p_args);
	String _cmd_timescale(const PackedStringArray &p_args);
	String _cmd_echo(const PackedStringArray &p_args);
	String _cmd_exec(const PackedStringArray &p_args);
	String _cmd_screenshot(const PackedStringArray &p_args);
	String _cmd_node(const PackedStringArray &p_args);
	String _cmd_cd(const PackedStringArray &p_args);
	String _cmd_ls(const PackedStringArray &p_args);
	String _cmd_pwd(const PackedStringArray &p_args);
	String _cmd_ui(const PackedStringArray &p_args);

	// Node command helpers.
	String _node_inspect(Node *p_node) const;
	String _node_ls(Node *p_node) const;
	Vector<Node *> _resolve_nodes(const String &p_target) const;
	String _resolve_path(const String &p_target) const; // Resolve relative to _cwd.

	// UI command helpers.
	String _ui_interact(Node *p_node, const PackedStringArray &p_args) const;
	String _ui_describe(Node *p_node) const;
	String _ui_find_controls(Node *p_root, const String &p_filter) const;

	// UI page navigation helpers.
	String _ui_pages() const;
	String _ui_where() const;
	String _ui_go(const String &p_page_name) const;
	String _ui_auto_detect_pages(Node *p_root) const;

public:
	// Called from Main::iteration().
	void poll(double p_delta);

	// Called before SceneTree gets input. Returns true if consumed.
	bool handle_input(const Ref<InputEvent> &p_event);

	// Console state.
	bool is_open() const { return _open; }
	void set_open(bool p_open);
	void toggle();

	// Output management.
	int get_output_count() const { return _output_count; }
	const OutputEntry &get_output_entry(int p_index) const;

	// Watch management.
	const Vector<WatchEntry> &get_watches() const { return _watches; }

	// Animation state.
	float get_open_progress() const { return _open_progress; }
	float get_target_height_ratio() const { return _target_height_ratio; }

	// Input state (for renderer).
	const String &get_input_text() const { return _input_text; }
	int get_cursor_pos() const { return _cursor_pos; }
	bool is_cursor_visible() const { return _cursor_visible; }
	int get_scroll_offset() const { return _scroll_offset; }
	bool is_completion_visible() const { return _completion_visible; }
	const Vector<const DebugConsoleAutocomplete::CompletionEntry *> &get_completions() const { return _current_completions; }
	int get_completion_selected() const { return _completion_selected; }
	const String &get_cwd() const { return _cwd; }

	// Renderer access.
	DebugConsoleRenderer *get_renderer() const { return _renderer; }

	// History access for renderer (returns the last N entries, most recent first).
	Vector<String> get_recent_history(int p_max = 6) const;

	// Actions triggered by tapping mobile UI elements.
	void select_completion(int p_index);
	void execute_quick_command(int p_index);
	void execute_history_pill(int p_index);
	void tap_discovery_pill(int p_index);
	void tap_discovery_tab(int p_index);
	void tap_transport(int p_index); // Transport control buttons in status bar.
	void tap_timescale(); // Cycle timescale presets.
	void toggle_watches_overlay(); // Toggle persistent watch overlay.

	// Watch overlay state.
	bool is_watches_overlay_visible() const { return _watches_overlay_visible; }
	bool has_watches() const { return !_watches.is_empty(); }

	// Discovery pill state (for renderer).
	bool is_discovery_visible() const { return _discovery_visible; }
	int get_discovery_active_tab() const { return _discovery_active_tab; }
	const Vector<DiscoveryItem> &get_discovery_items() const { return _discovery_items; }

	// Mini-badge popup state (for renderer).
	bool is_popup_visible() const { return _popup_visible && !_open; }
	Vector2 get_popup_pos() const { return _popup_pos; }
	int get_popup_info_count() const { return _popup_new_info; }
	int get_popup_warning_count() const { return _popup_new_warning; }
	int get_popup_error_count() const { return _popup_new_error; }
	void popup_tapped(); // Opens console and resets counts.
	void popup_drag(const Vector2 &p_pos);
	void popup_end_drag();

	// Log filter state (for renderer).
	bool is_filter_info() const { return _filter_info; }
	bool is_filter_warning() const { return _filter_warning; }
	bool is_filter_error() const { return _filter_error; }
	void toggle_filter_info();
	void toggle_filter_warning();
	void toggle_filter_error();
	bool passes_filter(const OutputEntry &p_entry) const;

	// Scroll state.
	bool is_auto_scroll() const { return _auto_scroll; }
	void snap_to_bottom();

	DebugConsole();
	~DebugConsole();
};

#endif // DEBUG_ENABLED
