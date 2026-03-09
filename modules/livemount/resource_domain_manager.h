/**************************************************************************/
/*  resource_domain_manager.h                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           Godot LiveMount Module                            */
/**************************************************************************/

#pragma once

#include "core/io/resource.h"
#include "core/object/object.h"
#include "core/string/ustring.h"
#include "core/templates/hash_set.h"

class ResourceDomainManager : public Object {
	GDCLASS(ResourceDomainManager, Object);

	static ResourceDomainManager *singleton;

	String project_root;
	HashSet<String> tracked_resource_paths;

protected:
	static void _bind_methods();

public:
	static ResourceDomainManager *get_singleton();

	void begin_tracking(const String &p_project_root);
	void register_loaded_resource(const Ref<Resource> &p_resource);
	void purge_project_resources();
	int get_tracked_resource_count() const;

	ResourceDomainManager();
	~ResourceDomainManager();
};
