/**************************************************************************/
/*  import_session_manager.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           GODOT BEAM ENGINE                            */
/**************************************************************************/

#include "import_session_manager.h"

#include "core/object/class_db.h"
#include "core/string/print_string.h"

#ifdef TOOLS_ENABLED
#include "editor/file_system/editor_file_system.h"
#endif

ImportSessionManager *ImportSessionManager::singleton = nullptr;

ImportSessionManager *ImportSessionManager::get_singleton() {
	return singleton;
}

void ImportSessionManager::_bind_methods() {
	ClassDB::bind_method(D_METHOD("bind_project_root", "path"), &ImportSessionManager::bind_project_root);
	ClassDB::bind_method(D_METHOD("scan_filesystem"), &ImportSessionManager::scan_filesystem);
	ClassDB::bind_method(D_METHOD("import_pending_assets"), &ImportSessionManager::import_pending_assets);
	ClassDB::bind_method(D_METHOD("clear_import_state"), &ImportSessionManager::clear_import_state);
}

void ImportSessionManager::bind_project_root(const String &p_path) {
	ERR_FAIL_COND_MSG(p_path.is_empty(), "[ImportSessionManager] Project root path is empty.");

	project_root = p_path;
	bound = true;

	print_line("[ImportSessionManager] Bound to project root: " + project_root);
}

void ImportSessionManager::scan_filesystem() {
	ERR_FAIL_COND_MSG(!bound, "[ImportSessionManager] Not bound to a project root. Call bind_project_root() first.");

	print_line("[ImportSessionManager] Scanning filesystem...");

#ifdef TOOLS_ENABLED
	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (efs) {
		efs->scan();
		print_line("[ImportSessionManager] EditorFileSystem scan initiated.");
	} else {
		print_line("[ImportSessionManager] EditorFileSystem not available; skipping scan.");
	}
#else
	print_line("[ImportSessionManager] TOOLS_ENABLED not defined; filesystem scanning not available in template builds.");
#endif
}

void ImportSessionManager::import_pending_assets() {
	ERR_FAIL_COND_MSG(!bound, "[ImportSessionManager] Not bound to a project root. Call bind_project_root() first.");

	print_line("[ImportSessionManager] Importing pending assets...");

#ifdef TOOLS_ENABLED
	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (efs) {
		efs->scan_changes();
		print_line("[ImportSessionManager] EditorFileSystem scan_changes initiated.");
	} else {
		print_line("[ImportSessionManager] EditorFileSystem not available; skipping import.");
	}
#else
	print_line("[ImportSessionManager] TOOLS_ENABLED not defined; asset importing not available in template builds.");
#endif
}

void ImportSessionManager::clear_import_state() {
	print_line("[ImportSessionManager] Clearing import state...");

	project_root = String();
	bound = false;

	print_line("[ImportSessionManager] Import state cleared.");
}

ImportSessionManager::ImportSessionManager() {
	singleton = this;
}

ImportSessionManager::~ImportSessionManager() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
