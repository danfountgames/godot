/**************************************************************************/
/*  resource_domain_manager.cpp                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           Godot LiveMount Module                            */
/**************************************************************************/

#include "resource_domain_manager.h"

#include "core/io/resource.h"
#include "core/object/class_db.h"
#include "core/string/print_string.h"

ResourceDomainManager *ResourceDomainManager::singleton = nullptr;

ResourceDomainManager *ResourceDomainManager::get_singleton() {
	return singleton;
}

void ResourceDomainManager::_bind_methods() {
	ClassDB::bind_method(D_METHOD("begin_tracking", "project_root"), &ResourceDomainManager::begin_tracking);
	ClassDB::bind_method(D_METHOD("purge_project_resources"), &ResourceDomainManager::purge_project_resources);
	ClassDB::bind_method(D_METHOD("get_tracked_resource_count"), &ResourceDomainManager::get_tracked_resource_count);
}

void ResourceDomainManager::begin_tracking(const String &p_project_root) {
	ERR_FAIL_COND_MSG(p_project_root.is_empty(), "[ResourceDomainManager] Project root is empty.");

	project_root = p_project_root;
	tracked_resource_paths.clear();

	print_line("[ResourceDomainManager] Began tracking resources for project: " + project_root);
}

void ResourceDomainManager::register_loaded_resource(const Ref<Resource> &p_resource) {
	ERR_FAIL_COND_MSG(p_resource.is_null(), "[ResourceDomainManager] Cannot register null resource.");

	String path = p_resource->get_path();
	if (!path.is_empty()) {
		tracked_resource_paths.insert(path);
	}
}

void ResourceDomainManager::purge_project_resources() {
	print_line("[ResourceDomainManager] Purging project resources...");

	// Collect all cached resources.
	List<Ref<Resource>> cached_resources;
	ResourceCache::get_cached_resources(&cached_resources);

	// Collect resources that need to be evicted from the cache.
	// We collect first and evict second to avoid modifying the cache while iterating.
	Vector<Ref<Resource>> to_evict;
	for (const Ref<Resource> &res : cached_resources) {
		if (res.is_null()) {
			continue;
		}
		String path = res->get_path();
		if (path.is_empty()) {
			continue;
		}
		// Evict any resource with a res:// path -- these belong to the mounted project.
		// Engine built-in resources do not use res:// paths in livemount mode.
		if (path.begins_with("res://")) {
			to_evict.push_back(res);
		}
	}

	// Actually evict from ResourceCache by clearing the resource path.
	// Resource::set_path("") removes the entry from ResourceCache::resources HashMap
	// via ResourceCache::resources.erase(path_cache) inside set_path().
	int purged_count = to_evict.size();
	for (int i = 0; i < to_evict.size(); i++) {
		Ref<Resource> res = to_evict[i];
		print_line("[ResourceDomainManager] Evicting: " + res->get_path());
		res->set_path("");
	}

	// Drop our references so the resources can be freed.
	to_evict.clear();
	tracked_resource_paths.clear();

	print_line("[ResourceDomainManager] Purged " + itos(purged_count) + " resources from cache.");
}

int ResourceDomainManager::get_tracked_resource_count() const {
	// Return count of res:// resources currently in ResourceCache.
	// This is more accurate than tracked_resource_paths.size() since
	// register_loaded_resource() is not hooked into the resource loading pipeline.
	List<Ref<Resource>> cached;
	ResourceCache::get_cached_resources(&cached);
	int count = 0;
	for (const Ref<Resource> &res : cached) {
		if (res.is_valid() && res->get_path().begins_with("res://")) {
			count++;
		}
	}
	return count;
}

ResourceDomainManager::ResourceDomainManager() {
	singleton = this;
}

ResourceDomainManager::~ResourceDomainManager() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
