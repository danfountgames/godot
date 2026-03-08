/**************************************************************************/
/*  project_settings_layer_manager.h                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           GODOT BEAM ENGINE                            */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

class ProjectSettingsLayerManager : public Object {
	GDCLASS(ProjectSettingsLayerManager, Object);

	static ProjectSettingsLayerManager *singleton;

	// Backup of base settings so we can restore after unmount.
	HashMap<StringName, Variant> base_settings_backup;
	String original_resource_path;
	bool base_captured = false;

protected:
	static void _bind_methods();

public:
	static ProjectSettingsLayerManager *get_singleton();

	void load_base_settings();
	Error load_project_settings(const String &p_path);
	void apply_session_overrides(const Dictionary &p_overrides);
	void commit_effective_settings();
	void clear_project_settings();

	ProjectSettingsLayerManager();
	~ProjectSettingsLayerManager();
};
