/**************************************************************************/
/*  launch_controller.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           Godot LiveMount Module                            */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/object/ref_counted.h"
#include "core/string/ustring.h"

class Node;
class PackedScene;
class ProjectDomainManager;
class ScriptDomainManager;
class AutoloadSessionManager;
class ProjectSettingsLayerManager;
class ResourceDomainManager;
class ImportSessionManager;
class LiveMountDebug;

class LaunchController : public Object {
	GDCLASS(LaunchController, Object);

	static LaunchController *singleton;

	ProjectDomainManager *project_domain_manager = nullptr;
	ScriptDomainManager *script_domain_manager = nullptr;
	AutoloadSessionManager *autoload_session_manager = nullptr;
	ProjectSettingsLayerManager *project_settings_layer_manager = nullptr;
	ResourceDomainManager *resource_domain_manager = nullptr;
	ImportSessionManager *import_session_manager = nullptr;
	LiveMountDebug *debug = nullptr;

	// The currently loaded project scene node, added to the SceneTree.
	Node *mounted_scene_node = nullptr;

protected:
	static void _bind_methods();

public:
	static LaunchController *get_singleton();

	void set_managers(
			ProjectDomainManager *p_project_domain,
			ScriptDomainManager *p_script_domain,
			AutoloadSessionManager *p_autoload_session,
			ProjectSettingsLayerManager *p_settings_layer,
			ResourceDomainManager *p_resource_domain,
			ImportSessionManager *p_import_session,
			LiveMountDebug *p_debug);

	Error launch_project(const String &p_path, const String &p_target_scene);
	void stop_project();
	void relaunch();

	LaunchController();
	~LaunchController();
};
