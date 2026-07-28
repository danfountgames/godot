/**************************************************************************/
/*  test_mcp_fs_helpers.h                                                 */
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

#ifndef TEST_MCP_FS_HELPERS_H
#define TEST_MCP_FS_HELPERS_H

#include "core/io/dir_access.h"
#include "core/os/os.h"

// Guarded recursive delete for test scratch directories.
//
// This exists because an earlier fixture used
// DirAccess::create(DirAccess::ACCESS_FILESYSTEM) followed by
// erase_contents_recursive(). That handle starts at the *process working
// directory*, so running the test binary from a source checkout deleted the whole
// repository. Nothing here may delete a path it was not explicitly given, and the
// guards below refuse anything that is not an obvious test scratch directory.
inline bool mcp_test_remove_tree(const String &p_absolute_path) {
	const String path = p_absolute_path.simplify_path();

	// Guard 1: must be absolute and deep enough that a truncated value cannot name
	// a root or a home directory.
	if (!path.begins_with("/") || path.get_slice_count("/") < 3) {
		ERR_PRINT(vformat("Refusing to delete suspicious test path '%s'.", path));
		return false;
	}
	// Guard 2: some component must carry the marker every fixture in this module
	// uses. Checking components rather than only the leaf lets a fixture delete a
	// subdirectory of its own scratch tree without weakening the confinement.
	bool marked = false;
	for (const String &component : path.split("/", false)) {
		if (component.begins_with("godot_ai_test_") || component.begins_with("godot_ai_outside_")) {
			marked = true;
			break;
		}
	}
	if (!marked) {
		ERR_PRINT(vformat("Refusing to delete '%s': not a godot_ai test directory.", path));
		return false;
	}
	// Guard 3: must live under the cache directory the fixtures create it in.
	const String cache = OS::get_singleton()->get_cache_path().simplify_path();
	if (cache.is_empty() || !path.begins_with(cache.trim_suffix("/") + "/")) {
		ERR_PRINT(vformat("Refusing to delete '%s': outside the cache directory.", path));
		return false;
	}

	Ref<DirAccess> dir = DirAccess::open(path);
	if (dir.is_null()) {
		return false;
	}
	dir->list_dir_begin();
	String entry = dir->get_next();
	while (!entry.is_empty()) {
		if (entry != "." && entry != "..") {
			const String child = path.path_join(entry);
			// Never follow a link out of the tree: remove the link itself.
			if (dir->current_is_dir() && !dir->is_link(child)) {
				Ref<DirAccess> child_dir = DirAccess::open(child);
				if (child_dir.is_valid()) {
					child_dir->list_dir_begin();
					String child_entry = child_dir->get_next();
					while (!child_entry.is_empty()) {
						if (child_entry != "." && child_entry != "..") {
							child_dir->remove(child.path_join(child_entry));
						}
						child_entry = child_dir->get_next();
					}
					child_dir->list_dir_end();
				}
				dir->remove(child);
			} else {
				dir->remove(child);
			}
		}
		entry = dir->get_next();
	}
	dir->list_dir_end();

	Ref<DirAccess> parent = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (parent.is_valid()) {
		parent->remove(path);
	}
	return true;
}

#endif // TEST_MCP_FS_HELPERS_H
