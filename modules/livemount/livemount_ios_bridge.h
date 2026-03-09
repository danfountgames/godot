/**************************************************************************/
/*  livemount_ios_bridge.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                        Godot LiveMount Module                          */
/**************************************************************************/
/*  Pure C API for bridging LiveMount C++ singletons to Swift/ObjC.      */
/*  Compiled into libgodot.a by scons. Called from the Xcode project's   */
/*  ObjC++/Swift code via the bridging header.                           */
/**************************************************************************/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ---- Project lifecycle ----
int livemount_is_project_mounted(void);
const char *livemount_get_mounted_path(void);
const char *livemount_get_mounted_scene(void);
int livemount_launch_project(const char *path, const char *scene);
void livemount_stop_project(void);
void livemount_relaunch(void);

// ---- Time controls ----
void livemount_set_paused(int paused);
int livemount_is_paused(void);
void livemount_step_frame(void);
void livemount_set_time_scale(float scale);
float livemount_get_time_scale(void);

// ---- Debug metrics ----
double livemount_get_last_mount_duration(void);
int livemount_get_tracked_resource_count(void);
int livemount_get_active_autoload_count(void);
int livemount_get_leaked_references(void);

// ---- Log capture ----
// Callback receives: message, is_error (1=error, 0=info), userdata.
typedef void (*livemount_log_callback_t)(const char *message, int is_error, void *userdata);
void livemount_register_log_callback(livemount_log_callback_t callback, void *userdata);
void livemount_unregister_log_callback(void);

// ---- File I/O ----
// Returns the base directory for synced projects on device.
const char *livemount_get_projects_dir(void);
// Write data to a file relative to a project root.
int livemount_write_file(const char *project_id, const char *rel_path,
		const void *data, int data_len);
// Delete a file relative to a project root.
int livemount_delete_file(const char *project_id, const char *rel_path);

// ---- LiveMount mode check ----
int livemount_is_active(void);

#ifdef __cplusplus
}
#endif
