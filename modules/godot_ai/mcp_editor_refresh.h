/**************************************************************************/
/*  mcp_editor_refresh.h                                                  */
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

#ifndef MCP_EDITOR_REFRESH_H
#define MCP_EDITOR_REFRESH_H

#include "core/string/ustring.h"

// Telling the editor that a tool changed a file, safely.
//
// Several tools write or delete a project file and then ask EditorFileSystem to notice.
// Every one of them used to guard on `EditorFileSystem::get_singleton()` being non-null,
// on the stated assumption that it is null in headless runs and unit tests.
//
// **That assumption is false, and it crashed the engine.** The singleton can exist while
// the node is not inside a SceneTree - which is exactly the state the full engine test
// suite leaves it in. `scan_changes()` then takes its early-return branch, calls
// `set_process(true)`, and `Node::_add_to_process_thread_group()` dereferences a tree
// that is not there:
//
//     SceneTree::_add_node_to_process_group (this=0x661)      <- not a pointer
//     Node::set_process
//     EditorFileSystem::scan_changes            editor_file_system.cpp:1706
//     WriteTextFileTool::run                    mcp_project_tools.cpp
//
// Intermittent, because the early-return branch is only taken when a scan happens to be
// pending. It reproduced about one full-suite run in four and never once in a targeted
// `[godot_ai]` run, which is why it went unseen: the module's own suite cannot reach it.
//
// So the guard is "in a tree", not "not null", and it lives here so no future caller has
// to rediscover the difference.
namespace MCPEditorRefresh {

// True when EditorFileSystem exists *and* is live enough to be driven.
bool can_refresh();

// Re-import/refresh one path. No-op when the editor cannot be driven.
void update_file(const String &p_res_path);

// Ask the editor to rescan for changes. No-op when the editor cannot be driven.
//
// Asynchronous even when it does run: a caller that needs the result now must not
// assume this has taken effect by the time it returns.
void scan_changes();

} // namespace MCPEditorRefresh

#endif // MCP_EDITOR_REFRESH_H
