/**************************************************************************/
/*  livemount_shell.h                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           Godot LiveMount Module                            */
/**************************************************************************/

#pragma once

#include "scene/gui/control.h"
#include "core/input/input_event.h"
#include "core/templates/vector.h"

class ColorRect;
class Label;
class Button;
class LineEdit;
class VBoxContainer;
class HBoxContainer;
class HSeparator;
class ItemList;
class RichTextLabel;
class ScrollContainer;
class LaunchController;
class ProjectDomainManager;
class AutoloadSessionManager;
class ResourceDomainManager;
class ImportSessionManager;
class LiveMountDebug;
class GitManager;
class SyncServer;

class LiveMountShell : public Control {
	GDCLASS(LiveMountShell, Control);

	// Manager references.
	LaunchController *launch_controller = nullptr;
	ProjectDomainManager *project_domain_manager = nullptr;
	AutoloadSessionManager *autoload_session_manager = nullptr;
	ResourceDomainManager *resource_domain_manager = nullptr;
	ImportSessionManager *import_session_manager = nullptr;
	LiveMountDebug *debug = nullptr;
	GitManager *git_manager = nullptr;
	SyncServer *sync_server = nullptr;

	// ---- Shell UI elements ----
	ColorRect *shell_bg = nullptr;
	VBoxContainer *shell_ui_root = nullptr;

	// Status section.
	Label *title_label = nullptr;
	Label *status_label = nullptr;
	Label *project_path_label = nullptr;
	Label *metrics_label = nullptr;

	// Project list section.
	ItemList *project_list = nullptr;
	Button *mount_selected_button = nullptr;
	Button *refresh_button = nullptr;

	// Custom path section.
	LineEdit *path_line_edit = nullptr;
	Button *mount_custom_button = nullptr;

	// Mounted project controls.
	Button *unmount_button = nullptr;
	Button *relaunch_button = nullptr;

	// Git section.
	Label *git_status_label = nullptr;
	LineEdit *git_repo_line_edit = nullptr;
	LineEdit *git_branch_line_edit = nullptr;
	Button *git_switch_button = nullptr;
	Button *git_list_branches_button = nullptr;
	Label *git_branches_label = nullptr;

	// Sync section.
	Label *sync_status_label = nullptr;
	LineEdit *sync_port_line_edit = nullptr;
	Button *sync_start_button = nullptr;
	Button *sync_stop_button = nullptr;

	// Log section.
	RichTextLabel *log_output = nullptr;

	// Back-to-shell overlay (visible when project is running).
	Button *back_to_shell_button = nullptr;

	// ---- Project discovery ----
	struct ProjectEntry {
		String name;
		String path;
		bool operator<(const ProjectEntry &p_other) const {
			return name.naturalcasecmp_to(p_other.name) < 0;
		}
	};
	Vector<ProjectEntry> discovered_projects;

	// ---- Build UI methods ----
	void _build_ui();
	void _build_status_section();
	void _build_project_section();
	void _build_git_section();
	void _build_sync_section();
	void _build_log_section();
	void _build_overlay();
	void _apply_ui_scale();

	// ---- UI callbacks ----
	void _on_mount_selected_pressed();
	void _on_mount_custom_pressed();
	void _on_unmount_pressed();
	void _on_relaunch_pressed();
	void _on_refresh_pressed();
	void _on_project_item_activated(int p_index);
	void _on_git_switch_pressed();
	void _on_git_list_branches_pressed();
	void _on_sync_start_pressed();
	void _on_sync_stop_pressed();
	void _on_back_to_shell_pressed();

	// ---- Runtime updates ----
	void _update_status_display();
	void _update_sync_display();
	void _update_button_states();
	void _scan_projects();
	void _populate_project_list();
	void _log(const String &p_message);
	void _mount_project(const String &p_path);

	// ---- Shell visibility ----
	bool ui_built = false;
	bool shell_visible = true;
	void _show_shell();
	void _hide_shell();

	// Engine root path — captured once at startup before any mount changes
	// the engine's resource_path. Used to construct test project paths.
	String engine_root_path;
	bool engine_root_captured = false;
	void _capture_engine_root();

	// ---- Automated test mode (--livemount-test) ----
	bool automated_test_mode = false;
	int test_step = 0;
	double test_timer = 0.0;
	int test_pass_count = 0;
	int test_fail_count = 0;
	int stress_cycle_index = 0;
	static const int STRESS_CYCLE_COUNT = 50;
	void _run_test_step();

	// Test helpers.
	void _test_check(bool p_condition, const String &p_pass_msg, const String &p_fail_msg);
	String _projects_base_dir() const;
	String _test_project_path(const String &p_project_name) const;

protected:
	virtual void input(const Ref<InputEvent> &p_event) override;
	virtual void unhandled_key_input(const Ref<InputEvent> &p_event) override;

	// Three-finger drag tracking for exit-to-shell gesture.
	HashMap<int, Vector2> active_touches;
	double three_finger_cumulative_y = 0.0;
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_managers(
			LaunchController *p_launch_controller,
			ProjectDomainManager *p_project_domain,
			AutoloadSessionManager *p_autoload_session,
			ResourceDomainManager *p_resource_domain,
			LiveMountDebug *p_debug);

	void set_automated_test_mode(bool p_enabled);

	LiveMountShell();
	~LiveMountShell();
};
