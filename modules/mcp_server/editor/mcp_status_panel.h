/**************************************************************************/
/*  mcp_status_panel.h                                                    */
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

#ifdef TOOLS_ENABLED

#include "mcp_status_data.h"

#include "scene/gui/box_container.h"

class Button;
class CheckBox;
class Label;
class LineEdit;
class MCPDebuggerBridge;
class MCPProtocol;
class MCPServerPlugin;
class OptionButton;
class TextureRect;
class Tree;
class TreeItem;

class MCPStatusPanel : public VBoxContainer {
	GDCLASS(MCPStatusPanel, VBoxContainer)

private:
	// External references (not owned).
	MCPProtocol *protocol = nullptr;
	MCPDebuggerBridge *debugger_bridge = nullptr;
	MCPServerPlugin *server_plugin = nullptr;

	// -- Server Status --
	TextureRect *status_dot = nullptr;
	Label *status_label = nullptr;
	Label *address_label = nullptr;
	Label *uptime_label = nullptr;
	Label *stats_label = nullptr;
	Button *toggle_button = nullptr;

	// -- Debugger Bridge --
	TextureRect *game_dot = nullptr;
	Label *game_label = nullptr;
	Label *pending_label = nullptr;
	Label *heartbeat_label = nullptr;

	// -- Clients --
	Label *client_count_label = nullptr;
	Tree *clients_tree = nullptr;

	// -- Request Log --
	LineEdit *search_edit = nullptr;
	OptionButton *method_filter = nullptr;
	OptionButton *status_filter = nullptr;
	CheckBox *auto_scroll_check = nullptr;
	Button *clear_button = nullptr;
	Button *export_button = nullptr;
	Tree *request_log_tree = nullptr;

	// -- Tool Summary --
	Tree *tool_summary_tree = nullptr;

	// -- State --
	uint64_t last_slow_update = 0;
	uint64_t last_event_cursor = 0;
	int64_t last_heartbeat_frame = -1;
	uint64_t last_heartbeat_change_time = 0;
	int log_row_count = 0;

	// Cached filter state.
	String current_search_text;
	int current_method_filter_idx = 0;
	int current_status_filter_idx = 0;

	// Observed method names for dynamic filter population.
	Vector<String> observed_methods;

	// Cached status dot textures (8x8 programmatic).
	Ref<Texture2D> dot_green;
	Ref<Texture2D> dot_red;
	Ref<Texture2D> dot_yellow;
	Ref<Texture2D> dot_gray;

	// -- Methods --
	void _build_ui();
	void _create_dot_textures();
	Ref<Texture2D> _make_dot_texture(const Color &p_color);

	void _update_server_status();
	void _update_clients();
	void _update_debugger_bridge();
	void _update_request_log();
	void _update_tool_summary();

	void _on_toggle_button_pressed();
	void _on_clear_pressed();
	void _on_export_pressed();
	void _on_filter_changed(int p_index);
	void _on_search_changed(const String &p_text);
	void _on_log_item_activated();

	bool _event_matches_filters(const MCPRequestEvent &p_event) const;
	String _format_timestamp(uint64_t p_usec) const;
	String _format_duration(uint64_t p_usec) const;
	String _format_relative_time(uint64_t p_from_usec, uint64_t p_now_usec) const;
	String _format_uptime(uint64_t p_seconds) const;
	Color _color_for_status(int p_http_status) const;
	String _status_text(int p_http_status) const;

	TreeItem *_add_log_entry(const MCPRequestEvent &p_event);
	void _rebuild_log_from_buffer();
	void _prune_log_rows();

	void _notification(int p_what);

protected:
	static void _bind_methods();

public:
	void set_protocol(MCPProtocol *p_protocol) { protocol = p_protocol; }
	void set_debugger_bridge(MCPDebuggerBridge *p_bridge) { debugger_bridge = p_bridge; }
	void set_server_plugin(MCPServerPlugin *p_plugin) { server_plugin = p_plugin; }

	MCPStatusPanel();
	~MCPStatusPanel();
};

#endif // TOOLS_ENABLED
