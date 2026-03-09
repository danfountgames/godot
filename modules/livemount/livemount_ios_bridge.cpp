/**************************************************************************/
/*  livemount_ios_bridge.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                        Godot LiveMount Module                          */
/**************************************************************************/
/*  C++ implementation of the pure-C bridge declared in                   */
/*  livemount_ios_bridge.h. Compiled by scons into libgodot.a.            */
/*  Swift/ObjC code in the Xcode project calls these functions            */
/*  via the bridging header.                                              */
/**************************************************************************/

#include "livemount_ios_bridge.h"

#include "livemount_debug.h"
#include "launch_controller.h"
#include "project_domain_manager.h"

#include "core/config/engine.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "scene/main/scene_tree.h"

// Static buffers for returning C strings. These persist until the next call
// to the same function. Thread safety: all calls must be from the main thread
// (which is guaranteed on iOS since UIKit + Godot both run on main).
static CharString s_mounted_path_buf;
static CharString s_mounted_scene_buf;
static CharString s_projects_dir_buf;

// ---- Log capture state ----
static livemount_log_callback_t s_log_callback = nullptr;
static void *s_log_userdata = nullptr;
static PrintHandlerList s_print_handler;

static void _bridge_print_handler(void *p_userdata, const String &p_string, bool p_error, bool p_rich) {
	if (s_log_callback) {
		CharString utf8 = p_string.utf8();
		s_log_callback(utf8.get_data(), p_error ? 1 : 0, s_log_userdata);
	}
}

// ===========================================================================
// Project lifecycle
// ===========================================================================

extern "C" {

int livemount_is_project_mounted(void) {
	ProjectDomainManager *pdm = ProjectDomainManager::get_singleton();
	return (pdm && pdm->is_project_mounted()) ? 1 : 0;
}

const char *livemount_get_mounted_path(void) {
	ProjectDomainManager *pdm = ProjectDomainManager::get_singleton();
	if (!pdm || !pdm->is_project_mounted()) {
		return "";
	}
	s_mounted_path_buf = pdm->get_mounted_project_path().utf8();
	return s_mounted_path_buf.get_data();
}

const char *livemount_get_mounted_scene(void) {
	ProjectDomainManager *pdm = ProjectDomainManager::get_singleton();
	if (!pdm || !pdm->is_project_mounted()) {
		return "";
	}
	s_mounted_scene_buf = pdm->get_target_scene().utf8();
	return s_mounted_scene_buf.get_data();
}

int livemount_launch_project(const char *path, const char *scene) {
	LaunchController *lc = LaunchController::get_singleton();
	if (!lc) {
		return 1;
	}
	String godot_path = String::utf8(path ? path : "");
	String godot_scene = String::utf8(scene ? scene : "");
	Error err = lc->launch_project(godot_path, godot_scene);
	return (err == OK) ? 0 : 1;
}

void livemount_stop_project(void) {
	LaunchController *lc = LaunchController::get_singleton();
	if (lc) {
		lc->stop_project();
	}
}

void livemount_relaunch(void) {
	LaunchController *lc = LaunchController::get_singleton();
	if (lc) {
		lc->relaunch();
	}
}

// ===========================================================================
// Time controls
// ===========================================================================

void livemount_set_paused(int paused) {
	SceneTree *st = SceneTree::get_singleton();
	if (st) {
		st->set_pause(paused != 0);
	}
}

int livemount_is_paused(void) {
	SceneTree *st = SceneTree::get_singleton();
	return (st && st->is_paused()) ? 1 : 0;
}

void livemount_step_frame(void) {
	// Step-frame: temporarily unpause so one frame processes.
	// The caller (SwiftUI layer) should re-pause on the next frame.
	SceneTree *st = SceneTree::get_singleton();
	if (st && st->is_paused()) {
		st->set_pause(false);
	}
}

void livemount_set_time_scale(float scale) {
	Engine *engine = Engine::get_singleton();
	if (engine) {
		engine->set_time_scale(scale > 0.0f ? scale : 0.01f);
	}
}

float livemount_get_time_scale(void) {
	Engine *engine = Engine::get_singleton();
	return engine ? (float)engine->get_time_scale() : 1.0f;
}

// ===========================================================================
// Debug metrics
// ===========================================================================

double livemount_get_last_mount_duration(void) {
	LiveMountDebug *dbg = LiveMountDebug::get_singleton();
	return dbg ? dbg->get_last_mount_duration() : 0.0;
}

int livemount_get_tracked_resource_count(void) {
	LiveMountDebug *dbg = LiveMountDebug::get_singleton();
	return dbg ? dbg->get_tracked_resource_count() : 0;
}

int livemount_get_active_autoload_count(void) {
	LiveMountDebug *dbg = LiveMountDebug::get_singleton();
	return dbg ? dbg->get_active_autoload_count() : 0;
}

int livemount_get_leaked_references(void) {
	LiveMountDebug *dbg = LiveMountDebug::get_singleton();
	return dbg ? dbg->get_leaked_references_after_unmount() : 0;
}

// ===========================================================================
// Log capture
// ===========================================================================

void livemount_register_log_callback(livemount_log_callback_t callback, void *userdata) {
	// Unregister previous handler if any.
	if (s_log_callback) {
		remove_print_handler(&s_print_handler);
	}

	s_log_callback = callback;
	s_log_userdata = userdata;

	if (callback) {
		s_print_handler.printfunc = _bridge_print_handler;
		s_print_handler.userdata = nullptr;
		s_print_handler.next = nullptr;
		add_print_handler(&s_print_handler);
	}
}

void livemount_unregister_log_callback(void) {
	if (s_log_callback) {
		remove_print_handler(&s_print_handler);
		s_log_callback = nullptr;
		s_log_userdata = nullptr;
	}
}

// ===========================================================================
// File I/O
// ===========================================================================

const char *livemount_get_projects_dir(void) {
	String dir = OS::get_singleton()->get_user_data_dir().path_join("projects");
	s_projects_dir_buf = dir.utf8();
	return s_projects_dir_buf.get_data();
}

int livemount_write_file(const char *project_id, const char *rel_path,
		const void *data, int data_len) {
	String base = OS::get_singleton()->get_user_data_dir().path_join("projects");
	String full_path = base.path_join(String::utf8(project_id ? project_id : ""))
								.path_join(String::utf8(rel_path ? rel_path : ""));

	// Ensure parent directory exists.
	String dir = full_path.get_base_dir();
	Ref<DirAccess> da = DirAccess::open(dir);
	if (da.is_null()) {
		DirAccess::make_dir_recursive_absolute(dir);
	}

	Ref<FileAccess> fa = FileAccess::open(full_path, FileAccess::WRITE);
	if (fa.is_null()) {
		ERR_PRINT("[LiveMountBridge] Failed to open file for writing: " + full_path);
		return 1;
	}

	if (data && data_len > 0) {
		fa->store_buffer((const uint8_t *)data, data_len);
	}
	fa->flush();
	return 0;
}

int livemount_delete_file(const char *project_id, const char *rel_path) {
	String base = OS::get_singleton()->get_user_data_dir().path_join("projects");
	String full_path = base.path_join(String::utf8(project_id ? project_id : ""))
								.path_join(String::utf8(rel_path ? rel_path : ""));

	Ref<DirAccess> da = DirAccess::open(full_path.get_base_dir());
	if (da.is_null()) {
		return 1;
	}

	Error err = da->remove(full_path.get_file());
	return (err == OK) ? 0 : 1;
}

// ===========================================================================
// LiveMount mode check
// ===========================================================================

int livemount_is_active(void) {
	// If LaunchController singleton exists, LiveMount module is active.
	return LaunchController::get_singleton() ? 1 : 0;
}

} // extern "C"
