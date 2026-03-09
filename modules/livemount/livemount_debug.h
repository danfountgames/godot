/**************************************************************************/
/*  livemount_debug.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           Godot LiveMount Module                            */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/string/ustring.h"

class LiveMountDebug : public Object {
	GDCLASS(LiveMountDebug, Object);

	static LiveMountDebug *singleton;

	String mounted_project_path;
	String target_scene;
	int active_autoload_count = 0;
	int tracked_resource_count = 0;
	int tracked_script_cache_count = 0;
	int active_script_class_count = 0;
	double last_import_duration = 0.0;
	double last_mount_duration = 0.0;
	double last_unmount_duration = 0.0;
	int reload_tier = 0;
	int leaked_references_after_unmount = 0;

protected:
	static void _bind_methods();

public:
	static LiveMountDebug *get_singleton();

	void set_mounted_project_path(const String &p_path);
	String get_mounted_project_path() const;

	void set_target_scene(const String &p_scene);
	String get_target_scene() const;

	void set_active_autoload_count(int p_count);
	int get_active_autoload_count() const;

	void set_tracked_resource_count(int p_count);
	int get_tracked_resource_count() const;

	void set_tracked_script_cache_count(int p_count);
	int get_tracked_script_cache_count() const;

	void set_active_script_class_count(int p_count);
	int get_active_script_class_count() const;

	void set_last_import_duration(double p_duration);
	double get_last_import_duration() const;

	void set_last_mount_duration(double p_duration);
	double get_last_mount_duration() const;

	void set_last_unmount_duration(double p_duration);
	double get_last_unmount_duration() const;

	void set_reload_tier(int p_tier);
	int get_reload_tier() const;

	void set_leaked_references_after_unmount(int p_count);
	int get_leaked_references_after_unmount() const;

	void reset_all();
	void print_status();

	LiveMountDebug();
	~LiveMountDebug();
};
