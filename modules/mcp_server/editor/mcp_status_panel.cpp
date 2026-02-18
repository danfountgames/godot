/**************************************************************************/
/*  mcp_status_panel.cpp                                                  */
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

#ifdef TOOLS_ENABLED

#include "mcp_status_panel.h"

#include "../mcp_debugger_bridge.h"
#include "../mcp_protocol.h"
#include "../mcp_server_plugin.h"

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "editor/editor_interface.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/separator.h"
#include "scene/gui/texture_rect.h"
#include "scene/gui/tree.h"
#include "scene/resources/image_texture.h"

// =========================================================================
// Constructor / Destructor
// =========================================================================

MCPStatusPanel::MCPStatusPanel() {
	set_name("MCPStatusPanel");
	_create_dot_textures();
	_build_ui();
}

MCPStatusPanel::~MCPStatusPanel() {
}

// =========================================================================
// Binding
// =========================================================================

void MCPStatusPanel::_bind_methods() {
	// No GDScript-exposed methods needed.
}

// =========================================================================
// Dot Texture Generation
// =========================================================================

Ref<Texture2D> MCPStatusPanel::_make_dot_texture(const Color &p_color) {
	const int size = 8;
	Ref<Image> img = Image::create_empty(size, size, false, Image::FORMAT_RGBA8);
	float center = (size - 1) / 2.0f;
	float radius = center;

	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			float dx = x - center;
			float dy = y - center;
			float dist = Math::sqrt(dx * dx + dy * dy);
			if (dist <= radius) {
				// Smooth edge with anti-aliasing.
				float alpha = CLAMP(1.0f - (dist - radius + 0.5f), 0.0f, 1.0f);
				img->set_pixel(x, y, Color(p_color.r, p_color.g, p_color.b, p_color.a * alpha));
			} else {
				img->set_pixel(x, y, Color(0, 0, 0, 0));
			}
		}
	}

	return ImageTexture::create_from_image(img);
}

void MCPStatusPanel::_create_dot_textures() {
	dot_green = _make_dot_texture(Color(0.4, 0.9, 0.4));
	dot_red = _make_dot_texture(Color(0.9, 0.3, 0.3));
	dot_yellow = _make_dot_texture(Color(0.9, 0.8, 0.3));
	dot_gray = _make_dot_texture(Color(0.6, 0.6, 0.6));
}

// =========================================================================
// UI Construction
// =========================================================================

void MCPStatusPanel::_build_ui() {
	// ── Row 1: Top Status Bar ──────────────────────────────────────────
	HBoxContainer *top_bar = memnew(HBoxContainer);
	top_bar->set_h_size_flags(SIZE_EXPAND_FILL);
	add_child(top_bar);

	// -- Server Status Panel --
	VBoxContainer *server_status_panel = memnew(VBoxContainer);
	server_status_panel->set_custom_minimum_size(Size2(220 * EDSCALE, 0));
	top_bar->add_child(server_status_panel);

	HBoxContainer *status_indicator = memnew(HBoxContainer);
	server_status_panel->add_child(status_indicator);

	status_dot = memnew(TextureRect);
	status_dot->set_texture(dot_red);
	status_dot->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	status_dot->set_custom_minimum_size(Size2(12 * EDSCALE, 12 * EDSCALE));
	status_indicator->add_child(status_dot);

	status_label = memnew(Label);
	status_label->set_text(TTR("Stopped"));
	status_indicator->add_child(status_label);

	address_label = memnew(Label);
	address_label->set_text(TTR("(not configured)"));
	address_label->add_theme_color_override("font_color", Color(0.7, 0.7, 0.7));
	server_status_panel->add_child(address_label);

	uptime_label = memnew(Label);
	uptime_label->set_text(TTR("Uptime: --:--:--"));
	server_status_panel->add_child(uptime_label);

	stats_label = memnew(Label);
	stats_label->set_text(TTR("Tools: 0  Resources: 0"));
	server_status_panel->add_child(stats_label);

	toggle_button = memnew(Button);
	toggle_button->set_text(TTR("Start Server"));
	toggle_button->connect("pressed", callable_mp(this, &MCPStatusPanel::_on_toggle_button_pressed));
	server_status_panel->add_child(toggle_button);

	// -- VSeparator --
	top_bar->add_child(memnew(VSeparator));

	// -- Debugger Bridge Panel --
	VBoxContainer *debugger_status_panel = memnew(VBoxContainer);
	debugger_status_panel->set_custom_minimum_size(Size2(200 * EDSCALE, 0));
	top_bar->add_child(debugger_status_panel);

	HBoxContainer *game_status = memnew(HBoxContainer);
	debugger_status_panel->add_child(game_status);

	game_dot = memnew(TextureRect);
	game_dot->set_texture(dot_gray);
	game_dot->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);
	game_dot->set_custom_minimum_size(Size2(12 * EDSCALE, 12 * EDSCALE));
	game_status->add_child(game_dot);

	game_label = memnew(Label);
	game_label->set_text(TTR("Game: Stopped"));
	game_status->add_child(game_label);

	pending_label = memnew(Label);
	pending_label->set_text(TTR("Pending msgs: 0"));
	debugger_status_panel->add_child(pending_label);

	heartbeat_label = memnew(Label);
	heartbeat_label->set_text(TTR("Heartbeat: No game"));
	debugger_status_panel->add_child(heartbeat_label);

	// -- VSeparator --
	top_bar->add_child(memnew(VSeparator));

	// -- Clients Panel --
	VBoxContainer *clients_panel = memnew(VBoxContainer);
	clients_panel->set_h_size_flags(SIZE_EXPAND_FILL);
	top_bar->add_child(clients_panel);

	HBoxContainer *clients_header = memnew(HBoxContainer);
	clients_panel->add_child(clients_header);

	Label *clients_title = memnew(Label);
	clients_title->set_text(TTR("Connected Clients"));
	clients_header->add_child(clients_title);

	client_count_label = memnew(Label);
	client_count_label->set_text("[0]");
	client_count_label->add_theme_color_override("font_color", Color(0.7, 0.7, 0.7));
	clients_header->add_child(client_count_label);

	clients_tree = memnew(Tree);
	clients_tree->set_columns(4);
	clients_tree->set_column_titles_visible(true);
	clients_tree->set_column_title(0, TTR("Session"));
	clients_tree->set_column_title(1, TTR("Type"));
	clients_tree->set_column_title(2, TTR("Duration"));
	clients_tree->set_column_title(3, TTR("Last Activity"));
	clients_tree->set_column_custom_minimum_width(0, 100 * EDSCALE);
	clients_tree->set_column_custom_minimum_width(1, 50 * EDSCALE);
	clients_tree->set_column_custom_minimum_width(2, 80 * EDSCALE);
	clients_tree->set_column_custom_minimum_width(3, 80 * EDSCALE);
	clients_tree->set_hide_root(true);
	clients_tree->set_custom_minimum_size(Size2(0, 80 * EDSCALE));
	clients_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	clients_panel->add_child(clients_tree);

	// ── HSeparator ────────────────────────────────────────────────────
	add_child(memnew(HSeparator));

	// ── Test Results Section ──────────────────────────────────────────
	_build_test_section();

	// ── HSeparator ────────────────────────────────────────────────────
	add_child(memnew(HSeparator));

	// ── Row 2: Log Toolbar ────────────────────────────────────────────
	HBoxContainer *log_toolbar = memnew(HBoxContainer);
	log_toolbar->set_h_size_flags(SIZE_EXPAND_FILL);
	add_child(log_toolbar);

	Label *filter_label = memnew(Label);
	filter_label->set_text(TTR("Filter:"));
	log_toolbar->add_child(filter_label);

	search_edit = memnew(LineEdit);
	search_edit->set_placeholder(TTR("Search..."));
	search_edit->set_custom_minimum_size(Size2(150 * EDSCALE, 0));
	search_edit->connect("text_changed", callable_mp(this, &MCPStatusPanel::_on_search_changed));
	log_toolbar->add_child(search_edit);

	method_filter = memnew(OptionButton);
	method_filter->add_item(TTR("All Methods"), 0);
	method_filter->connect("item_selected", callable_mp(this, &MCPStatusPanel::_on_filter_changed));
	log_toolbar->add_child(method_filter);

	status_filter = memnew(OptionButton);
	status_filter->add_item(TTR("All Status"), 0);
	status_filter->add_item(TTR("Success (2xx)"), 1);
	status_filter->add_item(TTR("Error (4xx+)"), 2);
	status_filter->add_item(TTR("In Progress"), 3);
	status_filter->connect("item_selected", callable_mp(this, &MCPStatusPanel::_on_filter_changed));
	log_toolbar->add_child(status_filter);

	// Spacer.
	Control *spacer = memnew(Control);
	spacer->set_h_size_flags(SIZE_EXPAND_FILL);
	log_toolbar->add_child(spacer);

	auto_scroll_check = memnew(CheckBox);
	auto_scroll_check->set_text(TTR("Auto-scroll"));
	auto_scroll_check->set_pressed(true);
	log_toolbar->add_child(auto_scroll_check);

	clear_button = memnew(Button);
	clear_button->set_text(TTR("Clear"));
	clear_button->connect("pressed", callable_mp(this, &MCPStatusPanel::_on_clear_pressed));
	log_toolbar->add_child(clear_button);

	export_button = memnew(Button);
	export_button->set_text(TTR("Export"));
	export_button->connect("pressed", callable_mp(this, &MCPStatusPanel::_on_export_pressed));
	log_toolbar->add_child(export_button);

	// ── Row 3: Request Log Tree ───────────────────────────────────────
	request_log_tree = memnew(Tree);
	request_log_tree->set_columns(6);
	request_log_tree->set_column_titles_visible(true);
	request_log_tree->set_column_title(0, TTR("Time"));
	request_log_tree->set_column_title(1, TTR("Method"));
	request_log_tree->set_column_title(2, TTR("Client"));
	request_log_tree->set_column_title(3, TTR("Status"));
	request_log_tree->set_column_title(4, TTR("Duration"));
	request_log_tree->set_column_title(5, TTR("Details"));
	request_log_tree->set_column_custom_minimum_width(0, 90 * EDSCALE);
	request_log_tree->set_column_custom_minimum_width(1, 160 * EDSCALE);
	request_log_tree->set_column_custom_minimum_width(2, 80 * EDSCALE);
	request_log_tree->set_column_custom_minimum_width(3, 70 * EDSCALE);
	request_log_tree->set_column_custom_minimum_width(4, 70 * EDSCALE);
	request_log_tree->set_column_custom_minimum_width(5, 40 * EDSCALE);
	request_log_tree->set_column_expand(0, false);
	request_log_tree->set_column_expand(1, true);
	request_log_tree->set_column_expand(2, false);
	request_log_tree->set_column_expand(3, false);
	request_log_tree->set_column_expand(4, false);
	request_log_tree->set_column_expand(5, false);
	request_log_tree->set_hide_root(true);
	request_log_tree->create_item(); // Root item.
	request_log_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	request_log_tree->connect("item_activated", callable_mp(this, &MCPStatusPanel::_on_log_item_activated));
	add_child(request_log_tree);

	// ── HSeparator ────────────────────────────────────────────────────
	add_child(memnew(HSeparator));

	// ── Row 4: Tool Activity Summary ──────────────────────────────────
	tool_summary_tree = memnew(Tree);
	tool_summary_tree->set_columns(5);
	tool_summary_tree->set_column_titles_visible(true);
	tool_summary_tree->set_column_title(0, TTR("Tool Name"));
	tool_summary_tree->set_column_title(1, TTR("Calls"));
	tool_summary_tree->set_column_title(2, TTR("Avg Time"));
	tool_summary_tree->set_column_title(3, TTR("Last Called"));
	tool_summary_tree->set_column_title(4, TTR("Last Status"));
	tool_summary_tree->set_column_custom_minimum_width(0, 200 * EDSCALE);
	tool_summary_tree->set_column_custom_minimum_width(1, 60 * EDSCALE);
	tool_summary_tree->set_column_custom_minimum_width(2, 80 * EDSCALE);
	tool_summary_tree->set_column_custom_minimum_width(3, 100 * EDSCALE);
	tool_summary_tree->set_column_custom_minimum_width(4, 80 * EDSCALE);
	tool_summary_tree->set_column_expand(0, true);
	tool_summary_tree->set_column_expand(1, false);
	tool_summary_tree->set_column_expand(2, false);
	tool_summary_tree->set_column_expand(3, false);
	tool_summary_tree->set_column_expand(4, false);
	tool_summary_tree->set_hide_root(true);
	tool_summary_tree->set_custom_minimum_size(Size2(0, 120 * EDSCALE));
	add_child(tool_summary_tree);
}

// =========================================================================
// Notification Handler
// =========================================================================

void MCPStatusPanel::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			set_process_internal(true);
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			uint64_t now = OS::get_singleton()->get_ticks_usec();

			// Notify protocol of panel visibility for conditional JSON capture.
			if (protocol) {
				protocol->set_panel_visible(is_visible_in_tree());
			}

			// Fast updates: request log and test results (every frame).
			_update_request_log();
			_update_test_results();

			// Slow updates: everything else (every 500ms).
			if (now - last_slow_update > 500000) {
				last_slow_update = now;
				_update_server_status();
				_update_clients();
				_update_debugger_bridge();
				_update_tool_summary();
			}
		} break;

		case NOTIFICATION_THEME_CHANGED: {
			// Could refresh colors from theme here in the future.
		} break;
	}
}

// =========================================================================
// Formatting Helpers
// =========================================================================

String MCPStatusPanel::_format_timestamp(uint64_t p_usec) const {
	// Convert ticks_usec to a time-of-day string HH:MM:SS.d.
	// We use OS::get_time() for the base and compute the fractional part.
	Dictionary time_dict = Time::get_singleton()->get_time_dict_from_system();
	int hour = time_dict["hour"];
	int minute = time_dict["minute"];
	int second = time_dict["second"];

	// Calculate offset from "now" to "then" to adjust.
	uint64_t now_usec = OS::get_singleton()->get_ticks_usec();
	int64_t diff_usec = (int64_t)now_usec - (int64_t)p_usec;
	int64_t diff_sec = diff_usec / 1000000;

	// Adjust time backward.
	int64_t total_sec = hour * 3600 + minute * 60 + second - diff_sec;
	if (total_sec < 0) {
		total_sec += 86400; // Wrap around midnight.
	}

	int h = (int)(total_sec / 3600) % 24;
	int m = (int)((total_sec % 3600) / 60);
	int s = (int)(total_sec % 60);
	int tenths = (int)((p_usec % 1000000) / 100000);

	return String::num_int64(h).lpad(2, "0") + ":" +
			String::num_int64(m).lpad(2, "0") + ":" +
			String::num_int64(s).lpad(2, "0") + "." +
			String::num_int64(tenths);
}

String MCPStatusPanel::_format_duration(uint64_t p_usec) const {
	if (p_usec >= 1000000) {
		double sec = (double)p_usec / 1000000.0;
		return String::num(sec, 1) + "s";
	}
	int ms = (int)(p_usec / 1000);
	return itos(ms) + "ms";
}

String MCPStatusPanel::_format_relative_time(uint64_t p_from_usec, uint64_t p_now_usec) const {
	if (p_from_usec == 0 || p_now_usec < p_from_usec) {
		return "-";
	}
	uint64_t diff_sec = (p_now_usec - p_from_usec) / 1000000;
	if (diff_sec < 60) {
		return itos(diff_sec) + "s ago";
	}
	uint64_t diff_min = diff_sec / 60;
	uint64_t rem_sec = diff_sec % 60;
	return itos(diff_min) + "m " + itos(rem_sec) + "s ago";
}

String MCPStatusPanel::_format_uptime(uint64_t p_seconds) const {
	int h = (int)(p_seconds / 3600);
	int m = (int)((p_seconds % 3600) / 60);
	int s = (int)(p_seconds % 60);
	return String::num_int64(h).lpad(2, "0") + ":" +
			String::num_int64(m).lpad(2, "0") + ":" +
			String::num_int64(s).lpad(2, "0");
}

Color MCPStatusPanel::_color_for_status(int p_http_status) const {
	if (p_http_status == 0) {
		return Color(0.9, 0.8, 0.3); // In-progress (yellow).
	}
	if (p_http_status >= 200 && p_http_status < 300) {
		if (p_http_status == 202) {
			return Color(0.6, 0.6, 0.6); // Notification (gray).
		}
		return Color(0.4, 0.9, 0.4); // Success (green).
	}
	if (p_http_status >= 400) {
		return Color(0.9, 0.3, 0.3); // Error (red).
	}
	return Color(0.8, 0.8, 0.8); // Unknown.
}

String MCPStatusPanel::_status_text(int p_http_status) const {
	if (p_http_status == 0) {
		return "IN PROG";
	}
	switch (p_http_status) {
		case 200:
			return "200 OK";
		case 202:
			return "202";
		case 204:
			return "204";
		case 400:
			return "400 Bad";
		case 401:
			return "401 Auth";
		case 403:
			return "403 Deny";
		case 404:
			return "404 N/F";
		case 405:
			return "405";
		case 500:
			return "500 Err";
		default:
			return itos(p_http_status);
	}
}

// =========================================================================
// Server Status Updates (slow path: every 500ms)
// =========================================================================

void MCPStatusPanel::_update_server_status() {
	if (!protocol) {
		return;
	}

	bool running = protocol->is_running();

	status_dot->set_texture(running ? dot_green : dot_red);
	status_label->set_text(running ? TTR("Running") : TTR("Stopped"));

	if (running) {
		address_label->set_text(protocol->get_listen_address() + ":" + itos(protocol->get_listen_port()));

		uint64_t start_usec = protocol->get_start_time_usec();
		if (start_usec > 0) {
			uint64_t now_usec = OS::get_singleton()->get_ticks_usec();
			uint64_t uptime_sec = (now_usec - start_usec) / 1000000;
			uptime_label->set_text(TTR("Uptime: ") + _format_uptime(uptime_sec));
		} else {
			uptime_label->set_text(TTR("Uptime: --:--:--"));
		}

		int tool_count = protocol->get_tool_registry()->get_tool_count();
		int resource_count = protocol->get_resource_registry()->get_resource_count();
		stats_label->set_text(TTR("Tools: ") + itos(tool_count) + "  " + TTR("Resources: ") + itos(resource_count));

		toggle_button->set_text(TTR("Stop Server"));
	} else {
		address_label->set_text(TTR("(not configured)"));
		uptime_label->set_text(TTR("Uptime: --:--:--"));
		stats_label->set_text(TTR("Tools: 0  Resources: 0"));
		toggle_button->set_text(TTR("Start Server"));
	}
}

// =========================================================================
// Client List Updates (slow path: every 500ms)
// =========================================================================

void MCPStatusPanel::_update_clients() {
	if (!protocol) {
		return;
	}

	Vector<MCPClientSnapshot> snapshots = protocol->get_client_snapshots();
	client_count_label->set_text("[" + itos(snapshots.size()) + "]");

	clients_tree->clear();
	TreeItem *root = clients_tree->create_item();
	uint64_t now_usec = OS::get_singleton()->get_ticks_usec();

	for (int i = 0; i < snapshots.size(); i++) {
		const MCPClientSnapshot &snap = snapshots[i];
		TreeItem *item = clients_tree->create_item(root);
		item->set_text(0, snap.session_id.is_empty() ? "-" : snap.session_id);
		item->set_text(1, snap.is_sse_stream ? "SSE" : "HTTP");

		if (snap.connected_since > 0) {
			uint64_t dur_sec = (now_usec - snap.connected_since) / 1000000;
			if (dur_sec >= 60) {
				item->set_text(2, itos(dur_sec / 60) + "m " + itos(dur_sec % 60) + "s");
			} else {
				item->set_text(2, itos(dur_sec) + "s");
			}
		} else {
			item->set_text(2, "-");
		}

		item->set_text(3, _format_relative_time(snap.last_activity, now_usec));
	}
}

// =========================================================================
// Debugger Bridge Updates (slow path: every 500ms)
// =========================================================================

void MCPStatusPanel::_update_debugger_bridge() {
	if (!debugger_bridge) {
		game_dot->set_texture(dot_gray);
		game_label->set_text(TTR("Game: No bridge"));
		pending_label->set_text(TTR("Pending msgs: -"));
		heartbeat_label->set_text(TTR("Heartbeat: N/A"));
		return;
	}

	uint64_t now_usec = OS::get_singleton()->get_ticks_usec();

	// Game status.
	if (debugger_bridge->is_game_running()) {
		if (debugger_bridge->is_game_paused()) {
			game_dot->set_texture(dot_yellow);
			game_label->set_text(TTR("Game: Paused"));
		} else {
			game_dot->set_texture(dot_green);
			game_label->set_text(TTR("Game: Running"));
		}
	} else if (debugger_bridge->is_game_launching()) {
		game_dot->set_texture(dot_yellow);
		game_label->set_text(TTR("Game: Launching"));
	} else {
		game_dot->set_texture(dot_gray);
		game_label->set_text(TTR("Game: Stopped"));
	}

	// Pending requests.
	int pending_count = debugger_bridge->get_pending_request_count();
	pending_label->set_text(TTR("Pending msgs: ") + itos(pending_count));

	// Heartbeat.
	if (debugger_bridge->is_game_running()) {
		int64_t frame = debugger_bridge->get_game_frame_count();

		// Detect heartbeat staleness.
		if (frame != last_heartbeat_frame) {
			last_heartbeat_frame = frame;
			last_heartbeat_change_time = now_usec;
		}

		uint64_t stale_threshold = 5000000; // 5 seconds.
		if (last_heartbeat_change_time > 0 && (now_usec - last_heartbeat_change_time) > stale_threshold) {
			heartbeat_label->set_text(TTR("Heartbeat: Stale"));
			heartbeat_label->add_theme_color_override("font_color", Color(0.9, 0.8, 0.3));
		} else {
			heartbeat_label->set_text(TTR("Heartbeat: OK (frame #") + itos(frame) + ")");
			heartbeat_label->remove_theme_color_override("font_color");
		}
	} else {
		heartbeat_label->set_text(TTR("Heartbeat: No game"));
		heartbeat_label->remove_theme_color_override("font_color");
		last_heartbeat_frame = -1;
		last_heartbeat_change_time = 0;
	}
}

// =========================================================================
// Request Log Updates (fast path: every frame)
// =========================================================================

bool MCPStatusPanel::_event_matches_filters(const MCPRequestEvent &p_event) const {
	// Method filter.
	if (current_method_filter_idx > 0 && current_method_filter_idx - 1 < observed_methods.size()) {
		String filter_method = observed_methods[current_method_filter_idx - 1];
		if (p_event.method != filter_method) {
			return false;
		}
	}

	// Status filter.
	if (current_status_filter_idx == 1) {
		// Success (2xx).
		if (p_event.http_status < 200 || p_event.http_status >= 300) {
			return false;
		}
	} else if (current_status_filter_idx == 2) {
		// Error (4xx+).
		if (p_event.http_status < 400) {
			return false;
		}
	} else if (current_status_filter_idx == 3) {
		// In progress.
		if (p_event.http_status != 0) {
			return false;
		}
	}

	// Search text filter (case-insensitive substring).
	if (!current_search_text.is_empty()) {
		String lower_search = current_search_text.to_lower();
		bool found = false;
		if (p_event.method.to_lower().find(lower_search) != -1) {
			found = true;
		} else if (p_event.tool_name.to_lower().find(lower_search) != -1) {
			found = true;
		} else if (p_event.session_id.to_lower().find(lower_search) != -1) {
			found = true;
		} else if (p_event.response_json.to_lower().find(lower_search) != -1) {
			found = true;
		}
		if (!found) {
			return false;
		}
	}

	return true;
}

TreeItem *MCPStatusPanel::_add_log_entry(const MCPRequestEvent &p_event) {
	TreeItem *root = request_log_tree->get_root();
	if (!root) {
		root = request_log_tree->create_item();
	}

	TreeItem *item = request_log_tree->create_item(root);

	// Column 0: Time.
	item->set_text(0, _format_timestamp(p_event.timestamp_usec));

	// Column 1: Method (with tool name if applicable).
	String method_text = p_event.method;
	if (!p_event.tool_name.is_empty()) {
		method_text += " (" + p_event.tool_name + ")";
	}
	item->set_text(1, method_text);

	// Column 2: Client (session ID).
	item->set_text(2, p_event.session_id);

	// Column 3: Status.
	item->set_text(3, _status_text(p_event.http_status));
	Color status_color = _color_for_status(p_event.http_status);
	item->set_custom_color(3, status_color);

	// Column 4: Duration.
	item->set_text(4, _format_duration(p_event.duration_usec));

	// Column 5: Expand toggle.
	bool has_json = !p_event.request_json.is_empty() || !p_event.response_json.is_empty();
	item->set_text(5, has_json ? "[>]" : "");

	// Store event data in metadata for expand/collapse.
	Dictionary meta;
	meta["request_json"] = p_event.request_json;
	meta["response_json"] = p_event.response_json;
	meta["seq"] = (int64_t)p_event.seq;
	meta["expanded"] = false;
	item->set_metadata(0, meta);

	// Apply row color tint based on error status.
	if (p_event.is_error) {
		for (int col = 0; col < 6; col++) {
			item->set_custom_color(col, Color(0.9, 0.5, 0.5));
		}
		// Re-apply the specific status column color.
		item->set_custom_color(3, status_color);
	}

	log_row_count++;
	return item;
}

void MCPStatusPanel::_prune_log_rows() {
	const int MAX_LOG_ROWS = 2000;
	TreeItem *root = request_log_tree->get_root();
	if (!root) {
		return;
	}

	while (log_row_count > MAX_LOG_ROWS) {
		TreeItem *first = root->get_first_child();
		if (!first) {
			break;
		}
		// Count children that will also be removed.
		TreeItem *child = first->get_first_child();
		while (child) {
			log_row_count--;
			child = child->get_next();
		}
		memdelete(first); // Automatically removed from parent.
		log_row_count--;
	}
}

void MCPStatusPanel::_update_request_log() {
	if (!protocol) {
		return;
	}

	MCPEventBuffer *buffer = protocol->get_event_buffer();
	if (!buffer) {
		return;
	}

	Vector<MCPRequestEvent> new_events = buffer->read_since(last_event_cursor, 200);

	if (new_events.is_empty()) {
		return;
	}

	for (int i = 0; i < new_events.size(); i++) {
		const MCPRequestEvent &evt = new_events[i];
		last_event_cursor = evt.seq;

		// Track observed methods for the filter dropdown.
		bool method_seen = false;
		for (int j = 0; j < observed_methods.size(); j++) {
			if (observed_methods[j] == evt.method) {
				method_seen = true;
				break;
			}
		}
		if (!method_seen) {
			observed_methods.push_back(evt.method);
			method_filter->add_item(evt.method, observed_methods.size());
		}

		// Apply filters.
		if (_event_matches_filters(evt)) {
			_add_log_entry(evt);
		}
	}

	_prune_log_rows();

	// Auto-scroll.
	if (auto_scroll_check && auto_scroll_check->is_pressed()) {
		TreeItem *root = request_log_tree->get_root();
		if (root) {
			// Find the last child by iterating.
			TreeItem *last = root->get_first_child();
			if (last) {
				while (last->get_next()) {
					last = last->get_next();
				}
				request_log_tree->scroll_to_item(last);
			}
		}
	}
}

void MCPStatusPanel::_rebuild_log_from_buffer() {
	// Clear all log rows.
	request_log_tree->clear();
	request_log_tree->create_item(); // Re-create root.
	log_row_count = 0;

	if (!protocol) {
		return;
	}

	MCPEventBuffer *buffer = protocol->get_event_buffer();
	if (!buffer) {
		return;
	}

	// Read all events from the buffer.
	Vector<MCPRequestEvent> all_events = buffer->read_since(0, MCPEventBuffer::CAPACITY);

	for (int i = 0; i < all_events.size(); i++) {
		const MCPRequestEvent &evt = all_events[i];
		if (_event_matches_filters(evt)) {
			_add_log_entry(evt);
		}
	}

	// Update cursor to latest.
	last_event_cursor = buffer->latest_seq();

	_prune_log_rows();
}

// =========================================================================
// Tool Summary Updates (slow path: every 500ms)
// =========================================================================

void MCPStatusPanel::_update_tool_summary() {
	if (!protocol) {
		return;
	}

	HashMap<String, MCPToolStats> stats = protocol->get_tool_stats();

	tool_summary_tree->clear();
	TreeItem *root = tool_summary_tree->create_item();

	// Sort by call count descending.
	struct ToolEntry {
		String name;
		MCPToolStats stats;
	};

	Vector<ToolEntry> sorted;
	for (const KeyValue<String, MCPToolStats> &E : stats) {
		ToolEntry entry;
		entry.name = E.key;
		entry.stats = E.value;
		sorted.push_back(entry);
	}

	// Simple insertion sort by call_count descending.
	for (int i = 1; i < sorted.size(); i++) {
		ToolEntry key_entry = sorted[i];
		int j = i - 1;
		while (j >= 0 && sorted[j].stats.call_count < key_entry.stats.call_count) {
			sorted.write[j + 1] = sorted[j];
			j--;
		}
		sorted.write[j + 1] = key_entry;
	}

	uint64_t now_usec = OS::get_singleton()->get_ticks_usec();

	for (int i = 0; i < sorted.size(); i++) {
		const ToolEntry &entry = sorted[i];
		TreeItem *item = tool_summary_tree->create_item(root);

		item->set_text(0, entry.name);
		item->set_text(1, itos(entry.stats.call_count));
		item->set_text_alignment(1, HORIZONTAL_ALIGNMENT_RIGHT);

		// Average time.
		if (entry.stats.call_count > 0) {
			uint64_t avg = entry.stats.total_duration_usec / entry.stats.call_count;
			item->set_text(2, _format_duration(avg));
		} else {
			item->set_text(2, "-");
		}

		// Last called.
		if (entry.stats.last_call_time_usec > 0) {
			item->set_text(3, _format_timestamp(entry.stats.last_call_time_usec));
		} else {
			item->set_text(3, "-");
		}

		// Last status.
		if (entry.stats.last_was_error) {
			item->set_text(4, TTR("Error"));
			item->set_custom_color(4, Color(0.9, 0.3, 0.3));
		} else {
			item->set_text(4, TTR("OK"));
			item->set_custom_color(4, Color(0.4, 0.9, 0.4));
		}

		// Highlight recently called tools (within 3 seconds).
		if (entry.stats.last_call_time_usec > 0 &&
				(now_usec - entry.stats.last_call_time_usec) < 3000000) {
			for (int col = 0; col < 5; col++) {
				item->set_custom_bg_color(col, Color(0.3, 0.3, 0.15, 0.3));
			}
		}
	}
}

// =========================================================================
// Test Results Section -- UI Construction
// =========================================================================

void MCPStatusPanel::_build_test_section() {
	// ── Summary Bar ──────────────────────────────────────────────────
	test_summary_bar = memnew(HBoxContainer);
	test_summary_bar->set_h_size_flags(SIZE_EXPAND_FILL);
	add_child(test_summary_bar);

	test_title_label = memnew(Label);
	test_title_label->set_text(TTR("Test Results"));
	test_title_label->add_theme_color_override("font_color", Color(0.7, 0.7, 0.7));
	test_summary_bar->add_child(test_title_label);

	test_run_label = memnew(Label);
	test_run_label->set_text(TTR("No test runs yet"));
	test_run_label->add_theme_color_override("font_color", Color(0.7, 0.7, 0.7));
	test_summary_bar->add_child(test_run_label);

	test_passed_label = memnew(Label);
	test_passed_label->add_theme_color_override("font_color", Color(0.4, 0.9, 0.4));
	test_passed_label->set_visible(false);
	test_summary_bar->add_child(test_passed_label);

	test_failed_label = memnew(Label);
	test_failed_label->add_theme_color_override("font_color", Color(0.9, 0.3, 0.3));
	test_failed_label->set_visible(false);
	test_summary_bar->add_child(test_failed_label);

	test_errors_label = memnew(Label);
	test_errors_label->add_theme_color_override("font_color", Color(0.9, 0.8, 0.3));
	test_errors_label->set_visible(false);
	test_summary_bar->add_child(test_errors_label);

	test_skipped_label = memnew(Label);
	test_skipped_label->add_theme_color_override("font_color", Color(0.6, 0.6, 0.6));
	test_skipped_label->set_visible(false);
	test_summary_bar->add_child(test_skipped_label);

	test_duration_label = memnew(Label);
	test_duration_label->add_theme_color_override("font_color", Color(0.7, 0.7, 0.7));
	test_duration_label->set_visible(false);
	test_summary_bar->add_child(test_duration_label);

	// Spacer.
	Control *test_spacer = memnew(Control);
	test_spacer->set_h_size_flags(SIZE_EXPAND_FILL);
	test_summary_bar->add_child(test_spacer);

	test_collapse_button = memnew(Button);
	test_collapse_button->set_text(String::utf8("\u25BC")); // ▼
	test_collapse_button->set_flat(true);
	test_collapse_button->connect("pressed", callable_mp(this, &MCPStatusPanel::_on_test_collapse_pressed));
	test_summary_bar->add_child(test_collapse_button);

	// ── Results Tree ─────────────────────────────────────────────────
	test_results_tree = memnew(Tree);
	test_results_tree->set_columns(4);
	test_results_tree->set_column_titles_visible(true);
	test_results_tree->set_column_title(0, TTR("Name"));
	test_results_tree->set_column_title(1, TTR("Result"));
	test_results_tree->set_column_title(2, TTR("Duration"));
	test_results_tree->set_column_title(3, TTR("Message"));
	test_results_tree->set_column_custom_minimum_width(0, 300 * EDSCALE);
	test_results_tree->set_column_custom_minimum_width(1, 120 * EDSCALE);
	test_results_tree->set_column_custom_minimum_width(2, 70 * EDSCALE);
	test_results_tree->set_column_custom_minimum_width(3, 40 * EDSCALE);
	test_results_tree->set_column_expand(0, true);
	test_results_tree->set_column_expand(1, false);
	test_results_tree->set_column_expand(2, false);
	test_results_tree->set_column_expand(3, true);
	test_results_tree->set_hide_root(true);
	test_results_tree->set_custom_minimum_size(Size2(0, 150 * EDSCALE));
	test_results_tree->connect("item_activated", callable_mp(this, &MCPStatusPanel::_on_test_item_activated));
	add_child(test_results_tree);

	// ── History Bar ──────────────────────────────────────────────────
	test_history_bar = memnew(HBoxContainer);
	test_history_bar->set_h_size_flags(SIZE_EXPAND_FILL);
	test_history_bar->set_visible(false);
	add_child(test_history_bar);

	test_prev_button = memnew(Button);
	test_prev_button->set_text(String::utf8("\u25C0")); // ◀
	test_prev_button->set_flat(true);
	test_prev_button->connect("pressed", callable_mp(this, &MCPStatusPanel::_on_test_prev_pressed));
	test_history_bar->add_child(test_prev_button);

	test_next_button = memnew(Button);
	test_next_button->set_text(String::utf8("\u25B6")); // ▶
	test_next_button->set_flat(true);
	test_next_button->connect("pressed", callable_mp(this, &MCPStatusPanel::_on_test_next_pressed));
	test_history_bar->add_child(test_next_button);

	// Spacer.
	Control *history_spacer = memnew(Control);
	history_spacer->set_h_size_flags(SIZE_EXPAND_FILL);
	test_history_bar->add_child(history_spacer);

	test_clear_history_button = memnew(Button);
	test_clear_history_button->set_text(TTR("Clear History"));
	test_clear_history_button->connect("pressed", callable_mp(this, &MCPStatusPanel::_on_test_clear_history));
	test_history_bar->add_child(test_clear_history_button);
}

// =========================================================================
// Test Results -- Update Logic (fast path: every frame)
// =========================================================================

void MCPStatusPanel::_update_test_results() {
	if (!debugger_bridge) {
		return;
	}

	MCPTestEventBuffer *buffer = debugger_bridge->get_test_event_buffer();
	if (!buffer) {
		return;
	}

	Vector<MCPTestEvent> new_events = buffer->read_since(last_test_event_cursor, 100);

	for (int i = 0; i < new_events.size(); i++) {
		const MCPTestEvent &evt = new_events[i];
		last_test_event_cursor = evt.seq;

		switch (evt.type) {
			case MCPTestEvent::TEST_RUN_STARTED: {
				current_test_run_number = evt.run_number;
				viewing_run_number = current_test_run_number;
				_clear_test_tree();
				_set_test_summary_running(evt.run_number);
				if (!test_section_expanded) {
					_expand_test_section();
				}
			} break;

			case MCPTestEvent::TEST_METHOD_RESULT: {
				if (evt.run_number != viewing_run_number) {
					break;
				}
				_add_or_update_test_method_row(evt);
				_update_test_summary_counts();
			} break;

			case MCPTestEvent::TEST_FILE_COMPILE_ERROR: {
				if (evt.run_number != viewing_run_number) {
					break;
				}
				_add_compile_error_row(evt);
				_update_test_summary_counts();
			} break;

			case MCPTestEvent::TEST_RUN_COMPLETE: {
				_finalize_test_run(evt);
				_store_in_history(evt);
				_rebuild_history_bar();
			} break;
		}
	}

	// Pulsing animation for run badge while tests are running.
	if (is_test_running) {
		float pulse = 0.5f + 0.5f * Math::sin(OS::get_singleton()->get_ticks_usec() / 300000.0f);
		test_run_label->add_theme_color_override("font_color",
				Color(0.9, 0.8, 0.3).lerp(Color(1, 1, 1), pulse));
	}
}

void MCPStatusPanel::_clear_test_tree() {
	test_results_tree->clear();
	test_results_tree->create_item(); // Root item.
	running_test_passed = 0;
	running_test_failed = 0;
	running_test_errors = 0;
	running_test_skipped = 0;
	running_test_total = 0;
}

void MCPStatusPanel::_set_test_summary_running(int p_run_number) {
	is_test_running = true;

	test_run_label->set_text(TTR("[Run #") + itos(p_run_number) + "]");
	test_run_label->add_theme_color_override("font_color", Color(0.9, 0.8, 0.3));
	test_run_label->set_visible(true);

	test_passed_label->set_text(TTR("0 passed"));
	test_passed_label->set_visible(true);

	test_failed_label->set_visible(false);
	test_errors_label->set_visible(false);
	test_skipped_label->set_visible(false);
	test_duration_label->set_visible(false);
}

void MCPStatusPanel::_set_test_summary_complete() {
	is_test_running = false;
	test_run_label->remove_theme_color_override("font_color");
	test_run_label->add_theme_color_override("font_color", Color(0.7, 0.7, 0.7));
}

void MCPStatusPanel::_expand_test_section() {
	test_section_expanded = true;
	test_results_tree->set_visible(true);
	test_history_bar->set_visible(test_history.size() > 0);
	test_collapse_button->set_text(String::utf8("\u25BC")); // ▼
}

void MCPStatusPanel::_collapse_test_section() {
	test_section_expanded = false;
	test_results_tree->set_visible(false);
	test_history_bar->set_visible(false);
	test_collapse_button->set_text(String::utf8("\u25B6")); // ▶
}

// =========================================================================
// Test Results -- Tree Row Management
// =========================================================================

TreeItem *MCPStatusPanel::_find_file_row(const String &p_path) {
	TreeItem *root = test_results_tree->get_root();
	if (!root) {
		root = test_results_tree->create_item();
	}

	TreeItem *child = root->get_first_child();
	while (child) {
		Variant meta_v = child->get_metadata(0);
		if (meta_v.get_type() == Variant::DICTIONARY) {
			Dictionary meta = meta_v;
			if (String(meta.get("file_path", "")) == p_path) {
				return child;
			}
		}
		child = child->get_next();
	}

	// Not found -- create a new file row.
	TreeItem *file_row = test_results_tree->create_item(root);
	file_row->set_text(0, p_path);

	Dictionary meta;
	meta["file_path"] = p_path;
	meta["is_file_row"] = true;
	meta["method_count"] = 0;
	meta["passed_count"] = 0;
	meta["failed_count"] = 0;
	meta["error_count"] = 0;
	meta["skipped_count"] = 0;
	meta["total_duration_ms"] = 0;
	file_row->set_metadata(0, meta);

	return file_row;
}

void MCPStatusPanel::_add_or_update_test_method_row(const MCPTestEvent &p_event) {
	TreeItem *file_row = _find_file_row(p_event.file_path);

	// Create method child row.
	TreeItem *method_row = test_results_tree->create_item(file_row);

	// Determine icon and color based on status.
	String icon;
	Color row_color;
	if (p_event.status == "passed") {
		icon = String::utf8("\u2713"); // ✓
		row_color = Color(0.4, 0.9, 0.4);
	} else if (p_event.status == "failed") {
		icon = String::utf8("\u2717"); // ✗
		row_color = Color(0.9, 0.3, 0.3);
	} else if (p_event.status == "error") {
		icon = String::utf8("\u2717"); // ✗
		row_color = Color(0.9, 0.8, 0.3);
	} else if (p_event.status == "skipped" || p_event.status == "pending") {
		icon = String::utf8("\u25CB"); // ○
		row_color = Color(0.6, 0.6, 0.6);
	} else if (p_event.status == "timeout") {
		icon = String::utf8("\u23F1"); // ⏱
		row_color = Color(0.9, 0.8, 0.3);
	} else {
		icon = "?";
		row_color = Color(0.8, 0.8, 0.8);
	}

	// Column 0: icon + method name.
	method_row->set_text(0, icon + " " + p_event.method_name);
	method_row->set_custom_color(0, row_color);

	// Column 1: empty (result summary lives on the file row).
	method_row->set_text(1, "");

	// Column 2: formatted duration.
	if (p_event.duration_ms >= 1000) {
		method_row->set_text(2, String::num((double)p_event.duration_ms / 1000.0, 2) + "s");
	} else {
		method_row->set_text(2, itos(p_event.duration_ms) + "ms");
	}

	// Column 3: assertion message (for failures/errors only).
	if (p_event.status == "failed" || p_event.status == "error" || p_event.status == "timeout") {
		method_row->set_text(3, p_event.message);
		method_row->set_custom_color(3, row_color);
	}

	// Store metadata on the method row for double-click handling.
	Dictionary method_meta;
	method_meta["file_path"] = p_event.file_path;
	method_meta["error_file"] = p_event.error_file;
	method_meta["error_line"] = p_event.error_line;
	method_meta["method_name"] = p_event.method_name;
	method_meta["is_file_row"] = false;
	method_row->set_metadata(0, method_meta);

	// Update running counters.
	running_test_total++;
	if (p_event.status == "passed") {
		running_test_passed++;
	} else if (p_event.status == "failed") {
		running_test_failed++;
	} else if (p_event.status == "error" || p_event.status == "timeout") {
		running_test_errors++;
	} else if (p_event.status == "skipped" || p_event.status == "pending") {
		running_test_skipped++;
	}

	// Update the parent file row's result summary.
	Variant file_meta_v = file_row->get_metadata(0);
	if (file_meta_v.get_type() == Variant::DICTIONARY) {
		Dictionary file_meta = file_meta_v;
		int method_count = (int)file_meta.get("method_count", 0) + 1;
		int passed_count = (int)file_meta.get("passed_count", 0);
		int failed_count = (int)file_meta.get("failed_count", 0);
		int error_count = (int)file_meta.get("error_count", 0);
		int skipped_count = (int)file_meta.get("skipped_count", 0);
		int total_dur = (int)file_meta.get("total_duration_ms", 0) + p_event.duration_ms;

		if (p_event.status == "passed") {
			passed_count++;
		} else if (p_event.status == "failed") {
			failed_count++;
		} else if (p_event.status == "error" || p_event.status == "timeout") {
			error_count++;
		} else if (p_event.status == "skipped" || p_event.status == "pending") {
			skipped_count++;
		}

		file_meta["method_count"] = method_count;
		file_meta["passed_count"] = passed_count;
		file_meta["failed_count"] = failed_count;
		file_meta["error_count"] = error_count;
		file_meta["skipped_count"] = skipped_count;
		file_meta["total_duration_ms"] = total_dur;
		file_row->set_metadata(0, file_meta);

		// Build result summary text for the file row.
		String result_text = itos(passed_count) + "/" + itos(method_count) + " passed";
		Color file_result_color = Color(0.4, 0.9, 0.4);

		if (failed_count > 0) {
			result_text += ", " + itos(failed_count) + " FAILED";
			file_result_color = Color(0.9, 0.3, 0.3);
		}
		if (error_count > 0) {
			result_text += ", " + itos(error_count) + " error" + (error_count > 1 ? "s" : "");
			if (failed_count == 0) {
				file_result_color = Color(0.9, 0.8, 0.3);
			}
		}

		file_row->set_text(1, result_text);
		file_row->set_custom_color(1, file_result_color);

		// Duration for file row.
		if (total_dur >= 1000) {
			file_row->set_text(2, String::num((double)total_dur / 1000.0, 2) + "s");
		} else {
			file_row->set_text(2, itos(total_dur) + "ms");
		}

		// Tint file row background if any failures.
		if (failed_count > 0 || error_count > 0) {
			for (int col = 0; col < 4; col++) {
				file_row->set_custom_bg_color(col, Color(0.15, 0.08, 0.08, 0.3));
			}
		}
	}
}

void MCPStatusPanel::_add_compile_error_row(const MCPTestEvent &p_event) {
	TreeItem *root = test_results_tree->get_root();
	if (!root) {
		root = test_results_tree->create_item();
	}

	// Create file row with compile error.
	TreeItem *file_row = test_results_tree->create_item(root);
	file_row->set_text(0, p_event.file_path);
	file_row->set_custom_color(0, Color(0.9, 0.3, 0.3));
	file_row->set_text(1, TTR("COMPILE ERROR"));
	file_row->set_custom_color(1, Color(0.9, 0.3, 0.3));

	Dictionary file_meta;
	file_meta["file_path"] = p_event.file_path;
	file_meta["is_file_row"] = true;
	file_meta["is_compile_error"] = true;
	file_row->set_metadata(0, file_meta);

	// Tint background red.
	for (int col = 0; col < 4; col++) {
		file_row->set_custom_bg_color(col, Color(0.15, 0.08, 0.08, 0.3));
	}

	// Add child rows for each error.
	for (int i = 0; i < p_event.compile_errors.size(); i++) {
		Variant err_v = p_event.compile_errors[i];
		if (err_v.get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary err = err_v;
		int line = err.get("line", 0);
		int column = err.get("column", 0);
		String message = err.get("message", "");

		TreeItem *err_row = test_results_tree->create_item(file_row);
		err_row->set_text(0, "Line " + itos(line) + ", Col " + itos(column) + ": " + message);
		err_row->set_custom_color(0, Color(0.9, 0.3, 0.3));

		Dictionary err_meta;
		err_meta["file_path"] = p_event.file_path;
		err_meta["error_file"] = p_event.file_path;
		err_meta["error_line"] = line;
		err_meta["is_file_row"] = false;
		err_row->set_metadata(0, err_meta);
	}

	// Increment error counter.
	running_test_errors++;
}

void MCPStatusPanel::_update_test_summary_counts() {
	test_passed_label->set_text(itos(running_test_passed) + " passed");
	test_passed_label->set_visible(true);

	if (running_test_failed > 0) {
		test_failed_label->set_text(itos(running_test_failed) + " failed");
		test_failed_label->set_visible(true);
	} else {
		test_failed_label->set_visible(false);
	}

	if (running_test_errors > 0) {
		test_errors_label->set_text(itos(running_test_errors) + " error" + (running_test_errors > 1 ? "s" : ""));
		test_errors_label->set_visible(true);
	} else {
		test_errors_label->set_visible(false);
	}

	if (running_test_skipped > 0) {
		test_skipped_label->set_text(itos(running_test_skipped) + " skipped");
		test_skipped_label->set_visible(true);
	} else {
		test_skipped_label->set_visible(false);
	}
}

void MCPStatusPanel::_finalize_test_run(const MCPTestEvent &p_event) {
	is_test_running = false;

	// Update run label to final state (no more pulsing).
	test_run_label->remove_theme_color_override("font_color");
	test_run_label->add_theme_color_override("font_color", Color(0.7, 0.7, 0.7));

	// Show final duration.
	if (p_event.total_duration_ms >= 1000) {
		test_duration_label->set_text(String::num((double)p_event.total_duration_ms / 1000.0, 2) + "s");
	} else {
		test_duration_label->set_text(itos(p_event.total_duration_ms) + "ms");
	}
	test_duration_label->set_visible(true);

	// Update final summary counts from the complete event.
	running_test_passed = p_event.passed;
	running_test_failed = p_event.failed;
	running_test_errors = p_event.errors;
	running_test_skipped = p_event.skipped;
	running_test_total = p_event.total;
	_update_test_summary_counts();
}

// =========================================================================
// Test Results -- History Management
// =========================================================================

void MCPStatusPanel::_store_in_history(const MCPTestEvent &p_event) {
	TestRunHistory entry;
	entry.run_number = p_event.run_number;
	entry.total = p_event.total;
	entry.passed = p_event.passed;
	entry.failed = p_event.failed;
	entry.errors = p_event.errors;
	entry.skipped = p_event.skipped;
	entry.duration_ms = p_event.total_duration_ms;

	// Capture current tree state as file_results for history replay.
	TreeItem *root = test_results_tree->get_root();
	if (root) {
		Array file_results;
		TreeItem *file_item = root->get_first_child();
		while (file_item) {
			Variant meta_v = file_item->get_metadata(0);
			if (meta_v.get_type() == Variant::DICTIONARY) {
				Dictionary file_data = meta_v;

				// Capture children (method rows or error rows).
				Array children_data;
				TreeItem *child = file_item->get_first_child();
				while (child) {
					Dictionary child_data;
					child_data["text_0"] = child->get_text(0);
					child_data["text_1"] = child->get_text(1);
					child_data["text_2"] = child->get_text(2);
					child_data["text_3"] = child->get_text(3);
					Variant child_meta_v = child->get_metadata(0);
					if (child_meta_v.get_type() == Variant::DICTIONARY) {
						child_data["meta"] = child_meta_v;
					}
					children_data.push_back(child_data);
					child = child->get_next();
				}

				Dictionary stored;
				stored["meta"] = file_data;
				stored["text_0"] = file_item->get_text(0);
				stored["text_1"] = file_item->get_text(1);
				stored["text_2"] = file_item->get_text(2);
				stored["children"] = children_data;
				file_results.push_back(stored);
			}
			file_item = file_item->get_next();
		}
		entry.file_results = file_results;
	}

	test_history.push_back(entry);

	// Evict oldest if over limit.
	while (test_history.size() > MAX_TEST_HISTORY) {
		test_history.remove_at(0);
	}
}

void MCPStatusPanel::_rebuild_history_bar() {
	if (test_history.size() == 0) {
		test_history_bar->set_visible(false);
		return;
	}

	test_history_bar->set_visible(test_section_expanded);

	// Enable/disable prev/next buttons.
	int viewing_idx = -1;
	for (int i = 0; i < test_history.size(); i++) {
		if (test_history[i].run_number == viewing_run_number) {
			viewing_idx = i;
			break;
		}
	}

	test_prev_button->set_disabled(viewing_idx <= 0);
	test_next_button->set_disabled(viewing_idx < 0 || viewing_idx >= test_history.size() - 1);
}

void MCPStatusPanel::_load_history_run(int p_run_number) {
	// Find the history entry.
	int idx = -1;
	for (int i = 0; i < test_history.size(); i++) {
		if (test_history[i].run_number == p_run_number) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		return;
	}

	viewing_run_number = p_run_number;
	const TestRunHistory &hist = test_history[idx];

	// Clear and rebuild tree from stored history.
	test_results_tree->clear();
	TreeItem *root = test_results_tree->create_item();

	for (int i = 0; i < hist.file_results.size(); i++) {
		Variant entry_v = hist.file_results[i];
		if (entry_v.get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary entry = entry_v;

		TreeItem *file_row = test_results_tree->create_item(root);
		file_row->set_text(0, entry.get("text_0", ""));
		file_row->set_text(1, entry.get("text_1", ""));
		file_row->set_text(2, entry.get("text_2", ""));

		Variant meta_v = entry.get("meta", Variant());
		if (meta_v.get_type() == Variant::DICTIONARY) {
			file_row->set_metadata(0, meta_v);

			// Restore file row colors.
			Dictionary meta = meta_v;
			int failed_count = (int)meta.get("failed_count", 0);
			int error_count = (int)meta.get("error_count", 0);
			bool is_compile_error = (bool)meta.get("is_compile_error", false);

			if (is_compile_error) {
				file_row->set_custom_color(0, Color(0.9, 0.3, 0.3));
				file_row->set_custom_color(1, Color(0.9, 0.3, 0.3));
			}

			if (failed_count > 0 || error_count > 0 || is_compile_error) {
				for (int col = 0; col < 4; col++) {
					file_row->set_custom_bg_color(col, Color(0.15, 0.08, 0.08, 0.3));
				}
				if (!is_compile_error) {
					file_row->set_custom_color(1, (failed_count > 0) ? Color(0.9, 0.3, 0.3) : Color(0.9, 0.8, 0.3));
				}
			} else {
				file_row->set_custom_color(1, Color(0.4, 0.9, 0.4));
			}
		}

		// Rebuild children.
		Array children = entry.get("children", Array());
		for (int j = 0; j < children.size(); j++) {
			Variant child_v = children[j];
			if (child_v.get_type() != Variant::DICTIONARY) {
				continue;
			}
			Dictionary child_data = child_v;

			TreeItem *child_row = test_results_tree->create_item(file_row);
			child_row->set_text(0, child_data.get("text_0", ""));
			child_row->set_text(1, child_data.get("text_1", ""));
			child_row->set_text(2, child_data.get("text_2", ""));
			child_row->set_text(3, child_data.get("text_3", ""));

			Variant child_meta_v = child_data.get("meta", Variant());
			if (child_meta_v.get_type() == Variant::DICTIONARY) {
				child_row->set_metadata(0, child_meta_v);
			}

			// Restore method row colors based on icon prefix.
			String text_0 = child_data.get("text_0", "");
			if (text_0.begins_with(String::utf8("\u2713"))) {
				child_row->set_custom_color(0, Color(0.4, 0.9, 0.4));
			} else if (text_0.begins_with(String::utf8("\u2717"))) {
				// Check if it's error (yellow) or failure (red) via message presence.
				String text_3 = child_data.get("text_3", "");
				if (!text_3.is_empty()) {
					// Look at the metadata to determine error vs failure.
					if (child_meta_v.get_type() == Variant::DICTIONARY) {
						// Default to red for failed.
						child_row->set_custom_color(0, Color(0.9, 0.3, 0.3));
						child_row->set_custom_color(3, Color(0.9, 0.3, 0.3));
					}
				} else {
					child_row->set_custom_color(0, Color(0.9, 0.3, 0.3));
				}
			} else if (text_0.begins_with(String::utf8("\u25CB"))) {
				child_row->set_custom_color(0, Color(0.6, 0.6, 0.6));
			} else if (text_0.begins_with(String::utf8("\u23F1"))) {
				child_row->set_custom_color(0, Color(0.9, 0.8, 0.3));
				child_row->set_custom_color(3, Color(0.9, 0.8, 0.3));
			} else if (text_0.begins_with("Line ")) {
				// Compile error child row.
				child_row->set_custom_color(0, Color(0.9, 0.3, 0.3));
			}
		}
	}

	// Update summary bar for the loaded history run.
	test_run_label->set_text(TTR("[Run #") + itos(p_run_number) + "]");
	test_run_label->remove_theme_color_override("font_color");
	test_run_label->add_theme_color_override("font_color", Color(0.7, 0.7, 0.7));

	running_test_passed = hist.passed;
	running_test_failed = hist.failed;
	running_test_errors = hist.errors;
	running_test_skipped = hist.skipped;
	running_test_total = hist.total;
	_update_test_summary_counts();

	if (hist.duration_ms >= 1000) {
		test_duration_label->set_text(String::num((double)hist.duration_ms / 1000.0, 2) + "s");
	} else {
		test_duration_label->set_text(itos(hist.duration_ms) + "ms");
	}
	test_duration_label->set_visible(true);

	_rebuild_history_bar();
}

// =========================================================================
// Test Results -- UI Callbacks
// =========================================================================

void MCPStatusPanel::_on_test_item_activated() {
	TreeItem *selected = test_results_tree->get_selected();
	if (!selected) {
		return;
	}

	Variant meta_v = selected->get_metadata(0);
	if (meta_v.get_type() != Variant::DICTIONARY) {
		return;
	}

	Dictionary meta = meta_v;
	String file = meta.get("error_file", "");
	int line = meta.get("error_line", 0);

	if (file.is_empty() || line <= 0) {
		// Fallback to the test file itself.
		file = meta.get("file_path", "");
		line = 1;
	}

	if (!file.is_empty()) {
		Ref<Script> script = ResourceLoader::load(file);
		if (script.is_valid()) {
			EditorInterface::get_singleton()->edit_script(script, line);
		}
	}
}

void MCPStatusPanel::_on_test_collapse_pressed() {
	if (test_section_expanded) {
		_collapse_test_section();
	} else {
		_expand_test_section();
	}
}

void MCPStatusPanel::_on_test_prev_pressed() {
	int viewing_idx = -1;
	for (int i = 0; i < test_history.size(); i++) {
		if (test_history[i].run_number == viewing_run_number) {
			viewing_idx = i;
			break;
		}
	}

	if (viewing_idx > 0) {
		_load_history_run(test_history[viewing_idx - 1].run_number);
	}
}

void MCPStatusPanel::_on_test_next_pressed() {
	int viewing_idx = -1;
	for (int i = 0; i < test_history.size(); i++) {
		if (test_history[i].run_number == viewing_run_number) {
			viewing_idx = i;
			break;
		}
	}

	if (viewing_idx >= 0 && viewing_idx < test_history.size() - 1) {
		_load_history_run(test_history[viewing_idx + 1].run_number);
	}
}

void MCPStatusPanel::_on_test_clear_history() {
	test_history.clear();
	_clear_test_tree();
	test_history_bar->set_visible(false);

	test_run_label->set_text(TTR("No test runs yet"));
	test_run_label->remove_theme_color_override("font_color");
	test_run_label->add_theme_color_override("font_color", Color(0.7, 0.7, 0.7));

	test_passed_label->set_visible(false);
	test_failed_label->set_visible(false);
	test_errors_label->set_visible(false);
	test_skipped_label->set_visible(false);
	test_duration_label->set_visible(false);

	current_test_run_number = 0;
	viewing_run_number = 0;
	is_test_running = false;
}

// =========================================================================
// UI Callbacks
// =========================================================================

void MCPStatusPanel::_on_toggle_button_pressed() {
	if (server_plugin) {
		server_plugin->toggle_server();
	}
}

void MCPStatusPanel::_on_clear_pressed() {
	if (protocol) {
		protocol->get_event_buffer()->clear();
	}
	request_log_tree->clear();
	request_log_tree->create_item(); // Re-create root.
	log_row_count = 0;
	last_event_cursor = 0;
}

void MCPStatusPanel::_on_export_pressed() {
	if (!protocol) {
		return;
	}

	MCPEventBuffer *buffer = protocol->get_event_buffer();
	if (!buffer) {
		return;
	}

	Vector<MCPRequestEvent> all_events = buffer->read_since(0, MCPEventBuffer::CAPACITY);

	// Build a JSON array of events.
	Array json_events;
	for (int i = 0; i < all_events.size(); i++) {
		const MCPRequestEvent &evt = all_events[i];
		Dictionary d;
		d["seq"] = (int64_t)evt.seq;
		d["timestamp_usec"] = (int64_t)evt.timestamp_usec;
		d["method"] = evt.method;
		d["session_id"] = evt.session_id;
		d["client_ip"] = evt.client_ip;
		d["http_status"] = evt.http_status;
		d["is_error"] = evt.is_error;
		d["duration_usec"] = (int64_t)evt.duration_usec;
		d["tool_name"] = evt.tool_name;
		d["request_json"] = evt.request_json;
		d["response_json"] = evt.response_json;
		json_events.push_back(d);
	}

	String json_str = JSON::stringify(json_events, "\t");

	// Write to user data directory.
	String export_path = "user://mcp_log_export.json";
	Ref<FileAccess> f = FileAccess::open(export_path, FileAccess::WRITE);
	if (f.is_valid()) {
		f->store_string(json_str);
		f.unref();
		print_line("[MCP] Log exported to: " + export_path);
	} else {
		ERR_PRINT("[MCP] Failed to export log to: " + export_path);
	}
}

void MCPStatusPanel::_on_filter_changed(int p_index) {
	current_method_filter_idx = method_filter->get_selected();
	current_status_filter_idx = status_filter->get_selected();
	_rebuild_log_from_buffer();
}

void MCPStatusPanel::_on_search_changed(const String &p_text) {
	current_search_text = p_text;
	_rebuild_log_from_buffer();
}

void MCPStatusPanel::_on_log_item_activated() {
	TreeItem *selected = request_log_tree->get_selected();
	if (!selected) {
		return;
	}

	Variant meta_v = selected->get_metadata(0);
	if (meta_v.get_type() != Variant::DICTIONARY) {
		return;
	}

	Dictionary meta = meta_v;
	bool expanded = meta.get("expanded", false);

	if (expanded) {
		// Collapse: remove child items.
		TreeItem *child = selected->get_first_child();
		while (child) {
			TreeItem *next = child->get_next();
			memdelete(child); // Automatically removed from parent.
			child = next;
			log_row_count--;
		}
		meta["expanded"] = false;
		selected->set_text(5, "[>]");
	} else {
		// Expand: add child items with JSON content.
		String req_json = meta.get("request_json", "");
		String res_json = meta.get("response_json", "");

		if (!req_json.is_empty()) {
			TreeItem *req_item = request_log_tree->create_item(selected);
			// Try to pretty-print JSON.
			JSON json;
			if (json.parse(req_json) == OK) {
				req_item->set_text(0, TTR("Request:"));
				req_item->set_text(1, JSON::stringify(json.get_data(), "  "));
			} else {
				req_item->set_text(0, TTR("Request:"));
				req_item->set_text(1, req_json);
			}
			req_item->set_custom_color(0, Color(0.6, 0.8, 1.0));
			log_row_count++;
		}

		if (!res_json.is_empty()) {
			TreeItem *res_item = request_log_tree->create_item(selected);
			JSON json;
			if (json.parse(res_json) == OK) {
				res_item->set_text(0, TTR("Response:"));
				res_item->set_text(1, JSON::stringify(json.get_data(), "  "));
			} else {
				res_item->set_text(0, TTR("Response:"));
				res_item->set_text(1, res_json);
			}
			res_item->set_custom_color(0, Color(0.6, 1.0, 0.8));
			log_row_count++;
		}

		if (req_json.is_empty() && res_json.is_empty()) {
			TreeItem *no_data = request_log_tree->create_item(selected);
			no_data->set_text(0, TTR("(no JSON captured - panel was hidden)"));
			no_data->set_custom_color(0, Color(0.6, 0.6, 0.6));
			log_row_count++;
		}

		meta["expanded"] = true;
		selected->set_text(5, "[v]");
	}

	selected->set_metadata(0, meta);
}

#endif // TOOLS_ENABLED
