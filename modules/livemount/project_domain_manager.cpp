/**************************************************************************/
/*  project_domain_manager.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           Godot LiveMount Module                            */
/**************************************************************************/

#include "project_domain_manager.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/object/class_db.h"
#include "core/string/print_string.h"

ProjectDomainManager *ProjectDomainManager::singleton = nullptr;

ProjectDomainManager *ProjectDomainManager::get_singleton() {
	return singleton;
}

void ProjectDomainManager::_bind_methods() {
	ClassDB::bind_method(D_METHOD("mount_project", "path", "target_scene"), &ProjectDomainManager::mount_project);
	ClassDB::bind_method(D_METHOD("unmount_project"), &ProjectDomainManager::unmount_project);
	ClassDB::bind_method(D_METHOD("relaunch_target_scene"), &ProjectDomainManager::relaunch_target_scene);
	ClassDB::bind_method(D_METHOD("is_project_mounted"), &ProjectDomainManager::is_project_mounted);
	ClassDB::bind_method(D_METHOD("get_mounted_project_path"), &ProjectDomainManager::get_mounted_project_path);
	ClassDB::bind_method(D_METHOD("get_target_scene"), &ProjectDomainManager::get_target_scene);
}

Error ProjectDomainManager::mount_project(const String &p_path, const String &p_target_scene) {
	ERR_FAIL_COND_V_MSG(mounted, ERR_ALREADY_IN_USE, "[ProjectDomainManager] A project is already mounted.");
	ERR_FAIL_COND_V_MSG(p_path.is_empty(), ERR_INVALID_PARAMETER, "[ProjectDomainManager] Project path is empty.");

	// Resolve the path: if it starts with res://, globalize it.
	String abs_path = p_path;
	if (abs_path.begins_with("res://")) {
		abs_path = ProjectSettings::get_singleton()->globalize_path(abs_path);
	}

	if (!DirAccess::exists(abs_path)) {
		ERR_FAIL_V_MSG(ERR_FILE_NOT_FOUND, "[ProjectDomainManager] Project path does not exist: " + abs_path);
	}

	project_path = abs_path;
	target_scene = p_target_scene;
	mounted = true;

	// At this point, ProjectSettings::setup() has already been called by the
	// ProjectSettingsLayerManager, which sets the resource_path internally.
	// This means res:// now resolves to the mounted project's directory.
	print_line("[ProjectDomainManager] Project mounted: " + project_path);
	print_line("[ProjectDomainManager] res:// now maps to: " + ProjectSettings::get_singleton()->get_resource_path());
	if (!target_scene.is_empty()) {
		print_line("[ProjectDomainManager] Target scene: " + target_scene);
	}
	return OK;
}

void ProjectDomainManager::unmount_project() {
	if (!mounted) {
		print_line("[ProjectDomainManager] No project is mounted, nothing to unmount.");
		return;
	}

	print_line("[ProjectDomainManager] Unmounting project: " + project_path);

	project_path = String();
	target_scene = String();
	mounted = false;

	print_line("[ProjectDomainManager] Project unmounted.");
}

void ProjectDomainManager::relaunch_target_scene() {
	ERR_FAIL_COND_MSG(!mounted, "[ProjectDomainManager] Cannot relaunch: no project mounted.");
	ERR_FAIL_COND_MSG(target_scene.is_empty(), "[ProjectDomainManager] Cannot relaunch: no target scene specified.");

	print_line("[ProjectDomainManager] Relaunching target scene: " + target_scene);
	// The actual scene change is orchestrated by LaunchController.
}

bool ProjectDomainManager::is_project_mounted() const {
	return mounted;
}

String ProjectDomainManager::get_mounted_project_path() const {
	return project_path;
}

String ProjectDomainManager::get_target_scene() const {
	return target_scene;
}

ProjectDomainManager::ProjectDomainManager() {
	singleton = this;
}

ProjectDomainManager::~ProjectDomainManager() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
