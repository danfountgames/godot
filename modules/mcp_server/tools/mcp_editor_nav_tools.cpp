/**************************************************************************/
/*  mcp_editor_nav_tools.cpp                                              */
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

#include "mcp_editor_nav_tools.h"

#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "editor/editor_interface.h"
#include "editor/script/script_editor_plugin.h"
#include "scene/main/node.h"
#include "core/object/script_language.h"

// ============================================================================
// Tool Registration
// ============================================================================

void MCPEditorNavTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ---- editor/get_open_scenes ----
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"editor/get_open_scenes",
				"Get Open Scenes",
				"List all scenes currently open in the editor as tabs. Returns the res:// path "
				"for each open scene and indicates which one is currently active (being edited). "
				"Use this to understand the editor's current state.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPEditorNavTools::handle_get_open_scenes));
	}

	// ---- editor/get_open_scripts ----
	{
		Dictionary props;
		props["include_contents"] = make_prop("boolean",
				"If true, include the full source code of each script (default: false). "
				"Warning: can be large for many scripts.");
		Array required;
		p_registry->register_tool(
				"editor/get_open_scripts", "Get Open Scripts",
				"List all scripts currently open in the script editor tabs. Returns "
				"file paths, class names, base classes, and tool status. Optionally "
				"includes the full source code of each script.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPEditorNavTools::handle_get_open_scripts));
	}
}

// ============================================================================
// Tool 1: editor/get_open_scenes
// ============================================================================

Dictionary MCPEditorNavTools::handle_get_open_scenes(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(ei, make_tool_error("EditorInterface singleton not available."));

	PackedStringArray scenes = ei->get_open_scenes();

	// Determine the active scene by checking the currently edited scene root.
	String active_scene_path;
	Node *edited_root = ei->get_edited_scene_root();
	if (edited_root) {
		active_scene_path = edited_root->get_scene_file_path();
	}

	// Build text response.
	String text;
	if (scenes.size() == 0) {
		text = "No scenes are currently open in the editor.";
	} else {
		text = vformat("Open scenes (%d):\n", scenes.size());
		for (int i = 0; i < scenes.size(); i++) {
			bool is_active = (scenes[i] == active_scene_path);
			text += vformat("  %s%s\n", scenes[i],
					is_active ? " [active]" : "");
		}
	}

	// Build structured response.
	Array scenes_array;
	for (int i = 0; i < scenes.size(); i++) {
		Dictionary scene_info;
		scene_info["path"] = scenes[i];
		scene_info["active"] = (scenes[i] == active_scene_path);
		scenes_array.push_back(scene_info);
	}

	Dictionary structured;
	structured["scenes"] = scenes_array;
	structured["count"] = scenes.size();
	structured["active_scene"] = active_scene_path;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 2: editor/get_open_scripts
// ============================================================================

Dictionary MCPEditorNavTools::handle_get_open_scripts(const Dictionary &p_args) {
	ScriptEditor *se = ScriptEditor::get_singleton();
	ERR_FAIL_NULL_V(se, make_tool_error("Script editor not available."));

	bool include_contents = (bool)p_args.get("include_contents", false);

	Vector<Ref<Script>> scripts = se->get_open_scripts();

	Array result_scripts;
	String text = "Open scripts (" + itos(scripts.size()) + "):\n";

	for (int i = 0; i < scripts.size(); i++) {
		Ref<Script> s = scripts[i];
		if (s.is_null()) {
			continue;
		}

		Dictionary entry;
		entry["path"] = s->get_path();
		entry["class_name"] = s->get_global_name();
		entry["base_class"] = s->get_instance_base_type();
		entry["is_tool"] = s->is_tool();

		if (include_contents) {
			entry["source"] = s->get_source_code();
		}

		result_scripts.push_back(entry);
		text += "  " + String(s->get_path()) + "\n";
	}

	Dictionary structured;
	structured["scripts"] = result_scripts;
	structured["count"] = result_scripts.size();

	return make_tool_result(text, structured);
}
