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

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/variant/variant.h"
#include "editor/file_system/editor_file_system.h"

// ============================================================================
// Tool Registration
// ============================================================================

void MCPEditorTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ---- project/get_info ----
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"project/get_info",
				"Get Project Info",
				"Get comprehensive project metadata: project name, Godot version, "
				"main scene path, autoload list, renderer settings, physics tick rate, "
				"and window size. CALL THIS FIRST when starting a new session to understand "
				"the project. All file paths use res:// format (Godot's virtual filesystem "
				"rooted at the project directory). Key Godot concepts: .tscn = scene files, "
				".gd = GDScript code, .tres = data resources. Scene tree paths (like "
				"'/root/Main/Player') are separate from file paths (like 'res://scenes/player.tscn').",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPEditorTools::handle_get_info));
	}

	// ---- project/get_input_map ----
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"project/get_input_map",
				"Get Input Map",
				"Get all input actions defined in the project's Input Map, with their bound "
				"events. Call this before using debug/send_input to discover valid action names. "
				"Actions prefixed with 'ui_' are Godot built-in actions (ui_accept, ui_cancel, "
				"ui_left, etc.). Returns immediately.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPEditorTools::handle_get_input_map));
	}

	// ---- editor/read_file ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"File path in res:// format (e.g., res://scripts/player.gd)");
		Array required;
		required.push_back("path");
		p_registry->register_tool(
				"editor/read_file",
				"Read Project File",
				"Read the contents of a text file from the Godot project. Supports .gd, .tscn, "
				".tres, .cfg, .json, .txt, .md, .shader, .gdshader, and other text formats. "
				"Returns the raw text content. Binary files (images, audio, fonts) will return "
				"an error. Path must use res:// format. If the file is not found, the error will "
				"suggest similar filenames in the project.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPEditorTools::handle_read_file));
	}

	// ---- editor/write_file ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"File path in res:// format (e.g., res://scripts/player.gd)");
		props["content"] = make_prop("string",
				"The full text content to write to the file");
		props["create_directories"] = make_prop("boolean",
				"Whether to create parent directories if they don't exist (default: true)");
		Array required;
		required.push_back("path");
		required.push_back("content");
		p_registry->register_tool(
				"editor/write_file",
				"Write Project File",
				"Write or create a text file in the Godot project. Creates parent directories "
				"automatically if they don't exist. After writing, automatically triggers reimport "
				"for known resource types (.tscn, .tres, .shader, .gdshader) and filesystem scan "
				"for new files. Path must use res:// format. Use this to complete the "
				"read-modify-write workflow: read with editor/read_file, modify, write back with "
				"this tool, then validate with gdscript/check_errors.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/true, /*idempotent=*/true),
				callable_mp_static(&MCPEditorTools::handle_write_file));
	}

	// ---- editor/list_files ----
	{
		Dictionary props;
		props["directory"] = make_prop("string",
				"Directory path in res:// format (default: res://)");
		props["extension"] = make_prop("string",
				"Filter by file extension without the dot (e.g., 'gd', 'tscn', 'tres'). "
				"Omit to list all files.");
		props["recursive"] = make_prop("boolean",
				"Whether to search subdirectories (default: false)");
		Array required;
		p_registry->register_tool(
				"editor/list_files",
				"List Project Files",
				"List files in a project directory. Returns file names with their full res:// "
				"paths. Optionally filter by extension and search recursively. Use 'res://' for "
				"the project root. Excludes .import, .godot, and other generated directories "
				"by default.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPEditorTools::handle_list_files));
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
				"Trigger the editor's filesystem change detection. Use this after adding, "
				"removing, or renaming files outside the editor (e.g., via command line or "
				"external tools). This is the equivalent of clicking 'Rescan Filesystem' in the "
				"editor. The scan happens asynchronously. Returns immediately after queuing.",
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
}

// ============================================================================
// Tool A1: project/get_info
// ============================================================================

Dictionary MCPEditorTools::handle_get_info(const Dictionary &p_args) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	ERR_FAIL_NULL_V(ps, make_tool_error("ProjectSettings singleton not available."));

	// Project name.
	String project_name = ps->get("application/config/name");
	if (project_name.is_empty()) {
		project_name = "<unnamed project>";
	}

	// Godot version.
	Dictionary version_info = Engine::get_singleton()->get_version_info();
	String godot_version = vformat("%d.%d.%s",
			(int)version_info["major"],
			(int)version_info["minor"],
			(String)version_info["status"]);

	// Main scene.
	String main_scene = ps->get("application/run/main_scene");
	if (main_scene.is_empty()) {
		main_scene = "<none>";
	}

	// Autoloads: iterate ProjectSettings properties matching "autoload/*".
	Array autoloads;
	String autoload_text;
	List<PropertyInfo> properties;
	ps->get_property_list(&properties);
	for (const PropertyInfo &pi : properties) {
		if (pi.name.begins_with("autoload/")) {
			String autoload_name = pi.name.get_slice("/", 1);
			String autoload_value = ps->get(pi.name);
			// Autoload values may be prefixed with "*" for singleton.
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

	// Renderer.
	String renderer = ps->get("rendering/renderer/rendering_method");
	if (renderer.is_empty()) {
		renderer = "forward_plus"; // Default in Godot 4.x.
	}

	// Physics tick rate.
	int physics_ticks = ps->get("physics/common/physics_ticks_per_second");
	if (physics_ticks <= 0) {
		physics_ticks = 60; // Default.
	}

	// Window size.
	int window_width = ps->get("display/window/size/viewport_width");
	int window_height = ps->get("display/window/size/viewport_height");
	if (window_width <= 0) {
		window_width = 1152; // Godot 4.x default.
	}
	if (window_height <= 0) {
		window_height = 648;
	}

	// Build text representation.
	String text = vformat(
			"Project: %s\n"
			"Godot Version: %s\n"
			"Main Scene: %s\n",
			project_name, godot_version, main_scene);

	if (autoloads.size() > 0) {
		text += vformat("\nAutoloads (%d):\n%s", autoloads.size(), autoload_text);
	} else {
		text += "\nAutoloads: none\n";
	}

	text += vformat(
			"\nRenderer: %s\n"
			"Physics Tick Rate: %d\n"
			"Window Size: %dx%d",
			renderer, physics_ticks, window_width, window_height);

	// Build structured content.
	Dictionary structured;
	structured["project_name"] = project_name;
	structured["godot_version"] = godot_version;
	structured["main_scene"] = main_scene;
	structured["autoloads"] = autoloads;
	structured["renderer"] = renderer;
	structured["physics_ticks_per_second"] = physics_ticks;
	Dictionary window_size;
	window_size["width"] = window_width;
	window_size["height"] = window_height;
	structured["window_size"] = window_size;

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
// Tool B1: editor/read_file
// ============================================================================

Dictionary MCPEditorTools::handle_read_file(const Dictionary &p_args) {
	String path = p_args.get("path", "");

	// Path validation.
	if (path.is_empty()) {
		return make_tool_error("Missing required parameter: path");
	}
	if (!validate_path(path)) {
		return make_tool_error(vformat(
				"Invalid path: %s\n\n"
				"Paths must start with res:// and cannot contain '..' traversal sequences.",
				path));
	}

	// Check if file exists.
	if (!FileAccess::exists(path)) {
		String basename = path.get_file();
		Vector<String> suggestions = _find_similar_files(basename);

		String msg = vformat("File not found: %s", path);
		if (suggestions.size() > 0) {
			msg += "\n\nDid you mean one of these?";
			for (int i = 0; i < suggestions.size() && i < 5; i++) {
				msg += vformat("\n  %s", suggestions[i]);
			}
		}
		return make_tool_error(msg);
	}

	// Check for binary files by extension.
	String ext = path.get_extension().to_lower();
	static const char *binary_exts[] = {
		"png", "jpg", "jpeg", "bmp", "tga", "webp", "svg",
		"ogg", "mp3", "wav",
		"ttf", "otf", "woff", "woff2",
		"res", "scn", // Binary resource formats.
		"ctex", "stex", "sample",
		"glb", "gltf",
		nullptr
	};
	for (int i = 0; binary_exts[i] != nullptr; i++) {
		if (ext == binary_exts[i]) {
			return make_tool_error(vformat(
					"Cannot read binary file: %s\n\n"
					"editor/read_file only supports text files (.gd, .tscn, .tres, .cfg, "
					".json, .txt, .md, .shader, .gdshader, etc.).",
					path));
		}
	}

	// Read the file.
	String content = FileAccess::get_file_as_string(path);
	Error err = FileAccess::get_open_error();
	if (err != OK) {
		return make_tool_error(vformat("Failed to read file: %s (error: %d)", path, (int)err));
	}

	// Build structured content.
	Dictionary structured;
	structured["path"] = path;
	structured["size_bytes"] = content.length();
	structured["content"] = content;

	return make_tool_result(content, structured);
}

// ============================================================================
// Tool B2: editor/write_file
// ============================================================================

Dictionary MCPEditorTools::handle_write_file(const Dictionary &p_args) {
	String path = p_args.get("path", "");
	String content = p_args.get("content", "");
	bool create_directories = p_args.get("create_directories", true);

	// Reject excessively large writes to prevent abuse.
	if (content.utf8().length() > 10 * 1024 * 1024) {
		return make_tool_error("Content too large: maximum write size is 10 MB.");
	}

	// Path validation.
	if (path.is_empty()) {
		return make_tool_error("Missing required parameter: path");
	}
	if (!p_args.has("content")) {
		return make_tool_error("Missing required parameter: content");
	}
	if (!validate_path(path)) {
		return make_tool_error(vformat(
				"Invalid path: %s\n\n"
				"Paths must start with res:// and cannot contain '..' traversal sequences.",
				path));
	}

	// Create parent directories if requested.
	bool created_dirs = false;
	String dir_path = path.get_base_dir();
	if (create_directories && !DirAccess::dir_exists_absolute(dir_path)) {
		Error dir_err = DirAccess::make_dir_recursive_absolute(dir_path);
		if (dir_err != OK) {
			return make_tool_error(vformat(
					"Failed to create directory: %s (error: %d)", dir_path, (int)dir_err));
		}
		created_dirs = true;
	}

	// Write the file.
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_null()) {
		return make_tool_error(vformat(
				"Failed to open file for writing: %s (error: %d)",
				path, (int)FileAccess::get_open_error()));
	}

	file->store_string(content);
	file->flush();
	file.unref(); // Close the file.

	int bytes_written = content.utf8().length();

	// After write: trigger reimport for known resource types, filesystem scan for all.
	bool reimport_queued = false;
	bool scan_queued = false;
	String ext = path.get_extension().to_lower();

	// Known resource types that need reimport after modification.
	if (ext == "tscn" || ext == "tres" || ext == "shader" || ext == "gdshader") {
		if (EditorFileSystem::get_singleton()) {
			callable_mp(EditorFileSystem::get_singleton(),
					&EditorFileSystem::update_file)
					.call_deferred(path);
			reimport_queued = true;
		}
	}

	// For GDScript files and all others, trigger a scan to pick up changes.
	if (EditorFileSystem::get_singleton()) {
		callable_mp(EditorFileSystem::get_singleton(),
				&EditorFileSystem::scan_changes)
				.call_deferred();
		scan_queued = true;
	}

	// Build response.
	String text = vformat("Written %d bytes to %s", bytes_written, path);
	if (created_dirs) {
		text += vformat("\nCreated directories: %s", dir_path);
	}
	if (reimport_queued) {
		text += "\nReimport queued.";
	}
	if (scan_queued) {
		text += "\nFilesystem scan queued.";
	}

	Dictionary structured;
	structured["path"] = path;
	structured["bytes_written"] = bytes_written;
	structured["created_directories"] = created_dirs;
	structured["reimport_queued"] = reimport_queued;
	structured["scan_queued"] = scan_queued;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool B3: editor/list_files
// ============================================================================

Dictionary MCPEditorTools::handle_list_files(const Dictionary &p_args) {
	String directory = p_args.get("directory", "res://");
	String extension = p_args.get("extension", "");
	bool recursive = p_args.get("recursive", false);

	// Path validation.
	if (!validate_path(directory)) {
		return make_tool_error(vformat(
				"Invalid directory: %s\n\n"
				"Paths must start with res:// and cannot contain '..' traversal sequences.",
				directory));
	}

	// Ensure trailing slash.
	if (!directory.ends_with("/")) {
		directory += "/";
	}

	// Check directory exists.
	if (!DirAccess::dir_exists_absolute(directory)) {
		return make_tool_error(vformat("Directory not found: %s", directory));
	}

	// Collect files.
	Vector<String> files;
	if (recursive) {
		_list_files_recursive(directory, extension, files);
	} else {
		Ref<DirAccess> da = DirAccess::open(directory);
		if (da.is_null()) {
			return make_tool_error(vformat(
					"Failed to open directory: %s", directory));
		}

		da->list_dir_begin();
		String item = da->get_next();
		while (!item.is_empty()) {
			if (!da->current_is_dir()) {
				if (extension.is_empty() || item.get_extension().to_lower() == extension.to_lower()) {
					files.push_back(directory + item);
				}
			}
			item = da->get_next();
		}
		da->list_dir_end();
	}

	// Sort alphabetically.
	files.sort();

	// Build text response.
	String ext_filter_text = extension.is_empty() ? "" : vformat(" (extension: %s)", extension);
	String text = vformat("Files in %s%s:\n", directory, ext_filter_text);
	for (int i = 0; i < files.size(); i++) {
		text += vformat("  %s\n", files[i]);
	}
	text += vformat("\n%d files found.", files.size());

	// Build structured response.
	Array files_array;
	for (int i = 0; i < files.size(); i++) {
		files_array.push_back(files[i]);
	}

	Dictionary structured;
	structured["directory"] = directory;
	structured["extension_filter"] = extension;
	structured["recursive"] = recursive;
	structured["files"] = files_array;
	structured["count"] = files.size();

	return make_tool_result(text, structured);
}

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

Dictionary MCPEditorTools::handle_scan_filesystem(const Dictionary &p_args) {
	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (!efs) {
		return make_tool_error(
				"EditorFileSystem not available. This tool requires the Godot editor.");
	}

	// Fire-and-forget: queue scan on the main thread.
	callable_mp(efs, &EditorFileSystem::scan_changes).call_deferred();

	Dictionary structured;
	structured["status"] = "queued";

	return make_tool_result("Filesystem scan queued.", structured);
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
// Helper: find similar files (for "did you mean?" suggestions)
// ============================================================================

Vector<String> MCPEditorTools::_find_similar_files(const String &p_basename) {
	Vector<String> results;

	// Recursively search the project for files with matching basename.
	Vector<String> all_files;
	_list_files_recursive("res://", "", all_files);

	if (all_files.size() > 5000) {
		return Vector<String>(); // Project too large for similarity search.
	}

	String target = p_basename.to_lower();
	for (int i = 0; i < all_files.size(); i++) {
		String file_basename = all_files[i].get_file().to_lower();
		// Exact basename match (different path).
		if (file_basename == target) {
			results.push_back(all_files[i]);
		}
		// Partial match (basename contains the target or vice versa).
		else if (file_basename.find(target) != -1 || target.find(file_basename) != -1) {
			results.push_back(all_files[i]);
		}
	}

	return results;
}
