/**************************************************************************/
/*  autoload_session_manager.cpp                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           Godot LiveMount Module                            */
/**************************************************************************/

#include "autoload_session_manager.h"

#include "core/config/project_settings.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/string/print_string.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/packed_scene.h"

AutoloadSessionManager *AutoloadSessionManager::singleton = nullptr;

AutoloadSessionManager *AutoloadSessionManager::get_singleton() {
	return singleton;
}

void AutoloadSessionManager::_bind_methods() {
	ClassDB::bind_method(D_METHOD("build_autoloads_from_project"), &AutoloadSessionManager::build_autoloads_from_project);
	ClassDB::bind_method(D_METHOD("destroy_autoloads"), &AutoloadSessionManager::destroy_autoloads);
	ClassDB::bind_method(D_METHOD("get_active_autoload_names"), &AutoloadSessionManager::get_active_autoload_names);
	ClassDB::bind_method(D_METHOD("get_active_autoload_count"), &AutoloadSessionManager::get_active_autoload_count);
}

void AutoloadSessionManager::build_autoloads_from_project() {
	print_line("[AutoloadSessionManager] Building autoloads from project settings...");

	SceneTree *scene_tree = SceneTree::get_singleton();
	ERR_FAIL_NULL_MSG(scene_tree, "[AutoloadSessionManager] SceneTree is not available.");

	Window *root = scene_tree->get_root();
	ERR_FAIL_NULL_MSG(root, "[AutoloadSessionManager] SceneTree root is null.");

	const HashMap<StringName, ProjectSettings::AutoloadInfo> &autoloads = ProjectSettings::get_singleton()->get_autoload_list();

	for (const KeyValue<StringName, ProjectSettings::AutoloadInfo> &E : autoloads) {
		const ProjectSettings::AutoloadInfo &info = E.value;
		String path = info.path;

		print_line("[AutoloadSessionManager] Loading autoload: " + String(info.name) + " from " + path);

		Node *node = nullptr;

		// Try to load as scene or script.
		Ref<Resource> res = ResourceLoader::load(path);
		ERR_CONTINUE_MSG(res.is_null(), "[AutoloadSessionManager] Failed to load autoload resource: " + path);

		Ref<PackedScene> scene = res;
		if (scene.is_valid()) {
			node = scene->instantiate();
			ERR_CONTINUE_MSG(!node, "[AutoloadSessionManager] Failed to instantiate autoload scene: " + path);
		} else {
			Ref<Script> script = res;
			if (script.is_valid()) {
				node = Object::cast_to<Node>(ClassDB::instantiate(script->get_instance_base_type()));
				ERR_CONTINUE_MSG(!node, "[AutoloadSessionManager] Failed to instantiate node for autoload script: " + path);
				node->set_script(script);
			} else {
				ERR_CONTINUE_MSG(true, "[AutoloadSessionManager] Autoload resource is neither a scene nor a script: " + path);
			}
		}

		node->set_name(info.name);
		root->add_child(node);
		active_autoloads.push_back(node);

		// Register as global constant in script languages so scripts can access it by name.
		if (info.is_singleton) {
			for (int i = 0; i < ScriptServer::get_language_count(); i++) {
				ScriptServer::get_language(i)->add_named_global_constant(info.name, node);
			}
			registered_global_names.push_back(info.name);
		}

		print_line("[AutoloadSessionManager] Autoload added: " + String(info.name));
	}

	print_line("[AutoloadSessionManager] Built " + itos(active_autoloads.size()) + " autoloads.");
}

void AutoloadSessionManager::destroy_autoloads() {
	print_line("[AutoloadSessionManager] Destroying " + itos(active_autoloads.size()) + " autoloads...");

	// Remove global constants that WE registered (not ones from ProjectSettings
	// that may have failed to load). This avoids the "!named_globals.has(p_name)"
	// error when trying to unregister a constant we never added.
	for (const StringName &name : registered_global_names) {
		for (int i = 0; i < ScriptServer::get_language_count(); i++) {
			ScriptServer::get_language(i)->remove_named_global_constant(name);
		}
	}
	registered_global_names.clear();

	// Use memdelete (not queue_free) to free autoload nodes immediately.
	// This mirrors the pattern in LaunchController::stop_project() for the
	// mounted scene. Deferred deletion via queue_free() keeps script references
	// alive past the GDScript cache flush, causing stale scripts on remount.
	for (int i = active_autoloads.size() - 1; i >= 0; i--) {
		Node *node = active_autoloads[i];
		if (node) {
			Node *parent = node->get_parent();
			if (parent) {
				parent->remove_child(node);
			}
			memdelete(node);
		}
	}

	active_autoloads.clear();
	print_line("[AutoloadSessionManager] All autoloads destroyed.");
}

PackedStringArray AutoloadSessionManager::get_active_autoload_names() const {
	PackedStringArray names;
	for (int i = 0; i < active_autoloads.size(); i++) {
		if (active_autoloads[i]) {
			names.push_back(active_autoloads[i]->get_name());
		}
	}
	return names;
}

int AutoloadSessionManager::get_active_autoload_count() const {
	return active_autoloads.size();
}

AutoloadSessionManager::AutoloadSessionManager() {
	singleton = this;
}

AutoloadSessionManager::~AutoloadSessionManager() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
