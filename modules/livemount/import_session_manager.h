/**************************************************************************/
/*  import_session_manager.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           Godot LiveMount Module                            */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

class EditorFileSystem;

class ImportSessionManager : public Object {
	GDCLASS(ImportSessionManager, Object);

	static ImportSessionManager *singleton;

	String project_root;
	bool bound = false;
	bool importers_registered = false;
	EditorFileSystem *editor_file_system = nullptr;

	// Results of the last scan.
	int total_importable_files = 0;
	int files_with_import_metadata = 0;
	int files_missing_import_metadata = 0;
	bool import_cache_exists = false;
	Vector<String> missing_imports;

	// Common importable extensions (subset — Godot supports many more via plugins).
	static const char *IMPORTABLE_EXTENSIONS[];

	void _scan_dir_recursive(const String &p_dir, const HashSet<String> &p_extensions);
	bool _is_importable(const String &p_file, const HashSet<String> &p_extensions) const;

protected:
	static void _bind_methods();

public:
	static ImportSessionManager *get_singleton();

	void bind_project_root(const String &p_path);
	void register_importers();
	void scan_and_import();
	void scan_filesystem();
	void import_pending_assets();
	void clear_import_state();

	// Accessors for scan results.
	int get_total_importable_files() const { return total_importable_files; }
	int get_files_with_import_metadata() const { return files_with_import_metadata; }
	int get_files_missing_import_metadata() const { return files_missing_import_metadata; }
	bool has_import_cache() const { return import_cache_exists; }
	Vector<String> get_missing_imports() const { return missing_imports; }

	ImportSessionManager();
	~ImportSessionManager();
};
