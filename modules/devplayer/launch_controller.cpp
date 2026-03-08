/**************************************************************************/
/*  launch_controller.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           GODOT BEAM ENGINE                            */
/**************************************************************************/

#include "launch_controller.h"

#include "autoload_session_manager.h"
#include "devplayer_debug.h"
#include "import_session_manager.h"
#include "project_domain_manager.h"
#include "project_settings_layer_manager.h"
#include "resource_domain_manager.h"
#include "script_domain_manager.h"

#include "core/config/project_settings.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/packed_scene.h"

LaunchController *LaunchController::singleton = nullptr;

LaunchController *LaunchController::get_singleton() {
	return singleton;
}

void LaunchController::_bind_methods() {
	ClassDB::bind_method(D_METHOD("launch_project", "path", "target_scene"), &LaunchController::launch_project);
	ClassDB::bind_method(D_METHOD("stop_project"), &LaunchController::stop_project);
	ClassDB::bind_method(D_METHOD("relaunch"), &LaunchController::relaunch);
}

void LaunchController::set_managers(
		ProjectDomainManager *p_project_domain,
		ScriptDomainManager *p_script_domain,
		AutoloadSessionManager *p_autoload_session,
		ProjectSettingsLayerManager *p_settings_layer,
		ResourceDomainManager *p_resource_domain,
		ImportSessionManager *p_import_session,
		DevPlayerDebug *p_debug) {
	project_domain_manager = p_project_domain;
	script_domain_manager = p_script_domain;
	autoload_session_manager = p_autoload_session;
	project_settings_layer_manager = p_settings_layer;
	resource_domain_manager = p_resource_domain;
	import_session_manager = p_import_session;
	debug = p_debug;
}

Error LaunchController::launch_project(const String &p_path, const String &p_target_scene) {
	ERR_FAIL_COND_V_MSG(p_path.is_empty(), ERR_INVALID_PARAMETER, "[LaunchController] Project path is empty.");
	ERR_FAIL_NULL_V_MSG(project_domain_manager, ERR_UNCONFIGURED, "[LaunchController] Managers not configured.");
	ERR_FAIL_NULL_V_MSG(script_domain_manager, ERR_UNCONFIGURED, "[LaunchController] ScriptDomainManager not configured.");
	ERR_FAIL_NULL_V_MSG(autoload_session_manager, ERR_UNCONFIGURED, "[LaunchController] AutoloadSessionManager not configured.");
	ERR_FAIL_NULL_V_MSG(project_settings_layer_manager, ERR_UNCONFIGURED, "[LaunchController] ProjectSettingsLayerManager not configured.");
	ERR_FAIL_NULL_V_MSG(resource_domain_manager, ERR_UNCONFIGURED, "[LaunchController] ResourceDomainManager not configured.");
	ERR_FAIL_NULL_V_MSG(import_session_manager, ERR_UNCONFIGURED, "[LaunchController] ImportSessionManager not configured.");

	if (project_domain_manager->is_project_mounted()) {
		ERR_FAIL_V_MSG(ERR_ALREADY_IN_USE, "[LaunchController] A project is already mounted. Call stop_project() first.");
	}

	print_line("[LaunchController] === LAUNCHING PROJECT === ");
	print_line("[LaunchController] Path: " + p_path);
	print_line("[LaunchController] Target scene: " + p_target_scene);

	uint64_t start_time = OS::get_singleton()->get_ticks_usec();

	// Step 1: Capture base settings.
	project_settings_layer_manager->load_base_settings();

	// Step 2: Load project settings from target project.
	// This calls ProjectSettings::setup() which sets the resource_path so that
	// res:// resolves to the mounted project directory.
	Error err = project_settings_layer_manager->load_project_settings(p_path);
	if (err != OK) {
		ERR_PRINT("[LaunchController] Failed to load project settings.");
		return err;
	}

	// Step 3: Determine the target scene.
	// If a target scene was explicitly provided, use it. Otherwise, read from
	// the project's "application/run/main_scene" setting.
	String scene_path = p_target_scene;
	if (scene_path.is_empty()) {
		scene_path = ProjectSettings::get_singleton()->get_setting("application/run/main_scene", String());
		print_line("[LaunchController] Resolved main scene from project settings: " + scene_path);
	}

	if (scene_path.is_empty()) {
		project_settings_layer_manager->clear_project_settings();
		ERR_FAIL_V_MSG(ERR_FILE_NOT_FOUND, "[LaunchController] No target scene specified and no main_scene found in project settings.");
	}

	// Step 4: Mount the project domain (with the resolved scene path).
	err = project_domain_manager->mount_project(p_path, scene_path);
	if (err != OK) {
		project_settings_layer_manager->clear_project_settings();
		ERR_PRINT("[LaunchController] Failed to mount project.");
		return err;
	}

	// Step 5: Initialize script domain.
	script_domain_manager->initialize_for_project(p_path);

	// Step 5b: Scan and register global classes (class_name) from the project's GDScript files.
	// This must happen BEFORE loading any scenes, so that scripts referencing
	// class_name identifiers (e.g. var e = Enemy.new()) can resolve them.
	script_domain_manager->scan_and_register_global_classes(p_path);

	// Step 6: Begin resource tracking.
	resource_domain_manager->begin_tracking(p_path);

	// Step 7: Bind and scan imports.
	import_session_manager->bind_project_root(p_path);
	import_session_manager->scan_filesystem();

	// Step 8: Build autoloads.
	autoload_session_manager->build_autoloads_from_project();

	// Step 9: Commit effective settings.
	project_settings_layer_manager->commit_effective_settings();

	// Step 10: Load and instantiate the target scene.
	print_line("[LaunchController] Loading scene: " + scene_path);

	// Use CACHE_MODE_REPLACE_DEEP to ensure we always load fresh from disk.
	// This is critical for project switching: res://main.tscn from project A
	// must not be reused when mounting project B (same res:// path, different disk file).
	Ref<PackedScene> packed_scene = ResourceLoader::load(scene_path, "", ResourceFormatLoader::CACHE_MODE_REPLACE_DEEP);
	if (packed_scene.is_null()) {
		ERR_PRINT("[LaunchController] Failed to load scene resource: " + scene_path);
		// Roll back: unmount everything.
		autoload_session_manager->destroy_autoloads();
		script_domain_manager->shutdown_project_scripts();
		resource_domain_manager->purge_project_resources();
		import_session_manager->clear_import_state();
		project_settings_layer_manager->clear_project_settings();
		project_domain_manager->unmount_project();
		return ERR_CANT_OPEN;
	}

	mounted_scene_node = packed_scene->instantiate();
	if (!mounted_scene_node) {
		ERR_PRINT("[LaunchController] Failed to instantiate scene: " + scene_path);
		autoload_session_manager->destroy_autoloads();
		script_domain_manager->shutdown_project_scripts();
		resource_domain_manager->purge_project_resources();
		import_session_manager->clear_import_state();
		project_settings_layer_manager->clear_project_settings();
		project_domain_manager->unmount_project();
		return ERR_CANT_CREATE;
	}

	// Add the scene to the SceneTree root.
	SceneTree *scene_tree = SceneTree::get_singleton();
	if (!scene_tree) {
		ERR_PRINT("[LaunchController] SceneTree is not available.");
		memdelete(mounted_scene_node);
		mounted_scene_node = nullptr;
		autoload_session_manager->destroy_autoloads();
		script_domain_manager->shutdown_project_scripts();
		resource_domain_manager->purge_project_resources();
		import_session_manager->clear_import_state();
		project_settings_layer_manager->clear_project_settings();
		project_domain_manager->unmount_project();
		return ERR_UNCONFIGURED;
	}

	Window *root = scene_tree->get_root();
	if (!root) {
		ERR_PRINT("[LaunchController] SceneTree root is null.");
		memdelete(mounted_scene_node);
		mounted_scene_node = nullptr;
		autoload_session_manager->destroy_autoloads();
		script_domain_manager->shutdown_project_scripts();
		resource_domain_manager->purge_project_resources();
		import_session_manager->clear_import_state();
		project_settings_layer_manager->clear_project_settings();
		project_domain_manager->unmount_project();
		return ERR_UNCONFIGURED;
	}

	root->add_child(mounted_scene_node);
	print_line("[LaunchController] Scene instantiated and added to SceneTree: " + scene_path);

	// Update debug metrics.
	uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - start_time;
	double mount_duration = elapsed / 1000000.0;

	if (debug) {
		debug->set_mounted_project_path(p_path);
		debug->set_target_scene(scene_path);
		debug->set_last_mount_duration(mount_duration);
		debug->set_active_autoload_count(autoload_session_manager->get_active_autoload_count());
		debug->set_tracked_resource_count(resource_domain_manager->get_tracked_resource_count());
	}

	print_line("[LaunchController] === PROJECT LAUNCHED in " + rtos(mount_duration) + "s ===");
	return OK;
}

void LaunchController::stop_project() {
	ERR_FAIL_NULL_MSG(project_domain_manager, "[LaunchController] Managers not configured.");
	ERR_FAIL_NULL_MSG(script_domain_manager, "[LaunchController] ScriptDomainManager not configured.");
	ERR_FAIL_NULL_MSG(autoload_session_manager, "[LaunchController] AutoloadSessionManager not configured.");
	ERR_FAIL_NULL_MSG(project_settings_layer_manager, "[LaunchController] ProjectSettingsLayerManager not configured.");
	ERR_FAIL_NULL_MSG(resource_domain_manager, "[LaunchController] ResourceDomainManager not configured.");
	ERR_FAIL_NULL_MSG(import_session_manager, "[LaunchController] ImportSessionManager not configured.");

	if (!project_domain_manager->is_project_mounted()) {
		print_line("[LaunchController] No project mounted, nothing to stop.");
		return;
	}

	print_line("[LaunchController] === STOPPING PROJECT ===");

	uint64_t start_time = OS::get_singleton()->get_ticks_usec();

	// Step 1: Remove the mounted scene from the SceneTree and free immediately.
	// We use memdelete instead of queue_free so that script instances are released
	// before we clear the GDScript cache and ResourceCache. If we used queue_free,
	// the node would still hold references to GDScript resources, preventing
	// proper cache eviction and causing stale scripts on remount.
	if (mounted_scene_node) {
		print_line("[LaunchController] Removing mounted scene from SceneTree...");
		Node *parent = mounted_scene_node->get_parent();
		if (parent) {
			parent->remove_child(mounted_scene_node);
		}
		memdelete(mounted_scene_node);
		mounted_scene_node = nullptr;
		print_line("[LaunchController] Mounted scene removed and freed.");
	}

	// Step 2: Destroy autoloads.
	autoload_session_manager->destroy_autoloads();

	// Step 3: Shutdown scripts.
	script_domain_manager->shutdown_project_scripts();

	// Step 4: Purge resources.
	resource_domain_manager->purge_project_resources();

	// Step 5: Clear import state.
	import_session_manager->clear_import_state();

	// Step 6: Restore base settings (this also restores the original resource_path).
	project_settings_layer_manager->clear_project_settings();

	// Step 7: Unmount project domain.
	project_domain_manager->unmount_project();

	// Update debug metrics.
	uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - start_time;
	double unmount_duration = elapsed / 1000000.0;

	if (debug) {
		debug->set_last_unmount_duration(unmount_duration);
		debug->set_mounted_project_path(String());
		debug->set_target_scene(String());
		debug->set_active_autoload_count(0);
		debug->set_tracked_resource_count(0);
	}

	print_line("[LaunchController] === PROJECT STOPPED in " + rtos(unmount_duration) + "s ===");
}

void LaunchController::relaunch() {
	ERR_FAIL_NULL_MSG(project_domain_manager, "[LaunchController] Managers not configured.");

	if (!project_domain_manager->is_project_mounted()) {
		ERR_PRINT("[LaunchController] Cannot relaunch: no project is mounted.");
		return;
	}

	String path = project_domain_manager->get_mounted_project_path();
	String scene = project_domain_manager->get_target_scene();

	print_line("[LaunchController] Relaunching project: " + path);

	stop_project();
	launch_project(path, scene);
}

LaunchController::LaunchController() {
	singleton = this;
}

LaunchController::~LaunchController() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
