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
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/script/script_editor_plugin.h"
#include "scene/main/node.h"
#include "core/object/script_language.h"

// ============================================================================
// Tool Registration
// ============================================================================

void MCPEditorNavTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ---- editor/focus_node ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Path to the node relative to the scene root (e.g., 'Player/Sprite2D'). "
				"Use scene/get_tree to discover valid paths.");
		props["center_view"] = make_prop("boolean",
				"Whether to center the 2D/3D viewport on the selected node (default: true). "
				"Note: viewport centering is a future enhancement; currently this only selects "
				"the node and opens it in the Inspector.");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"editor/focus_node",
				"Focus Node in Editor",
				"Select a node in the scene tree and open it in the Inspector panel. The node "
				"is identified by its path relative to the edited scene root (e.g., 'Player' or "
				"'Player/Sprite2D'). After calling this, the Inspector will show the node's "
				"properties and it will be highlighted in the Scene tree dock. Use scene/get_tree "
				"first to discover valid node paths.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPEditorNavTools::handle_focus_node));
	}

	// ---- editor/focus_script ----
	{
		Dictionary props;
		props["script_path"] = make_prop("string",
				"Path to the script file in res:// format (e.g., res://scripts/player.gd)");
		props["line"] = make_prop("integer",
				"Line number to jump to (default: 1). Lines are 1-based.");
		props["column"] = make_prop("integer",
				"Column number to place the caret at (default: 0). Columns are 0-based.");
		props["grab_focus"] = make_prop("boolean",
				"Whether to switch to the Script editor tab (default: true). Set to false "
				"to load the script without switching tabs.");
		Array required;
		required.push_back("script_path");
		p_registry->register_tool(
				"editor/focus_script",
				"Focus Script in Editor",
				"Open a GDScript file in the Script editor and optionally jump to a specific "
				"line and column. If grab_focus is true (default), the editor switches to the "
				"Script tab. The script must exist as a valid .gd file in the project. Use this "
				"after testing/check_script to navigate directly to reported error locations.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPEditorNavTools::handle_focus_script));
	}

	// ---- editor/switch_tab ----
	{
		Dictionary props;
		props["tab"] = make_prop("string",
				"The main editor tab to switch to. Valid values: '2d', '3d', 'script', 'assetlib'.");
		Array required;
		required.push_back("tab");
		p_registry->register_tool(
				"editor/switch_tab",
				"Switch Editor Tab",
				"Switch the main editor view to one of the four primary tabs: '2d' (Canvas editor), "
				"'3d' (Spatial editor), 'script' (Script editor), or 'assetlib' (Asset Library). "
				"This is equivalent to clicking the tab buttons at the top center of the editor. "
				"Returns immediately.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPEditorNavTools::handle_switch_tab));
	}

	// ---- editor/open_scene ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Path to the scene file in res:// format (e.g., res://scenes/main.tscn)");
		Array required;
		required.push_back("path");
		p_registry->register_tool(
				"editor/open_scene",
				"Open Scene",
				"Open a .tscn or .scn scene file in the editor. If the scene is already open, "
				"it will be made the active tab. The path must use res:// format and the file "
				"must exist. After opening, the scene tree dock will show the new scene's node "
				"hierarchy.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPEditorNavTools::handle_open_scene));
	}

	// ---- editor/get_open_scenes ----
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"editor/get_open_scenes",
				"Get Open Scenes",
				"List all scenes currently open in the editor as tabs. Returns the res:// path "
				"for each open scene and indicates which one is currently active (being edited). "
				"Use this to understand the editor's current state before performing navigation "
				"operations.",
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
// Tool 1: editor/focus_node
// ============================================================================

Dictionary MCPEditorNavTools::handle_focus_node(const Dictionary &p_args) {
	String node_path = p_args.get("node_path", "");
	bool center_view = p_args.get("center_view", true);

	if (node_path.is_empty()) {
		return make_tool_error("Missing required parameter: node_path");
	}

	EditorInterface *ei = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(ei, make_tool_error("EditorInterface singleton not available."));

	Node *root = ei->get_edited_scene_root();
	if (!root) {
		return make_tool_error(
				"No scene is currently being edited. Open a scene first with "
				"editor/open_scene.");
	}

	// Resolve the node relative to the scene root.
	Node *node = root->get_node_or_null(NodePath(node_path));
	if (!node) {
		// Build a helpful error with the scene root name.
		String root_name = root->get_name();
		return make_tool_error(vformat(
				"Node not found: '%s' (relative to scene root '%s').\n\n"
				"Use scene/get_tree to list valid node paths in the current scene.",
				node_path, root_name));
	}

	// Select the node in the Scene tree dock.
	EditorSelection *selection = ei->get_selection();
	ERR_FAIL_NULL_V(selection, make_tool_error("EditorSelection not available."));

	// These calls mutate editor UI state and must run on the main thread.
	// Since MCP tool handlers are dispatched from a background thread, we
	// use call_deferred to schedule them safely.
	callable_mp(selection, &EditorSelection::clear).call_deferred();
	callable_mp(selection, &EditorSelection::add_node).call_deferred(node);

	// Open the node's properties in the Inspector.
	callable_mp(ei, &EditorInterface::inspect_object)
			.call_deferred(node, "", false);

	// Build response.
	String node_class = node->get_class();
	String node_name = node->get_name();
	String full_path = String(root->get_path_to(node));

	String text = vformat("Focused node: %s (%s)\nPath: %s",
			node_name, node_class, full_path);

	if (center_view) {
		text += "\nNote: Viewport centering is not yet implemented. The node has been "
				"selected in the Scene tree and opened in the Inspector.";
	}

	Dictionary structured;
	structured["node_name"] = node_name;
	structured["node_class"] = node_class;
	structured["node_path"] = full_path;
	structured["selected"] = true;
	structured["inspector_opened"] = true;
	structured["viewport_centered"] = false;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 2: editor/focus_script
// ============================================================================

Dictionary MCPEditorNavTools::handle_focus_script(const Dictionary &p_args) {
	String script_path = p_args.get("script_path", "");
	int line = p_args.get("line", 1);
	int col = p_args.get("column", 0);
	bool grab_focus = p_args.get("grab_focus", true);

	if (script_path.is_empty()) {
		return make_tool_error("Missing required parameter: script_path");
	}

	// Validate the path.
	if (!validate_path(script_path)) {
		return make_tool_error(vformat(
				"Invalid path: %s\n\n"
				"Paths must start with res:// and cannot contain '..' traversal sequences.",
				script_path));
	}

	// Check the file exists.
	if (!FileAccess::exists(script_path)) {
		return make_tool_error(vformat(
				"Script file not found: %s\n\n"
				"Use your native file tools to find .gd script files.",
				script_path));
	}

	// Verify it has a script-like extension.
	String ext = script_path.get_extension().to_lower();
	if (ext != "gd" && ext != "gdshader" && ext != "shader" && ext != "gdscript") {
		return make_tool_error(vformat(
				"Not a script file: %s\n\n"
				"editor/focus_script supports .gd, .gdshader, and .shader files.",
				script_path));
	}

	// Clamp line to minimum of 1 (Godot uses -1 as "don't jump" but we want
	// to default to jumping to line 1).
	if (line < 1) {
		line = 1;
	}
	if (col < 0) {
		col = 0;
	}

	// Load the script resource.
	Ref<Script> script = ResourceLoader::load(script_path);
	if (script.is_null()) {
		return make_tool_error(vformat(
				"Failed to load script: %s\n\n"
				"The file exists but could not be loaded as a Script resource. "
				"Check that it is a valid GDScript file.",
				script_path));
	}

	EditorInterface *ei = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(ei, make_tool_error("EditorInterface singleton not available."));

	// Open the script in the Script editor. This must run on the main thread.
	callable_mp(ei, &EditorInterface::edit_script)
			.call_deferred(script, line, col, grab_focus);

	// Build response.
	String text = vformat("Opened script: %s at line %d, column %d",
			script_path, line, col);
	if (grab_focus) {
		text += "\nSwitched to Script editor tab.";
	} else {
		text += "\nScript loaded (tab not switched).";
	}

	Dictionary structured;
	structured["script_path"] = script_path;
	structured["line"] = line;
	structured["column"] = col;
	structured["grab_focus"] = grab_focus;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 3: editor/switch_tab
// ============================================================================

Dictionary MCPEditorNavTools::handle_switch_tab(const Dictionary &p_args) {
	String tab = p_args.get("tab", "");

	if (tab.is_empty()) {
		return make_tool_error("Missing required parameter: tab");
	}

	// Map shorthand tab names to the internal editor screen names.
	// Godot's EditorInterface::set_main_screen_editor() expects the
	// exact capitalized name as shown in the editor UI.
	String screen_name;
	String tab_lower = tab.to_lower();

	if (tab_lower == "2d") {
		screen_name = "2D";
	} else if (tab_lower == "3d") {
		screen_name = "3D";
	} else if (tab_lower == "script") {
		screen_name = "Script";
	} else if (tab_lower == "assetlib") {
		screen_name = "AssetLib";
	} else {
		return make_tool_error(vformat(
				"Invalid tab name: '%s'\n\n"
				"Valid tab names are: '2d', '3d', 'script', 'assetlib'.",
				tab));
	}

	EditorInterface *ei = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(ei, make_tool_error("EditorInterface singleton not available."));

	// Switch the main editor screen. Must run on the main thread since it
	// modifies the editor UI.
	callable_mp(ei, &EditorInterface::set_main_screen_editor)
			.call_deferred(screen_name);

	String text = vformat("Switched to %s editor tab.", screen_name);

	Dictionary structured;
	structured["tab"] = tab_lower;
	structured["screen_name"] = screen_name;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 4: editor/open_scene
// ============================================================================

Dictionary MCPEditorNavTools::handle_open_scene(const Dictionary &p_args) {
	String path = p_args.get("path", "");

	if (path.is_empty()) {
		return make_tool_error("Missing required parameter: path");
	}

	// Validate the path.
	if (!validate_path(path)) {
		return make_tool_error(vformat(
				"Invalid path: %s\n\n"
				"Paths must start with res:// and cannot contain '..' traversal sequences.",
				path));
	}

	// Check the file exists.
	if (!FileAccess::exists(path)) {
		return make_tool_error(vformat(
				"Scene file not found: %s\n\n"
				"Use your native file tools to find .tscn scene files.",
				path));
	}

	// Verify it has a scene-like extension.
	String ext = path.get_extension().to_lower();
	if (ext != "tscn" && ext != "scn") {
		return make_tool_error(vformat(
				"Not a scene file: %s\n\n"
				"editor/open_scene supports .tscn (text scene) and .scn (binary scene) files.",
				path));
	}

	EditorInterface *ei = EditorInterface::get_singleton();
	ERR_FAIL_NULL_V(ei, make_tool_error("EditorInterface singleton not available."));

	// Open the scene. Must run on the main thread since it modifies the editor UI.
	callable_mp(ei, &EditorInterface::open_scene_from_path)
			.call_deferred(path, false);

	String text = vformat("Opened scene: %s", path);

	Dictionary structured;
	structured["path"] = path;
	structured["opened"] = true;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 5: editor/get_open_scenes
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
// Tool 6: editor/get_open_scripts
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
