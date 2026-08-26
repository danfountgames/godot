/**************************************************************************/
/*  mcp_editor_refresh.cpp                                                */
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

#include "mcp_editor_refresh.h"

#include "editor/file_system/editor_file_system.h"

bool MCPEditorRefresh::can_refresh() {
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	// is_inside_tree(), not just non-null. See the header: a singleton outside the tree
	// is what turned scan_changes() into a segfault.
	return filesystem && filesystem->is_inside_tree();
}

void MCPEditorRefresh::update_file(const String &p_res_path) {
	if (!can_refresh() || p_res_path.is_empty()) {
		return;
	}
	EditorFileSystem::get_singleton()->update_file(p_res_path);
}

void MCPEditorRefresh::scan_changes() {
	if (!can_refresh()) {
		return;
	}
	EditorFileSystem::get_singleton()->scan_changes();
}
