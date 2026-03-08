/**************************************************************************/
/*  autoload_session_manager.h                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           GODOT BEAM ENGINE                            */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

class Node;

class AutoloadSessionManager : public Object {
	GDCLASS(AutoloadSessionManager, Object);

	static AutoloadSessionManager *singleton;

	Vector<Node *> active_autoloads;

protected:
	static void _bind_methods();

public:
	static AutoloadSessionManager *get_singleton();

	void build_autoloads_from_project();
	void destroy_autoloads();
	PackedStringArray get_active_autoload_names() const;
	int get_active_autoload_count() const;

	AutoloadSessionManager();
	~AutoloadSessionManager();
};
