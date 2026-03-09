/**************************************************************************/
/*  project_domain_manager.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           Godot LiveMount Module                            */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/string/ustring.h"

class ProjectDomainManager : public Object {
	GDCLASS(ProjectDomainManager, Object);

	static ProjectDomainManager *singleton;

	bool mounted = false;
	String project_path;
	String target_scene;

protected:
	static void _bind_methods();

public:
	static ProjectDomainManager *get_singleton();

	Error mount_project(const String &p_path, const String &p_target_scene);
	void unmount_project();
	void relaunch_target_scene();
	bool is_project_mounted() const;
	String get_mounted_project_path() const;
	String get_target_scene() const;

	ProjectDomainManager();
	~ProjectDomainManager();
};
