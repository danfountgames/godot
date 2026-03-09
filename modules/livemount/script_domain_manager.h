/**************************************************************************/
/*  script_domain_manager.h                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           Godot LiveMount Module                            */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

class ScriptDomainManager : public Object {
	GDCLASS(ScriptDomainManager, Object);

	static ScriptDomainManager *singleton;

	String current_project_path;

	// Recursively collect all .gd files in a directory.
	void _collect_gd_files(const String &p_dir, Vector<String> &r_files);

protected:
	static void _bind_methods();

public:
	static ScriptDomainManager *get_singleton();

	void initialize_for_project(const String &p_path);
	void scan_and_register_global_classes(const String &p_project_root);
	void shutdown_project_scripts();
	void clear_script_caches();
	void clear_global_class_registrations();

	ScriptDomainManager();
	~ScriptDomainManager();
};
