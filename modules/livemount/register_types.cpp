/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                        Godot LiveMount Module                          */
/**************************************************************************/

#include "register_types.h"

#include "autoload_session_manager.h"
#include "livemount_shell.h"
#include "livemount_debug.h"
#include "git_manager.h"
#include "import_session_manager.h"
#include "launch_controller.h"
#include "project_domain_manager.h"
#include "project_settings_layer_manager.h"
#include "resource_domain_manager.h"
#include "script_domain_manager.h"
#include "sync_server.h"

#include "core/config/engine.h"
#include "core/string/print_string.h"

static LiveMountDebug *livemount_debug = nullptr;
static ProjectDomainManager *project_domain_manager = nullptr;
static ScriptDomainManager *script_domain_manager = nullptr;
static AutoloadSessionManager *autoload_session_manager = nullptr;
static ProjectSettingsLayerManager *project_settings_layer_manager = nullptr;
static ResourceDomainManager *resource_domain_manager = nullptr;
static ImportSessionManager *import_session_manager = nullptr;
static LaunchController *launch_controller = nullptr;
static GitManager *git_manager = nullptr;
static SyncServer *sync_server = nullptr;

void initialize_livemount_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		// Register all classes.
		GDREGISTER_CLASS(LiveMountDebug);
		GDREGISTER_CLASS(ProjectDomainManager);
		GDREGISTER_CLASS(ScriptDomainManager);
		GDREGISTER_CLASS(AutoloadSessionManager);
		GDREGISTER_CLASS(ProjectSettingsLayerManager);
		GDREGISTER_CLASS(ResourceDomainManager);
		GDREGISTER_CLASS(ImportSessionManager);
		GDREGISTER_CLASS(LaunchController);
		GDREGISTER_CLASS(LiveMountShell);
		GDREGISTER_CLASS(GitManager);
		GDREGISTER_CLASS(SyncServer);

		// Create singleton instances.
		livemount_debug = memnew(LiveMountDebug);
		project_domain_manager = memnew(ProjectDomainManager);
		script_domain_manager = memnew(ScriptDomainManager);
		autoload_session_manager = memnew(AutoloadSessionManager);
		project_settings_layer_manager = memnew(ProjectSettingsLayerManager);
		resource_domain_manager = memnew(ResourceDomainManager);
		import_session_manager = memnew(ImportSessionManager);
		launch_controller = memnew(LaunchController);
		git_manager = memnew(GitManager);
		sync_server = memnew(SyncServer);

		// Wire up the LaunchController with all manager references.
		launch_controller->set_managers(
				project_domain_manager,
				script_domain_manager,
				autoload_session_manager,
				project_settings_layer_manager,
				resource_domain_manager,
				import_session_manager,
				livemount_debug);

		// Wire up the GitManager with a reference to LaunchController.
		git_manager->set_launch_controller(launch_controller);

		// Register Engine singletons so they are accessible from GDScript.
		Engine::get_singleton()->add_singleton(Engine::Singleton("LiveMountDebug", LiveMountDebug::get_singleton()));
		Engine::get_singleton()->add_singleton(Engine::Singleton("ProjectDomainManager", ProjectDomainManager::get_singleton()));
		Engine::get_singleton()->add_singleton(Engine::Singleton("ScriptDomainManager", ScriptDomainManager::get_singleton()));
		Engine::get_singleton()->add_singleton(Engine::Singleton("AutoloadSessionManager", AutoloadSessionManager::get_singleton()));
		Engine::get_singleton()->add_singleton(Engine::Singleton("ProjectSettingsLayerManager", ProjectSettingsLayerManager::get_singleton()));
		Engine::get_singleton()->add_singleton(Engine::Singleton("ResourceDomainManager", ResourceDomainManager::get_singleton()));
		Engine::get_singleton()->add_singleton(Engine::Singleton("ImportSessionManager", ImportSessionManager::get_singleton()));
		Engine::get_singleton()->add_singleton(Engine::Singleton("LaunchController", LaunchController::get_singleton()));
		Engine::get_singleton()->add_singleton(Engine::Singleton("GitManager", GitManager::get_singleton()));
		Engine::get_singleton()->add_singleton(Engine::Singleton("SyncServer", SyncServer::get_singleton()));

		print_line("[LiveMount] Module initialized. All singletons registered.");
	}
}

void uninitialize_livemount_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		print_line("[LiveMount] Module shutting down...");

		Engine::get_singleton()->remove_singleton("SyncServer");
		Engine::get_singleton()->remove_singleton("GitManager");
		Engine::get_singleton()->remove_singleton("LaunchController");
		Engine::get_singleton()->remove_singleton("ImportSessionManager");
		Engine::get_singleton()->remove_singleton("ResourceDomainManager");
		Engine::get_singleton()->remove_singleton("ProjectSettingsLayerManager");
		Engine::get_singleton()->remove_singleton("AutoloadSessionManager");
		Engine::get_singleton()->remove_singleton("ScriptDomainManager");
		Engine::get_singleton()->remove_singleton("ProjectDomainManager");
		Engine::get_singleton()->remove_singleton("LiveMountDebug");

		if (sync_server) {
			memdelete(sync_server);
			sync_server = nullptr;
		}
		if (git_manager) {
			memdelete(git_manager);
			git_manager = nullptr;
		}
		if (launch_controller) {
			memdelete(launch_controller);
			launch_controller = nullptr;
		}
		if (import_session_manager) {
			memdelete(import_session_manager);
			import_session_manager = nullptr;
		}
		if (resource_domain_manager) {
			memdelete(resource_domain_manager);
			resource_domain_manager = nullptr;
		}
		if (project_settings_layer_manager) {
			memdelete(project_settings_layer_manager);
			project_settings_layer_manager = nullptr;
		}
		if (autoload_session_manager) {
			memdelete(autoload_session_manager);
			autoload_session_manager = nullptr;
		}
		if (script_domain_manager) {
			memdelete(script_domain_manager);
			script_domain_manager = nullptr;
		}
		if (project_domain_manager) {
			memdelete(project_domain_manager);
			project_domain_manager = nullptr;
		}
		if (livemount_debug) {
			memdelete(livemount_debug);
			livemount_debug = nullptr;
		}

		print_line("[LiveMount] Module shut down.");
	}
}
