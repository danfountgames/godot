/**************************************************************************/
/*  dev_player_shell.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           GODOT BEAM ENGINE                            */
/**************************************************************************/

#include "dev_player_shell.h"

#include "autoload_session_manager.h"
#include "devplayer_debug.h"
#include "git_manager.h"
#include "import_session_manager.h"
#include "launch_controller.h"
#include "project_domain_manager.h"
#include "resource_domain_manager.h"
#include "sync_server.h"

#include "core/config/project_settings.h"
#include "core/input/input_event.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#include "scene/main/scene_tree.h"

// ==========================================================================
// Bind methods
// ==========================================================================

void DevPlayerShell::_bind_methods() {
}

// ==========================================================================
// Notification handler
// ==========================================================================

void DevPlayerShell::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			_build_ui();
		} break;
		case NOTIFICATION_PROCESS: {
			_update_status_display();
			_update_sync_display();
			_update_button_states();
			if (automated_test_mode) {
				_run_test_step();
			}
		} break;
	}
}

void DevPlayerShell::unhandled_key_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && key->get_keycode() == Key::F12) {
		if (project_domain_manager && project_domain_manager->is_project_mounted()) {
			if (shell_visible) {
				_hide_shell();
			} else {
				_show_shell();
			}
		}
		accept_event();
	}
}

// ==========================================================================
// Engine root capture
// ==========================================================================

void DevPlayerShell::_capture_engine_root() {
	if (engine_root_captured) {
		return;
	}
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps) {
		engine_root_path = ps->get_resource_path();
		engine_root_captured = true;
		print_line("[DevPlayerShell] Captured engine root path: " + engine_root_path);
	}
}

String DevPlayerShell::_test_project_path(const String &p_project_name) const {
	return engine_root_path.path_join("test_projects").path_join(p_project_name);
}

// ==========================================================================
// UI Construction
// ==========================================================================

void DevPlayerShell::_build_ui() {
	if (ui_built) {
		return;
	}
	ui_built = true;

	_capture_engine_root();

	// Grab singleton references.
	if (!launch_controller) {
		launch_controller = LaunchController::get_singleton();
	}
	if (!project_domain_manager) {
		project_domain_manager = ProjectDomainManager::get_singleton();
	}
	if (!autoload_session_manager) {
		autoload_session_manager = AutoloadSessionManager::get_singleton();
	}
	if (!resource_domain_manager) {
		resource_domain_manager = ResourceDomainManager::get_singleton();
	}
	if (!import_session_manager) {
		import_session_manager = ImportSessionManager::get_singleton();
	}
	if (!debug) {
		debug = DevPlayerDebug::get_singleton();
	}
	if (!git_manager) {
		git_manager = GitManager::get_singleton();
	}
	if (!sync_server) {
		sync_server = SyncServer::get_singleton();
	}

	set_anchors_preset(PRESET_FULL_RECT);

	// Root container for the entire shell UI.
	shell_ui_root = memnew(VBoxContainer);
	shell_ui_root->set_anchors_preset(PRESET_FULL_RECT);
	shell_ui_root->set_offset(SIDE_LEFT, 16);
	shell_ui_root->set_offset(SIDE_TOP, 16);
	shell_ui_root->set_offset(SIDE_RIGHT, -16);
	shell_ui_root->set_offset(SIDE_BOTTOM, -16);
	add_child(shell_ui_root);

	_build_status_section();
	_build_project_section();
	_build_git_section();
	_build_sync_section();
	_build_log_section();
	_build_overlay();

	// Discover projects on startup.
	_scan_projects();
	_populate_project_list();

	set_process(true);
	set_process_unhandled_key_input(true);

	print_line("DevPlayer: Shell UI created.");
	_log("DevPlayer shell ready. Select a project to mount.");
}

void DevPlayerShell::_build_status_section() {
	// Title.
	title_label = memnew(Label);
	title_label->set_text("GodotBeam DevPlayer");
	title_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	shell_ui_root->add_child(title_label);

	shell_ui_root->add_child(memnew(HSeparator));

	// Status info.
	HBoxContainer *status_row = memnew(HBoxContainer);
	shell_ui_root->add_child(status_row);

	status_label = memnew(Label);
	status_label->set_text("No project mounted");
	status_label->set_h_size_flags(SIZE_EXPAND_FILL);
	status_row->add_child(status_label);

	metrics_label = memnew(Label);
	metrics_label->set_text("");
	metrics_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	status_row->add_child(metrics_label);

	project_path_label = memnew(Label);
	project_path_label->set_text("");
	shell_ui_root->add_child(project_path_label);

	shell_ui_root->add_child(memnew(HSeparator));
}

void DevPlayerShell::_build_project_section() {
	// Section label.
	Label *section_label = memnew(Label);
	section_label->set_text("Projects");
	shell_ui_root->add_child(section_label);

	// Project list.
	project_list = memnew(ItemList);
	project_list->set_select_mode(ItemList::SELECT_SINGLE);
	project_list->set_custom_minimum_size(Size2(0, 120));
	project_list->set_v_size_flags(SIZE_EXPAND_FILL);
	project_list->set_max_columns(1);
	project_list->set_same_column_width(true);
	project_list->connect("item_activated", callable_mp(this, &DevPlayerShell::_on_project_item_activated));
	shell_ui_root->add_child(project_list);

	// Project action buttons.
	HBoxContainer *proj_buttons = memnew(HBoxContainer);
	shell_ui_root->add_child(proj_buttons);

	mount_selected_button = memnew(Button);
	mount_selected_button->set_text("Mount Selected");
	mount_selected_button->connect("pressed", callable_mp(this, &DevPlayerShell::_on_mount_selected_pressed));
	proj_buttons->add_child(mount_selected_button);

	refresh_button = memnew(Button);
	refresh_button->set_text("Refresh");
	refresh_button->connect("pressed", callable_mp(this, &DevPlayerShell::_on_refresh_pressed));
	proj_buttons->add_child(refresh_button);

	unmount_button = memnew(Button);
	unmount_button->set_text("Unmount");
	unmount_button->connect("pressed", callable_mp(this, &DevPlayerShell::_on_unmount_pressed));
	unmount_button->set_disabled(true);
	proj_buttons->add_child(unmount_button);

	relaunch_button = memnew(Button);
	relaunch_button->set_text("Relaunch");
	relaunch_button->connect("pressed", callable_mp(this, &DevPlayerShell::_on_relaunch_pressed));
	relaunch_button->set_disabled(true);
	proj_buttons->add_child(relaunch_button);

	// Custom path row.
	HBoxContainer *custom_row = memnew(HBoxContainer);
	shell_ui_root->add_child(custom_row);

	Label *custom_label = memnew(Label);
	custom_label->set_text("Path:");
	custom_row->add_child(custom_label);

	path_line_edit = memnew(LineEdit);
	path_line_edit->set_placeholder("Enter absolute path to project...");
	path_line_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	custom_row->add_child(path_line_edit);

	mount_custom_button = memnew(Button);
	mount_custom_button->set_text("Mount");
	mount_custom_button->connect("pressed", callable_mp(this, &DevPlayerShell::_on_mount_custom_pressed));
	custom_row->add_child(mount_custom_button);

	shell_ui_root->add_child(memnew(HSeparator));
}

void DevPlayerShell::_build_git_section() {
	Label *section_label = memnew(Label);
	section_label->set_text("Git");
	shell_ui_root->add_child(section_label);

	// Repo path row.
	HBoxContainer *repo_row = memnew(HBoxContainer);
	shell_ui_root->add_child(repo_row);

	Label *repo_label = memnew(Label);
	repo_label->set_text("Repo:");
	repo_row->add_child(repo_label);

	git_repo_line_edit = memnew(LineEdit);
	git_repo_line_edit->set_placeholder("Path to git repository...");
	git_repo_line_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	repo_row->add_child(git_repo_line_edit);

	git_list_branches_button = memnew(Button);
	git_list_branches_button->set_text("List Branches");
	git_list_branches_button->connect("pressed", callable_mp(this, &DevPlayerShell::_on_git_list_branches_pressed));
	repo_row->add_child(git_list_branches_button);

	// Branch switch row.
	HBoxContainer *branch_row = memnew(HBoxContainer);
	shell_ui_root->add_child(branch_row);

	Label *branch_label = memnew(Label);
	branch_label->set_text("Switch to:");
	branch_row->add_child(branch_label);

	git_branch_line_edit = memnew(LineEdit);
	git_branch_line_edit->set_placeholder("Branch name...");
	git_branch_line_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	branch_row->add_child(git_branch_line_edit);

	git_switch_button = memnew(Button);
	git_switch_button->set_text("Switch & Remount");
	git_switch_button->connect("pressed", callable_mp(this, &DevPlayerShell::_on_git_switch_pressed));
	branch_row->add_child(git_switch_button);

	// Git status.
	git_status_label = memnew(Label);
	git_status_label->set_text("");
	shell_ui_root->add_child(git_status_label);

	git_branches_label = memnew(Label);
	git_branches_label->set_text("");
	shell_ui_root->add_child(git_branches_label);

	shell_ui_root->add_child(memnew(HSeparator));
}

void DevPlayerShell::_build_sync_section() {
	Label *section_label = memnew(Label);
	section_label->set_text("Live Sync");
	shell_ui_root->add_child(section_label);

	HBoxContainer *sync_row = memnew(HBoxContainer);
	shell_ui_root->add_child(sync_row);

	sync_status_label = memnew(Label);
	sync_status_label->set_text("Server: Stopped");
	sync_status_label->set_h_size_flags(SIZE_EXPAND_FILL);
	sync_row->add_child(sync_status_label);

	Label *port_label = memnew(Label);
	port_label->set_text("Port:");
	sync_row->add_child(port_label);

	sync_port_line_edit = memnew(LineEdit);
	sync_port_line_edit->set_text("6850");
	sync_port_line_edit->set_custom_minimum_size(Size2(60, 0));
	sync_row->add_child(sync_port_line_edit);

	sync_start_button = memnew(Button);
	sync_start_button->set_text("Start");
	sync_start_button->connect("pressed", callable_mp(this, &DevPlayerShell::_on_sync_start_pressed));
	sync_row->add_child(sync_start_button);

	sync_stop_button = memnew(Button);
	sync_stop_button->set_text("Stop");
	sync_stop_button->set_disabled(true);
	sync_stop_button->connect("pressed", callable_mp(this, &DevPlayerShell::_on_sync_stop_pressed));
	sync_row->add_child(sync_stop_button);

	shell_ui_root->add_child(memnew(HSeparator));
}

void DevPlayerShell::_build_log_section() {
	Label *section_label = memnew(Label);
	section_label->set_text("Log");
	shell_ui_root->add_child(section_label);

	log_output = memnew(RichTextLabel);
	log_output->set_use_bbcode(true);
	log_output->set_scroll_active(true);
	log_output->set_selection_enabled(true);
	log_output->set_custom_minimum_size(Size2(0, 100));
	log_output->set_v_size_flags(SIZE_EXPAND_FILL);
	shell_ui_root->add_child(log_output);
}

void DevPlayerShell::_build_overlay() {
	// "Back to Shell" button — positioned in top-right corner, always on top.
	// Visible only when a project is mounted and shell is hidden.
	back_to_shell_button = memnew(Button);
	back_to_shell_button->set_text("Back to Shell (F12)");
	back_to_shell_button->set_anchors_preset(PRESET_TOP_RIGHT);
	back_to_shell_button->set_offset(SIDE_RIGHT, -10);
	back_to_shell_button->set_offset(SIDE_TOP, 10);
	back_to_shell_button->set_offset(SIDE_LEFT, -180);
	back_to_shell_button->set_offset(SIDE_BOTTOM, 40);
	back_to_shell_button->connect("pressed", callable_mp(this, &DevPlayerShell::_on_back_to_shell_pressed));
	back_to_shell_button->set_visible(false);
	add_child(back_to_shell_button);
}

// ==========================================================================
// Project Discovery
// ==========================================================================

void DevPlayerShell::_scan_projects() {
	discovered_projects.clear();

	String projects_dir = engine_root_path.path_join("test_projects");

	Ref<DirAccess> da = DirAccess::open(projects_dir);
	if (da.is_null()) {
		_log("Could not open projects directory: " + projects_dir);
		return;
	}

	da->list_dir_begin();
	String fname = da->get_next();
	while (!fname.is_empty()) {
		if (da->current_is_dir() && !fname.begins_with(".")) {
			String full_path = projects_dir.path_join(fname);
			String project_file = full_path.path_join("project.godot");
			if (FileAccess::exists(project_file)) {
				ProjectEntry entry;
				entry.name = fname;
				entry.path = full_path;
				discovered_projects.push_back(entry);
			}
		}
		fname = da->get_next();
	}
	da->list_dir_end();

	// Sort by name.
	discovered_projects.sort();

	_log("Found " + itos(discovered_projects.size()) + " projects in " + projects_dir);
}

void DevPlayerShell::_populate_project_list() {
	if (!project_list) {
		return;
	}
	project_list->clear();

	for (int i = 0; i < discovered_projects.size(); i++) {
		int idx = project_list->add_item(discovered_projects[i].name);
		project_list->set_item_metadata(idx, discovered_projects[i].path);
		project_list->set_item_tooltip(idx, discovered_projects[i].path);
	}
}

// ==========================================================================
// UI Callbacks
// ==========================================================================

void DevPlayerShell::_on_mount_selected_pressed() {
	if (!project_list) {
		return;
	}
	Vector<int> selected = project_list->get_selected_items();
	if (selected.is_empty()) {
		_log("No project selected.");
		return;
	}
	String path = project_list->get_item_metadata(selected[0]);
	_mount_project(path);
}

void DevPlayerShell::_on_mount_custom_pressed() {
	if (!path_line_edit) {
		return;
	}
	String path = path_line_edit->get_text().strip_edges();
	if (path.is_empty()) {
		_log("No path entered.");
		return;
	}
	_mount_project(path);
}

void DevPlayerShell::_on_unmount_pressed() {
	if (!launch_controller) {
		return;
	}
	_log("Unmounting project...");
	launch_controller->stop_project();
	_show_shell();
	_log("Project unmounted. Shell visible.");
}

void DevPlayerShell::_on_relaunch_pressed() {
	if (!launch_controller) {
		return;
	}
	_log("Relaunching project...");
	launch_controller->relaunch();
	_hide_shell();
	_log("Project relaunched.");
}

void DevPlayerShell::_on_refresh_pressed() {
	_scan_projects();
	_populate_project_list();
}

void DevPlayerShell::_on_project_item_activated(int p_index) {
	// Double-click on project list item = mount immediately.
	if (p_index < 0 || p_index >= discovered_projects.size()) {
		return;
	}
	String path = project_list->get_item_metadata(p_index);
	_mount_project(path);
}

void DevPlayerShell::_on_git_switch_pressed() {
	if (!git_manager) {
		_log("GitManager not available.");
		return;
	}

	String repo_path = git_repo_line_edit ? git_repo_line_edit->get_text().strip_edges() : String();
	String branch = git_branch_line_edit ? git_branch_line_edit->get_text().strip_edges() : String();

	if (repo_path.is_empty()) {
		_log("Git: No repo path specified.");
		return;
	}
	if (branch.is_empty()) {
		_log("Git: No branch specified.");
		return;
	}

	_log("Git: Switching to branch '" + branch + "' in " + repo_path + "...");

	Error err = git_manager->switch_and_remount(repo_path, branch);
	if (err == OK) {
		_log("Git: Branch switch complete. Now on '" + branch + "'.");
		_hide_shell();
	} else {
		_log("Git: Branch switch failed (error " + itos(err) + ").");
	}
}

void DevPlayerShell::_on_git_list_branches_pressed() {
	if (!git_manager) {
		_log("GitManager not available.");
		return;
	}

	String repo_path = git_repo_line_edit ? git_repo_line_edit->get_text().strip_edges() : String();
	if (repo_path.is_empty()) {
		_log("Git: No repo path specified.");
		return;
	}

	String current = git_manager->get_current_branch(repo_path);
	PackedStringArray branches = git_manager->list_branches(repo_path);

	String info = "Branch: " + current + "  |  Available: ";
	for (int i = 0; i < branches.size(); i++) {
		if (i > 0) {
			info += ", ";
		}
		info += branches[i];
	}

	if (git_status_label) {
		git_status_label->set_text("Current: " + current);
	}
	if (git_branches_label) {
		git_branches_label->set_text("Branches: " + String(", ").join(branches));
	}

	_log("Git: " + info);
}

void DevPlayerShell::_on_sync_start_pressed() {
	if (!sync_server) {
		_log("SyncServer not available.");
		return;
	}

	int port = 6850;
	if (sync_port_line_edit) {
		port = sync_port_line_edit->get_text().to_int();
		if (port < 1 || port > 65535) {
			port = 6850;
		}
	}

	Error err = sync_server->start_server(port);
	if (err == OK) {
		_log("Sync: Server started on port " + itos(port) + ".");
	} else {
		_log("Sync: Failed to start server (error " + itos(err) + ").");
	}
}

void DevPlayerShell::_on_sync_stop_pressed() {
	if (!sync_server) {
		return;
	}
	sync_server->stop_server();
	_log("Sync: Server stopped.");
}

void DevPlayerShell::_on_back_to_shell_pressed() {
	_show_shell();
}

// ==========================================================================
// Mount helper
// ==========================================================================

void DevPlayerShell::_mount_project(const String &p_path) {
	if (!launch_controller) {
		_log("Error: LaunchController not available.");
		return;
	}

	// If a project is already mounted, unmount first.
	if (project_domain_manager && project_domain_manager->is_project_mounted()) {
		_log("Unmounting current project first...");
		launch_controller->stop_project();
	}

	_log("Mounting project: " + p_path);
	Error err = launch_controller->launch_project(p_path, String());
	if (err != OK) {
		_log("Failed to mount project (error " + itos(err) + ").");
		return;
	}

	_log("Project mounted and running.");
	_hide_shell();

	// Auto-populate git repo path if this looks like a git repo.
	if (git_repo_line_edit) {
		String git_dir = p_path.path_join(".git");
		if (DirAccess::exists(git_dir)) {
			git_repo_line_edit->set_text(p_path);
		}
	}
}

// ==========================================================================
// Runtime display updates
// ==========================================================================

void DevPlayerShell::_update_status_display() {
	bool mounted = project_domain_manager && project_domain_manager->is_project_mounted();

	if (status_label) {
		if (mounted) {
			status_label->set_text("MOUNTED - Press F12 to toggle shell");
		} else {
			status_label->set_text("No project mounted");
		}
	}

	if (project_path_label) {
		if (mounted && debug) {
			project_path_label->set_text(debug->get_mounted_project_path());
		} else {
			project_path_label->set_text("");
		}
	}

	if (metrics_label && debug) {
		if (mounted) {
			String m = "res:" + itos(debug->get_tracked_resource_count()) +
					"  autoloads:" + itos(debug->get_active_autoload_count()) +
					"  mount:" + rtos(debug->get_last_mount_duration()) + "s";
			metrics_label->set_text(m);
		} else {
			int leaked = debug->get_leaked_references_after_unmount();
			if (leaked > 0) {
				metrics_label->set_text("Post-unmount counters: " + itos(leaked) + " residual");
			} else {
				metrics_label->set_text("");
			}
		}
	}
}

void DevPlayerShell::_update_sync_display() {
	if (!sync_server || !sync_status_label) {
		return;
	}

	if (sync_server->is_running()) {
		sync_status_label->set_text("Server: Running on :" + itos(sync_server->get_port()) +
				"  Clients: " + itos(sync_server->get_connected_client_count()));
		// Poll the sync server each frame.
		sync_server->poll();
	} else {
		sync_status_label->set_text("Server: Stopped");
	}
}

void DevPlayerShell::_update_button_states() {
	bool mounted = project_domain_manager && project_domain_manager->is_project_mounted();

	if (mount_selected_button) {
		mount_selected_button->set_disabled(mounted);
	}
	if (mount_custom_button) {
		mount_custom_button->set_disabled(mounted);
	}
	if (unmount_button) {
		unmount_button->set_disabled(!mounted);
	}
	if (relaunch_button) {
		relaunch_button->set_disabled(!mounted);
	}

	// Sync buttons.
	bool sync_running = sync_server && sync_server->is_running();
	if (sync_start_button) {
		sync_start_button->set_disabled(sync_running);
	}
	if (sync_stop_button) {
		sync_stop_button->set_disabled(!sync_running);
	}
}

// ==========================================================================
// Shell visibility
// ==========================================================================

void DevPlayerShell::_show_shell() {
	shell_visible = true;
	if (shell_ui_root) {
		shell_ui_root->set_visible(true);
	}
	if (back_to_shell_button) {
		back_to_shell_button->set_visible(false);
	}
}

void DevPlayerShell::_hide_shell() {
	shell_visible = false;
	if (shell_ui_root) {
		shell_ui_root->set_visible(false);
	}
	// Show the overlay button so user can get back.
	bool mounted = project_domain_manager && project_domain_manager->is_project_mounted();
	if (back_to_shell_button && mounted) {
		back_to_shell_button->set_visible(true);
	}
}

// ==========================================================================
// Log helper
// ==========================================================================

void DevPlayerShell::_log(const String &p_message) {
	print_line("[DevPlayerShell] " + p_message);
	if (log_output) {
		log_output->add_text(p_message + "\n");
		// Auto-scroll to bottom.
		log_output->scroll_to_line(log_output->get_line_count() - 1);
	}
}

// ==========================================================================
// Manager wiring
// ==========================================================================

void DevPlayerShell::set_managers(
		LaunchController *p_launch_controller,
		ProjectDomainManager *p_project_domain,
		AutoloadSessionManager *p_autoload_session,
		ResourceDomainManager *p_resource_domain,
		DevPlayerDebug *p_debug) {
	launch_controller = p_launch_controller;
	project_domain_manager = p_project_domain;
	autoload_session_manager = p_autoload_session;
	resource_domain_manager = p_resource_domain;
	debug = p_debug;
}

// ==========================================================================
// Automated test mode
// ==========================================================================

void DevPlayerShell::set_automated_test_mode(bool p_enabled) {
	automated_test_mode = p_enabled;
	if (p_enabled) {
		print_line("[DEVPLAYER TEST] Automated test mode enabled.");
		test_step = 0;
		test_timer = 0.0;
		test_pass_count = 0;
		test_fail_count = 0;
		stress_cycle_index = 0;
	}
}

void DevPlayerShell::_test_check(bool p_condition, const String &p_pass_msg, const String &p_fail_msg) {
	if (p_condition) {
		print_line("[TEST PASS] " + p_pass_msg);
		test_pass_count++;
	} else {
		print_line("[TEST FAIL] " + p_fail_msg);
		test_fail_count++;
	}
}

void DevPlayerShell::_run_test_step() {
	// Grab singletons lazily.
	if (!launch_controller) {
		launch_controller = LaunchController::get_singleton();
	}
	if (!project_domain_manager) {
		project_domain_manager = ProjectDomainManager::get_singleton();
	}
	if (!debug) {
		debug = DevPlayerDebug::get_singleton();
	}

	if (!launch_controller || !project_domain_manager) {
		return;
	}

	_capture_engine_root();

	double delta = get_process_delta_time();

	const String path_minimal_2d = _test_project_path("minimal_2d");
	const String path_collision_a = _test_project_path("class_name_collision_test_a");
	const String path_collision_b = _test_project_path("class_name_collision_test_b");
	const String path_autoload = _test_project_path("autoload_reset_test");

	// =========================================================================
	// Test state machine — same as before.
	// =========================================================================

	switch (test_step) {
		// Cycle 1: Mount minimal_2d
		case 0: {
			print_line("========================================");
			print_line("[DEVPLAYER TEST] Cycle 1: Mount minimal_2d");
			print_line("========================================");
			Error err = launch_controller->launch_project(path_minimal_2d, String());
			_test_check(err == OK,
					"Mount call succeeded for " + path_minimal_2d,
					"Failed to mount " + path_minimal_2d + " (error " + itos(err) + ")");
			test_timer = 0.0;
			test_step = 1;
		} break;

		case 1: {
			test_timer += delta;
			if (test_timer >= 0.3) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(mounted, "Project is mounted after wait (cycle 1).",
						"Project should be mounted but is not (cycle 1).");
				launch_controller->stop_project();
				test_timer = 0.0;
				test_step = 2;
			}
		} break;

		case 2: {
			test_timer += delta;
			if (test_timer >= delta) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(!mounted, "Project unmounted successfully (cycle 1).",
						"Project still mounted after stop_project (cycle 1).");
				if (debug) {
					int leaked = debug->get_leaked_references_after_unmount();
					_test_check(leaked == 0, "No leaked references after unmount (cycle 1).",
							"Leaked references after unmount (cycle 1): " + itos(leaked));
				}
				test_timer = 0.0;
				test_step = 3;
			}
		} break;

		// Cycle 2: Mount class_name_collision_test_a
		case 3: {
			print_line("========================================");
			print_line("[DEVPLAYER TEST] Cycle 2: Mount class_name_collision_test_a");
			print_line("========================================");
			Error err = launch_controller->launch_project(path_collision_a, String());
			_test_check(err == OK, "Mount call succeeded for " + path_collision_a,
					"Failed to mount " + path_collision_a + " (error " + itos(err) + ")");
			test_timer = 0.0;
			test_step = 4;
		} break;

		case 4: {
			test_timer += delta;
			if (test_timer >= 0.3) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(mounted, "Project is mounted after wait (cycle 2).",
						"Project should be mounted but is not (cycle 2).");
				launch_controller->stop_project();
				test_timer = 0.0;
				test_step = 5;
			}
		} break;

		case 5: {
			test_timer += delta;
			if (test_timer >= delta) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(!mounted, "Project unmounted successfully (cycle 2).",
						"Project still mounted after stop_project (cycle 2).");
				if (debug) {
					int leaked = debug->get_leaked_references_after_unmount();
					_test_check(leaked == 0, "No leaked references after unmount (cycle 2).",
							"Leaked references after unmount (cycle 2): " + itos(leaked));
				}
				test_timer = 0.0;
				test_step = 6;
			}
		} break;

		// Cycle 3: Remount minimal_2d (A->B->A)
		case 6: {
			print_line("========================================");
			print_line("[DEVPLAYER TEST] Cycle 3: Remount minimal_2d (A->B->A)");
			print_line("========================================");
			Error err = launch_controller->launch_project(path_minimal_2d, String());
			_test_check(err == OK, "Remount call succeeded for " + path_minimal_2d,
					"Failed to remount " + path_minimal_2d + " (error " + itos(err) + ")");
			test_timer = 0.0;
			test_step = 7;
		} break;

		case 7: {
			test_timer += delta;
			if (test_timer >= 0.3) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(mounted, "Project is mounted after remount (cycle 3).",
						"Project should be mounted but is not (cycle 3).");
				launch_controller->stop_project();
				test_timer = 0.0;
				test_step = 8;
			}
		} break;

		case 8: {
			test_timer += delta;
			if (test_timer >= delta) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(!mounted, "Project unmounted successfully (cycle 3).",
						"Project still mounted after stop_project (cycle 3).");
				if (debug) {
					int leaked = debug->get_leaked_references_after_unmount();
					_test_check(leaked == 0, "No leaked references after unmount (cycle 3).",
							"Leaked references after unmount (cycle 3): " + itos(leaked));
				}
				test_timer = 0.0;
				test_step = 9;
			}
		} break;

		// Cycle 4: Mount class_name_collision_test_b
		case 9: {
			print_line("========================================");
			print_line("[DEVPLAYER TEST] Cycle 4: Mount class_name_collision_test_b");
			print_line("========================================");
			Error err = launch_controller->launch_project(path_collision_b, String());
			_test_check(err == OK, "Mount call succeeded for " + path_collision_b,
					"Failed to mount " + path_collision_b + " (error " + itos(err) + ")");
			test_timer = 0.0;
			test_step = 10;
		} break;

		case 10: {
			test_timer += delta;
			if (test_timer >= 0.3) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(mounted, "Project is mounted after wait (cycle 4).",
						"Project should be mounted but is not (cycle 4).");
				launch_controller->stop_project();
				test_timer = 0.0;
				test_step = 11;
			}
		} break;

		case 11: {
			test_timer += delta;
			if (test_timer >= delta) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(!mounted, "Project unmounted successfully (cycle 4).",
						"Project still mounted after stop_project (cycle 4).");
				if (debug) {
					int leaked = debug->get_leaked_references_after_unmount();
					_test_check(leaked == 0, "No leaked references after unmount (cycle 4).",
							"Leaked references after unmount (cycle 4): " + itos(leaked));
				}
				test_timer = 0.0;
				test_step = 12;
			}
		} break;

		// Cycle 5: Mount autoload_reset_test
		case 12: {
			print_line("========================================");
			print_line("[DEVPLAYER TEST] Cycle 5: Mount autoload_reset_test");
			print_line("========================================");
			Error err = launch_controller->launch_project(path_autoload, String());
			_test_check(err == OK, "Mount call succeeded for " + path_autoload,
					"Failed to mount " + path_autoload + " (error " + itos(err) + ")");
			test_timer = 0.0;
			test_step = 13;
		} break;

		case 13: {
			test_timer += delta;
			if (test_timer >= 0.3) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(mounted, "Project is mounted after wait (cycle 5).",
						"Project should be mounted but is not (cycle 5).");
				if (debug) {
					int autoloads = debug->get_active_autoload_count();
					_test_check(autoloads > 0,
							"Autoloads created for autoload_reset_test: " + itos(autoloads),
							"No autoloads created for autoload_reset_test (expected > 0).");
				}
				launch_controller->stop_project();
				test_timer = 0.0;
				test_step = 14;
			}
		} break;

		case 14: {
			test_timer += delta;
			if (test_timer >= delta) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(!mounted, "Project unmounted successfully (cycle 5).",
						"Project still mounted after stop_project (cycle 5).");
				if (debug) {
					int leaked = debug->get_leaked_references_after_unmount();
					_test_check(leaked == 0, "No leaked references after unmount (cycle 5).",
							"Leaked references after unmount (cycle 5): " + itos(leaked));
				}
				test_timer = 0.0;
				test_step = 15;
			}
		} break;

		// Cycle 6: Remount autoload_reset_test
		case 15: {
			print_line("========================================");
			print_line("[DEVPLAYER TEST] Cycle 6: Remount autoload_reset_test");
			print_line("========================================");
			Error err = launch_controller->launch_project(path_autoload, String());
			_test_check(err == OK, "Remount call succeeded for " + path_autoload,
					"Failed to remount " + path_autoload + " (error " + itos(err) + ")");
			test_timer = 0.0;
			test_step = 16;
		} break;

		case 16: {
			test_timer += delta;
			if (test_timer >= 0.3) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(mounted, "Project is mounted after remount (cycle 6).",
						"Project should be mounted but is not (cycle 6).");
				if (debug) {
					int autoloads = debug->get_active_autoload_count();
					_test_check(autoloads > 0,
							"Autoloads re-created on remount: " + itos(autoloads),
							"No autoloads on remount (expected > 0, count was reset).");
				}
				launch_controller->stop_project();
				test_timer = 0.0;
				test_step = 17;
			}
		} break;

		case 17: {
			test_timer += delta;
			if (test_timer >= delta) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(!mounted, "Project unmounted successfully (cycle 6).",
						"Project still mounted after stop_project (cycle 6).");
				if (debug) {
					int leaked = debug->get_leaked_references_after_unmount();
					_test_check(leaked == 0, "No leaked references after unmount (cycle 6).",
							"Leaked references after unmount (cycle 6): " + itos(leaked));
				}
				stress_cycle_index = 0;
				test_timer = 0.0;
				test_step = 18;
			}
		} break;

		// Stress test: 50 rapid A/B cycles
		case 18: {
			if (stress_cycle_index >= STRESS_CYCLE_COUNT) {
				test_step = 20;
				break;
			}
			bool use_a = (stress_cycle_index % 2 == 0);
			String stress_path = use_a ? path_minimal_2d : path_collision_a;
			String stress_name = use_a ? "minimal_2d" : "class_name_collision_test_a";
			print_line("[DEVPLAYER TEST] Stress cycle " + itos(stress_cycle_index + 1) +
					"/" + itos(STRESS_CYCLE_COUNT) + ": Mounting " + stress_name);
			Error err = launch_controller->launch_project(stress_path, String());
			_test_check(err == OK,
					"Stress mount succeeded: " + stress_name + " (cycle " + itos(stress_cycle_index + 1) + ")",
					"Stress mount failed: " + stress_name + " (cycle " + itos(stress_cycle_index + 1) + ", error " + itos(err) + ")");
			test_timer = 0.0;
			test_step = 19;
		} break;

		case 19: {
			test_timer += delta;
			if (test_timer >= 0.3) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(mounted, "Project mounted in stress cycle " + itos(stress_cycle_index + 1) + ".",
						"Project NOT mounted in stress cycle " + itos(stress_cycle_index + 1) + ".");
				launch_controller->stop_project();
				mounted = project_domain_manager->is_project_mounted();
				_test_check(!mounted, "Project unmounted in stress cycle " + itos(stress_cycle_index + 1) + ".",
						"Project still mounted after stop in stress cycle " + itos(stress_cycle_index + 1) + ".");
				if (debug) {
					int leaked = debug->get_leaked_references_after_unmount();
					_test_check(leaked == 0,
							"No leaked refs in stress cycle " + itos(stress_cycle_index + 1) + ".",
							"Leaked refs in stress cycle " + itos(stress_cycle_index + 1) + ": " + itos(leaked));
				}
				stress_cycle_index++;
				test_timer = 0.0;
				test_step = 18;
			}
		} break;

		// Final: Report results and quit
		case 20: {
			print_line("");
			print_line("========================================");
			print_line("[DEVPLAYER TEST] All test cycles complete.");
			print_line("========================================");
			print_line("[DEVPLAYER TEST] Test projects exercised:");
			print_line("  - minimal_2d");
			print_line("  - class_name_collision_test_a");
			print_line("  - class_name_collision_test_b");
			print_line("  - autoload_reset_test");
			print_line("[DEVPLAYER TEST] Cycles: 6 named + " + itos(STRESS_CYCLE_COUNT) + " stress = " + itos(6 + STRESS_CYCLE_COUNT) + " total");
			print_line("----------------------------------------");
			print_line("[DEVPLAYER TEST] Passed: " + itos(test_pass_count));
			print_line("[DEVPLAYER TEST] Failed: " + itos(test_fail_count));
			print_line("[DEVPLAYER TEST] Total:  " + itos(test_pass_count + test_fail_count));
			if (test_fail_count == 0) {
				print_line("[TEST PASS] ALL TESTS PASSED");
			} else {
				print_line("[TEST FAIL] SOME TESTS FAILED");
			}
			print_line("========================================");
			SceneTree *tree = SceneTree::get_singleton();
			if (tree) {
				tree->quit(test_fail_count > 0 ? 1 : 0);
			}
			automated_test_mode = false;
			test_step = 21;
		} break;

		default:
			break;
	}
}

// ==========================================================================
// Constructor / Destructor
// ==========================================================================

DevPlayerShell::DevPlayerShell() {
	// Capture engine root path immediately in the constructor, before any
	// auto-mount can change the resource path.
	_capture_engine_root();
}

DevPlayerShell::~DevPlayerShell() {
}
