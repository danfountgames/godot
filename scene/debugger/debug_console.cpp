/**************************************************************************/
/*  debug_console.cpp                                                     */
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

#include "debug_console.h"

#include "core/config/engine.h"
#include "core/input/input.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "debug_console_renderer.h"
#include "scene/gui/base_button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/check_button.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/menu_button.h"
#include "scene/gui/option_button.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/range.h"
#include "scene/gui/slider.h"
#include "scene/gui/spin_box.h"
#include "scene/gui/tab_bar.h"
#include "scene/gui/tab_container.h"
#include "scene/gui/text_edit.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_server.h"

DebugConsole *DebugConsole::singleton = nullptr;

const char *DebugConsole::discovery_tab_labels[DISCOVERY_TAB_COUNT] = {
	"Actions", "Queries", "CVars", "Commands"
};

DebugConsole::DebugConsole() {
	singleton = this;

	_output.resize(OUTPUT_CAPACITY);

	// Only create the renderer if we have a display (not headless).
	DisplayServer *ds = DisplayServer::get_singleton();
	if (ds && ds->get_name() != "headless") {
		_renderer = memnew(DebugConsoleRenderer);
	}

	// Welcome message.
	_push_output("Godot Debug Console", Color(0.6, 0.8, 1.0));
	_push_output("Type 'help' for available commands.", Color(0.5, 0.5, 0.6));
}

DebugConsole::~DebugConsole() {
	if (_renderer) {
		memdelete(_renderer);
		_renderer = nullptr;
	}
	singleton = nullptr;
}

// =============================================================================
// Main update loop — called from Main::iteration()
// =============================================================================

void DebugConsole::poll(double p_delta) {
	// --- Frame stepping logic ---
	// poll() is called at the end of Main::iteration(). If we unsuspended
	// for a step, the SceneTree just processed a frame. Re-suspend and
	// chain the next frame if more remain.
	if (_step_frames_remaining > 0) {
		_step_frames_remaining--;
		if (_step_frames_remaining > 0) {
			// More frames to step — chain another.
			_step_one_frame();
		} else {
			// Done stepping — re-suspend.
			SceneTree *tree = SceneTree::get_singleton();
			if (tree) {
				tree->set_suspend(true);
			}
		}
	}

	// Animate open/close.
	float target = _open ? 1.0f : 0.0f;
	if (_open_progress != target) {
		float speed = _open ? (1.0f / 0.15f) : (1.0f / 0.1f); // 0.15s open, 0.1s close.
		if (_open_progress < target) {
			_open_progress = MIN(_open_progress + (float)p_delta * speed, 1.0f);
		} else {
			_open_progress = MAX(_open_progress - (float)p_delta * speed, 0.0f);
		}
	}

	// Initialize popup position on first frame.
	if (!_popup_initialized && _renderer) {
		Window *root = SceneTree::get_singleton() ? SceneTree::get_singleton()->get_root() : nullptr;
		if (root) {
			Size2 ss = root->get_size();
			_popup_pos = Vector2(ss.x - 124.0f, 4.0f); // Top-right corner.
			_popup_initialized = true;
		}
	}

	// Semantic registry log sync removed.

	// Skip main processing if fully closed.
	if (_open_progress <= 0.0f && !_open) {
		// Still update watches and draw popup.
		_update_watches();
		if (_renderer) {
			_renderer->draw_watches(this);
			_renderer->draw_popup(this);
		}
		return;
	}

	// Cursor blink.
	_cursor_blink_timer += (float)p_delta;
	if (_cursor_blink_timer >= 0.5f) {
		_cursor_blink_timer -= 0.5f;
		_cursor_visible = !_cursor_visible;
	}

	// Rebuild autocomplete if needed.
	_autocomplete.rebuild_if_dirty();

	// Update watches.
	_update_watches();

	// Update discovery pills (every 10 frames to keep live values fresh).
	uint64_t frame = Engine::get_singleton()->get_process_frames();
	if (frame - _discovery_last_update_frame >= 10) {
		_discovery_last_update_frame = frame;
		_update_discovery_items();
	}

	// Render.
	if (_renderer) {
		_renderer->draw(this);
	}
}

// =============================================================================
// Input handling — called before SceneTree
// =============================================================================

bool DebugConsole::handle_input(const Ref<InputEvent> &p_event) {
	// --- Toggle key ---
	Ref<InputEventKey> key_event = p_event;
	if (key_event.is_valid() && key_event->is_pressed() && !key_event->is_echo()) {
		if (key_event->get_keycode() == _toggle_key && !key_event->is_ctrl_pressed() && !key_event->is_alt_pressed()) {
			toggle();
			return true;
		}
	}

	// --- Mobile gesture: two-finger swipe down to open ---
	Ref<InputEventScreenTouch> touch_event = p_event;
	if (touch_event.is_valid()) {
		if (touch_event->is_pressed()) {
			_touch_count++;
			if (_touch_count >= SWIPE_FINGER_COUNT && !_open) {
				_swipe_start = touch_event->get_position();
				_swipe_tracking = true;
			}
		} else {
			_touch_count = MAX(_touch_count - 1, 0);
			_swipe_tracking = false;
		}
	}

	Ref<InputEventScreenDrag> drag_event = p_event;
	if (drag_event.is_valid() && _swipe_tracking && _touch_count >= SWIPE_FINGER_COUNT) {
		float dy = drag_event->get_position().y - _swipe_start.y;
		if (dy > 50.0f) { // 50 pixels threshold.
			set_open(true);
			_swipe_tracking = false;
		}
	}

	// --- Popup interaction (when console is closed) ---
	if (!_open && _popup_visible && _renderer) {
		// Handle popup dragging.
		if (drag_event.is_valid() && _popup_dragging) {
			popup_drag(drag_event->get_position());
			return true;
		}

		// Touch on popup.
		if (touch_event.is_valid()) {
			if (touch_event->is_pressed() && _touch_count == 1) {
				if (_renderer->popup_hit_test(touch_event->get_position())) {
					_popup_drag_offset = touch_event->get_position() - _popup_pos;
					_popup_dragging = true;
					return true;
				}
			} else if (!touch_event->is_pressed() && _popup_dragging) {
				// Check if it was a tap (barely moved) vs a drag.
				Vector2 delta = touch_event->get_position() - (_popup_pos + _popup_drag_offset);
				if (delta.length() < 10.0f) {
					_popup_dragging = false;
					popup_tapped();
				} else {
					popup_end_drag();
				}
				return true;
			}
		}

		// Mouse click on popup.
		Ref<InputEventMouseButton> popup_click = p_event;
		if (popup_click.is_valid() && popup_click->is_pressed() &&
				popup_click->get_button_index() == MouseButton::LEFT) {
			if (_renderer->popup_hit_test(popup_click->get_position())) {
				popup_tapped();
				return true;
			}
		}
	}

	// If console is closed, don't consume any more input.
	if (!_open) {
		return false;
	}

	// --- Touch/click hit-testing for tappable pills ---
	if (_renderer) {
		Vector2 tap_pos;
		bool is_tap = false;

		// Touch release = tap (single finger only).
		if (touch_event.is_valid() && !touch_event->is_pressed() && _touch_count == 0) {
			tap_pos = touch_event->get_position();
			is_tap = true;
		}

		// Mouse left-click = tap.
		Ref<InputEventMouseButton> mouse_click = p_event;
		if (mouse_click.is_valid() && mouse_click->is_pressed() &&
				mouse_click->get_button_index() == MouseButton::LEFT) {
			tap_pos = mouse_click->get_position();
			is_tap = true;
		}

		if (is_tap) {
			int payload = 0;
			DebugConsoleRenderer::HitAction action = _renderer->hit_test(tap_pos, payload);

			switch (action) {
				case DebugConsoleRenderer::HitAction::AUTOCOMPLETE_SELECT:
					select_completion(payload);
					return true;
				case DebugConsoleRenderer::HitAction::QUICK_COMMAND:
					execute_quick_command(payload);
					return true;
				case DebugConsoleRenderer::HitAction::HISTORY_PILL:
					execute_history_pill(payload);
					return true;
				case DebugConsoleRenderer::HitAction::FILTER_INFO:
					toggle_filter_info();
					return true;
				case DebugConsoleRenderer::HitAction::FILTER_WARNING:
					toggle_filter_warning();
					return true;
				case DebugConsoleRenderer::HitAction::FILTER_ERROR:
					toggle_filter_error();
					return true;
				case DebugConsoleRenderer::HitAction::SNAP_TO_BOTTOM:
					snap_to_bottom();
					return true;
				case DebugConsoleRenderer::HitAction::DISCOVERY_PILL:
					tap_discovery_pill(payload);
					return true;
				case DebugConsoleRenderer::HitAction::DISCOVERY_TAB:
					tap_discovery_tab(payload);
					return true;
				case DebugConsoleRenderer::HitAction::TRANSPORT:
					tap_transport(payload);
					return true;
				case DebugConsoleRenderer::HitAction::TIMESCALE:
					tap_timescale();
					return true;
				case DebugConsoleRenderer::HitAction::WATCH_TOGGLE:
					toggle_watches_overlay();
					return true;
				case DebugConsoleRenderer::HitAction::POPUP_TAP:
					popup_tapped();
					return true;
				case DebugConsoleRenderer::HitAction::NONE:
					break;
			}
		}
	}

	// --- Key input when console is open ---
	if (key_event.is_valid() && key_event->is_pressed()) {
		Key keycode = key_event->get_keycode();

		switch (keycode) {
			case Key::ESCAPE: {
				set_open(false);
				return true;
			}
			case Key::ENTER:
			case Key::KP_ENTER: {
				if (_completion_visible && _completion_selected >= 0 &&
						_completion_selected < _current_completions.size()) {
					// Accept completion.
					_input_text = _current_completions[_completion_selected]->name + " ";
					_cursor_pos = _input_text.length();
					_completion_visible = false;
					_update_completions();
				} else {
					_execute_input();
				}
				return true;
			}
			case Key::TAB: {
				if (!_current_completions.is_empty()) {
					if (!_completion_visible) {
						_completion_visible = true;
						_completion_selected = 0;
					} else {
						// Cycle through completions.
						_completion_selected = (_completion_selected + 1) % _current_completions.size();
					}
				}
				return true;
			}
			case Key::UP: {
				if (_completion_visible && !_current_completions.is_empty()) {
					_completion_selected = (_completion_selected - 1 + _current_completions.size()) % _current_completions.size();
				} else {
					// History navigation.
					if (_history_pos == -1) {
						_saved_input = _input_text;
						_history_pos = _history.size() - 1;
					} else if (_history_pos > 0) {
						_history_pos--;
					}
					if (_history_pos >= 0 && _history_pos < _history.size()) {
						_input_text = _history[_history_pos];
						_cursor_pos = _input_text.length();
					}
				}
				return true;
			}
			case Key::DOWN: {
				if (_completion_visible && !_current_completions.is_empty()) {
					_completion_selected = (_completion_selected + 1) % _current_completions.size();
				} else {
					// History navigation.
					if (_history_pos >= 0) {
						_history_pos++;
						if (_history_pos >= _history.size()) {
							_history_pos = -1;
							_input_text = _saved_input;
						} else {
							_input_text = _history[_history_pos];
						}
						_cursor_pos = _input_text.length();
					}
				}
				return true;
			}
			case Key::BACKSPACE: {
				if (_cursor_pos > 0) {
					_input_text = _input_text.left(_cursor_pos - 1) + _input_text.substr(_cursor_pos);
					_cursor_pos--;
					_update_completions();
				}
				return true;
			}
			case Key::KEY_DELETE: {
				if (_cursor_pos < _input_text.length()) {
					_input_text = _input_text.left(_cursor_pos) + _input_text.substr(_cursor_pos + 1);
					_update_completions();
				}
				return true;
			}
			case Key::LEFT: {
				_cursor_pos = MAX(_cursor_pos - 1, 0);
				return true;
			}
			case Key::RIGHT: {
				_cursor_pos = MIN(_cursor_pos + 1, _input_text.length());
				return true;
			}
			case Key::HOME: {
				_cursor_pos = 0;
				return true;
			}
			case Key::END: {
				_cursor_pos = _input_text.length();
				return true;
			}
			case Key::PAGEUP: {
				_scroll_offset += 10;
				_auto_scroll = false;
				return true;
			}
			case Key::PAGEDOWN: {
				_scroll_offset = MAX(_scroll_offset - 10, 0);
				if (_scroll_offset == 0) {
					_auto_scroll = true;
				}
				return true;
			}
			default: {
				// Character input.
				char32_t unicode = key_event->get_unicode();
				if (unicode >= 32) {
					_input_text = _input_text.left(_cursor_pos) + String::chr(unicode) + _input_text.substr(_cursor_pos);
					_cursor_pos++;
					_update_completions();
					_cursor_visible = true;
					_cursor_blink_timer = 0.0f;
				}
				return true;
			}
		}
	}

	// --- Mouse wheel for scrolling ---
	Ref<InputEventMouseButton> mouse_button = p_event;
	if (mouse_button.is_valid() && mouse_button->is_pressed()) {
		if (mouse_button->get_button_index() == MouseButton::WHEEL_UP) {
			_scroll_offset += 3;
			_auto_scroll = false;
			return true;
		} else if (mouse_button->get_button_index() == MouseButton::WHEEL_DOWN) {
			_scroll_offset = MAX(_scroll_offset - 3, 0);
			if (_scroll_offset == 0) {
				_auto_scroll = true;
			}
			return true;
		}
	}

	// Consume all input when console is open (prevent game from receiving it).
	return true;
}

// =============================================================================
// Console state management
// =============================================================================

void DebugConsole::set_open(bool p_open) {
	if (_open == p_open) {
		return;
	}
	_open = p_open;
	if (_open) {
		_cursor_visible = true;
		_cursor_blink_timer = 0.0f;
		_completion_visible = false;
		_reset_popup_counts();
	}
}

void DebugConsole::toggle() {
	set_open(!_open);
}

// =============================================================================
// Mobile UI action methods
// =============================================================================

Vector<String> DebugConsole::get_recent_history(int p_max) const {
	Vector<String> result;
	int count = MIN(p_max, _history.size());
	for (int i = 0; i < count; i++) {
		// Most recent first.
		result.push_back(_history[_history.size() - 1 - i]);
	}
	return result;
}

void DebugConsole::select_completion(int p_index) {
	if (p_index >= 0 && p_index < _current_completions.size()) {
		_input_text = _current_completions[p_index]->name + " ";
		_cursor_pos = _input_text.length();
		_completion_visible = false;
		_update_completions();
	}
}

void DebugConsole::execute_quick_command(int p_index) {
	if (p_index < 0 || p_index >= DebugConsoleRenderer::QUICK_COMMAND_COUNT) {
		return;
	}

	String cmd = DebugConsoleRenderer::quick_commands[p_index].command;
	_push_output("> " + cmd, Color(0.8, 0.8, 0.8));
	String result = _execute_command_string(cmd);
	if (!result.is_empty()) {
		_push_output(result, Color(0.4, 1.0, 0.4));
	}

	// Add to history (avoid duplicating the most recent entry).
	if (_history.is_empty() || _history[_history.size() - 1] != cmd) {
		if (_history.size() >= HISTORY_CAPACITY) {
			_history.remove_at(0);
		}
		_history.push_back(cmd);
	}
	_history_pos = -1;
}

void DebugConsole::execute_history_pill(int p_index) {
	Vector<String> recent = get_recent_history(DebugConsoleRenderer::MAX_HISTORY_PILLS);
	if (p_index < 0 || p_index >= recent.size()) {
		return;
	}

	String cmd = recent[p_index];
	_input_text = cmd;
	_cursor_pos = _input_text.length();
	_execute_input();
}

// =============================================================================
// Mini-badge popup
// =============================================================================

void DebugConsole::_reset_popup_counts() {
	_popup_new_info = 0;
	_popup_new_warning = 0;
	_popup_new_error = 0;
}

void DebugConsole::_snap_popup_to_edge(const Size2 &p_screen_size) {
	// Snap the popup to the nearest horizontal edge.
	float center_x = _popup_pos.x + 60.0f; // Half popup width.
	if (center_x < p_screen_size.x * 0.5f) {
		_popup_pos.x = 4.0f; // Snap left.
	} else {
		_popup_pos.x = p_screen_size.x - 124.0f; // Snap right.
	}
	// Clamp vertical.
	_popup_pos.y = CLAMP(_popup_pos.y, 4.0f, p_screen_size.y - 36.0f);
}

void DebugConsole::popup_tapped() {
	set_open(true);
}

void DebugConsole::popup_drag(const Vector2 &p_pos) {
	_popup_dragging = true;
	_popup_pos = p_pos - _popup_drag_offset;
}

void DebugConsole::popup_end_drag() {
	if (_popup_dragging) {
		_popup_dragging = false;
		Window *root = SceneTree::get_singleton()->get_root();
		if (root) {
			_snap_popup_to_edge(root->get_size());
		}
	}
}

// =============================================================================
// Log type filters
// =============================================================================

void DebugConsole::toggle_filter_info() {
	_filter_info = !_filter_info;
}

void DebugConsole::toggle_filter_warning() {
	_filter_warning = !_filter_warning;
}

void DebugConsole::toggle_filter_error() {
	_filter_error = !_filter_error;
}

bool DebugConsole::passes_filter(const OutputEntry &p_entry) const {
	// Yellow-ish = warning, Red-ish = error. Everything else = info/normal.
	// We use the color channels to classify. This avoids needing a separate type field.
	if (p_entry.color.r > 0.8 && p_entry.color.g < 0.5) {
		return _filter_error; // Red.
	}
	if (p_entry.color.r > 0.8 && p_entry.color.g > 0.7 && p_entry.color.b < 0.5) {
		return _filter_warning; // Yellow.
	}
	return _filter_info; // Everything else.
}

// =============================================================================
// Scroll snap
// =============================================================================

void DebugConsole::snap_to_bottom() {
	_scroll_offset = 0;
	_auto_scroll = true;
}

// =============================================================================
// Output management
// =============================================================================

void DebugConsole::_push_output(const String &p_text, const Color &p_color) {
	OutputEntry &entry = _output.write[_output_write_pos];
	entry.text = p_text;
	entry.color = p_color;
	entry.timestamp_msec = Time::get_singleton()->get_ticks_msec();

	_output_write_pos = (_output_write_pos + 1) % OUTPUT_CAPACITY;
	if (_output_count < OUTPUT_CAPACITY) {
		_output_count++;
	}

	if (_auto_scroll) {
		_scroll_offset = 0;
	}
}

const DebugConsole::OutputEntry &DebugConsole::get_output_entry(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, _output_count, _output.get(0));
	int start = (_output_count < OUTPUT_CAPACITY) ? 0 : _output_write_pos;
	int actual = (start + p_index) % OUTPUT_CAPACITY;
	return _output.get(actual);
}

// =============================================================================
// Autocomplete
// =============================================================================

void DebugConsole::_update_completions() {
	_autocomplete.rebuild_if_dirty();

	if (_input_text.is_empty()) {
		_current_completions.clear();
		_completion_visible = false;
		return;
	}

	// Check if we're typing arguments (space in input).
	int space_pos = _input_text.find(" ");
	if (space_pos != -1) {
		String cmd = _input_text.substr(0, space_pos);
		String partial_arg = _input_text.substr(space_pos + 1);
		PackedStringArray arg_completions = _autocomplete.get_argument_completions(cmd, partial_arg);
		_current_completions.clear();
		// We can't return CompletionEntry* for dynamic arg completions easily,
		// so we just hide the popup for now during arg typing.
		_completion_visible = false;
		return;
	}

	_current_completions = _autocomplete.get_completions(_input_text, 8);
	_completion_selected = 0;
	_completion_visible = !_current_completions.is_empty();
}

// =============================================================================
// Watch system
// =============================================================================

void DebugConsole::_update_watches() {
	// TODO: Reimplement watches with DebugIntrospector path expressions.
}

// =============================================================================
// Discovery pills — live browsing of registered semantic items
// =============================================================================

void DebugConsole::_update_discovery_items() {
	_discovery_items.clear();
	_discovery_visible = false;
	// TODO: Reimplement discovery with DebugIntrospector tree browsing.
}

void DebugConsole::tap_discovery_pill(int p_index) {
	if (p_index < 0 || p_index >= _discovery_items.size()) {
		return;
	}

	const DiscoveryItem &item = _discovery_items[p_index];

	if (item.type == DISCOVERY_QUERY) {
		// For queries, execute immediately and show the result.
		String cmd = "query." + item.name;
		_push_output("> " + cmd, Color(0.8, 0.8, 0.8));
		String result = _execute_command_string(cmd);
		if (!result.is_empty()) {
			_push_output(result, Color(0.4, 1.0, 0.4));
		}
	} else {
		// For actions/cvars/commands, populate the input field so user can
		// add parameters before pressing Enter.
		_input_text = item.command;
		_cursor_pos = _input_text.length();
		_completion_visible = false;
		_update_completions();
	}
}

void DebugConsole::tap_discovery_tab(int p_index) {
	if (p_index < 0 || p_index >= DISCOVERY_TAB_COUNT) {
		return;
	}
	_discovery_active_tab = p_index;
	_update_discovery_items();
}

void DebugConsole::tap_transport(int p_index) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree) {
		return;
	}

	bool suspended = tree->is_suspended();

	switch (p_index) {
		case 0: {
			// Toggle pause/resume.
			if (suspended) {
				tree->set_suspend(false);
				_push_output("Game resumed.", Color(0.4, 1.0, 0.4));
			} else {
				tree->set_suspend(true);
				_push_output("Game suspended.", Color(1.0, 0.9, 0.3));
			}
		} break;
		case 1: {
			// Step 1 frame.
			if (!suspended) {
				tree->set_suspend(true);
			}
			_step_frames_remaining = 1;
			_step_one_frame();
			_push_output("Stepping 1 frame.", Color(0.7, 0.9, 1.0));
		} break;
		case 2: {
			// Step 10 frames.
			if (!suspended) {
				tree->set_suspend(true);
			}
			_step_frames_remaining = 10;
			_step_one_frame();
			_push_output("Stepping 10 frames.", Color(0.7, 0.9, 1.0));
		} break;
	}
}

void DebugConsole::tap_timescale() {
	static const double presets[] = { 0.25, 0.5, 1.0, 2.0, 4.0 };
	static const int preset_count = 5;

	double current = Engine::get_singleton()->get_time_scale();

	// Find the next preset above current (with tolerance).
	int next_idx = 0;
	for (int i = 0; i < preset_count; i++) {
		if (current < presets[i] - 0.01) {
			next_idx = i;
			break;
		}
		if (i == preset_count - 1) {
			next_idx = 0; // Wrap around.
		}
	}

	Engine::get_singleton()->set_time_scale(presets[next_idx]);
	_push_output(vformat("Time scale: x%s", String::num(presets[next_idx], 2)), Color(0.7, 0.9, 1.0));
}

void DebugConsole::toggle_watches_overlay() {
	_watches_overlay_visible = !_watches_overlay_visible;
	if (_watches_overlay_visible) {
		_push_output("Watch overlay enabled.", Color(0.7, 0.9, 1.0));
	} else {
		_push_output("Watch overlay disabled.", Color(0.7, 0.9, 1.0));
	}
}

// =============================================================================
// Command tokenizer
// =============================================================================

PackedStringArray DebugConsole::_tokenize(const String &p_input) const {
	PackedStringArray tokens;
	String current;
	bool in_quotes = false;
	char32_t quote_char = 0;

	for (int i = 0; i < p_input.length(); i++) {
		char32_t c = p_input[i];

		if (in_quotes) {
			if (c == quote_char) {
				in_quotes = false;
				tokens.push_back(current);
				current = "";
			} else {
				current += String::chr(c);
			}
		} else if (c == '"' || c == '\'') {
			in_quotes = true;
			quote_char = c;
			if (!current.is_empty()) {
				tokens.push_back(current);
				current = "";
			}
		} else if (c == ' ' || c == '\t') {
			if (!current.is_empty()) {
				tokens.push_back(current);
				current = "";
			}
		} else {
			current += String::chr(c);
		}
	}

	if (!current.is_empty()) {
		tokens.push_back(current);
	}

	return tokens;
}

// =============================================================================
// Command execution
// =============================================================================

void DebugConsole::_execute_input() {
	String input = _input_text.strip_edges();
	if (input.is_empty()) {
		return;
	}

	// Add to history.
	if (_history.is_empty() || _history[_history.size() - 1] != input) {
		if (_history.size() >= HISTORY_CAPACITY) {
			_history.remove_at(0);
		}
		_history.push_back(input);
	}
	_history_pos = -1;

	// Echo the command.
	_push_output("> " + input, Color(0.8, 0.8, 0.8));

	// Execute.
	String result = _execute_command_string(input);
	if (!result.is_empty()) {
		_push_output(result, Color(0.4, 1.0, 0.4));
	}

	// Clear input.
	_input_text = "";
	_cursor_pos = 0;
	_completion_visible = false;
	_current_completions.clear();
}

String DebugConsole::_execute_command_string(const String &p_input) {
	PackedStringArray tokens = _tokenize(p_input);
	if (tokens.is_empty()) {
		return "";
	}

	String cmd = tokens[0];
	PackedStringArray args;
	for (int i = 1; i < tokens.size(); i++) {
		args.push_back(tokens[i]);
	}

	// --- Built-in commands ---
	if (cmd == "help") {
		return _cmd_help(args);
	}
	if (cmd == "clear") {
		return _cmd_clear(args);
	}
	if (cmd == "list") {
		return _cmd_list(args);
	}
	if (cmd == "watch") {
		return _cmd_watch(args);
	}
	if (cmd == "unwatch") {
		return _cmd_unwatch(args);
	}
	if (cmd == "pause") {
		return _cmd_pause(args);
	}
	if (cmd == "resume") {
		return _cmd_resume(args);
	}
	if (cmd == "step") {
		return _cmd_step(args);
	}
	if (cmd == "step!") {
		return _cmd_step_instant(args);
	}
	if (cmd == "timescale") {
		return _cmd_timescale(args);
	}
	if (cmd == "echo") {
		return _cmd_echo(args);
	}
	if (cmd == "exec") {
		return _cmd_exec(args);
	}
	if (cmd == "screenshot") {
		return _cmd_screenshot(args);
	}
	if (cmd == "node") {
		return _cmd_node(args);
	}
	if (cmd == "cd") {
		return _cmd_cd(args);
	}
	if (cmd == "ls") {
		return _cmd_ls(args);
	}
	if (cmd == "pwd") {
		return _cmd_pwd(args);
	}
	if (cmd == "ui") {
		return _cmd_ui(args);
	}

	// --- Implicit node shorthand ---
	// If the first token looks like a node target (contains '/', '.', ':' or
	// starts with '@'), treat the entire input as an implicit "node" command.
	// This lets users type "Player.health 100" instead of "node Player.health 100"
	// when they've cd'd into the parent.
	if (cmd.contains("/") || cmd.contains(":") || cmd.begins_with("@") || cmd.begins_with(".")) {
		// Rebuild args: the "cmd" is actually the first arg to node.
		PackedStringArray node_args;
		node_args.push_back(cmd);
		for (const String &a : args) {
			node_args.push_back(a);
		}
		return _cmd_node(node_args);
	}

	// --- Last resort: try as a bare child name in cwd ---
	// If the user types "Player" or "Player.health" without any '/' or '@',
	// and cwd has a child with that name, treat it as an implicit node command.
	{
		// Extract the node name part (before any '.' or ':').
		String child_name = cmd;
		int prop_sep = cmd.find(".");
		int meth_sep = cmd.find(":");
		if (prop_sep != -1) {
			child_name = cmd.substr(0, prop_sep);
		} else if (meth_sep != -1) {
			child_name = cmd.substr(0, meth_sep);
		}

		SceneTree *tree = SceneTree::get_singleton();
		if (tree && tree->get_root()) {
			Node *cwd_node = tree->get_root()->get_node_or_null(NodePath(_cwd));
			if (cwd_node) {
				Node *child = cwd_node->get_node_or_null(NodePath(child_name));
				if (child) {
					// Found a child — treat the entire input as a node command.
					PackedStringArray node_args;
					node_args.push_back(cmd);
					for (const String &a : args) {
						node_args.push_back(a);
					}
					return _cmd_node(node_args);
				}
			}
		}
	}

	// --- Legacy semantic registry dispatch removed ---
	// TODO: Add $Path.property and $Path.method() dispatch via DebugIntrospector.
	return vformat("Unknown command: '%s'. Type 'help' for a list of commands.", cmd);
}

// =============================================================================
// Built-in command implementations
// =============================================================================

String DebugConsole::_cmd_help(const PackedStringArray &p_args) {
	if (!p_args.is_empty()) {
		String name = p_args[0];

		// =====================================================================
		// Navigation commands
		// =====================================================================
		if (name == "cd") {
			return "cd [path]  — Change working directory in scene tree.\n"
				   "  cd                    — Go to /root\n"
				   "  cd ..                 — Go up one level\n"
				   "  cd Level/Enemies      — Relative path from cwd\n"
				   "  cd /root/Level        — Absolute path\n"
				   "Once inside a node, you can use child names directly:\n"
				   "  Player.health         — Read property on child 'Player'\n"
				   "  Player.health 100     — Write property\n"
				   "  Player:take_damage 50 — Call method on child 'Player'";
		}
		if (name == "ls") {
			return "ls [path]  — List children of current (or specified) node.\n"
				   "Shows each child's name, class, and grandchild count.\n"
				   "  ls              — List children of cwd\n"
				   "  ls ..           — List parent's children\n"
				   "  ls Player       — List children of 'Player'\n"
				   "  ls /root/Level  — Absolute path";
		}
		if (name == "pwd") {
			return "pwd  — Print the current working directory in the scene tree.\n"
				   "The prompt also shows this: ~/Level/Enemies>";
		}

		// =====================================================================
		// Node command
		// =====================================================================
		if (name == "node") {
			return "node <target>[.property] [value]  — Direct node I/O on the scene tree.\n"
				   "\n"
				   "Inspect:\n"
				   "  node Player              — Show class, properties, children (relative to cwd)\n"
				   "  node /root/Level/Player  — Absolute path\n"
				   "  node Player/             — List children (trailing slash)\n"
				   "\n"
				   "Properties:\n"
				   "  node Player.health       — Read property\n"
				   "  node Player.health 100   — Write (auto-detects type: bool, int, float,\n"
				   "                             Vector2 'x,y', Vector3 'x,y,z', Color '#hex')\n"
				   "\n"
				   "Methods:\n"
				   "  node Player:take_damage 50   — Call method with args\n"
				   "  node Player:queue_free       — No-arg method call\n"
				   "\n"
				   "Groups:\n"
				   "  node @enemies                — List all nodes in group\n"
				   "  node @enemies.health 999     — Set property on all in group\n"
				   "  node @enemies:queue_free     — Call method on all in group\n"
				   "\n"
				   "Shorthand: you can omit 'node' when input contains / . : or @\n"
				   "  Player.health       — works if cwd has child 'Player'\n"
				   "  ../Boss:die         — relative path\n"
				   "  @enemies.health 999 — group targeting";
		}

		// =====================================================================
		// UI command
		// =====================================================================
		if (name == "ui") {
			return "ui <path> [action] [value]  — Semantic UI control interaction and page navigation.\n"
				   "\n"
				   "Control scanning:\n"
				   "  ui                      — List all interactive controls in cwd\n"
				   "  ui /root/UI             — List controls in subtree\n"
				   "  ui buttons              — Filter: only buttons (also: sliders, toggles, inputs, tabs, menus)\n"
				   "\n"
				   "Control interaction:\n"
				   "  ui PlayBtn              — Describe control (type, state, available actions)\n"
				   "  ui PlayBtn press        — Press a button\n"
				   "  ui GodMode toggle       — Toggle a checkbox (also: on, off)\n"
				   "  ui Volume 0.8           — Set slider/range value\n"
				   "  ui Difficulty 2         — Select option by index\n"
				   "  ui Difficulty select 2  — Same, explicit form\n"
				   "  ui Name text Hello      — Set text on LineEdit/TextEdit\n"
				   "  ui Name clear           — Clear text field\n"
				   "  ui Tabs tab 2           — Switch to tab index (also: next, prev)\n"
				   "  ui Menu select 3        — Activate menu item by index\n"
				   "\n"
				   "Page navigation:\n"
				   "  ui pages                — Show registered UI page hierarchy with [ACTIVE] markers\n"
				   "  ui where                — Show current active page, breadcrumb, and navigation options\n"
				   "  ui go <page>            — Navigate to a named page (hides siblings, shows target)\n"
				   "  ui detect               — Auto-detect page-like structures (TabContainers, page stacks)\n"
				   "\n"
				   "All paths resolve relative to cwd. Emits correct signals (pressed, item_selected, etc).";
		}

		// =====================================================================
		// Debug and inspection commands
		// =====================================================================
		if (name == "help") {
			return "help [command]  — Show help.\n"
				   "  help           — Full reference of all commands and concepts\n"
				   "  help cd        — Help for a specific built-in command\n"
				   "  help my_cvar   — Show CVar value, default, and description\n"
				   "  help action.x  — Show action params and description\n"
				   "  help query.x   — Show query description";
		}
		if (name == "clear") {
			return "clear  — Clear all console output.\n"
				   "Clears the scrollback buffer. History and watches are preserved.";
		}
		if (name == "list") {
			return "list [category]  — List all registered debug items.\n"
				   "  list             — Same as 'list all'\n"
				   "  list all         — Everything\n"
				   "  list cvars       — Configuration variables (name = value)\n"
				   "  list commands    — Registered console commands\n"
				   "  list actions     — Named operations (invokable with params)\n"
				   "  list queries     — Live-readable values\n"
				   "  list events      — Registered signal monitors\n"
				   "  list interactables — Semantic game objects\n"
				   "  list pages       — UI page navigation graph";
		}
		if (name == "watch") {
			return "watch <query.name>  — Pin a query to the on-screen watch overlay.\n"
				   "The value updates every frame, visible even when console is closed.\n"
				   "  watch query.player.health  — Pin player health\n"
				   "  watch query.fps            — Pin framerate\n"
				   "  watch player.health        — 'query.' prefix is optional";
		}
		if (name == "unwatch") {
			return "unwatch [query.name]  — Remove a query from the watch overlay.\n"
				   "  unwatch query.player.health  — Remove specific watch\n"
				   "  unwatch player.health        — 'query.' prefix is optional\n"
				   "  unwatch                      — Remove ALL watches";
		}

		// =====================================================================
		// Time control commands
		// =====================================================================
		if (name == "pause") {
			return "pause  — Suspend the game.\n"
				   "Freezes SceneTree processing. The console stays interactive.\n"
				   "Use 'resume' to continue, or 'step' to advance frame-by-frame.";
		}
		if (name == "resume") {
			return "resume  — Resume the game after pause or step.\n"
				   "Restores normal SceneTree processing.";
		}
		if (name == "step") {
			return "step [N]  — Advance N frames then re-suspend.\n"
				   "  step       — Advance 1 frame\n"
				   "  step 10    — Advance 10 frames (renders each one)\n"
				   "The game re-suspends automatically after the last frame.\n"
				   "Use 'resume' to cancel stepping and run freely.";
		}
		if (name == "step!") {
			return "step! [N]  — Advance N frames instantly (no rendering).\n"
				   "Like 'step' but skips rendering for speed.\n"
				   "  step! 100  — Fast-forward 100 frames\n"
				   "NOTE: Requires the debug-time-control branch. Currently stubbed.";
		}
		if (name == "timescale") {
			return "timescale [value]  — Get or set the Engine time scale.\n"
				   "  timescale        — Show current time scale\n"
				   "  timescale 0.5    — Half speed (slow motion)\n"
				   "  timescale 2.0    — Double speed\n"
				   "  timescale 0      — Effectively pauses (but use 'pause' instead)\n"
				   "Range: 0.0 to 100.0.";
		}

		// =====================================================================
		// Utility commands
		// =====================================================================
		if (name == "echo") {
			return "echo <text>  — Print text to the console output.\n"
				   "  echo Hello world     — Prints 'Hello world'\n"
				   "Useful in 'exec' scripts for logging.";
		}
		if (name == "exec") {
			return "exec <file_path>  — Execute commands from a text file.\n"
				   "Reads each line as a console command. Skips empty lines and comments.\n"
				   "  exec user://startup.txt    — Run commands from file\n"
				   "  exec res://debug/init.cfg  — Also works with res:// paths\n"
				   "Comment syntax: lines starting with // or # are skipped.";
		}
		if (name == "screenshot") {
			return "screenshot [path]  — Capture a screenshot.\n"
				   "  screenshot                        — Auto-names: user://screenshot_YYYYMMDD_HHMMSS.png\n"
				   "  screenshot user://my_shot.png     — Custom path";
		}

		// =====================================================================
		// Concept help topics
		// =====================================================================
		if (name == "cvars") {
			return "CVars — Configuration Variables\n"
				   "Typed, range-clamped tuning knobs registered from GDScript.\n"
				   "\n"
				   "Console usage:\n"
				   "  player.speed             — Read value\n"
				   "  player.speed 500         — Write value (auto-clamps to min/max)\n"
				   "  list cvars               — List all CVars with current values\n"
				   "  help player.speed        — Show details for a specific CVar\n"
				   "\n"
				   "GDScript registration:\n"
				   "  Debug.register_cvar(\"player.speed\", 300.0, \"Walk speed\", {\"min\": 50, \"max\": 1000})\n"
				   "\n"
				   "Typed read (safe in release builds — returns the default):\n"
				   "  var spd = Debug.cvar_float(\"player.speed\", 300.0)\n"
				   "\n"
				   "Flags: CVAR_ARCHIVE (save), CVAR_READONLY, CVAR_CHEAT, CVAR_HIDDEN";
		}
		if (name == "actions") {
			return "Actions — Named Operations with Parameters\n"
				   "Functions callable from console or MCP with key=value parameters.\n"
				   "\n"
				   "Console usage:\n"
				   "  action.heal_player amount=50  — Invoke with params\n"
				   "  list actions                  — List all actions\n"
				   "\n"
				   "GDScript registration:\n"
				   "  Debug.register_action(\"heal_player\", _heal, \"Heal\", {\"amount\": \"int\"})";
		}
		if (name == "queries") {
			return "Queries — Live-Readable Values\n"
				   "Zero-arg callables that return a value, callable each frame when watched.\n"
				   "\n"
				   "Console usage:\n"
				   "  query.player.health          — Read once\n"
				   "  watch query.player.health    — Pin to on-screen overlay\n"
				   "  unwatch player.health        — Remove from overlay\n"
				   "  list queries                 — List all queries with current values\n"
				   "\n"
				   "GDScript registration:\n"
				   "  Debug.register_query(\"player.health\", func(): return player.health, \"HP\")";
		}
		if (name == "events") {
			return "Events — Signal Monitoring\n"
				   "Connects to Godot signals and logs when they fire.\n"
				   "\n"
				   "Console usage:\n"
				   "  list events                  — Show all registered events\n"
				   "Events auto-log to the console output when they fire.\n"
				   "\n"
				   "GDScript registration:\n"
				   "  Debug.register_event(\"player_died\", player.died, \"Player death\")";
		}
		if (name == "interactables") {
			return "Interactables — Semantic Game Object Hints\n"
				   "Optional annotations that help MCP agents discover game objects.\n"
				   "\n"
				   "Console usage:\n"
				   "  list interactables           — Show all registered interactables\n"
				   "\n"
				   "GDScript registration:\n"
				   "  Debug.register_interactable(\"boss\", $Boss, \"world_3d\", \"Level boss\",\n"
				   "      [\"attack\", \"kill\"], \"enemies\")\n"
				   "\n"
				   "Types: ui, world_2d, world_3d, logic\n"
				   "Not required for the 'node' command — that works on any node via path.";
		}
		if (name == "pages") {
			return "UI Pages — Navigation Graph\n"
				   "Annotate your game's screen/page structure for console and MCP navigation.\n"
				   "\n"
				   "Console usage:\n"
				   "  ui pages                — Show page hierarchy with [ACTIVE] markers\n"
				   "  ui where                — Current location, breadcrumb, and navigation options\n"
				   "  ui go settings           — Navigate to a named page\n"
				   "  ui detect               — Auto-detect page structures (TabContainers, page stacks)\n"
				   "  list pages              — List all registered pages\n"
				   "\n"
				   "GDScript registration:\n"
				   "  Debug.register_ui_page(\"settings\", $Settings, \"Settings page\", {\n"
				   "      \"parent\": \"main_menu\",\n"
				   "      \"children\": [\"settings.audio\", \"settings.video\"],\n"
				   "      \"back\": \"BackBtn\",\n"
				   "  })\n"
				   "\n"
				   "Options: parent, children, back, enter_actions, metadata\n"
				   "Auto-cleanup when the Control exits the scene tree.";
		}
		if (name == "auto_expose" || name == "autoexpose") {
			return "auto_expose — One-Line Bulk Registration\n"
				   "Scans a Node for @export properties and debug_*() methods.\n"
				   "\n"
				   "GDScript:\n"
				   "  Debug.auto_expose(self)               — Tag defaults to class_name\n"
				   "  Debug.auto_expose(self, \"my_tag\")      — Custom tag prefix\n"
				   "\n"
				   "@export properties become live-bound CVars (read/write through).\n"
				   "debug_xyz() methods become commands named 'Tag.xyz'.\n"
				   "Auto-cleanup when the Node exits the tree.\n"
				   "\n"
				   "For multiple instances, use unique tags:\n"
				   "  Debug.auto_expose(self, \"enemy_%d\" % get_index())";
		}
		if (name == "groups") {
			return "Group Targeting — @ Prefix\n"
				   "Address all nodes in a SceneTree group at once.\n"
				   "\n"
				   "  @enemies                — List all nodes in group 'enemies'\n"
				   "  @enemies.health         — Read 'health' on every node in group\n"
				   "  @enemies.health 999     — Set 'health' on all\n"
				   "  @enemies:queue_free     — Call method on all\n"
				   "\n"
				   "Groups are defined in the editor or via add_to_group() in GDScript.";
		}
		if (name == "manifest") {
			return "Manifest — Full Debug Introspection\n"
				   "Returns a Dictionary with everything registered in the debug system.\n"
				   "\n"
				   "GDScript:\n"
				   "  var m = Debug.get_manifest()\n"
				   "  # Keys: actions, queries, events, interactables, cvars, commands,\n"
				   "  #        ui_pages, active_ui_page\n"
				   "\n"
				   "Used by the MCP server to discover all debug capabilities.\n"
				   "Returns {} in release builds.";
		}

		return vformat("Unknown help topic: '%s'. Type 'help' for the full reference.", name);
	}

	// =========================================================================
	// General help — comprehensive reference
	// =========================================================================
	String h;
	h += "=== Godot Debug Console ===\n";
	h += "Toggle: backtick `  |  Mobile: three-finger swipe down  |  Close: Esc\n";
	h += "\n";
	h += "--- Scene Tree Navigation ---\n";
	h += "  cd [path]           Change directory (cd .. | cd Level/Enemies | cd /root/Level)\n";
	h += "  ls [path]           List children of current or specified node\n";
	h += "  pwd                 Print current working directory\n";
	h += "  The prompt shows your location: ~/Level/Enemies>\n";
	h += "\n";
	h += "--- Node I/O ---\n";
	h += "  node Player              Inspect node (class, properties, children)\n";
	h += "  Player.health            Read property (bare child name — no 'node' prefix needed)\n";
	h += "  Player.health 100        Write property (auto-detects type)\n";
	h += "  Player:take_damage 50    Call method with args\n";
	h += "  @enemies.health 999      Group targeting: set on all nodes in group\n";
	h += "  @enemies:queue_free      Group targeting: call on all\n";
	h += "\n";
	h += "--- UI Interaction ---\n";
	h += "  ui                       Scan cwd for interactive controls\n";
	h += "  ui PlayBtn press         Press button  |  ui Toggle toggle  |  ui Slider 0.5\n";
	h += "  ui Name text Hello       Set text  |  ui Tabs tab 2  |  ui Menu select 0\n";
	h += "  ui pages                 Show UI page navigation tree\n";
	h += "  ui where                 Current page + breadcrumb + navigation options\n";
	h += "  ui go <page>             Navigate to a named UI page\n";
	h += "  ui detect                Auto-detect page structures (no registration needed)\n";
	h += "\n";
	h += "--- CVars (type name to read, 'name value' to write) ---\n";
	h += "  player.speed             Read CVar\n";
	h += "  player.speed 500         Write CVar (auto-clamps to registered min/max)\n";
	h += "\n";
	h += "--- Queries & Actions ---\n";
	h += "  query.player.health      Read a query value\n";
	h += "  watch query.player.health   Pin to on-screen overlay (updates every frame)\n";
	h += "  unwatch [query.name]     Remove from overlay (no args = clear all)\n";
	h += "  action.heal amount=50    Invoke a registered action with key=value params\n";
	h += "\n";
	h += "--- Time Control ---\n";
	h += "  pause                    Suspend the game (console stays interactive)\n";
	h += "  resume                   Resume normal execution\n";
	h += "  step [N]                 Advance N frames then re-suspend (default: 1)\n";
	h += "  timescale [val]          Get/set time scale (0.0-100.0, e.g., 0.5 = slow-mo)\n";
	h += "\n";
	h += "--- Utility ---\n";
	h += "  list [category]          List registered items (cvars, commands, actions, queries,\n";
	h += "                           events, interactables, pages, all)\n";
	h += "  help [command]           Detailed help (try: help node, help ui, help cvars)\n";
	h += "  clear                    Clear console output\n";
	h += "  echo <text>              Print text to console\n";
	h += "  exec <file>              Run commands from a text file\n";
	h += "  screenshot [path]        Capture screenshot to user://\n";
	h += "\n";
	h += "--- GDScript API (Debug singleton) ---\n";
	h += "  Debug.register_cvar(name, default, desc, {min, max, flags})\n";
	h += "  Debug.cvar_float(name, default)  — typed read, safe in release builds\n";
	h += "  Debug.register_command(name, callable, desc)  — console command\n";
	h += "  Debug.register_query(name, callable, desc)    — watchable value\n";
	h += "  Debug.register_action(name, callable, desc, params)  — parameterized op\n";
	h += "  Debug.register_event(name, signal, desc)      — signal monitor\n";
	h += "  Debug.register_ui_page(name, node, desc, {parent, children, back})\n";
	h += "  Debug.auto_expose(self)  — bulk-register @exports as CVars, debug_* as commands\n";
	h += "  Debug.log(msg)  |  Debug.log_warning(msg)  |  Debug.log_error(msg)\n";
	h += "  Debug.get_manifest()     — full introspection dict for MCP\n";
	h += "\n";
	h += "All Debug.* calls are no-ops in release builds. No #ifdef needed in GDScript.\n";
	h += "Type 'help <topic>' for details: cd ls pwd node ui help clear list watch unwatch\n";
	h += "  pause resume step timescale echo exec screenshot cvars actions queries events\n";
	h += "  interactables pages auto_expose groups manifest";
	return h;
}

String DebugConsole::_cmd_clear(const PackedStringArray &p_args) {
	_output_count = 0;
	_output_write_pos = 0;
	_scroll_offset = 0;
	_auto_scroll = true;
	return "";
}

String DebugConsole::_cmd_list(const PackedStringArray &p_args) {
	// TODO: Reimplement with DebugIntrospector.
	return "List command not yet reimplemented. Use 'ls' to browse the scene tree.";
}

String DebugConsole::_cmd_watch(const PackedStringArray &p_args) {
	if (p_args.is_empty()) {
		return "Usage: watch <query.name>";
	}

	// TODO: Watches are being reimplemented with DebugIntrospector.
	return "Watch command is temporarily disabled during reimplementation.";
}

String DebugConsole::_cmd_unwatch(const PackedStringArray &p_args) {
	if (p_args.is_empty()) {
		// Remove all watches.
		_watches.clear();
		return "All watches cleared.";
	}

	String name = p_args[0];
	String query_name = name.begins_with("query.") ? name.substr(6) : name;

	for (int i = 0; i < _watches.size(); i++) {
		if (_watches[i].query_name == query_name) {
			_watches.remove_at(i);
			return vformat("Unwatched query.%s", query_name);
		}
	}

	return vformat("Not watching '%s'.", query_name);
}

String DebugConsole::_cmd_pause(const PackedStringArray &p_args) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree) {
		return "SceneTree not available.";
	}
	tree->set_suspend(true);
	return "Game suspended.";
}

String DebugConsole::_cmd_resume(const PackedStringArray &p_args) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree) {
		return "SceneTree not available.";
	}
	tree->set_suspend(false);
	return "Game resumed.";
}

String DebugConsole::_cmd_step(const PackedStringArray &p_args) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree) {
		return "SceneTree not available.";
	}

	int count = 1;
	if (!p_args.is_empty()) {
		count = MAX(p_args[0].to_int(), 1);
	}

	// Begin frame stepping. We unsuspend for one frame, then poll()
	// re-suspends and chains subsequent frames until count is exhausted.
	// TODO: When merging with feature/debug-time-control, use
	// SceneDebugger::_advance_n_frames_natural(count) instead.
	_step_frames_remaining = count;
	_step_one_frame();

	return vformat("Stepping %d frame(s).", count);
}

void DebugConsole::_step_one_frame() {
	if (_step_frames_remaining <= 0) {
		return;
	}

	SceneTree *tree = SceneTree::get_singleton();
	if (!tree) {
		_step_frames_remaining = 0;
		return;
	}

	// Unsuspend so the next Main::iteration() processes one frame.
	// poll() will detect _step_frames_remaining > 0, decrement it,
	// and re-suspend after the frame has been processed.
	tree->set_suspend(false);
}

String DebugConsole::_cmd_step_instant(const PackedStringArray &p_args) {
	// Instant stepping (process without rendering) requires direct integration
	// with the time control branch to avoid re-entering SceneTree::process()
	// from within Main::iteration(). This is unsafe without the branch's
	// _advance_n_frames_instant() which handles the state correctly.
	// TODO: When merging with feature/debug-time-control, use
	// SceneDebugger::_advance_n_frames_instant(count) instead.
	return "step! requires the debug-time-control branch. Use 'step' for natural frame stepping.";
}

String DebugConsole::_cmd_timescale(const PackedStringArray &p_args) {
	if (p_args.is_empty()) {
		return vformat("Time scale: %f", Engine::get_singleton()->get_time_scale());
	}

	double scale = p_args[0].to_float();
	scale = CLAMP(scale, 0.0, 100.0);
	Engine::get_singleton()->set_time_scale(scale);
	return vformat("Time scale set to %f", scale);
}

String DebugConsole::_cmd_echo(const PackedStringArray &p_args) {
	String text;
	for (const String &arg : p_args) {
		if (!text.is_empty()) {
			text += " ";
		}
		text += arg;
	}
	return text;
}

String DebugConsole::_cmd_screenshot(const PackedStringArray &p_args) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		return "Cannot capture screenshot: no root window.";
	}

	Ref<ViewportTexture> vp_texture = tree->get_root()->get_texture();
	if (vp_texture.is_null()) {
		return "Failed to capture screenshot: no viewport texture.";
	}
	Ref<Image> image = vp_texture->get_image();
	if (image.is_null()) {
		return "Failed to capture screenshot.";
	}

	String path;
	if (!p_args.is_empty()) {
		path = p_args[0];
	} else {
		// Default path: user://screenshot_YYYYMMDD_HHMMSS.png
		Dictionary datetime = Time::get_singleton()->get_datetime_dict_from_system();
		path = vformat("user://screenshot_%04d%02d%02d_%02d%02d%02d.png",
				(int)datetime["year"], (int)datetime["month"], (int)datetime["day"],
				(int)datetime["hour"], (int)datetime["minute"], (int)datetime["second"]);
	}

	Error err = image->save_png(path);
	if (err != OK) {
		return vformat("Failed to save screenshot to '%s': error %d", path, (int)err);
	}

	return vformat("Screenshot saved: %s", path);
}

// =============================================================================
// Scene tree navigation — cd, ls, pwd
// =============================================================================

// Resolve a path relative to _cwd into an absolute path.
// Handles: absolute ("/root/..."), relative ("Player", "../Enemies"),
// ".." (parent), "." (current).
String DebugConsole::_resolve_path(const String &p_target) const {
	if (p_target.begins_with("/")) {
		// Already absolute.
		return p_target;
	}

	// Start from cwd and apply relative components.
	String base = _cwd;
	PackedStringArray parts = p_target.split("/", false);
	for (const String &part : parts) {
		if (part == "..") {
			int last_sep = base.rfind("/");
			if (last_sep > 0) {
				base = base.left(last_sep);
			}
			// Don't go above "/" — but "/root" is the practical minimum.
		} else if (part == ".") {
			// Stay in place.
		} else {
			base += "/" + part;
		}
	}
	return base;
}

String DebugConsole::_cmd_cd(const PackedStringArray &p_args) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		return "SceneTree not available.";
	}

	if (p_args.is_empty()) {
		// cd with no args → go to /root.
		_cwd = "/root";
		return _cwd;
	}

	String target = p_args[0];

	// Handle ".." specially for quick navigation.
	if (target == "..") {
		int last_sep = _cwd.rfind("/");
		if (last_sep > 0) {
			_cwd = _cwd.left(last_sep);
		}
		return _cwd;
	}

	String resolved = _resolve_path(target);

	// Validate the node exists.
	Node *node = tree->get_root()->get_node_or_null(NodePath(resolved));
	if (!node) {
		return vformat("Node not found: '%s'.", resolved);
	}

	_cwd = String(node->get_path());
	return _cwd;
}

String DebugConsole::_cmd_ls(const PackedStringArray &p_args) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		return "SceneTree not available.";
	}

	String target_path = _cwd;
	if (!p_args.is_empty()) {
		target_path = _resolve_path(p_args[0]);
	}

	Node *node = tree->get_root()->get_node_or_null(NodePath(target_path));
	if (!node) {
		return vformat("Node not found: '%s'.", target_path);
	}

	return _node_ls(node);
}

String DebugConsole::_cmd_pwd(const PackedStringArray &p_args) {
	return _cwd;
}

// =============================================================================
// Node command — scene tree bridge (Source-style entity I/O)
// =============================================================================

// Resolve a target string to a list of nodes.
// - "/root/Level/Player" → single node via absolute path
// - "@enemies" → all nodes in group "enemies"
// - "Player" or "../Enemies" → relative to _cwd
Vector<Node *> DebugConsole::_resolve_nodes(const String &p_target) const {
	Vector<Node *> results;
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		return results;
	}

	if (p_target.begins_with("@")) {
		// Group targeting.
		StringName group = p_target.substr(1);
		results = tree->get_nodes_in_group(group);
	} else {
		// Resolve relative to cwd.
		String resolved = _resolve_path(p_target);
		Node *node = tree->get_root()->get_node_or_null(NodePath(resolved));
		if (node) {
			results.push_back(node);
		}
	}
	return results;
}

// Inspect a single node — show class, key properties, children.
String DebugConsole::_node_inspect(Node *p_node) const {
	String result;
	result += vformat("[%s] %s\n", p_node->get_class(), p_node->get_path());

	// Show a summary of exported/important properties (skip internal ones).
	List<PropertyInfo> props;
	p_node->get_property_list(&props);

	int prop_count = 0;
	for (const PropertyInfo &pi : props) {
		// Only show storage/editor properties, skip internal/config ones.
		if (!(pi.usage & PROPERTY_USAGE_EDITOR)) {
			continue;
		}
		// Skip overly internal property names.
		if (pi.name.begins_with("_") || pi.name.begins_with("metadata/")) {
			continue;
		}

		bool valid = false;
		Variant val = p_node->get(pi.name, &valid);
		if (valid) {
			String val_str = val.stringify();
			// Truncate long values for display.
			if (val_str.length() > 80) {
				val_str = val_str.left(77) + "...";
			}
			result += vformat("  .%s = %s\n", pi.name, val_str);
			prop_count++;
			if (prop_count >= 30) {
				result += "  ... (truncated, use node <path>.property_name to read specific properties)\n";
				break;
			}
		}
	}

	// Show children summary.
	int child_count = p_node->get_child_count();
	if (child_count > 0) {
		result += vformat("  Children: %d", child_count);
		if (child_count <= 10) {
			result += " [";
			for (int i = 0; i < child_count; i++) {
				if (i > 0) {
					result += ", ";
				}
				result += p_node->get_child(i)->get_name();
			}
			result += "]";
		}
	}

	return result;
}

// List children of a node.
String DebugConsole::_node_ls(Node *p_node) const {
	int child_count = p_node->get_child_count();
	if (child_count == 0) {
		return vformat("%s has no children.", String(p_node->get_path()));
	}

	String result = vformat("%s (%d children):\n", String(p_node->get_path()), child_count);
	for (int i = 0; i < child_count; i++) {
		Node *child = p_node->get_child(i);
		int grandchild_count = child->get_child_count();
		String suffix = grandchild_count > 0 ? vformat("/ (%d)", grandchild_count) : "";
		result += vformat("  %s [%s] %s\n", child->get_name(), child->get_class(), suffix);
	}
	return result.strip_edges();
}

// Parse a Variant from a string, attempting to infer type from target property.
static Variant _parse_value_for_property(Object *p_obj, const StringName &p_property, const String &p_str) {
	// Try to match the existing property type.
	bool valid = false;
	Variant current = p_obj->get(p_property, &valid);
	if (valid) {
		switch (current.get_type()) {
			case Variant::BOOL:
				return (p_str == "true" || p_str == "1" || p_str == "yes");
			case Variant::INT:
				return p_str.to_int();
			case Variant::FLOAT:
				return p_str.to_float();
			case Variant::VECTOR2:
				// Accept "x,y" format.
				if (p_str.contains(",")) {
					PackedStringArray parts = p_str.split(",");
					if (parts.size() >= 2) {
						return Vector2(parts[0].strip_edges().to_float(), parts[1].strip_edges().to_float());
					}
				}
				break;
			case Variant::VECTOR3:
				if (p_str.contains(",")) {
					PackedStringArray parts = p_str.split(",");
					if (parts.size() >= 3) {
						return Vector3(parts[0].strip_edges().to_float(), parts[1].strip_edges().to_float(), parts[2].strip_edges().to_float());
					}
				}
				break;
			case Variant::COLOR:
				if (p_str.begins_with("#")) {
					return Color::html(p_str);
				} else if (p_str.contains(",")) {
					PackedStringArray parts = p_str.split(",");
					if (parts.size() >= 3) {
						float r = parts[0].strip_edges().to_float();
						float g = parts[1].strip_edges().to_float();
						float b = parts[2].strip_edges().to_float();
						float a = parts.size() >= 4 ? parts[3].strip_edges().to_float() : 1.0f;
						return Color(r, g, b, a);
					}
				}
				break;
			default:
				break;
		}
	}

	// Fallback: try common patterns.
	if (p_str == "true") {
		return true;
	}
	if (p_str == "false") {
		return false;
	}
	if (p_str == "null" || p_str == "nil") {
		return Variant();
	}
	if (p_str.is_valid_int()) {
		return p_str.to_int();
	}
	if (p_str.is_valid_float()) {
		return p_str.to_float();
	}
	return p_str; // String fallback.
}

String DebugConsole::_cmd_node(const PackedStringArray &p_args) {
	if (p_args.is_empty()) {
		return "Usage: node <path>[.property] [value]  or  node <path>:method [args...]\n"
			   "       node @group[.property] [value]  or  node @group:method [args...]\n"
			   "Type 'help node' for full syntax.";
	}

	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		return "SceneTree not available.";
	}

	String target = p_args[0];
	PackedStringArray extra_args;
	for (int i = 1; i < p_args.size(); i++) {
		extra_args.push_back(p_args[i]);
	}

	// ===================================================================
	// Case 1: Method call — target contains ':'
	// Syntax: node /root/Path:method arg1 arg2
	//         node @group:method arg1 arg2
	// ===================================================================
	int colon_pos = target.find(":");
	if (colon_pos != -1) {
		String node_part = target.substr(0, colon_pos);
		String method_name = target.substr(colon_pos + 1);

		if (method_name.is_empty()) {
			return "Error: empty method name after ':'";
		}

		Vector<Node *> nodes = _resolve_nodes(node_part);
		if (nodes.is_empty()) {
			return vformat("No nodes found for '%s'.", node_part);
		}

		// Build argument array. Parse each arg as a Variant.
		Array call_args;
		for (const String &arg_str : extra_args) {
			// Best-effort parsing.
			if (arg_str == "true") {
				call_args.append(true);
			} else if (arg_str == "false") {
				call_args.append(false);
			} else if (arg_str == "null" || arg_str == "nil") {
				call_args.append(Variant());
			} else if (arg_str.is_valid_int()) {
				call_args.append(arg_str.to_int());
			} else if (arg_str.is_valid_float()) {
				call_args.append(arg_str.to_float());
			} else {
				call_args.append(arg_str);
			}
		}

		String result;
		for (Node *node : nodes) {
			Variant ret = node->callv(StringName(method_name), call_args);
			if (nodes.size() > 1) {
				result += vformat("  %s → %s\n", String(node->get_path()), ret.stringify());
			} else {
				if (ret.get_type() != Variant::NIL) {
					result = vformat("%s → %s", method_name, ret.stringify());
				} else {
					result = vformat("Called %s() on %s", method_name, String(node->get_path()));
				}
			}
		}

		if (nodes.size() > 1) {
			result = vformat("Called %s() on %d nodes:\n%s", method_name, nodes.size(), result);
		}

		return result.strip_edges();
	}

	// ===================================================================
	// Case 2: Property access — target contains '.'
	// Syntax: node /root/Path.property          (read)
	//         node /root/Path.property value     (write)
	//         node @group.property               (read all)
	//         node @group.property value          (write all)
	// ===================================================================
	// Find the FIRST '.' that separates path from property.
	// For node paths, '/' separates path segments, so we look for '.'
	// after the last '/'.
	int dot_pos = -1;
	int last_slash = target.rfind("/");
	if (last_slash != -1) {
		dot_pos = target.find(".", last_slash);
	} else {
		// Group syntax (@enemies.health) — find first '.' after '@'.
		dot_pos = target.find(".");
	}

	if (dot_pos != -1) {
		String node_part = target.substr(0, dot_pos);
		String property_name = target.substr(dot_pos + 1);

		if (property_name.is_empty()) {
			return "Error: empty property name after '.'";
		}

		Vector<Node *> nodes = _resolve_nodes(node_part);
		if (nodes.is_empty()) {
			return vformat("No nodes found for '%s'.", node_part);
		}

		if (extra_args.is_empty()) {
			// --- Read property ---
			String result;
			for (Node *node : nodes) {
				bool valid = false;
				Variant val = node->get(StringName(property_name), &valid);
				if (!valid) {
					if (nodes.size() == 1) {
						return vformat("Property '%s' not found on %s [%s].", property_name, String(node->get_path()), node->get_class());
					}
					result += vformat("  %s — [no such property]\n", String(node->get_path()));
				} else {
					if (nodes.size() == 1) {
						return vformat("%s.%s = %s", String(node->get_path()), property_name, val.stringify());
					}
					result += vformat("  %s = %s\n", String(node->get_path()), val.stringify());
				}
			}
			return vformat("%s on %d nodes:\n%s", property_name, nodes.size(), result).strip_edges();
		} else {
			// --- Write property ---
			// Join all extra args as the value string (allows "node /Path.name hello world").
			String value_str;
			for (const String &a : extra_args) {
				if (!value_str.is_empty()) {
					value_str += " ";
				}
				value_str += a;
			}

			int success_count = 0;
			String result;
			for (Node *node : nodes) {
				Variant val = _parse_value_for_property(node, StringName(property_name), value_str);
				bool valid = false;
				node->set(StringName(property_name), val, &valid);
				if (valid) {
					success_count++;
				} else {
					result += vformat("  %s — failed to set '%s'\n", String(node->get_path()), property_name);
				}
			}

			if (nodes.size() == 1) {
				if (success_count == 1) {
					// Read back the value to confirm.
					Variant readback = nodes[0]->get(StringName(property_name));
					return vformat("%s.%s = %s", String(nodes[0]->get_path()), property_name, readback.stringify());
				} else {
					return result.strip_edges();
				}
			}
			return vformat("Set %s on %d/%d nodes.%s", property_name, success_count, nodes.size(),
					result.is_empty() ? "" : "\n" + result)
					.strip_edges();
		}
	}

	// ===================================================================
	// Case 3: Inspect / list children — no '.' or ':'
	// Syntax: node /root/Path    → inspect
	//         node /root/Path/   → list children (trailing slash)
	//         node @group        → list all nodes in group
	// ===================================================================

	// Trailing slash = list children.
	if (target.ends_with("/")) {
		String path = target.left(target.length() - 1);
		Vector<Node *> nodes = _resolve_nodes(path);
		if (nodes.is_empty()) {
			return vformat("Node not found: '%s'.", path);
		}
		return _node_ls(nodes[0]);
	}

	// Group with no property = list all nodes in group.
	if (target.begins_with("@")) {
		StringName group = target.substr(1);
		Vector<Node *> nodes = tree->get_nodes_in_group(group);
		if (nodes.is_empty()) {
			return vformat("No nodes in group '%s'.", String(group));
		}
		String result = vformat("Group @%s (%d nodes):\n", String(group), nodes.size());
		for (Node *node : nodes) {
			result += vformat("  %s [%s]\n", String(node->get_path()), node->get_class());
		}
		return result.strip_edges();
	}

	// Single node inspect.
	Vector<Node *> nodes = _resolve_nodes(target);
	if (nodes.is_empty()) {
		return vformat("Node not found: '%s'. Make sure to use the full path (e.g., /root/Level/Player).", target);
	}

	return _node_inspect(nodes[0]);
}

// =============================================================================
// UI command — semantic interaction with controls
// =============================================================================

// Recursively find interactive controls in a subtree.
String DebugConsole::_ui_find_controls(Node *p_root, const String &p_filter) const {
	String result;
	int count = 0;

	// BFS to find all Control descendants.
	Vector<Node *> queue;
	queue.push_back(p_root);

	while (!queue.is_empty()) {
		Node *current = queue[0];
		queue.remove_at(0);

		Control *ctrl = Object::cast_to<Control>(current);
		if (ctrl && ctrl != p_root && ctrl->is_visible_in_tree()) {
			// Determine if this is an interactive control.
			String kind;
			String state;

			if (Object::cast_to<BaseButton>(ctrl)) {
				BaseButton *btn = Object::cast_to<BaseButton>(ctrl);
				if (Object::cast_to<CheckBox>(ctrl) || Object::cast_to<CheckButton>(ctrl)) {
					kind = "toggle";
					state = btn->is_pressed() ? "ON" : "OFF";
				} else if (Object::cast_to<OptionButton>(ctrl)) {
					OptionButton *ob = Object::cast_to<OptionButton>(ctrl);
					kind = "select";
					state = vformat("=%d/%d", ob->get_selected(), ob->get_item_count());
				} else if (Object::cast_to<MenuButton>(ctrl)) {
					kind = "menu";
				} else {
					kind = "button";
					if (btn->is_disabled()) {
						state = "(disabled)";
					}
				}
			} else if (Object::cast_to<Range>(ctrl)) {
				Range *range = Object::cast_to<Range>(ctrl);
				kind = "slider";
				state = vformat("=%.2f [%.0f-%.0f]", range->get_value(), range->get_min(), range->get_max());
			} else if (Object::cast_to<LineEdit>(ctrl)) {
				LineEdit *le = Object::cast_to<LineEdit>(ctrl);
				kind = "input";
				String txt = le->get_text();
				if (txt.length() > 20) {
					txt = txt.left(17) + "...";
				}
				state = vformat("=\"%s\"", txt);
			} else if (Object::cast_to<TextEdit>(ctrl)) {
				kind = "text";
			} else if (Object::cast_to<TabContainer>(ctrl)) {
				TabContainer *tc = Object::cast_to<TabContainer>(ctrl);
				kind = "tabs";
				state = vformat("=%d/%d", tc->get_current_tab(), tc->get_tab_count());
			} else if (Object::cast_to<TabBar>(ctrl)) {
				TabBar *tb = Object::cast_to<TabBar>(ctrl);
				kind = "tabs";
				state = vformat("=%d/%d", tb->get_current_tab(), tb->get_tab_count());
			}

			if (!kind.is_empty()) {
				if (p_filter.is_empty() || kind.contains(p_filter) ||
						String(ctrl->get_name()).to_lower().contains(p_filter.to_lower())) {
					// Show relative path from root.
					String rel_path = String(ctrl->get_path()).replace(String(p_root->get_path()) + "/", "");
					result += vformat("  %-8s %s %s\n", "[" + kind + "]", rel_path, state);
					count++;
				}
			}
		}

		for (int i = 0; i < current->get_child_count(); i++) {
			queue.push_back(current->get_child(i));
		}
	}

	if (count == 0) {
		return "No interactive controls found.";
	}
	return vformat("%d controls in %s:\n%s", count, String(p_root->get_path()), result).strip_edges();
}

// Describe a single control — what it is, its state, what you can do.
String DebugConsole::_ui_describe(Node *p_node) const {
	Control *ctrl = Object::cast_to<Control>(p_node);
	if (!ctrl) {
		return vformat("%s is not a UI Control.", String(p_node->get_path()));
	}

	String result = vformat("[%s] %s\n", ctrl->get_class(), String(ctrl->get_path()));

	if (Object::cast_to<CheckBox>(ctrl) || Object::cast_to<CheckButton>(ctrl)) {
		BaseButton *btn = Object::cast_to<BaseButton>(ctrl);
		result += vformat("  State: %s\n", btn->is_pressed() ? "ON (checked)" : "OFF (unchecked)");
		result += "  Actions: 'toggle', 'press', 'on', 'off'";
	} else if (Object::cast_to<OptionButton>(ctrl)) {
		OptionButton *ob = Object::cast_to<OptionButton>(ctrl);
		result += vformat("  Selected: %d\n", ob->get_selected());
		result += "  Items:\n";
		for (int i = 0; i < ob->get_item_count(); i++) {
			String marker = (i == ob->get_selected()) ? " → " : "   ";
			result += vformat("  %s%d: %s\n", marker, i, ob->get_item_text(i));
		}
		result += "  Actions: '<index>' to select";
	} else if (Object::cast_to<MenuButton>(ctrl)) {
		MenuButton *mb = Object::cast_to<MenuButton>(ctrl);
		PopupMenu *popup = mb->get_popup();
		result += "  Menu items:\n";
		for (int i = 0; i < popup->get_item_count(); i++) {
			if (popup->is_item_separator(i)) {
				result += "    ---\n";
			} else {
				result += vformat("    %d: %s\n", i, popup->get_item_text(i));
			}
		}
		result += "  Actions: 'select <index>'";
	} else if (Object::cast_to<BaseButton>(ctrl)) {
		BaseButton *btn = Object::cast_to<BaseButton>(ctrl);
		result += vformat("  Disabled: %s\n", btn->is_disabled() ? "yes" : "no");
		result += "  Actions: 'press'";
	} else if (Object::cast_to<Range>(ctrl)) {
		Range *range = Object::cast_to<Range>(ctrl);
		result += vformat("  Value: %.4f\n", range->get_value());
		result += vformat("  Range: %.2f to %.2f (step: %.2f)\n", range->get_min(), range->get_max(), range->get_step());
		result += "  Actions: '<value>' to set";
	} else if (Object::cast_to<LineEdit>(ctrl)) {
		LineEdit *le = Object::cast_to<LineEdit>(ctrl);
		result += vformat("  Text: \"%s\"\n", le->get_text());
		result += "  Actions: 'text <value>' to set";
	} else if (Object::cast_to<TextEdit>(ctrl)) {
		TextEdit *te = Object::cast_to<TextEdit>(ctrl);
		String txt = te->get_text();
		if (txt.length() > 60) {
			txt = txt.left(57) + "...";
		}
		result += vformat("  Text: \"%s\"\n", txt);
		result += "  Actions: 'text <value>' to set";
	} else if (Object::cast_to<TabContainer>(ctrl)) {
		TabContainer *tc = Object::cast_to<TabContainer>(ctrl);
		result += vformat("  Current tab: %d / %d\n", tc->get_current_tab(), tc->get_tab_count());
		result += "  Tabs:\n";
		for (int i = 0; i < tc->get_tab_count(); i++) {
			String marker = (i == tc->get_current_tab()) ? " → " : "   ";
			result += vformat("  %s%d: %s\n", marker, i, tc->get_tab_title(i));
		}
		result += "  Actions: 'tab <index>'";
	} else if (Object::cast_to<TabBar>(ctrl)) {
		TabBar *tb = Object::cast_to<TabBar>(ctrl);
		result += vformat("  Current tab: %d / %d\n", tb->get_current_tab(), tb->get_tab_count());
		result += "  Actions: 'tab <index>'";
	} else {
		result += vformat("  Visible: %s, Focus: %s\n",
				ctrl->is_visible_in_tree() ? "yes" : "no",
				ctrl->has_focus() ? "yes" : "no");
		result += "  (Not a recognized interactive control. Use 'node' for raw property access.)";
	}

	return result;
}

// Perform a semantic interaction with a control.
String DebugConsole::_ui_interact(Node *p_node, const PackedStringArray &p_args) const {
	Control *ctrl = Object::cast_to<Control>(p_node);
	if (!ctrl) {
		return vformat("%s is not a UI Control.", String(p_node->get_path()));
	}

	String action = p_args.is_empty() ? "" : p_args[0].to_lower();
	String path = String(p_node->get_path());

	// --- CheckBox / CheckButton ---
	if (Object::cast_to<CheckBox>(ctrl) || Object::cast_to<CheckButton>(ctrl)) {
		BaseButton *btn = Object::cast_to<BaseButton>(ctrl);
		if (action == "toggle" || action.is_empty()) {
			btn->set_pressed(!btn->is_pressed());
			return vformat("%s → %s", path, btn->is_pressed() ? "ON" : "OFF");
		}
		if (action == "on" || action == "true" || action == "1") {
			btn->set_pressed(true);
			return vformat("%s → ON", path);
		}
		if (action == "off" || action == "false" || action == "0") {
			btn->set_pressed(false);
			return vformat("%s → OFF", path);
		}
		if (action == "press") {
			btn->set_pressed(!btn->is_pressed());
			return vformat("%s → %s", path, btn->is_pressed() ? "ON" : "OFF");
		}
		return vformat("Unknown action '%s' for toggle. Use: toggle, on, off, press", action);
	}

	// --- OptionButton ---
	if (Object::cast_to<OptionButton>(ctrl)) {
		OptionButton *ob = Object::cast_to<OptionButton>(ctrl);
		if (action.is_empty()) {
			return _ui_describe(p_node);
		}
		if (action == "select" && p_args.size() > 1) {
			int idx = p_args[1].to_int();
			if (idx < 0 || idx >= ob->get_item_count()) {
				return vformat("Index %d out of range (0-%d).", idx, ob->get_item_count() - 1);
			}
			ob->select(idx);
			ob->emit_signal(SNAME("item_selected"), idx);
			return vformat("%s → selected %d: %s", path, idx, ob->get_item_text(idx));
		}
		// Try as a direct index.
		if (action.is_valid_int()) {
			int idx = action.to_int();
			if (idx >= 0 && idx < ob->get_item_count()) {
				ob->select(idx);
				ob->emit_signal(SNAME("item_selected"), idx);
				return vformat("%s → selected %d: %s", path, idx, ob->get_item_text(idx));
			}
			return vformat("Index %d out of range (0-%d).", idx, ob->get_item_count() - 1);
		}
		return vformat("Unknown action '%s' for OptionButton. Use: <index> or 'select <index>'", action);
	}

	// --- MenuButton ---
	if (Object::cast_to<MenuButton>(ctrl)) {
		MenuButton *mb = Object::cast_to<MenuButton>(ctrl);
		PopupMenu *popup = mb->get_popup();
		if (action.is_empty()) {
			return _ui_describe(p_node);
		}
		if ((action == "select" || action == "activate") && p_args.size() > 1) {
			int idx = p_args[1].to_int();
			if (idx < 0 || idx >= popup->get_item_count()) {
				return vformat("Index %d out of range (0-%d).", idx, popup->get_item_count() - 1);
			}
			popup->activate_item(idx);
			return vformat("%s → activated item %d: %s", path, idx, popup->get_item_text(idx));
		}
		if (action.is_valid_int()) {
			int idx = action.to_int();
			if (idx >= 0 && idx < popup->get_item_count()) {
				popup->activate_item(idx);
				return vformat("%s → activated item %d: %s", path, idx, popup->get_item_text(idx));
			}
		}
		return vformat("Unknown action '%s' for MenuButton. Use: 'select <index>'", action);
	}

	// --- Generic BaseButton (Button, TextureButton, etc.) ---
	if (Object::cast_to<BaseButton>(ctrl)) {
		BaseButton *btn = Object::cast_to<BaseButton>(ctrl);
		if (action.is_empty() || action == "press" || action == "click") {
			if (btn->is_disabled()) {
				return vformat("%s is disabled.", path);
			}
			// Emit pressed signal to simulate a click.
			btn->emit_signal(SNAME("pressed"));
			return vformat("Pressed %s", path);
		}
		return vformat("Unknown action '%s' for Button. Use: press", action);
	}

	// --- Range (Slider, SpinBox, ScrollBar, ProgressBar) ---
	if (Object::cast_to<Range>(ctrl)) {
		Range *range = Object::cast_to<Range>(ctrl);
		if (action.is_empty()) {
			return vformat("%s = %.4f [%.2f-%.2f]", path, range->get_value(), range->get_min(), range->get_max());
		}
		if (action.is_valid_float() || action.is_valid_int()) {
			double val = action.to_float();
			range->set_value(val);
			return vformat("%s → %.4f", path, range->get_value());
		}
		return vformat("Unknown action '%s' for Slider/Range. Use: <value>", action);
	}

	// --- LineEdit ---
	if (Object::cast_to<LineEdit>(ctrl)) {
		LineEdit *le = Object::cast_to<LineEdit>(ctrl);
		if (action.is_empty()) {
			return vformat("%s = \"%s\"", path, le->get_text());
		}
		if (action == "text" && p_args.size() > 1) {
			// Join remaining args as the text value.
			String text;
			for (int i = 1; i < p_args.size(); i++) {
				if (!text.is_empty()) {
					text += " ";
				}
				text += p_args[i];
			}
			le->set_text(text);
			le->emit_signal(SNAME("text_changed"), text);
			return vformat("%s → \"%s\"", path, text);
		}
		if (action == "clear") {
			le->clear();
			le->emit_signal(SNAME("text_changed"), String());
			return vformat("%s → cleared", path);
		}
		// Treat everything as text to set.
		{
			String text;
			for (const String &a : p_args) {
				if (!text.is_empty()) {
					text += " ";
				}
				text += a;
			}
			le->set_text(text);
			le->emit_signal(SNAME("text_changed"), text);
			return vformat("%s → \"%s\"", path, text);
		}
	}

	// --- TextEdit ---
	if (Object::cast_to<TextEdit>(ctrl)) {
		TextEdit *te = Object::cast_to<TextEdit>(ctrl);
		if (action.is_empty()) {
			String txt = te->get_text();
			if (txt.length() > 80) {
				txt = txt.left(77) + "...";
			}
			return vformat("%s = \"%s\"", path, txt);
		}
		if (action == "text" && p_args.size() > 1) {
			String text;
			for (int i = 1; i < p_args.size(); i++) {
				if (!text.is_empty()) {
					text += " ";
				}
				text += p_args[i];
			}
			te->set_text(text);
			return vformat("%s → text set", path);
		}
		if (action == "clear") {
			te->set_text("");
			return vformat("%s → cleared", path);
		}
		return vformat("Unknown action '%s' for TextEdit. Use: 'text <value>' or 'clear'", action);
	}

	// --- TabContainer ---
	if (Object::cast_to<TabContainer>(ctrl)) {
		TabContainer *tc = Object::cast_to<TabContainer>(ctrl);
		if (action.is_empty()) {
			return _ui_describe(p_node);
		}
		if (action == "tab" && p_args.size() > 1) {
			int idx = p_args[1].to_int();
			if (idx < 0 || idx >= tc->get_tab_count()) {
				return vformat("Tab %d out of range (0-%d).", idx, tc->get_tab_count() - 1);
			}
			tc->set_current_tab(idx);
			return vformat("%s → tab %d: %s", path, idx, tc->get_tab_title(idx));
		}
		if (action == "next") {
			tc->select_next_available();
			return vformat("%s → tab %d", path, tc->get_current_tab());
		}
		if (action == "prev") {
			tc->select_previous_available();
			return vformat("%s → tab %d", path, tc->get_current_tab());
		}
		if (action.is_valid_int()) {
			int idx = action.to_int();
			if (idx >= 0 && idx < tc->get_tab_count()) {
				tc->set_current_tab(idx);
				return vformat("%s → tab %d: %s", path, idx, tc->get_tab_title(idx));
			}
		}
		return vformat("Unknown action '%s' for TabContainer. Use: 'tab <index>', 'next', 'prev'", action);
	}

	// --- TabBar ---
	if (Object::cast_to<TabBar>(ctrl)) {
		TabBar *tb = Object::cast_to<TabBar>(ctrl);
		if (action.is_empty()) {
			return _ui_describe(p_node);
		}
		if (action == "tab" && p_args.size() > 1) {
			int idx = p_args[1].to_int();
			if (idx < 0 || idx >= tb->get_tab_count()) {
				return vformat("Tab %d out of range (0-%d).", idx, tb->get_tab_count() - 1);
			}
			tb->set_current_tab(idx);
			return vformat("%s → tab %d", path, idx);
		}
		if (action.is_valid_int()) {
			int idx = action.to_int();
			if (idx >= 0 && idx < tb->get_tab_count()) {
				tb->set_current_tab(idx);
				return vformat("%s → tab %d", path, idx);
			}
		}
		return vformat("Unknown action '%s' for TabBar. Use: 'tab <index>'", action);
	}

	// --- Unrecognized control ---
	return vformat("[%s] %s — not a recognized interactive control.\n"
				   "Use 'node %s.property_name' for raw property access.",
			ctrl->get_class(), path, path);
}

// --- UI page navigation helpers ---

String DebugConsole::_ui_pages() const {
	return "UI page navigation removed — use 'tree' and 'ls' to navigate.";
}

String DebugConsole::_ui_where() const {
	return "UI page navigation removed — use 'tree' and 'ls' to navigate.";
}

String DebugConsole::_ui_go(const String &p_page_name) const {
	return "UI page navigation removed — use 'tree' and 'ls' to navigate.";
}

String DebugConsole::_ui_auto_detect_pages(Node *p_root) const {
	// Auto-detect common UI page patterns:
	// 1. TabContainers — each tab child is a "page"
	// 2. Sibling Controls where visibility seems exclusive (common page pattern)
	// 3. CanvasLayer children that are full-screen or major containers.

	String result = "=== Auto-Detected UI Structure ===\n";
	int found = 0;

	// BFS through the tree looking for page-like patterns.
	Vector<Node *> queue;
	queue.push_back(p_root);

	while (!queue.is_empty()) {
		Node *current = queue[0];
		queue.remove_at(0);

		// Pattern 1: TabContainer — each tab is a page.
		TabContainer *tc = Object::cast_to<TabContainer>(current);
		if (tc && tc->get_tab_count() > 0) {
			result += vformat("  [TabContainer] %s — %d tabs:\n", String(tc->get_path()), tc->get_tab_count());
			for (int i = 0; i < tc->get_tab_count(); i++) {
				String marker = (i == tc->get_current_tab()) ? " → " : "   ";
				result += vformat("  %s%d: %s\n", marker, i, tc->get_tab_title(i));
			}
			found++;
			// Don't recurse into TabContainer children — they are the pages.
			continue;
		}

		// Pattern 2: Control parent with multiple Control children where some are hidden.
		// This is the classic "page stack" pattern: one child visible, rest hidden.
		Control *ctrl = Object::cast_to<Control>(current);
		if (ctrl && ctrl->get_child_count() >= 2) {
			int control_children = 0;
			int visible_controls = 0;
			int hidden_controls = 0;

			for (int i = 0; i < ctrl->get_child_count(); i++) {
				Control *child_ctrl = Object::cast_to<Control>(ctrl->get_child(i));
				if (child_ctrl) {
					control_children++;
					if (child_ctrl->is_visible()) {
						visible_controls++;
					} else {
						hidden_controls++;
					}
				}
			}

			// Heuristic: if there are 2+ Control children and at least one is hidden,
			// this looks like a page container. A strong signal is exactly 1 visible.
			if (control_children >= 2 && hidden_controls >= 1 && visible_controls <= 2) {
				result += vformat("  [Page Stack] %s — %d pages (%d visible, %d hidden):\n",
						String(ctrl->get_path()), control_children, visible_controls, hidden_controls);
				for (int i = 0; i < ctrl->get_child_count(); i++) {
					Control *child_ctrl = Object::cast_to<Control>(ctrl->get_child(i));
					if (child_ctrl) {
						String vis = child_ctrl->is_visible() ? " [ACTIVE]" : "";
						result += vformat("    %s [%s]%s\n", child_ctrl->get_name(), child_ctrl->get_class(), vis);
					}
				}
				found++;
			}
		}

		// Recurse.
		for (int i = 0; i < current->get_child_count(); i++) {
			queue.push_back(current->get_child(i));
		}
	}

	if (found == 0) {
		result += "  No page-like patterns detected.\n";
		result += "  Tip: Use Debug.register_ui_page() in GDScript to annotate game screens.";
	} else {
		result += vformat("\n%d page structure(s) detected.\n", found);
		result += "Tip: Register these with Debug.register_ui_page() for full navigation support.";
	}

	return result;
}

String DebugConsole::_cmd_ui(const PackedStringArray &p_args) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		return "SceneTree not available.";
	}

	// No args: scan cwd for interactive controls.
	if (p_args.is_empty()) {
		Node *cwd_node = tree->get_root()->get_node_or_null(NodePath(_cwd));
		if (!cwd_node) {
			return vformat("cwd not found: '%s'.", _cwd);
		}
		return _ui_find_controls(cwd_node, "");
	}

	String target = p_args[0];

	// --- Page navigation subcommands ---
	if (target == "pages" || target == "map") {
		return _ui_pages();
	}
	if (target == "where" || target == "current") {
		return _ui_where();
	}
	if (target == "go" || target == "navigate") {
		if (p_args.size() < 2) {
			return "Usage: ui go <page_name>";
		}
		return _ui_go(p_args[1]);
	}
	if (target == "detect" || target == "scan") {
		return _ui_auto_detect_pages(tree->get_root());
	}

	// If target is a filter keyword (button, slider, toggle, input, tabs, menu),
	// scan cwd with that filter.
	if (target == "buttons" || target == "sliders" || target == "toggles" ||
			target == "inputs" || target == "tabs" || target == "menus" ||
			target == "button" || target == "slider" || target == "toggle" ||
			target == "input" || target == "tab" || target == "menu" ||
			target == "select") {
		Node *cwd_node = tree->get_root()->get_node_or_null(NodePath(_cwd));
		if (!cwd_node) {
			return vformat("cwd not found: '%s'.", _cwd);
		}
		return _ui_find_controls(cwd_node, target);
	}

	// Resolve the target node.
	String resolved = _resolve_path(target);
	Node *node = tree->get_root()->get_node_or_null(NodePath(resolved));
	if (!node) {
		return vformat("Node not found: '%s'.", resolved);
	}

	// If no further args, describe or scan.
	PackedStringArray action_args;
	for (int i = 1; i < p_args.size(); i++) {
		action_args.push_back(p_args[i]);
	}

	// If the node is a Control, interact with it.
	if (Object::cast_to<Control>(node)) {
		if (action_args.is_empty()) {
			return _ui_describe(node);
		}
		return _ui_interact(node, action_args);
	}

	// If not a Control, scan its children for controls.
	return _ui_find_controls(node, "");
}

String DebugConsole::_cmd_exec(const PackedStringArray &p_args) {
	if (p_args.is_empty()) {
		return "Usage: exec <file_path>";
	}

	String path = p_args[0];

	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
	if (!file.is_valid()) {
		return vformat("Cannot open file: '%s'", path);
	}

	int line_count = 0;
	while (!file->eof_reached()) {
		String line = file->get_line().strip_edges();
		if (line.is_empty() || line.begins_with("//") || line.begins_with("#")) {
			continue; // Skip comments and empty lines.
		}
		_push_output("> " + line, Color(0.6, 0.6, 0.7));
		String result = _execute_command_string(line);
		if (!result.is_empty()) {
			_push_output(result, Color(0.4, 1.0, 0.4));
		}
		line_count++;
	}

	return vformat("Executed %d commands from '%s'.", line_count, path);
}

#endif // DEBUG_ENABLED
