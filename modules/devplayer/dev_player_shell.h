/**************************************************************************/
/*  dev_player_shell.h                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           GODOT BEAM ENGINE                            */
/**************************************************************************/

#pragma once

#include "scene/gui/control.h"

class Label;
class Button;
class LineEdit;
class VBoxContainer;
class LaunchController;
class ProjectDomainManager;
class AutoloadSessionManager;
class ResourceDomainManager;
class DevPlayerDebug;

class DevPlayerShell : public Control {
	GDCLASS(DevPlayerShell, Control);

	LaunchController *launch_controller = nullptr;
	ProjectDomainManager *project_domain_manager = nullptr;
	AutoloadSessionManager *autoload_session_manager = nullptr;
	ResourceDomainManager *resource_domain_manager = nullptr;
	DevPlayerDebug *debug = nullptr;

	// UI elements.
	Label *title_label = nullptr;
	VBoxContainer *info_container = nullptr;
	Label *status_label = nullptr;
	Label *project_path_label = nullptr;
	Label *resource_count_label = nullptr;
	Label *autoload_count_label = nullptr;
	Label *mount_duration_label = nullptr;
	LineEdit *path_line_edit = nullptr;
	Button *mount_button = nullptr;
	Button *unmount_button = nullptr;

	// Container for the shell UI so we can hide/show it.
	VBoxContainer *shell_ui_root = nullptr;

	void _build_ui();
	void _update_debug_display();
	void _on_mount_pressed();
	void _on_unmount_pressed();

	// Engine root path — captured once at startup before any mount changes
	// the engine's resource_path. Used to construct test project paths.
	String engine_root_path;
	bool engine_root_captured = false;
	void _capture_engine_root();

	// Automated test mode (--devplayer-test).
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
	String _test_project_path(const String &p_project_name) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_managers(
			LaunchController *p_launch_controller,
			ProjectDomainManager *p_project_domain,
			AutoloadSessionManager *p_autoload_session,
			ResourceDomainManager *p_resource_domain,
			DevPlayerDebug *p_debug);

	void set_automated_test_mode(bool p_enabled);

	DevPlayerShell();
	~DevPlayerShell();
};
