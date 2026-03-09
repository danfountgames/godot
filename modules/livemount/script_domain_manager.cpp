/**************************************************************************/
/*  script_domain_manager.cpp                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           Godot LiveMount Module                            */
/**************************************************************************/

#include "script_domain_manager.h"

#include "core/io/dir_access.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/string/print_string.h"
#include "modules/gdscript/gdscript_cache.h"

ScriptDomainManager *ScriptDomainManager::singleton = nullptr;

ScriptDomainManager *ScriptDomainManager::get_singleton() {
	return singleton;
}

void ScriptDomainManager::_bind_methods() {
	ClassDB::bind_method(D_METHOD("initialize_for_project", "path"), &ScriptDomainManager::initialize_for_project);
	ClassDB::bind_method(D_METHOD("scan_and_register_global_classes", "project_root"), &ScriptDomainManager::scan_and_register_global_classes);
	ClassDB::bind_method(D_METHOD("shutdown_project_scripts"), &ScriptDomainManager::shutdown_project_scripts);
	ClassDB::bind_method(D_METHOD("clear_script_caches"), &ScriptDomainManager::clear_script_caches);
	ClassDB::bind_method(D_METHOD("clear_global_class_registrations"), &ScriptDomainManager::clear_global_class_registrations);
}

void ScriptDomainManager::_collect_gd_files(const String &p_dir, Vector<String> &r_files) {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return;
	}

	da->list_dir_begin();
	String item = da->get_next();
	while (!item.is_empty()) {
		if (da->current_is_dir()) {
			// Skip hidden directories (like .godot, .git, .import).
			if (!item.begins_with(".")) {
				_collect_gd_files(p_dir.path_join(item), r_files);
			}
		} else if (item.ends_with(".gd")) {
			r_files.push_back(p_dir.path_join(item));
		}
		item = da->get_next();
	}
	da->list_dir_end();
}

void ScriptDomainManager::initialize_for_project(const String &p_path) {
	ERR_FAIL_COND_MSG(p_path.is_empty(), "[ScriptDomainManager] Project path is empty.");

	current_project_path = p_path;
	print_line("[ScriptDomainManager] Initialized script domain for project: " + p_path);
}

void ScriptDomainManager::scan_and_register_global_classes(const String &p_project_root) {
	print_line("[ScriptDomainManager] Scanning for global classes in: " + p_project_root);

	// Collect all .gd files in the project.
	Vector<String> gd_files;
	_collect_gd_files(p_project_root, gd_files);

	print_line("[ScriptDomainManager] Found " + itos(gd_files.size()) + " GDScript files to scan.");

	// Find the GDScript language.
	ScriptLanguage *gdscript_lang = nullptr;
	for (int i = 0; i < ScriptServer::get_language_count(); i++) {
		if (ScriptServer::get_language(i)->get_name() == "GDScript") {
			gdscript_lang = ScriptServer::get_language(i);
			break;
		}
	}

	if (!gdscript_lang) {
		ERR_PRINT("[ScriptDomainManager] GDScript language not found in ScriptServer.");
		return;
	}

	int registered_count = 0;

	for (const String &abs_path : gd_files) {
		// Convert absolute path to res:// path for registration.
		String res_path;
		if (abs_path.begins_with(p_project_root)) {
			res_path = "res://" + abs_path.substr(p_project_root.length() + 1);
		} else {
			res_path = abs_path;
		}

		String base_type;
		String icon_path;
		bool is_abstract = false;
		bool is_tool = false;

		String class_name = gdscript_lang->get_global_class_name(res_path, &base_type, &icon_path, &is_abstract, &is_tool);

		if (!class_name.is_empty()) {
			ScriptServer::add_global_class(
					StringName(class_name),
					StringName(base_type),
					gdscript_lang->get_name(),
					res_path,
					is_abstract,
					is_tool);
			print_line("[ScriptDomainManager] Registered global class: " + class_name + " (base: " + base_type + ", path: " + res_path + ")");
			registered_count++;
		}
	}

	print_line("[ScriptDomainManager] Registered " + itos(registered_count) + " global classes.");
}

void ScriptDomainManager::shutdown_project_scripts() {
	print_line("[ScriptDomainManager] Shutting down project scripts...");

	clear_script_caches();
	clear_global_class_registrations();

	current_project_path = String();
	print_line("[ScriptDomainManager] Project scripts shut down.");
}

void ScriptDomainManager::clear_script_caches() {
	print_line("[ScriptDomainManager] Flushing GDScript caches for domain switch...");
	// Use flush_project_caches() instead of clear().
	// clear() sets a `cleared` flag that prevents subsequent calls from working,
	// and it doesn't clear the `dependencies` map. flush_project_caches() is
	// designed for repeated dynamic project switching.
	GDScriptCache::flush_project_caches();
	print_line("[ScriptDomainManager] GDScript caches flushed.");
}

void ScriptDomainManager::clear_global_class_registrations() {
	print_line("[ScriptDomainManager] Clearing global class registrations...");
	ScriptServer::global_classes_clear();
	print_line("[ScriptDomainManager] Global class registrations cleared.");
}

ScriptDomainManager::ScriptDomainManager() {
	singleton = this;
}

ScriptDomainManager::~ScriptDomainManager() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
