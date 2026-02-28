/**************************************************************************/
/*  mcp_editor_tools.cpp                                                  */
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

#include "mcp_editor_tools.h"

#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "modules/gdscript/gdscript.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/variant/variant.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/script/editor_script.h"
#include "editor/script/script_editor_plugin.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"

// ============================================================================
// Deferred Execution Infrastructure
// ============================================================================
//
// MCP tool handlers may run on a background poll thread (when use_thread=true
// in editor settings). Certain operations (GDScript execution, viewport
// capture) require the main thread. These statics provide a simple hand-off:
//
//   1. Poll thread stores args, then call_deferred() the _do_*_main helper.
//   2. Poll thread blocks on _deferred_semaphore.wait().
//   3. Main thread executes the helper, stores result in _deferred_result,
//      posts the semaphore.
//   4. Poll thread wakes up and returns _deferred_result.
//
// This is safe because the poll thread processes requests one at a time,
// so only one deferred operation can be in flight at once.
// When use_thread=false, the handler already runs on the main thread and
// calls the implementation directly (no semaphore/deferred).

static Semaphore _deferred_semaphore;
static Dictionary _deferred_result;

// ============================================================================
// Tool Registration
// ============================================================================

void MCPEditorTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ---- project/get_overview ----
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"project/get_overview",
				"Get Project Overview",
				"CALL THIS FIRST. Single-call project orientation: project name, Godot version, "
				"main scene, autoloads, renderer, window size, file counts by type (.gd, .tscn, "
				".tres), top-level directory listing, and GDScript error count. "
				"Replaces the need to call multiple tools to understand the project. "
				"All file paths use res:// format. Key concepts: .tscn = scenes, "
				".gd = GDScript, .tres = resources. After writing files with your native tools, "
				"call editor/scan_filesystem so the editor picks up changes.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPEditorTools::handle_get_overview));
	}

	// ---- project/get_input_map ----
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"project/get_input_map",
				"Get Input Map",
				"Get all input actions defined in the project's Input Map, with their bound "
				"events. Call this before using runtime/input/send_input to discover valid action names. "
				"Actions prefixed with 'ui_' are Godot built-in actions (ui_accept, ui_cancel, "
				"ui_left, etc.). Returns immediately.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPEditorTools::handle_get_input_map));
	}

	// ---- editor/reimport ----
	{
		Dictionary props;
		// The "files" property is an array of strings.
		Dictionary files_prop;
		files_prop["type"] = "array";
		Dictionary items;
		items["type"] = "string";
		files_prop["items"] = items;
		files_prop["description"] = "List of file paths to reimport in res:// format";
		props["files"] = files_prop;
		Array required;
		required.push_back("files");
		p_registry->register_tool(
				"editor/reimport",
				"Reimport Files",
				"Trigger reimport of specified resource files. Use this after modifying import "
				"settings or when resources appear stale. The reimport happens asynchronously on "
				"the editor's main thread. Returns immediately after queuing. Paths must use "
				"res:// format.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPEditorTools::handle_reimport));
	}

	// ---- editor/scan_filesystem ----
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"editor/scan_filesystem",
				"Scan Filesystem",
				"IMPORTANT: Call this after writing any project file with your native file tools. "
				"Triggers the editor's filesystem change detection so Godot picks up new/modified "
				"files. Auto-reimports .tscn, .tres, .shader, .gdshader. GDScript changes are "
				"hot-reloaded if a game is running. Returns immediately after queuing.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPEditorTools::handle_scan_filesystem));
	}

	// ---- editor/get_uid ----
	{
		Dictionary props;
		props["path"] = make_prop("string", "File path in res:// format");
		Array required;
		required.push_back("path");
		p_registry->register_tool(
				"editor/get_uid",
				"Get Resource UID",
				"Get the UID (unique identifier) for a resource file path. UIDs are Godot's "
				"way of tracking resources across renames. Returns the uid:// format string. "
				"Use this when you need a stable reference to a resource that won't break if "
				"the file is renamed.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPEditorTools::handle_get_uid));
	}

	// ---- editor/resolve_uid ----
	{
		Dictionary props;
		props["identifier"] = make_prop("string",
				"A uid:// or res:// identifier to resolve");
		Array required;
		required.push_back("identifier");
		p_registry->register_tool(
				"editor/resolve_uid",
				"Resolve UID to Path",
				"Resolve a UID string (uid://...) to its current file path (res://...). Use "
				"this when you encounter a uid:// reference in a .tscn or .tres file and need "
				"the actual file path. Also accepts res:// paths and returns them unchanged.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPEditorTools::handle_resolve_uid));
	}

	// ---- editor/execute_script ----
	{
		Dictionary props;
		props["code"] = make_prop("string",
				"GDScript code to execute as a @tool script in the editor. "
				"Has access to EditorInterface, the edited scene, and all editor APIs. "
				"The code runs in the editor process, NOT in the running game. "
				"Use runtime/evaluate for game-side evaluation.");
		Array required;
		required.push_back("code");
		p_registry->register_tool(
				"editor/execute_script", "Execute Editor Script",
				"Run a GDScript code snippet in the editor context. The script runs as a "
				"@tool script with full access to EditorInterface, EditorFileSystem, the "
				"scene tree, and all editor APIs. Use this for batch operations, custom "
				"import logic, or anything that needs editor-side access.\n\n"
				"SAFETY: The code runs with full editor permissions. Avoid destructive "
				"operations without confirmation.\n\n"
				"Example: 'var ei = EditorInterface.get_singleton()\\n"
				"print(ei.get_edited_scene_root().get_children())'",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/true, /*idempotent=*/false),
				callable_mp_static(&MCPEditorTools::handle_execute_script));
	}

	// ---- editor/get_screenshot ---- DISABLED: screenshot tools commented out
	// {
	// 	Dictionary props;
	// 	Array required;
	// 	props["save_path"] = make_prop("string",
	// 			"Optional absolute file path to save the PNG to disk (e.g. '/tmp/editor.png'). "
	// 			"When provided, the image is saved to disk AND returned inline.");
	// 	p_registry->register_tool(
	// 			"editor/get_screenshot", "Get Editor Screenshot",
	// 			"Capture the Godot editor window as a PNG image. Shows the current state "
	// 			"of the editor including open panels, scene tree, inspector, and any "
	// 			"visible errors. Use runtime/get_screenshot for the running game viewport. "
	// 			"Optionally saves to disk via save_path.",
	// 			make_schema(props, required),
	// 			make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
	// 			callable_mp_static(&MCPEditorTools::handle_get_editor_screenshot));
	// }
}

// ============================================================================
// Tool A1: project/get_overview
// ============================================================================

Dictionary MCPEditorTools::handle_get_overview(const Dictionary &p_args) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_V(ps, make_tool_error("ProjectSettings singleton not available."));

	// --- Project metadata ---

	String project_name = ps->get("application/config/name");
	if (project_name.is_empty()) {
		project_name = "<unnamed project>";
	}

	Dictionary version_info = Engine::get_singleton()->get_version_info();
	String godot_version = vformat("%d.%d.%s",
			(int)version_info["major"],
			(int)version_info["minor"],
			(String)version_info["status"]);

	String main_scene = ps->get("application/run/main_scene");
	if (main_scene.is_empty()) {
		main_scene = "<none>";
	}

	// Autoloads.
	Array autoloads;
	String autoload_text;
	List<PropertyInfo> properties;
	ps->get_property_list(&properties);
	for (const PropertyInfo &pi : properties) {
		if (pi.name.begins_with("autoload/")) {
			String autoload_name = pi.name.get_slice("/", 1);
			String autoload_value = ps->get(pi.name);
			String autoload_path = autoload_value;
			if (autoload_path.begins_with("*")) {
				autoload_path = autoload_path.substr(1);
			}

			Dictionary al;
			al["name"] = autoload_name;
			al["path"] = autoload_path;
			autoloads.push_back(al);

			autoload_text += vformat("  %s -> %s\n", autoload_name, autoload_path);
		}
	}

	String renderer = ps->get("rendering/renderer/rendering_method");
	if (renderer.is_empty()) {
		renderer = "forward_plus";
	}

	int window_width = ps->get("display/window/size/viewport_width");
	int window_height = ps->get("display/window/size/viewport_height");
	if (window_width <= 0) {
		window_width = 1152;
	}
	if (window_height <= 0) {
		window_height = 648;
	}

	// --- File counts by type ---

	Vector<String> all_files;
	_list_files_recursive("res://", "", all_files);

	int script_count = 0;
	int scene_count = 0;
	int resource_count = 0;
	int other_count = 0;

	for (int i = 0; i < all_files.size(); i++) {
		String ext = all_files[i].get_extension().to_lower();
		if (ext == "gd") {
			script_count++;
		} else if (ext == "tscn" || ext == "scn") {
			scene_count++;
		} else if (ext == "tres") {
			resource_count++;
		} else {
			other_count++;
		}
	}

	// --- Top-level directories ---

	String dirs_text;
	Array dirs_array;
	Ref<DirAccess> da = DirAccess::open("res://");
	if (da.is_valid()) {
		da->list_dir_begin();
		String item = da->get_next();
		while (!item.is_empty()) {
			if (da->current_is_dir() && !item.begins_with(".") &&
					!is_skip_directory(item)) {
				dirs_array.push_back(item);
				dirs_text += "  " + item + "/\n";
			}
			item = da->get_next();
		}
		da->list_dir_end();
	}

	// --- GDScript error count (quick validation) ---

	int gdscript_errors = 0;
	{
		Vector<String> gd_files;
		_list_files_recursive("res://", "gd", gd_files);

		GDScriptLanguage *gdscript = GDScriptLanguage::get_singleton();
		if (gdscript) {
			for (int i = 0; i < gd_files.size(); i++) {
				String source = FileAccess::get_file_as_string(gd_files[i]);
				if (source.is_empty()) {
					continue;
				}
				List<String> functions;
				List<ScriptLanguage::ScriptError> errors;
				List<ScriptLanguage::Warning> warnings;
				HashSet<int> safe_lines;

				bool valid = gdscript->validate(source, gd_files[i],
						&functions, &errors, &warnings, &safe_lines);
				if (!valid) {
					gdscript_errors++;
				}
			}
		}
	}

	// --- Build text ---

	String text = vformat(
			"=== PROJECT OVERVIEW ===\n\n"
			"Project: %s\n"
			"Godot: %s\n"
			"Main Scene: %s\n"
			"Renderer: %s | Window: %dx%d\n",
			project_name, godot_version, main_scene,
			renderer, window_width, window_height);

	if (autoloads.size() > 0) {
		text += vformat("\nAutoloads (%d):\n%s", autoloads.size(), autoload_text);
	}

	text += vformat(
			"\nFiles: %d scripts, %d scenes, %d resources, %d other (%d total)\n",
			script_count, scene_count, resource_count, other_count,
			all_files.size());

	if (dirs_array.size() > 0) {
		text += vformat("\nDirectories (%d):\n%s", dirs_array.size(), dirs_text);
	}

	if (gdscript_errors > 0) {
		text += vformat("\nGDScript: %d file(s) with errors (use testing/check_all_scripts for details)\n",
				gdscript_errors);
	} else {
		text += vformat("\nGDScript: all %d scripts clean\n", script_count);
	}

	text += "\n=== END OVERVIEW ===";

	// --- Build structured ---

	Dictionary structured;
	structured["project_name"] = project_name;
	structured["godot_version"] = godot_version;
	structured["main_scene"] = main_scene;
	structured["autoloads"] = autoloads;
	structured["renderer"] = renderer;
	Dictionary window_size;
	window_size["width"] = window_width;
	window_size["height"] = window_height;
	structured["window_size"] = window_size;

	Dictionary file_counts;
	file_counts["scripts"] = script_count;
	file_counts["scenes"] = scene_count;
	file_counts["resources"] = resource_count;
	file_counts["other"] = other_count;
	file_counts["total"] = all_files.size();
	structured["file_counts"] = file_counts;

	structured["directories"] = dirs_array;
	structured["gdscript_errors"] = gdscript_errors;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool A2: project/get_input_map
// ============================================================================

Dictionary MCPEditorTools::handle_get_input_map(const Dictionary &p_args) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_V(ps, make_tool_error("ProjectSettings singleton not available."));

	Array actions;
	String text;
	int action_count = 0;

	List<PropertyInfo> properties;
	ps->get_property_list(&properties);

	for (const PropertyInfo &pi : properties) {
		if (!pi.name.begins_with("input/")) {
			continue;
		}

		String action_name = pi.name.get_slice("/", 1);
		Dictionary action_dict = ps->get(pi.name);

		// action_dict has: deadzone (float), events (Array of InputEvent resources).
		float deadzone = action_dict.get("deadzone", 0.5f);
		Array events_raw = action_dict.get("events", Array());

		Array events_serialized;
		String events_text;

		for (int i = 0; i < events_raw.size(); i++) {
			Ref<InputEvent> event = events_raw[i];
			if (event.is_null()) {
				continue;
			}

			Dictionary event_dict;
			String class_name = event->get_class();
			event_dict["type"] = class_name;

			// NOTE: InputEvent::as_text() uses RTR() which accesses
			// TranslationServer. This is technically a data race when called
			// from the MCP background thread, but translation data is
			// effectively read-only after editor startup, making this safe
			// in practice. If as_text() ever becomes problematic, the
			// event type alone is sufficient for MCP client use.
			String event_text = event->as_text();
			event_dict["as_text"] = event_text;

			events_serialized.push_back(event_dict);
			if (i > 0) {
				events_text += ", ";
			}
			events_text += event_text;
		}

		Dictionary action;
		action["name"] = action_name;
		action["deadzone"] = deadzone;
		action["events"] = events_serialized;
		actions.push_back(action);

		text += vformat("  %s -> %s\n", action_name, events_text);
		action_count++;
	}

	String header = vformat("Input Actions (%d total):\n\n", action_count);
	text = header + text;

	Dictionary structured;
	structured["action_count"] = action_count;
	structured["actions"] = actions;

	return make_tool_result(text, structured);
}

// ============================================================================
// Helper: recursively list files (used by get_overview)
// ============================================================================

void MCPEditorTools::_list_files_recursive(const String &p_dir,
		const String &p_extension, Vector<String> &r_files) {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return;
	}

	da->list_dir_begin();
	String item = da->get_next();
	while (!item.is_empty()) {
		if (da->current_is_dir()) {
			if (!is_skip_directory(item) && !item.begins_with(".")) {
				_list_files_recursive(p_dir + item + "/", p_extension, r_files);
			}
		} else {
			if (p_extension.is_empty() ||
					item.get_extension().to_lower() == p_extension.to_lower()) {
				r_files.push_back(p_dir + item);
			}
		}
		item = da->get_next();
	}
	da->list_dir_end();
}

// ============================================================================
// Tool B4: editor/reimport
// ============================================================================

Dictionary MCPEditorTools::handle_reimport(const Dictionary &p_args) {
	if (!p_args.has("files")) {
		return make_tool_error("Missing required parameter: files");
	}

	Array files_arr = p_args["files"];
	if (files_arr.size() == 0) {
		return make_tool_error("Parameter 'files' must contain at least one file path.");
	}

	// Validate all paths first.
	Vector<String> validated_files;
	for (int i = 0; i < files_arr.size(); i++) {
		String path = files_arr[i];
		if (!validate_path(path)) {
			return make_tool_error(vformat(
					"Invalid path at index %d: %s\n\n"
					"Paths must start with res:// and cannot contain '..' traversal sequences.",
					i, path));
		}
		validated_files.push_back(path);
	}

	// Queue reimport on main thread via call_deferred.
	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (!efs) {
		return make_tool_error(
				"EditorFileSystem not available. This tool requires the Godot editor.");
	}

	// Convert Vector<String> to PackedStringArray for Variant-based call_deferred.
	PackedStringArray packed_files;
	for (const String &f : validated_files) {
		packed_files.push_back(f);
	}
	callable_mp(efs, &EditorFileSystem::reimport_files)
			.call_deferred(packed_files);

	// Build response.
	String text = vformat("Reimport queued for %d files:", validated_files.size());
	for (int i = 0; i < validated_files.size(); i++) {
		text += vformat("\n  %s", validated_files[i]);
	}

	Dictionary structured;
	structured["status"] = "queued";
	Array files_out;
	for (int i = 0; i < validated_files.size(); i++) {
		files_out.push_back(validated_files[i]);
	}
	structured["files"] = files_out;
	structured["count"] = validated_files.size();

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool C1: editor/scan_filesystem
// ============================================================================

// Helper: update cached modification times for open scenes AND scripts so the
// "Files modified outside Godot" dialog doesn't fire for MCP-caused writes.
// Must run on the main thread.
static void _mcp_update_scene_mtimes() {
	EditorNode *en = EditorNode::get_singleton();
	if (!en) {
		return;
	}
	EditorData &ed = EditorNode::get_editor_data();

	// Update each open scene tab's cached mtime to match the current disk mtime.
	for (int i = 0; i < ed.get_edited_scene_count(); i++) {
		String scene_path = ed.get_scene_path(i);
		if (scene_path.is_empty()) {
			continue;
		}
		uint64_t disk_time = FileAccess::get_modified_time(scene_path);
		if (disk_time > 0) {
			ed.set_scene_modified_time(i, disk_time);
		}
	}

	// Also update the project.godot cached time so it doesn't trigger
	// the "project.godot was modified externally" dialog entry.
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps) {
		String project_path = ps->get_resource_path().path_join("project.godot");
		uint64_t proj_disk_time = FileAccess::get_modified_time(project_path);
		if (proj_disk_time > 0) {
			ps->set_last_saved_time(proj_disk_time);
		}
	}

	// Update open script tabs' cached mtimes to suppress the ScriptEditor's
	// "Files modified outside Godot" dialog for MCP-caused script writes.
	ScriptEditor *se = ScriptEditor::get_singleton();
	if (se) {
		se->update_script_times();
	}
}

Dictionary MCPEditorTools::handle_scan_filesystem(const Dictionary &p_args) {
	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (!efs) {
		return make_tool_error(
				"EditorFileSystem not available. This tool requires the Godot editor.");
	}

	// Update cached scene modification times BEFORE scanning, so that when
	// the editor regains focus, _scan_external_changes() won't show the
	// "Files modified outside Godot" dialog for MCP-caused writes.
	callable_mp_static(&_mcp_update_scene_mtimes).call_deferred();

	// Fire-and-forget: queue scan on the main thread.
	callable_mp(efs, &EditorFileSystem::scan_changes).call_deferred();

	Dictionary structured;
	structured["status"] = "queued";

	return make_tool_result("Filesystem scan queued. Scene modification times updated to suppress external-change dialog.", structured);
}

// ============================================================================
// Tool C2: editor/get_uid
// ============================================================================

Dictionary MCPEditorTools::handle_get_uid(const Dictionary &p_args) {
	String path = p_args.get("path", "");

	if (path.is_empty()) {
		return make_tool_error("Missing required parameter: path");
	}
	if (!validate_path(path)) {
		return make_tool_error(vformat(
				"Invalid path: %s\n\n"
				"Paths must start with res:// and cannot contain '..' traversal sequences.",
				path));
	}

	ResourceUID *ruid = ResourceUID::get_singleton();
	ERR_FAIL_NULL_V(ruid, make_tool_error("ResourceUID singleton not available."));

	// Use the engine's own UID lookup which handles all resource types:
	// .import files, .uid sidecar files, and embedded uid:// in text resources.
	ResourceUID::ID found_id = ResourceLoader::get_resource_uid(path);

	if (found_id == ResourceUID::INVALID_ID) {
		return make_tool_error(vformat(
				"No UID found for: %s\n\n"
				"This file may not have been assigned a UID yet. "
				"Try editor/scan_filesystem first, or check that the file exists.",
				path));
	}

	String uid_text = ruid->id_to_text(found_id);

	String text = vformat("%s -> %s", path, uid_text);

	Dictionary structured;
	structured["path"] = path;
	structured["uid"] = uid_text;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool C3: editor/resolve_uid
// ============================================================================

Dictionary MCPEditorTools::handle_resolve_uid(const Dictionary &p_args) {
	String identifier = p_args.get("identifier", "");

	if (identifier.is_empty()) {
		return make_tool_error("Missing required parameter: identifier");
	}

	// If it's already a res:// path, just return it.
	if (identifier.begins_with("res://")) {
		String text = vformat("%s -> %s (already a res:// path)", identifier, identifier);
		Dictionary structured;
		structured["identifier"] = identifier;
		structured["path"] = identifier;
		return make_tool_result(text, structured);
	}

	// Must be a uid:// identifier.
	if (!identifier.begins_with("uid://")) {
		return make_tool_error(vformat(
				"Invalid identifier: %s\n\n"
				"Must be a uid:// or res:// identifier. Example: uid://b3c5d7e9f1a2",
				identifier));
	}

	ResourceUID *ruid = ResourceUID::get_singleton();
	ERR_FAIL_NULL_V(ruid, make_tool_error("ResourceUID singleton not available."));

	// Convert text to numeric ID.
	ResourceUID::ID id = ruid->text_to_id(identifier);
	if (id == ResourceUID::INVALID_ID) {
		return make_tool_error(vformat(
				"Invalid UID format: %s\n\n"
				"UIDs should be in the format: uid://[base64 characters]",
				identifier));
	}

	// Look up the path.
	if (!ruid->has_id(id)) {
		return make_tool_error(vformat(
				"UID not found: %s\n\n"
				"This UID is not registered in the project. It may refer to a deleted resource.",
				identifier));
	}

	String resolved_path = ruid->get_id_path(id);

	String text = vformat("%s -> %s", identifier, resolved_path);

	Dictionary structured;
	structured["identifier"] = identifier;
	structured["path"] = resolved_path;

	return make_tool_result(text, structured);
}

// ============================================================================
// Helper: _cleanup_mcp_temp
// ============================================================================

void MCPEditorTools::_cleanup_mcp_temp() {
	String temp_dir = "res://addons/mcp_temp";
	Ref<DirAccess> da = DirAccess::open(temp_dir);
	if (da.is_valid()) {
		da->list_dir_begin();
		String item = da->get_next();
		while (!item.is_empty()) {
			if (!da->current_is_dir()) {
				da->remove(item);
			}
			item = da->get_next();
		}
		da->list_dir_end();
	}
	// Try to remove the directory itself (will only succeed if empty).
	Ref<DirAccess> parent = DirAccess::open("res://addons");
	if (parent.is_valid()) {
		parent->remove("mcp_temp");
	}
}

// ============================================================================
// Tool: editor/execute_script
// ============================================================================

// Deferred wrapper: called on main thread via call_deferred.
// Runs the implementation, stores result, and posts the semaphore so the
// blocked poll thread can continue.
void MCPEditorTools::_do_execute_script_main(const String &p_full_script) {
	// Write to temporary file.
	String temp_path = "res://addons/mcp_temp/editor_script.gd";
	{
		Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
		if (da.is_valid()) {
			da->make_dir_recursive("res://addons/mcp_temp");
		}
	}
	{
		Ref<FileAccess> fa = FileAccess::open(temp_path, FileAccess::WRITE);
		if (fa.is_null()) {
			_deferred_result = make_tool_error("Failed to write temp script file.");
			_deferred_semaphore.post();
			return;
		}
		fa->store_string(p_full_script);
	}

	// Load and execute.
	Ref<Script> script = ResourceLoader::load(temp_path, "GDScript", ResourceFormatLoader::CACHE_MODE_IGNORE);
	if (script.is_null()) {
		_cleanup_mcp_temp();
		_deferred_result = make_tool_error(
				"Failed to load editor script. Check for syntax errors.\n\n"
				"Script contents:\n" + p_full_script);
		_deferred_semaphore.post();
		return;
	}

	// Instantiate EditorScript and set its script.
	Ref<EditorScript> es;
	es.instantiate();
	es->set_script(script);

	// Run the script. This calls the virtual _run() method via GDScript.
	es->run();

	// Cleanup.
	_cleanup_mcp_temp();

	int code_lines = p_full_script.split("\n").size() - 4; // Subtract wrapper lines.
	Dictionary structured;
	structured["executed"] = true;
	structured["code_lines"] = code_lines;

	_deferred_result = make_tool_result("Editor script executed successfully.", structured);
	_deferred_semaphore.post();
}

Dictionary MCPEditorTools::handle_execute_script(const Dictionary &p_args) {
	String code = p_args.get("code", "");
	if (code.is_empty()) {
		return make_tool_error("Missing required parameter: code");
	}

	// Wrap the code in an EditorScript class.
	String full_script = "@tool\nextends EditorScript\n\nfunc _run():\n";

	// Indent user code.
	Vector<String> lines = code.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		full_script += "\t" + lines[i] + "\n";
	}

	if (Thread::is_main_thread()) {
		// Non-threaded mode: handler is already on the main thread.
		// Call the deferred helper directly -- it will post the semaphore,
		// which we immediately consume so it doesn't leak.
		_do_execute_script_main(full_script);
		_deferred_semaphore.wait(); // Consume the post() from above.
		return _deferred_result;
	}

	// Threaded mode: defer to main thread and wait.
	// GDScript execution and EditorScript::run() require the main thread
	// for safe access to the scene tree, editor UI, and GDScript VM state.
	callable_mp_static(&MCPEditorTools::_do_execute_script_main)
			.call_deferred(full_script);
	_deferred_semaphore.wait();
	return _deferred_result;
}

// ============================================================================
// Tool: editor/get_screenshot
// ============================================================================

// Main-thread implementation: captures viewport, encodes as base64 PNG.
// Stores result in _deferred_result and posts _deferred_semaphore.
void MCPEditorTools::_do_get_screenshot_main() {
	// Get the editor's main window.
	EditorNode *editor_node = EditorNode::get_singleton();
	if (!editor_node) {
		_deferred_result = make_tool_error("EditorNode not available.");
		_deferred_semaphore.post();
		return;
	}

	Window *main_window = editor_node->get_window();
	if (!main_window) {
		_deferred_result = make_tool_error("Editor window not available.");
		_deferred_semaphore.post();
		return;
	}

	Viewport *vp = main_window->get_viewport();
	if (!vp) {
		_deferred_result = make_tool_error("Editor viewport not available.");
		_deferred_semaphore.post();
		return;
	}

	// Capture the viewport texture.
	Ref<ViewportTexture> tex = vp->get_texture();
	if (tex.is_null()) {
		_deferred_result = make_tool_error("Failed to get editor viewport texture.");
		_deferred_semaphore.post();
		return;
	}

	Ref<Image> img = tex->get_image();
	if (img.is_null() || img->is_empty()) {
		_deferred_result = make_tool_error(
				"Failed to capture editor screenshot.\n\n"
				"The viewport image may not be ready. Try again in a moment.");
		_deferred_semaphore.post();
		return;
	}

	// Encode as PNG.
	PackedByteArray png_data = img->save_png_to_buffer();
	if (png_data.is_empty()) {
		_deferred_result = make_tool_error("Failed to encode editor screenshot as PNG.");
		_deferred_semaphore.post();
		return;
	}

	// Encode as base64.
	String base64 = CryptoCore::b64_encode_str(png_data.ptr(), png_data.size());

	int width = img->get_width();
	int height = img->get_height();
	int data_size = png_data.size();
	double size_kb = (double)data_size / 1024.0;

	// Build response with both text and image content blocks.
	Dictionary text_content;
	text_content["type"] = "text";
	text_content["text"] = "Editor screenshot captured (" +
			itos(width) + "x" + itos(height) + ", " +
			String::num(size_kb, 1) + " KB PNG)";

	Dictionary image_content;
	image_content["type"] = "image";
	image_content["data"] = base64;
	image_content["mimeType"] = "image/png";

	Array content;
	content.push_back(text_content);
	content.push_back(image_content);

	Dictionary structured;
	structured["width"] = width;
	structured["height"] = height;
	structured["format"] = "png";
	structured["size_bytes"] = data_size;

	Dictionary response;
	response["content"] = content;
	response["structuredContent"] = structured;

	_deferred_result = response;
	_deferred_semaphore.post();
}

Dictionary MCPEditorTools::handle_get_editor_screenshot(const Dictionary &p_args) {
	if (Thread::is_main_thread()) {
		// Non-threaded mode: handler is already on the main thread.
		// Call the deferred helper directly -- it will post the semaphore,
		// which we immediately consume so it doesn't leak.
		_do_get_screenshot_main();
		_deferred_semaphore.wait(); // Consume the post() from above.
	} else {
		// Threaded mode: defer viewport capture to main thread and wait.
		// Viewport texture readback (RS::texture_2d_get) and window/viewport
		// access are safest on the main thread where the rendering state is
		// consistent.
		callable_mp_static(&MCPEditorTools::_do_get_screenshot_main)
				.call_deferred();
		_deferred_semaphore.wait();
	}

	Dictionary response = _deferred_result;

	// Optionally save to disk if save_path was provided.
	String save_path = p_args.get("save_path", "");
	if (!save_path.is_empty() && response.has("content")) {
		// Find the base64 image data in the content array.
		Array content = response["content"];
		String base64_data;
		for (int i = 0; i < content.size(); i++) {
			Dictionary item = content[i];
			if (String(item.get("type", "")) == "image") {
				base64_data = item.get("data", "");
				break;
			}
		}

		String save_info;
		if (!base64_data.is_empty()) {
			CharString b64_ascii = base64_data.ascii();
			size_t max_len = b64_ascii.length();
			PackedByteArray png_bytes;
			png_bytes.resize(max_len);
			size_t decoded_len = 0;
			Error decode_err = CryptoCore::b64_decode(
					png_bytes.ptrw(), max_len, &decoded_len,
					(const uint8_t *)b64_ascii.get_data(), b64_ascii.length());
			if (decode_err == OK && decoded_len > 0) {
				png_bytes.resize(decoded_len);
				Ref<FileAccess> f = FileAccess::open(save_path, FileAccess::WRITE);
				if (f.is_valid()) {
					f->store_buffer(png_bytes.ptr(), png_bytes.size());
					f->close();
					save_info = "Saved to: " + save_path;
				} else {
					save_info = "WARNING: Could not write to '" + save_path + "'";
				}
			} else {
				save_info = "WARNING: Failed to decode PNG data for save.";
			}
		}

		if (!save_info.is_empty()) {
			// Append save info to the text content block.
			for (int i = 0; i < content.size(); i++) {
				Dictionary item = content[i];
				if (String(item.get("type", "")) == "text") {
					item["text"] = String(item["text"]) + "\n" + save_info;
					content[i] = item;
					break;
				}
			}
			response["content"] = content;

			// Add save_path to structured content.
			if (response.has("structuredContent")) {
				Dictionary structured = response["structuredContent"];
				structured["save_path"] = save_path;
				structured["saved"] = save_info.begins_with("Saved");
				response["structuredContent"] = structured;
			}
		}
	}

	return response;
}

