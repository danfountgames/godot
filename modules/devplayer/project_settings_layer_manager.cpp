/**************************************************************************/
/*  project_settings_layer_manager.cpp                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           GODOT BEAM ENGINE                            */
/**************************************************************************/

#include "project_settings_layer_manager.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/string/print_string.h"

ProjectSettingsLayerManager *ProjectSettingsLayerManager::singleton = nullptr;

ProjectSettingsLayerManager *ProjectSettingsLayerManager::get_singleton() {
	return singleton;
}

void ProjectSettingsLayerManager::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_base_settings"), &ProjectSettingsLayerManager::load_base_settings);
	ClassDB::bind_method(D_METHOD("load_project_settings", "path"), &ProjectSettingsLayerManager::load_project_settings);
	ClassDB::bind_method(D_METHOD("apply_session_overrides", "overrides"), &ProjectSettingsLayerManager::apply_session_overrides);
	ClassDB::bind_method(D_METHOD("commit_effective_settings"), &ProjectSettingsLayerManager::commit_effective_settings);
	ClassDB::bind_method(D_METHOD("clear_project_settings"), &ProjectSettingsLayerManager::clear_project_settings);
}

void ProjectSettingsLayerManager::load_base_settings() {
	print_line("[ProjectSettingsLayerManager] Capturing base settings snapshot...");

	ProjectSettings *ps = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_MSG(ps, "[ProjectSettingsLayerManager] ProjectSettings singleton is null.");

	base_settings_backup.clear();

	// Save the original resource path so we can restore it on unmount.
	original_resource_path = ps->get_resource_path();

	List<PropertyInfo> props;
	ps->get_property_list(&props);
	for (const PropertyInfo &pi : props) {
		if (pi.name.begins_with("_") || pi.name.is_empty()) {
			continue;
		}
		base_settings_backup[pi.name] = ps->get(pi.name);
	}

	base_captured = true;
	print_line("[ProjectSettingsLayerManager] Base settings captured (" + itos(base_settings_backup.size()) + " properties).");
	print_line("[ProjectSettingsLayerManager] Original resource path: " + original_resource_path);
}

Error ProjectSettingsLayerManager::load_project_settings(const String &p_path) {
	ERR_FAIL_COND_V_MSG(p_path.is_empty(), ERR_INVALID_PARAMETER, "[ProjectSettingsLayerManager] Project path is empty.");

	print_line("[ProjectSettingsLayerManager] Loading project settings from: " + p_path);

	ProjectSettings *ps = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_V_MSG(ps, ERR_BUG, "[ProjectSettingsLayerManager] ProjectSettings singleton is null.");

	// Resolve path to absolute.
	String abs_path = p_path;
	if (abs_path.begins_with("res://")) {
		abs_path = ps->globalize_path(abs_path);
	}
	if (abs_path.ends_with("/")) {
		abs_path = abs_path.substr(0, abs_path.length() - 1);
	}

	print_line("[ProjectSettingsLayerManager] Resolved absolute path: " + abs_path);

	// Directly set the resource path so res:// maps to the mounted project.
	// This bypasses setup()'s OS::get_resource_dir() check which would
	// re-use the engine's own project path instead of the mounted one.
	ps->set_resource_path(abs_path);

	// Load the project.godot from the mounted project path.
	String project_file = abs_path.path_join("project.godot");
	Error err = ps->load_custom(project_file);
	if (err != OK) {
		ERR_PRINT("[ProjectSettingsLayerManager] Failed to load project.godot from: " + project_file);
		// Restore original resource path on failure.
		if (!original_resource_path.is_empty()) {
			ps->set_resource_path(original_resource_path);
		}
		return err;
	}

	// Verify the main scene setting is readable.
	String main_scene = ps->get_setting("application/run/main_scene", String());
	print_line("[ProjectSettingsLayerManager] Project settings loaded successfully.");
	print_line("[ProjectSettingsLayerManager] Resource path is now: " + ps->get_resource_path());
	print_line("[ProjectSettingsLayerManager] Main scene setting: " + main_scene);

	return OK;
}

void ProjectSettingsLayerManager::apply_session_overrides(const Dictionary &p_overrides) {
	print_line("[ProjectSettingsLayerManager] Applying " + itos(p_overrides.size()) + " session overrides...");

	ProjectSettings *ps = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_MSG(ps, "[ProjectSettingsLayerManager] ProjectSettings singleton is null.");

	LocalVector<Variant> keys = p_overrides.get_key_list();
	for (const Variant &key : keys) {
		String key_str = key;
		ps->set(key_str, p_overrides[key]);
	}

	print_line("[ProjectSettingsLayerManager] Session overrides applied.");
}

void ProjectSettingsLayerManager::commit_effective_settings() {
	print_line("[ProjectSettingsLayerManager] Committing effective settings (no-op in current implementation).");
	// In the future, this could trigger a settings version bump or notification
	// to subsystems that depend on ProjectSettings changes.
}

void ProjectSettingsLayerManager::clear_project_settings() {
	print_line("[ProjectSettingsLayerManager] Restoring base settings...");

	if (!base_captured) {
		ERR_PRINT("[ProjectSettingsLayerManager] Cannot restore: base settings were never captured.");
		return;
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_MSG(ps, "[ProjectSettingsLayerManager] ProjectSettings singleton is null.");

	// Restore the original resource path so res:// maps back to the host/shell project.
	if (!original_resource_path.is_empty()) {
		print_line("[ProjectSettingsLayerManager] Restoring resource path to: " + original_resource_path);
		ps->set_resource_path(original_resource_path);
	}

	for (const KeyValue<StringName, Variant> &E : base_settings_backup) {
		ps->set(E.key, E.value);
	}

	print_line("[ProjectSettingsLayerManager] Base settings restored (" + itos(base_settings_backup.size()) + " properties).");
}

ProjectSettingsLayerManager::ProjectSettingsLayerManager() {
	singleton = this;
}

ProjectSettingsLayerManager::~ProjectSettingsLayerManager() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
