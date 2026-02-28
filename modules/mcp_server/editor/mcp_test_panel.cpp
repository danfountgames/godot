/**************************************************************************/
/*  mcp_test_panel.cpp                                                    */
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

#include "mcp_test_panel.h"

#include "../mcp_debugger_bridge.h"
#include "../tools/mcp_testing_tools.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/os/os.h"
#include "editor/settings/editor_settings.h"
#include "servers/display/display_server.h"
#include "editor/editor_interface.h"
#include "editor/run/editor_run_bar.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/separator.h"
#include "scene/gui/tree.h"

// Context menu item IDs.
enum ContextMenuID {
	CTX_RUN_FILE = 0,
	CTX_RUN_METHOD = 1,
	CTX_OPEN_IN_EDITOR = 2,
	CTX_COPY_PATH = 3,
};

// =========================================================================
// Constructor / Destructor
// =========================================================================

MCPTestPanel::MCPTestPanel() {
	set_name("MCPTestPanel");
	_build_ui();
}

MCPTestPanel::~MCPTestPanel() {
}

// =========================================================================
// Binding
// =========================================================================

void MCPTestPanel::_bind_methods() {
}

// =========================================================================
// UI Construction
// =========================================================================

void MCPTestPanel::_build_ui() {
	// ── Toolbar ──────────────────────────────────────────────────────
	HBoxContainer *toolbar = memnew(HBoxContainer);
	toolbar->set_h_size_flags(SIZE_EXPAND_FILL);
	add_child(toolbar);

	run_all_button = memnew(Button);
	run_all_button->set_text(TTR("Run All"));
	run_all_button->set_tooltip_text(TTR("Run all discovered test files (Alt+Shift+T)"));
	run_all_button->set_shortcut(ED_GET_SHORTCUT("mcp_server/run_all_tests"));
	run_all_button->connect("pressed", callable_mp(this, &MCPTestPanel::_run_all));
	toolbar->add_child(run_all_button);

	run_selected_button = memnew(Button);
	run_selected_button->set_text(TTR("Run Selected"));
	run_selected_button->set_tooltip_text(TTR("Run the selected test file or method"));
	run_selected_button->connect("pressed", callable_mp(this, &MCPTestPanel::_run_selected));
	toolbar->add_child(run_selected_button);

	rerun_failed_button = memnew(Button);
	rerun_failed_button->set_text(TTR("Rerun Failed"));
	rerun_failed_button->set_tooltip_text(TTR("Re-run test files that had failures (Alt+Shift+R)"));
	rerun_failed_button->set_shortcut(ED_GET_SHORTCUT("mcp_server/rerun_failed"));
	rerun_failed_button->connect("pressed", callable_mp(this, &MCPTestPanel::_rerun_failed));
	rerun_failed_button->set_visible(false);
	toolbar->add_child(rerun_failed_button);

	stop_button = memnew(Button);
	stop_button->set_text(TTR("Stop"));
	stop_button->set_tooltip_text(TTR("Stop the running test process"));
	stop_button->connect("pressed", callable_mp(this, &MCPTestPanel::_stop_tests));
	stop_button->set_visible(false);
	toolbar->add_child(stop_button);

	toolbar->add_child(memnew(VSeparator));

	refresh_button = memnew(Button);
	refresh_button->set_text(TTR("Refresh"));
	refresh_button->set_tooltip_text(TTR("Re-scan for test files"));
	refresh_button->connect("pressed", callable_mp(this, &MCPTestPanel::_on_refresh_pressed));
	toolbar->add_child(refresh_button);

	// Spacer.
	Control *spacer = memnew(Control);
	spacer->set_h_size_flags(SIZE_EXPAND_FILL);
	toolbar->add_child(spacer);

	filter_edit = memnew(LineEdit);
	filter_edit->set_placeholder(TTR("Filter tests..."));
	filter_edit->set_custom_minimum_size(Size2(200 * EDSCALE, 0));
	filter_edit->connect("text_changed", callable_mp(this, &MCPTestPanel::_on_filter_changed));
	toolbar->add_child(filter_edit);

	// ── Test Tree ────────────────────────────────────────────────────
	test_tree = memnew(Tree);
	test_tree->set_columns(4);
	test_tree->set_column_titles_visible(true);
	test_tree->set_column_title(0, TTR("Test"));
	test_tree->set_column_title(1, TTR("Status"));
	test_tree->set_column_title(2, TTR("Duration"));
	test_tree->set_column_title(3, TTR("Message"));
	test_tree->set_column_custom_minimum_width(0, 300 * EDSCALE);
	test_tree->set_column_custom_minimum_width(1, 100 * EDSCALE);
	test_tree->set_column_custom_minimum_width(2, 70 * EDSCALE);
	test_tree->set_column_custom_minimum_width(3, 40 * EDSCALE);
	test_tree->set_column_expand(0, true);
	test_tree->set_column_expand(1, false);
	test_tree->set_column_expand(2, false);
	test_tree->set_column_expand(3, true);
	test_tree->set_hide_root(true);
	test_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	test_tree->set_allow_rmb_select(true);
	test_tree->connect("item_activated", callable_mp(this, &MCPTestPanel::_on_item_activated));
	test_tree->connect("item_mouse_selected", callable_mp(this, &MCPTestPanel::_on_item_rmb));
	add_child(test_tree);

	// ── Context Menu ─────────────────────────────────────────────────
	context_menu = memnew(PopupMenu);
	context_menu->connect("id_pressed", callable_mp(this, &MCPTestPanel::_on_context_menu_id));
	add_child(context_menu);

	// ── Summary Bar ──────────────────────────────────────────────────
	summary_bar = memnew(HBoxContainer);
	summary_bar->set_h_size_flags(SIZE_EXPAND_FILL);
	add_child(summary_bar);

	status_label = memnew(Label);
	status_label->set_text(TTR("No tests discovered yet. Click Refresh."));
	status_label->add_theme_color_override("font_color", Color(0.6, 0.6, 0.6));
	summary_bar->add_child(status_label);

	// Spacer.
	Control *summary_spacer = memnew(Control);
	summary_spacer->set_h_size_flags(SIZE_EXPAND_FILL);
	summary_bar->add_child(summary_spacer);

	passed_label = memnew(Label);
	passed_label->add_theme_color_override("font_color", Color(0.4, 0.9, 0.4));
	passed_label->set_visible(false);
	summary_bar->add_child(passed_label);

	failed_label = memnew(Label);
	failed_label->add_theme_color_override("font_color", Color(0.9, 0.3, 0.3));
	failed_label->set_visible(false);
	summary_bar->add_child(failed_label);

	errors_label = memnew(Label);
	errors_label->add_theme_color_override("font_color", Color(0.9, 0.8, 0.3));
	errors_label->set_visible(false);
	summary_bar->add_child(errors_label);

	skipped_label = memnew(Label);
	skipped_label->add_theme_color_override("font_color", Color(0.6, 0.6, 0.6));
	skipped_label->set_visible(false);
	summary_bar->add_child(skipped_label);

	duration_label = memnew(Label);
	duration_label->add_theme_color_override("font_color", Color(0.7, 0.7, 0.7));
	duration_label->set_visible(false);
	summary_bar->add_child(duration_label);

	run_number_label = memnew(Label);
	run_number_label->add_theme_color_override("font_color", Color(0.7, 0.7, 0.7));
	run_number_label->set_visible(false);
	summary_bar->add_child(run_number_label);
}

// =========================================================================
// Notification
// =========================================================================

void MCPTestPanel::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			// Discover tests when first shown.
			callable_mp(this, &MCPTestPanel::_discover_tests).call_deferred();
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			if (is_running) {
				_process_test_events();
			}
		} break;
	}
}

// =========================================================================
// Test Discovery
// =========================================================================

void MCPTestPanel::_discover_tests() {
	discovered_files.clear();
	test_tree->clear();
	TreeItem *root = test_tree->create_item();

	// Check if test root directory exists.
	if (!DirAccess::dir_exists_absolute(test_root)) {
		status_label->set_text(TTR("No test directory found: ") + test_root);
		return;
	}

	Vector<String> test_files = MCPTestingTools::collect_test_files(test_root);

	if (test_files.is_empty()) {
		status_label->set_text(TTR("No test_*.gd files found in ") + test_root);
		return;
	}

	for (const String &file_path : test_files) {
		Vector<String> methods = MCPTestingTools::extract_test_methods(file_path);

		// Get or create directory chain.
		String dir = file_path.get_base_dir();
		TreeItem *parent = _get_or_create_dir_item(root, dir);

		// Create file item.
		TreeItem *file_item = test_tree->create_item(parent);
		file_item->set_text(0, file_path.get_file());
		file_item->set_text(1, itos(methods.size()) + " method" + (methods.size() != 1 ? "s" : ""));
		file_item->set_custom_color(1, Color(0.6, 0.6, 0.6));

		Dictionary file_meta;
		file_meta["type"] = "file";
		file_meta["path"] = file_path;
		file_item->set_metadata(0, file_meta);

		DiscoveredFile disc_file;
		disc_file.path = file_path;
		disc_file.tree_item = file_item;

		// Create method items.
		for (const String &method_name : methods) {
			TreeItem *method_item = test_tree->create_item(file_item);
			method_item->set_text(0, method_name);
			method_item->set_text(1, String::utf8("\u2014")); // —
			method_item->set_custom_color(1, Color(0.5, 0.5, 0.5));

			Dictionary method_meta;
			method_meta["type"] = "method";
			method_meta["path"] = file_path;
			method_meta["method"] = method_name;
			method_item->set_metadata(0, method_meta);

			DiscoveredMethod disc_method;
			disc_method.name = method_name;
			disc_method.tree_item = method_item;
			disc_file.methods.push_back(disc_method);
		}

		// Collapse files by default to keep tree clean.
		file_item->set_collapsed(true);

		discovered_files.push_back(disc_file);
	}

	int total_methods = 0;
	for (const DiscoveredFile &f : discovered_files) {
		total_methods += f.methods.size();
	}

	status_label->set_text(
			itos(discovered_files.size()) + " test file" +
			(discovered_files.size() != 1 ? "s" : "") +
			", " + itos(total_methods) + " method" +
			(total_methods != 1 ? "s" : ""));
}

TreeItem *MCPTestPanel::_get_or_create_dir_item(TreeItem *p_root, const String &p_dir_path) {
	// If the directory is the test root itself, return root.
	String dir = p_dir_path.ends_with("/") ? p_dir_path : p_dir_path + "/";
	String root_dir = test_root.ends_with("/") ? test_root : test_root + "/";

	if (dir == root_dir) {
		return p_root;
	}

	// Check if we already have an item for this directory.
	TreeItem *child = p_root->get_first_child();
	while (child) {
		Variant meta = child->get_metadata(0);
		if (meta.get_type() == Variant::DICTIONARY) {
			Dictionary d = meta;
			if (String(d.get("type", "")) == "dir" && String(d.get("path", "")) == dir) {
				return child;
			}
		}
		child = child->get_next();
	}

	// Create parent directories first (recursive).
	String parent_dir = dir.substr(0, dir.length() - 1).get_base_dir();
	TreeItem *parent = _get_or_create_dir_item(p_root, parent_dir);

	// Create directory item.
	TreeItem *dir_item = test_tree->create_item(parent);
	String display_name = dir.substr(0, dir.length() - 1).get_file() + "/";
	dir_item->set_text(0, display_name);
	dir_item->set_custom_color(0, Color(0.7, 0.7, 0.7));

	Dictionary meta;
	meta["type"] = "dir";
	meta["path"] = dir;
	dir_item->set_metadata(0, meta);

	return dir_item;
}

// =========================================================================
// Tree Filtering
// =========================================================================

void MCPTestPanel::_update_tree_filter(const String &p_filter) {
	for (DiscoveredFile &file : discovered_files) {
		bool any_method_visible = false;

		for (DiscoveredMethod &method : file.methods) {
			if (method.tree_item) {
				bool method_visible = p_filter.is_empty() || method.name.containsn(p_filter);
				method.tree_item->set_visible(method_visible);
				if (method_visible) {
					any_method_visible = true;
				}
			}
		}

		if (file.tree_item) {
			bool file_visible = p_filter.is_empty() ||
					file.path.get_file().containsn(p_filter) ||
					any_method_visible;
			file.tree_item->set_visible(file_visible);

			// Auto-expand files with matching methods when filtering.
			if (!p_filter.is_empty() && any_method_visible) {
				file.tree_item->set_collapsed(false);
			}
		}
	}
}

void MCPTestPanel::_on_filter_changed(const String &p_text) {
	_update_tree_filter(p_text);
}

// =========================================================================
// Test Execution
// =========================================================================

void MCPTestPanel::_run_all() {
	Vector<String> files;
	for (const DiscoveredFile &f : discovered_files) {
		files.push_back(f.path);
	}
	if (files.is_empty()) {
		status_label->set_text(TTR("No test files to run. Click Refresh."));
		return;
	}
	_run_files(files);
}

void MCPTestPanel::_run_selected() {
	TreeItem *selected = test_tree->get_selected();
	if (!selected) {
		status_label->set_text(TTR("Select a test file or method first."));
		return;
	}

	Variant meta_v = selected->get_metadata(0);
	if (meta_v.get_type() != Variant::DICTIONARY) {
		return;
	}
	Dictionary meta = meta_v;
	String type = meta.get("type", "");

	if (type == "file") {
		Vector<String> files;
		files.push_back(meta.get("path", ""));
		_run_files(files);
	} else if (type == "method") {
		Vector<String> files;
		files.push_back(meta.get("path", ""));
		_run_files(files, meta.get("method", ""));
	} else if (type == "dir") {
		// Run all files under this directory.
		String dir_path = meta.get("path", "");
		Vector<String> files;
		for (const DiscoveredFile &f : discovered_files) {
			if (f.path.begins_with(dir_path)) {
				files.push_back(f.path);
			}
		}
		if (!files.is_empty()) {
			_run_files(files);
		}
	}
}

void MCPTestPanel::_rerun_failed() {
	if (last_failed_files.is_empty()) {
		return;
	}
	_run_files(last_failed_files);
}

void MCPTestPanel::_run_files(const Vector<String> &p_files, const String &p_filter) {
	if (!debugger_bridge) {
		status_label->set_text(TTR("Error: debugger bridge not available."));
		return;
	}

	if (debugger_bridge->is_game_running()) {
		status_label->set_text(TTR("Game is running. Stop it first."));
		return;
	}

	// Compile-check all files first.
	bool any_compile_fail = false;
	for (const String &file : p_files) {
		Dictionary validation = MCPTestingTools::validate_single_file(file);
		if (!(bool)validation["valid"]) {
			any_compile_fail = true;
			// Mark the file as compile error in the tree.
			TreeItem *file_item = _find_file_tree_item(file);
			if (file_item) {
				file_item->set_text(1, TTR("COMPILE ERROR"));
				file_item->set_custom_color(1, Color(0.9, 0.3, 0.3));
				file_item->set_collapsed(false);

				// Show individual errors.
				Array errors = validation["errors"];
				for (int i = 0; i < errors.size(); i++) {
					Dictionary err = errors[i];
					// Find or create an error child.
					TreeItem *err_item = test_tree->create_item(file_item);
					err_item->set_text(0, vformat("Line %d: %s",
							(int)err["line"], (String)err["message"]));
					err_item->set_custom_color(0, Color(0.9, 0.3, 0.3));

					Dictionary err_meta;
					err_meta["type"] = "error";
					err_meta["path"] = file;
					err_meta["line"] = err["line"];
					err_item->set_metadata(0, err_meta);
				}
			}
		}
	}

	if (any_compile_fail) {
		status_label->set_text(TTR("Compile errors found. Fix them and retry."));
		return;
	}

	// Write test manifest.
	Dictionary manifest;
	Array file_array;
	for (const String &f : p_files) {
		file_array.push_back(f);
	}
	manifest["files"] = file_array;
	manifest["filter"] = p_filter;
	manifest["timeout_ms"] = 30000;
	manifest["verbose"] = false;
	manifest["method_timeout_ms"] = 5000;

	{
		Ref<FileAccess> fa = FileAccess::open("user://mcp_test_manifest.json", FileAccess::WRITE);
		if (fa.is_null()) {
			status_label->set_text(TTR("Failed to write test manifest."));
			return;
		}
		fa->store_string(JSON::stringify(manifest, "  "));
	}

	// Copy runner files.
	MCPTestingTools::copy_runner_files_to_project();

	// Reset tree status for the files we're running.
	_reset_tree_status();

	// Set running state.
	is_running = true;
	debugger_bridge->set_game_launching();
	debugger_bridge->reset_test_run_state();
	current_run_number = debugger_bridge->get_current_test_run_number();

	// Update toolbar.
	run_all_button->set_disabled(true);
	run_selected_button->set_disabled(true);
	rerun_failed_button->set_visible(false);
	stop_button->set_visible(true);
	refresh_button->set_disabled(true);

	status_label->set_text(TTR("Running tests..."));
	status_label->add_theme_color_override("font_color", Color(0.9, 0.8, 0.3));

	// Enable internal process to poll for results.
	set_process_internal(true);

	// Launch the test runner scene.
	callable_mp(EditorRunBar::get_singleton(),
			&EditorRunBar::play_custom_scene)
			.bind(String("res://addons/mcp_test/runner.tscn"), Vector<String>())
			.call_deferred();
}

void MCPTestPanel::_stop_tests() {
	if (EditorRunBar::get_singleton()) {
		EditorRunBar::get_singleton()->stop_playing();
	}
	is_running = false;
	_update_summary();

	// Update toolbar.
	run_all_button->set_disabled(false);
	run_selected_button->set_disabled(false);
	stop_button->set_visible(false);
	refresh_button->set_disabled(false);
	status_label->set_text(TTR("Tests stopped."));
	status_label->add_theme_color_override("font_color", Color(0.9, 0.3, 0.3));

	MCPTestingTools::cleanup_runner_files();
}

void MCPTestPanel::_reset_tree_status() {
	for (DiscoveredFile &file : discovered_files) {
		if (file.tree_item) {
			file.tree_item->set_text(1, TTR("running..."));
			file.tree_item->set_custom_color(1, Color(0.9, 0.8, 0.3));
			file.tree_item->set_text(2, "");
			file.tree_item->set_text(3, "");
			// Clear any old background tints.
			for (int col = 0; col < 4; col++) {
				file.tree_item->clear_custom_bg_color(col);
			}
		}
		for (DiscoveredMethod &method : file.methods) {
			if (method.tree_item) {
				method.tree_item->set_text(1, String::utf8("\u2014")); // —
				method.tree_item->set_custom_color(0, Color(0.8, 0.8, 0.8));
				method.tree_item->set_custom_color(1, Color(0.5, 0.5, 0.5));
				method.tree_item->set_text(2, "");
				method.tree_item->set_text(3, "");
			}
		}
	}

	// Hide result labels.
	passed_label->set_visible(false);
	failed_label->set_visible(false);
	errors_label->set_visible(false);
	skipped_label->set_visible(false);
	duration_label->set_visible(false);
	run_number_label->set_visible(false);
}

// =========================================================================
// Result Processing
// =========================================================================

void MCPTestPanel::_process_test_events() {
	if (!debugger_bridge) {
		return;
	}

	MCPTestEventBuffer *buffer = debugger_bridge->get_test_event_buffer();
	if (!buffer) {
		return;
	}

	Vector<MCPTestEvent> events = buffer->read_since(last_event_cursor, 100);

	for (const MCPTestEvent &evt : events) {
		last_event_cursor = evt.seq;

		switch (evt.type) {
			case MCPTestEvent::TEST_RUN_STARTED: {
				_on_run_started(evt);
			} break;

			case MCPTestEvent::TEST_METHOD_RESULT: {
				if (evt.run_number == current_run_number) {
					_update_method_result(evt);
				}
			} break;

			case MCPTestEvent::TEST_FILE_COMPILE_ERROR: {
				if (evt.run_number == current_run_number) {
					_on_compile_error(evt);
				}
			} break;

			case MCPTestEvent::TEST_RUN_COMPLETE: {
				if (evt.run_number == current_run_number) {
					_on_run_complete(evt);
				}
			} break;
		}
	}
}

void MCPTestPanel::_on_run_started(const MCPTestEvent &p_event) {
	current_run_number = p_event.run_number;
}

void MCPTestPanel::_update_method_result(const MCPTestEvent &p_event) {
	// Find the method tree item.
	TreeItem *method_item = _find_method_tree_item(p_event.file_path, p_event.method_name);
	if (!method_item) {
		return;
	}

	// Update status icon and color.
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

	method_item->set_text(0, icon + " " + p_event.method_name);
	method_item->set_custom_color(0, row_color);

	// Status column.
	String status_upper = p_event.status.to_upper();
	method_item->set_text(1, status_upper);
	method_item->set_custom_color(1, row_color);

	// Duration column.
	if (p_event.duration_ms >= 1000) {
		method_item->set_text(2, String::num((double)p_event.duration_ms / 1000.0, 2) + "s");
	} else {
		method_item->set_text(2, itos(p_event.duration_ms) + "ms");
	}

	// Message column (failures only).
	if (p_event.status == "failed" || p_event.status == "error" || p_event.status == "timeout") {
		method_item->set_text(3, p_event.message);
		method_item->set_custom_color(3, row_color);

		// Store error location for navigation.
		Variant meta_v = method_item->get_metadata(0);
		if (meta_v.get_type() == Variant::DICTIONARY) {
			Dictionary meta = meta_v;
			meta["error_file"] = p_event.error_file;
			meta["error_line"] = p_event.error_line;
			method_item->set_metadata(0, meta);
		}

		// Expand the parent file to show the failure.
		TreeItem *file_item = method_item->get_parent();
		if (file_item) {
			file_item->set_collapsed(false);
		}
	} else {
		method_item->set_text(3, "");
	}

	// Update parent file row summary.
	TreeItem *parent = method_item->get_parent();
	if (parent) {
		_update_file_row_summary(parent);
	}

	_update_summary();
}

void MCPTestPanel::_on_compile_error(const MCPTestEvent &p_event) {
	TreeItem *file_item = _find_file_tree_item(p_event.file_path);
	if (file_item) {
		file_item->set_text(1, TTR("COMPILE ERROR"));
		file_item->set_custom_color(1, Color(0.9, 0.3, 0.3));
		file_item->set_collapsed(false);
		for (int col = 0; col < 4; col++) {
			file_item->set_custom_bg_color(col, Color(0.15, 0.08, 0.08, 0.3));
		}
	}
}

void MCPTestPanel::_on_run_complete(const MCPTestEvent &p_event) {
	is_running = false;

	// Update toolbar.
	run_all_button->set_disabled(false);
	run_selected_button->set_disabled(false);
	stop_button->set_visible(false);
	refresh_button->set_disabled(false);

	// Show summary.
	passed_label->set_text(itos(p_event.passed) + " passed");
	passed_label->set_visible(true);

	if (p_event.failed > 0) {
		failed_label->set_text(itos(p_event.failed) + " failed");
		failed_label->set_visible(true);
	} else {
		failed_label->set_visible(false);
	}

	if (p_event.errors > 0) {
		errors_label->set_text(itos(p_event.errors) + " error" + (p_event.errors > 1 ? "s" : ""));
		errors_label->set_visible(true);
	} else {
		errors_label->set_visible(false);
	}

	if (p_event.skipped > 0) {
		skipped_label->set_text(itos(p_event.skipped) + " skipped");
		skipped_label->set_visible(true);
	} else {
		skipped_label->set_visible(false);
	}

	if (p_event.total_duration_ms >= 1000) {
		duration_label->set_text(String::num((double)p_event.total_duration_ms / 1000.0, 2) + "s");
	} else {
		duration_label->set_text(itos(p_event.total_duration_ms) + "ms");
	}
	duration_label->set_visible(true);

	run_number_label->set_text("Run #" + itos(p_event.run_number));
	run_number_label->set_visible(true);

	// Update status label.
	if (p_event.failed == 0 && p_event.errors == 0) {
		status_label->set_text(TTR("All tests passed."));
		status_label->add_theme_color_override("font_color", Color(0.4, 0.9, 0.4));
		rerun_failed_button->set_visible(false);
	} else {
		status_label->set_text(TTR("Some tests failed."));
		status_label->add_theme_color_override("font_color", Color(0.9, 0.3, 0.3));
		rerun_failed_button->set_visible(true);

		// Collect failed files for rerun.
		last_failed_files.clear();
		for (const DiscoveredFile &f : discovered_files) {
			if (f.tree_item) {
				Variant meta_v = f.tree_item->get_metadata(0);
				// Check if any child method failed.
				TreeItem *child = f.tree_item->get_first_child();
				while (child) {
					Variant child_meta_v = child->get_metadata(0);
					if (child_meta_v.get_type() == Variant::DICTIONARY) {
						Dictionary cm = child_meta_v;
						// Check the displayed status text.
						String status_text = child->get_text(1);
						if (status_text == "FAILED" || status_text == "ERROR" || status_text == "TIMEOUT") {
							last_failed_files.push_back(f.path);
							break;
						}
					}
					child = child->get_next();
				}
			}
		}
	}

	// Cleanup runner files.
	MCPTestingTools::cleanup_runner_files();
}

void MCPTestPanel::_update_summary() {
	// Pulsing animation while running.
	if (is_running) {
		float pulse = 0.5f + 0.5f * Math::sin(OS::get_singleton()->get_ticks_usec() / 300000.0f);
		status_label->add_theme_color_override("font_color",
				Color(0.9, 0.8, 0.3).lerp(Color(1, 1, 1), pulse));
	}
}

void MCPTestPanel::_update_file_row_summary(TreeItem *p_file_item) {
	int total = 0, passed = 0, failed = 0, errors = 0;
	int total_duration = 0;

	TreeItem *child = p_file_item->get_first_child();
	while (child) {
		Variant meta_v = child->get_metadata(0);
		if (meta_v.get_type() == Variant::DICTIONARY) {
			Dictionary meta = meta_v;
			if (String(meta.get("type", "")) == "method") {
				String status_text = child->get_text(1);
				total++;
				if (status_text == "PASSED") {
					passed++;
				} else if (status_text == "FAILED") {
					failed++;
				} else if (status_text == "ERROR" || status_text == "TIMEOUT") {
					errors++;
				}
				// Parse duration.
				String dur_text = child->get_text(2);
				if (dur_text.ends_with("ms")) {
					total_duration += dur_text.replace("ms", "").to_int();
				} else if (dur_text.ends_with("s")) {
					total_duration += (int)(dur_text.replace("s", "").to_float() * 1000);
				}
			}
		}
		child = child->get_next();
	}

	// Build summary text.
	String result_text = itos(passed) + "/" + itos(total) + " passed";
	Color result_color = Color(0.4, 0.9, 0.4);

	if (failed > 0) {
		result_text += ", " + itos(failed) + " FAILED";
		result_color = Color(0.9, 0.3, 0.3);
	}
	if (errors > 0) {
		result_text += ", " + itos(errors) + " error" + (errors > 1 ? "s" : "");
		if (failed == 0) {
			result_color = Color(0.9, 0.8, 0.3);
		}
	}

	p_file_item->set_text(1, result_text);
	p_file_item->set_custom_color(1, result_color);

	// Duration.
	if (total_duration >= 1000) {
		p_file_item->set_text(2, String::num((double)total_duration / 1000.0, 2) + "s");
	} else if (total_duration > 0) {
		p_file_item->set_text(2, itos(total_duration) + "ms");
	}

	// Background tint for failures.
	if (failed > 0 || errors > 0) {
		for (int col = 0; col < 4; col++) {
			p_file_item->set_custom_bg_color(col, Color(0.15, 0.08, 0.08, 0.3));
		}
	} else {
		for (int col = 0; col < 4; col++) {
			p_file_item->clear_custom_bg_color(col);
		}
	}
}

// =========================================================================
// Tree Interaction
// =========================================================================

void MCPTestPanel::_on_item_activated() {
	TreeItem *selected = test_tree->get_selected();
	if (selected) {
		_navigate_to_item(selected);
	}
}

void MCPTestPanel::_navigate_to_item(TreeItem *p_item) {
	Variant meta_v = p_item->get_metadata(0);
	if (meta_v.get_type() != Variant::DICTIONARY) {
		return;
	}
	Dictionary meta = meta_v;
	String type = meta.get("type", "");
	String path = meta.get("path", "");

	if (path.is_empty()) {
		return;
	}

	// If there's an error file/line, navigate there.
	String error_file = meta.get("error_file", "");
	int error_line = meta.get("error_line", 0);

	if (!error_file.is_empty() && error_line > 0) {
		Ref<Script> script = ResourceLoader::load(error_file);
		if (script.is_valid()) {
			EditorInterface::get_singleton()->edit_script(script, error_line);
		}
		return;
	}

	// Otherwise navigate to the file/method.
	Ref<Script> script = ResourceLoader::load(path);
	if (script.is_valid()) {
		int line = meta.get("line", 1);
		EditorInterface::get_singleton()->edit_script(script, MAX(line, 1));
	}
}

void MCPTestPanel::_on_item_rmb(const Vector2 &p_pos) {
	TreeItem *selected = test_tree->get_selected();
	if (!selected) {
		return;
	}

	context_menu_item = selected;
	context_menu->clear();

	Variant meta_v = selected->get_metadata(0);
	if (meta_v.get_type() != Variant::DICTIONARY) {
		return;
	}
	Dictionary meta = meta_v;
	String type = meta.get("type", "");

	if (type == "file") {
		context_menu->add_item(TTR("Run This File"), CTX_RUN_FILE);
		context_menu->add_item(TTR("Open in Editor"), CTX_OPEN_IN_EDITOR);
		context_menu->add_separator();
		context_menu->add_item(TTR("Copy Path"), CTX_COPY_PATH);
	} else if (type == "method") {
		context_menu->add_item(TTR("Run This Test"), CTX_RUN_METHOD);
		context_menu->add_item(TTR("Open in Editor"), CTX_OPEN_IN_EDITOR);
		context_menu->add_separator();
		context_menu->add_item(TTR("Copy Path"), CTX_COPY_PATH);
	} else if (type == "dir") {
		context_menu->add_item(TTR("Run All in Directory"), CTX_RUN_FILE);
	}

	if (context_menu->get_item_count() > 0) {
		context_menu->set_position(test_tree->get_screen_position() + p_pos);
		context_menu->reset_size();
		context_menu->popup();
	}
}

void MCPTestPanel::_on_context_menu_id(int p_id) {
	if (!context_menu_item) {
		return;
	}

	Variant meta_v = context_menu_item->get_metadata(0);
	if (meta_v.get_type() != Variant::DICTIONARY) {
		return;
	}
	Dictionary meta = meta_v;
	String type = meta.get("type", "");
	String path = meta.get("path", "");

	switch (p_id) {
		case CTX_RUN_FILE: {
			if (type == "file") {
				Vector<String> files;
				files.push_back(path);
				_run_files(files);
			} else if (type == "dir") {
				Vector<String> files;
				for (const DiscoveredFile &f : discovered_files) {
					if (f.path.begins_with(path)) {
						files.push_back(f.path);
					}
				}
				if (!files.is_empty()) {
					_run_files(files);
				}
			}
		} break;

		case CTX_RUN_METHOD: {
			if (type == "method") {
				Vector<String> files;
				files.push_back(path);
				_run_files(files, meta.get("method", ""));
			}
		} break;

		case CTX_OPEN_IN_EDITOR: {
			_navigate_to_item(context_menu_item);
		} break;

		case CTX_COPY_PATH: {
			if (!path.is_empty()) {
				DisplayServer::get_singleton()->clipboard_set(path);
			}
		} break;
	}

	context_menu_item = nullptr;
}

void MCPTestPanel::_on_refresh_pressed() {
	_discover_tests();
}

// =========================================================================
// Helpers
// =========================================================================

TreeItem *MCPTestPanel::_find_file_tree_item(const String &p_path) {
	for (const DiscoveredFile &f : discovered_files) {
		if (f.path == p_path && f.tree_item) {
			return f.tree_item;
		}
	}
	return nullptr;
}

TreeItem *MCPTestPanel::_find_method_tree_item(const String &p_file, const String &p_method) {
	for (const DiscoveredFile &f : discovered_files) {
		if (f.path == p_file) {
			for (const DiscoveredMethod &m : f.methods) {
				if (m.name == p_method && m.tree_item) {
					return m.tree_item;
				}
			}
			break;
		}
	}
	return nullptr;
}

int MCPTestPanel::_find_discovered_file_index(const String &p_path) {
	for (int i = 0; i < discovered_files.size(); i++) {
		if (discovered_files[i].path == p_path) {
			return i;
		}
	}
	return -1;
}

// =========================================================================
// Public API
// =========================================================================

void MCPTestPanel::run_file(const String &p_path) {
	if (p_path.is_empty()) {
		return;
	}
	// Ensure discovery has happened.
	if (discovered_files.is_empty()) {
		_discover_tests();
	}
	Vector<String> files;
	files.push_back(p_path);
	_run_files(files);
}

void MCPTestPanel::run_all_tests() {
	if (discovered_files.is_empty()) {
		_discover_tests();
	}
	_run_all();
}

void MCPTestPanel::rerun_failed_tests() {
	_rerun_failed();
}

void MCPTestPanel::set_test_root(const String &p_root) {
	if (test_root != p_root) {
		test_root = p_root;
		_discover_tests();
	}
}

#endif // TOOLS_ENABLED
