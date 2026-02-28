/**************************************************************************/
/*  mcp_test_panel.h                                                      */
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
class Label;
class LineEdit;
class MCPDebuggerBridge;
class PopupMenu;
class Tree;
class TreeItem;

class MCPTestPanel : public VBoxContainer {
	GDCLASS(MCPTestPanel, VBoxContainer)

private:
	MCPDebuggerBridge *debugger_bridge = nullptr;

	// -- Toolbar --
	Button *run_all_button = nullptr;
	Button *run_selected_button = nullptr;
	Button *rerun_failed_button = nullptr;
	Button *stop_button = nullptr;
	Button *refresh_button = nullptr;
	LineEdit *filter_edit = nullptr;

	// -- Test Tree --
	Tree *test_tree = nullptr;
	PopupMenu *context_menu = nullptr;

	// -- Summary Bar --
	HBoxContainer *summary_bar = nullptr;
	Label *passed_label = nullptr;
	Label *failed_label = nullptr;
	Label *errors_label = nullptr;
	Label *skipped_label = nullptr;
	Label *duration_label = nullptr;
	Label *run_number_label = nullptr;
	Label *status_label = nullptr;

	// -- State --
	String test_root = "res://tests/";
	bool is_running = false;
	uint64_t last_event_cursor = 0;
	int current_run_number = 0;

	// Discovered test structure (cached for filtering and result mapping).
	struct DiscoveredMethod {
		String name;
		TreeItem *tree_item = nullptr;
	};
	struct DiscoveredFile {
		String path;
		Vector<DiscoveredMethod> methods;
		TreeItem *tree_item = nullptr;
	};
	Vector<DiscoveredFile> discovered_files;

	// Failed files from last run (for rerun).
	Vector<String> last_failed_files;

	// Context menu target.
	TreeItem *context_menu_item = nullptr;

	// -- UI Construction --
	void _build_ui();

	// -- Discovery --
	void _discover_tests();
	TreeItem *_get_or_create_dir_item(TreeItem *p_root, const String &p_dir_path);
	void _update_tree_filter(const String &p_filter);

	// -- Execution --
	void _run_all();
	void _run_selected();
	void _run_files(const Vector<String> &p_files, const String &p_filter = "");
	void _rerun_failed();
	void _stop_tests();

	// -- Result Processing --
	void _process_test_events();
	void _update_method_result(const MCPTestEvent &p_event);
	void _on_run_started(const MCPTestEvent &p_event);
	void _on_run_complete(const MCPTestEvent &p_event);
	void _on_compile_error(const MCPTestEvent &p_event);
	void _update_summary();
	void _reset_tree_status();

	// -- Tree Interaction --
	void _on_item_activated();
	void _on_item_rmb(const Vector2 &p_pos);
	void _on_context_menu_id(int p_id);
	void _on_filter_changed(const String &p_text);
	void _on_refresh_pressed();
	void _navigate_to_item(TreeItem *p_item);

	// -- Helpers --
	TreeItem *_find_file_tree_item(const String &p_path);
	TreeItem *_find_method_tree_item(const String &p_file, const String &p_method);
	int _find_discovered_file_index(const String &p_path);
	void _update_file_row_summary(TreeItem *p_file_item);

	void _notification(int p_what);

protected:
	static void _bind_methods();

public:
	void set_debugger_bridge(MCPDebuggerBridge *p_bridge) { debugger_bridge = p_bridge; }

	// Public API for MCPServerPlugin integration.
	void run_file(const String &p_path);
	void run_all_tests();
	void rerun_failed_tests();
	String get_test_root() const { return test_root; }
	void set_test_root(const String &p_root);

	MCPTestPanel();
	~MCPTestPanel();
};

#endif // TOOLS_ENABLED
