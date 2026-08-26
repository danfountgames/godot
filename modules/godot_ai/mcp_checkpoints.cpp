/**************************************************************************/
/*  mcp_checkpoints.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "mcp_checkpoints.h"

#include "mcp_paths.h"
#include "mcp_editor_refresh.h"
#include "mcp_service.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/time.h"

#ifdef TOOLS_ENABLED
#include "editor/file_system/editor_file_system.h"
#endif

static String s_root_override;

// Enough history to undo a session's worth of mistakes without growing without bound.
static const int DEFAULT_KEEP = 50;
// A checkpoint is a safety net for source-like files, not a backup system for large
// binary assets; snapshotting those would be slow and is not what this protects.
static const int64_t MAX_SNAPSHOT_BYTES = 16 * 1024 * 1024;

void MCPCheckpoints::set_root_override(const String &p_root) {
	s_root_override = p_root;
}

void MCPCheckpoints::clear_root_override() {
	s_root_override = String();
}

String MCPCheckpoints::_project_key() {
	if (!ProjectSettings::get_singleton()) {
		return "no-project";
	}
	const String resource_path = ProjectSettings::get_singleton()->get_resource_path();
	if (resource_path.is_empty()) {
		return "no-project";
	}
	return resource_path.get_file() + "-" + String::num_uint64(resource_path.hash(), 16);
}

String MCPCheckpoints::get_root() {
	if (!s_root_override.is_empty()) {
		return s_root_override;
	}
	// Outside the project on purpose: a snapshot inside res:// would be picked up by
	// the importer and could be committed by accident.
	return MCPService::get_state_dir().path_join("checkpoints").path_join(_project_key());
}

bool MCPCheckpoints::_copy_file(const String &p_from, const String &p_to) {
	Ref<FileAccess> source = FileAccess::open(p_from, FileAccess::READ);
	if (source.is_null()) {
		return false;
	}
	const uint64_t length = source->get_length();
	const Vector<uint8_t> bytes = source->get_buffer((int64_t)length);

	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (dir.is_valid()) {
		dir->make_dir_recursive(p_to.get_base_dir());
	}
	Ref<FileAccess> destination = FileAccess::open(p_to, FileAccess::WRITE);
	if (destination.is_null()) {
		return false;
	}
	if (!bytes.is_empty()) {
		destination->store_buffer(bytes.ptr(), bytes.size());
	}
	destination->flush();
	return true;
}

String MCPCheckpoints::create(const String &p_tool, const String &p_invocation, const Vector<String> &p_res_paths, String &r_error) {
	if (p_res_paths.is_empty()) {
		return String();
	}

	const String root = get_root();
	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (dir.is_null()) {
		r_error = "could not access the filesystem to write a checkpoint";
		return String();
	}

	// Sortable, unique, and readable in a directory listing.
	const String id = Time::get_singleton()->get_datetime_string_from_system(false, false).replace(":", "").replace("-", "").replace(" ", "-") +
			"-" + String::num_uint64((uint64_t)Time::get_singleton()->get_ticks_usec(), 16).substr(0, 6);
	const String directory = root.path_join(id);
	if (dir->make_dir_recursive(directory) != OK) {
		r_error = vformat("could not create the checkpoint directory '%s'", directory);
		return String();
	}

	Array files;
	int stored_index = 0;
	for (const String &res_path : p_res_paths) {
		MCPPaths::Resolved resolved;
		String path_error;
		if (!MCPPaths::resolve(res_path, resolved, path_error)) {
			// A path the tool could not have written anyway.
			continue;
		}

		Dictionary entry;
		entry["path"] = resolved.res_path;
		entry["existed"] = resolved.exists && !resolved.is_directory;

		if (resolved.exists && !resolved.is_directory) {
			Ref<FileAccess> source = FileAccess::open(resolved.absolute, FileAccess::READ);
			if (source.is_null()) {
				continue;
			}
			if ((int64_t)source->get_length() > MAX_SNAPSHOT_BYTES) {
				// Recorded so a restore does not silently claim to cover it.
				entry["skipped"] = "larger than the snapshot limit";
				files.push_back(entry);
				continue;
			}
			source.unref();

			const String stored_name = itos(stored_index++) + "-" + resolved.res_path.get_file();
			if (!_copy_file(resolved.absolute, directory.path_join(stored_name))) {
				continue;
			}
			entry["stored"] = stored_name;
		}
		files.push_back(entry);
	}

	if (files.is_empty()) {
		dir->remove(directory);
		return String();
	}

	Dictionary manifest;
	manifest["id"] = id;
	manifest["time"] = Time::get_singleton()->get_datetime_string_from_system(true);
	manifest["tool"] = p_tool;
	// Already redacted by MCPTool::describe_invocation.
	manifest["invocation"] = p_invocation;
	manifest["files"] = files;

	Ref<FileAccess> manifest_file = FileAccess::open(directory.path_join("manifest.json"), FileAccess::WRITE);
	if (manifest_file.is_null()) {
		r_error = "could not write the checkpoint manifest";
		return String();
	}
	manifest_file->store_string(JSON::stringify(manifest, "  "));
	manifest_file.unref();

	prune(DEFAULT_KEEP);
	return id;
}

Array MCPCheckpoints::list() {
	Array checkpoints;
	Ref<DirAccess> dir = DirAccess::open(get_root());
	if (dir.is_null()) {
		return checkpoints;
	}

	Vector<String> ids;
	dir->list_dir_begin();
	String entry = dir->get_next();
	while (!entry.is_empty()) {
		if (dir->current_is_dir() && entry != "." && entry != "..") {
			ids.push_back(entry);
		}
		entry = dir->get_next();
	}
	dir->list_dir_end();

	// Ids are timestamp-prefixed, so sorting them sorts by age.
	ids.sort();
	ids.reverse();

	for (const String &id : ids) {
		const String manifest_path = get_root().path_join(id).path_join("manifest.json");
		if (!FileAccess::exists(manifest_path)) {
			continue;
		}
		const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(manifest_path));
		if (parsed.get_type() != Variant::DICTIONARY) {
			continue;
		}
		checkpoints.push_back(parsed);
	}
	return checkpoints;
}

bool MCPCheckpoints::restore(const String &p_id, int &r_restored, int &r_removed, String &r_error) {
	r_restored = 0;
	r_removed = 0;

	const String directory = get_root().path_join(p_id);
	const String manifest_path = directory.path_join("manifest.json");
	if (!FileAccess::exists(manifest_path)) {
		r_error = vformat("no checkpoint '%s'", p_id);
		return false;
	}
	const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(manifest_path));
	if (parsed.get_type() != Variant::DICTIONARY) {
		r_error = vformat("checkpoint '%s' has an unreadable manifest", p_id);
		return false;
	}

	const Dictionary manifest = parsed;
	const Array files = manifest["files"];
	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);

	for (int i = 0; i < files.size(); i++) {
		const Dictionary entry = files[i];
		const String res_path = entry["path"];

		MCPPaths::Resolved resolved;
		String path_error;
		if (!MCPPaths::resolve(res_path, resolved, path_error)) {
			// The boundary applies to restoring too: a manifest must not be a way to
			// write outside the project.
			r_error = path_error;
			return false;
		}
		if (entry.has("skipped")) {
			continue;
		}

		if ((bool)entry["existed"] && entry.has("stored")) {
			if (!_copy_file(directory.path_join(entry["stored"]), resolved.absolute)) {
				r_error = vformat("could not restore '%s'", res_path);
				return false;
			}
			r_restored++;
		} else if (!(bool)entry["existed"]) {
			// The tool created this file; undoing that means removing it again.
			if (resolved.exists && dir.is_valid()) {
				dir->remove(resolved.absolute);
				r_removed++;
			}
		}

#ifdef TOOLS_ENABLED
		// Not a null check: the singleton can exist outside the tree. See mcp_editor_refresh.h.
		if (MCPEditorRefresh::can_refresh()) {
			MCPEditorRefresh::update_file(resolved.res_path);
		}
#endif
	}

	return true;
}

void MCPCheckpoints::prune(int p_keep) {
	Ref<DirAccess> dir = DirAccess::open(get_root());
	if (dir.is_null()) {
		return;
	}

	Vector<String> ids;
	dir->list_dir_begin();
	String entry = dir->get_next();
	while (!entry.is_empty()) {
		if (dir->current_is_dir() && entry != "." && entry != "..") {
			ids.push_back(entry);
		}
		entry = dir->get_next();
	}
	dir->list_dir_end();

	if (ids.size() <= p_keep) {
		return;
	}
	ids.sort();

	for (int i = 0; i < ids.size() - p_keep; i++) {
		const String directory = get_root().path_join(ids[i]);
		Ref<DirAccess> victim = DirAccess::open(directory);
		if (victim.is_null()) {
			continue;
		}
		victim->list_dir_begin();
		String file = victim->get_next();
		while (!file.is_empty()) {
			if (file != "." && file != "..") {
				victim->remove(directory.path_join(file));
			}
			file = victim->get_next();
		}
		victim->list_dir_end();
		dir->remove(directory);
	}
}
