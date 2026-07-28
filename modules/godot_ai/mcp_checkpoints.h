/**************************************************************************/
/*  mcp_checkpoints.h                                                     */
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

#ifndef MCP_CHECKPOINTS_H
#define MCP_CHECKPOINTS_H

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

// File-level snapshots taken before a tool changes the project on disk.
//
// This is deliberately one scope among several, and they are not interchangeable:
//
//  - editor undo    reverts in-memory scene edits (Godot_UndoLastAction)
//  - checkpoints    revert files that a tool wrote (this class)
//  - version control is the user's own history and is never touched here
//
// A tool that only changes the edited scene in memory does not create a checkpoint,
// because undo already covers it and a snapshot of an unsaved scene would restore
// nothing. Tools declare the files they may write via MCPTool::get_checkpoint_paths.
//
// Snapshots live outside the project so they never end up committed.
class MCPCheckpoints {
	static String _project_key();
	static bool _copy_file(const String &p_from, const String &p_to);

public:
	// Root directory for this project's checkpoints.
	static String get_root();

	// Snapshots the current contents of p_res_paths. Returns the checkpoint id, or
	// an empty string when nothing needed snapshotting or creation failed.
	static String create(const String &p_tool, const String &p_invocation, const Vector<String> &p_res_paths, String &r_error);

	// Newest first.
	static Array list();

	// Restores every file in a checkpoint: files that existed are put back, files
	// the tool created are removed again.
	static bool restore(const String &p_id, int &r_restored, int &r_removed, String &r_error);

	// Keeps storage bounded; called after each creation.
	static void prune(int p_keep);

	// Test seam.
	static void set_root_override(const String &p_root);
	static void clear_root_override();
};

#endif // MCP_CHECKPOINTS_H
