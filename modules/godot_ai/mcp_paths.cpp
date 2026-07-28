/**************************************************************************/
/*  mcp_paths.cpp                                                         */
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

#include "mcp_paths.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"

static String s_project_root_override;

void MCPPaths::set_project_root_override(const String &p_root) {
	s_project_root_override = p_root;
}

void MCPPaths::clear_project_root_override() {
	s_project_root_override = String();
}

// Resolves symlinks component by component. Godot exposes is_link()/read_link()
// rather than a realpath() equivalent, and doing this in engine terms keeps the
// boundary check portable.
String MCPPaths::_canonicalize(const String &p_absolute) {
	String path = p_absolute.simplify_path();
	if (path.is_empty()) {
		return path;
	}

	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (dir.is_null()) {
		return path;
	}

	// Bounded so a symlink cycle cannot spin here.
	const int MAX_RESOLUTIONS = 64;
	int resolutions = 0;

	bool changed = true;
	while (changed && resolutions < MAX_RESOLUTIONS) {
		changed = false;
		const PackedStringArray components = path.split("/", false);
		String prefix = path.begins_with("/") ? "/" : "";
		for (int i = 0; i < components.size(); i++) {
			if (!prefix.is_empty() && !prefix.ends_with("/")) {
				prefix += "/";
			}
			prefix += components[i];

			if (!dir->is_link(prefix)) {
				continue;
			}
			String target = dir->read_link(prefix);
			if (target.is_empty()) {
				continue;
			}
			if (!target.begins_with("/")) {
				target = prefix.get_base_dir().path_join(target);
			}
			// Re-attach whatever followed the link and resolve again from the top.
			String remainder;
			for (int j = i + 1; j < components.size(); j++) {
				remainder = remainder.path_join(components[j]);
			}
			path = (remainder.is_empty() ? target : target.path_join(remainder)).simplify_path();
			resolutions++;
			changed = true;
			break;
		}
	}

	return path;
}

String MCPPaths::get_project_root() {
	if (!s_project_root_override.is_empty()) {
		return _canonicalize(s_project_root_override);
	}
	if (!ProjectSettings::get_singleton()) {
		return String();
	}
	const String resource_path = ProjectSettings::get_singleton()->get_resource_path();
	if (resource_path.is_empty()) {
		return String();
	}
	return _canonicalize(resource_path);
}

bool MCPPaths::is_inside(const String &p_root, const String &p_absolute) {
	if (p_root.is_empty()) {
		return false;
	}
	const String root = p_root.trim_suffix("/");
	if (p_absolute == root) {
		return true;
	}
	return p_absolute.begins_with(root + "/");
}

bool MCPPaths::resolve(const String &p_path, Resolved &r_resolved, String &r_error) {
	const String root = get_project_root();
	if (root.is_empty()) {
		r_error = "no Godot project is open, so project paths cannot be resolved";
		return false;
	}

	const String path = p_path.strip_edges();
	if (path.is_empty()) {
		r_error = "path must not be empty";
		return false;
	}

	// Only the project's own scheme is in reach. `user://` is deliberately excluded:
	// it lives outside the project directory.
	if (path.contains("://") && !path.begins_with("res://")) {
		r_error = vformat("path '%s' uses a scheme outside the project; only 'res://' and project-relative paths are allowed", p_path);
		return false;
	}

	String absolute;
	if (path.begins_with("res://")) {
		absolute = root.path_join(path.trim_prefix("res://"));
	} else if (path.begins_with("/")) {
		absolute = path;
	} else {
		absolute = root.path_join(path);
	}

	// Reject traversal before touching the filesystem, so the message is about the
	// caller's input rather than about whatever the traversal happened to hit.
	const String simplified = absolute.simplify_path();
	if (!is_inside(root, simplified)) {
		r_error = vformat("path '%s' resolves outside the project directory", p_path);
		return false;
	}

	const String canonical = _canonicalize(simplified);
	// Re-check after symlink resolution.
	if (!is_inside(root, canonical)) {
		r_error = vformat("path '%s' points outside the project directory through a symbolic link", p_path);
		return false;
	}

	r_resolved.absolute = canonical;
	const String relative = canonical == root ? String() : canonical.trim_prefix(root + "/");
	r_resolved.res_path = relative.is_empty() ? "res://" : ("res://" + relative);
	r_resolved.is_directory = DirAccess::exists(canonical);
	r_resolved.exists = r_resolved.is_directory || FileAccess::exists(canonical);
	return true;
}

bool MCPPaths::resolve_existing(const String &p_path, Resolved &r_resolved, String &r_error) {
	if (!resolve(p_path, r_resolved, r_error)) {
		return false;
	}
	if (!r_resolved.exists) {
		r_error = vformat("path '%s' does not exist in the project", p_path);
		return false;
	}
	return true;
}
