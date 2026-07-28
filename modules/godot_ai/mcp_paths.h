/**************************************************************************/
/*  mcp_paths.h                                                           */
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

#ifndef MCP_PATHS_H
#define MCP_PATHS_H

#include "core/string/ustring.h"

// Filesystem boundary for every tool that touches a path.
//
// The default reach of an AI client is the project, not the host machine. Paths are
// normalised, `..` traversal is rejected, and the result is re-checked after symlink
// resolution so a link inside the project cannot be a door out of it.
class MCPPaths {
	static String _canonicalize(const String &p_absolute);

public:
	struct Resolved {
		String res_path; // res://-relative form, for engine APIs.
		String absolute; // Globalized absolute path, for FileAccess/DirAccess.
		bool exists = false;
		bool is_directory = false;
	};

	// The absolute, symlink-resolved project root. Empty when no project is open.
	static String get_project_root();

	// Resolves a caller-supplied path. Accepts `res://…` and project-relative paths;
	// absolute paths only when they land inside the project root. Returns false and
	// fills r_error with a user-facing reason on rejection.
	static bool resolve(const String &p_path, Resolved &r_resolved, String &r_error);

	// As resolve(), but also fails when the target does not exist.
	static bool resolve_existing(const String &p_path, Resolved &r_resolved, String &r_error);

	// True when p_absolute is inside p_root (or equal to it). Both must be canonical.
	static bool is_inside(const String &p_root, const String &p_absolute);

	// Test-only seam: the doctest binary has no open project, so tests point the
	// boundary at a temporary directory.
	static void set_project_root_override(const String &p_root);
	static void clear_project_root_override();
};

#endif // MCP_PATHS_H
