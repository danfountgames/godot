/**************************************************************************/
/*  import_session_manager.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           GODOT BEAM ENGINE                            */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/string/ustring.h"

class ImportSessionManager : public Object {
	GDCLASS(ImportSessionManager, Object);

	static ImportSessionManager *singleton;

	String project_root;
	bool bound = false;

protected:
	static void _bind_methods();

public:
	static ImportSessionManager *get_singleton();

	void bind_project_root(const String &p_path);
	void scan_filesystem();
	void import_pending_assets();
	void clear_import_state();

	ImportSessionManager();
	~ImportSessionManager();
};
