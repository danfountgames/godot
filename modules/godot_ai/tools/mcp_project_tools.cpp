/**************************************************************************/
/*  mcp_project_tools.cpp                                                 */
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

#include "mcp_builtin_tools.h"

#include "../mcp_paths.h"
#include "../mcp_tool_registry.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/variant/array.h"
#include "editor/editor_file_system.h"

// Bounds so a single call cannot return an unbounded payload to the model or spend
// unbounded time walking a large project.
static const int MAX_LISTED_FILES = 5000;
static const int MAX_READ_BYTES = 4 * 1024 * 1024;
static const int MAX_SEARCH_MATCHES = 500;

// Joins a res:// directory with a child name. `res://` already ends in a slash, so
// naive path_join()/concatenation produces `res:/child` and every later lookup of
// that path fails.
static String res_join(const String &p_base, const String &p_name) {
	if (p_base.ends_with("/")) {
		return p_base + p_name;
	}
	return p_base + "/" + p_name;
}

// Directories that never contain project-authored content worth showing an agent.
static bool is_ignored_directory(const String &p_name) {
	return p_name == "." || p_name == ".." || p_name == ".godot" || p_name == ".import" || p_name == ".git";
}

static void collect_files(const String &p_absolute_dir, const String &p_res_dir, const Vector<String> &p_extensions, bool p_recursive, Array &r_files) {
	if (r_files.size() >= MAX_LISTED_FILES) {
		return;
	}
	Ref<DirAccess> dir = DirAccess::open(p_absolute_dir);
	if (dir.is_null()) {
		return;
	}
	dir->list_dir_begin();
	String entry = dir->get_next();
	while (!entry.is_empty()) {
		if (dir->current_is_dir()) {
			if (p_recursive && !is_ignored_directory(entry)) {
				collect_files(p_absolute_dir.path_join(entry), res_join(p_res_dir, entry), p_extensions, true, r_files);
			}
		} else {
			const bool wanted = p_extensions.is_empty() || p_extensions.has(entry.get_extension().to_lower());
			if (wanted && r_files.size() < MAX_LISTED_FILES) {
				r_files.push_back(res_join(p_res_dir, entry));
			}
		}
		entry = dir->get_next();
	}
	dir->list_dir_end();
}

namespace {

// Shared plumbing for the tools that take a `folder` and walk it.
static Dictionary list_folder(const Dictionary &p_arguments, const Vector<String> &p_extensions, const String &p_result_key, MCPToolError &r_error) {
	MCPPaths::Resolved resolved;
	String error;
	const String folder = p_arguments.has("folder") ? String(p_arguments["folder"]) : String("res://");
	if (!MCPPaths::resolve(folder, resolved, error)) {
		r_error.set(MCPToolError::INVALID_ARGUMENTS, error);
		return Dictionary();
	}
	if (!resolved.exists || !resolved.is_directory) {
		r_error.set(MCPToolError::NOT_FOUND, vformat("'%s' is not a folder in this project", folder));
		return Dictionary();
	}

	Array files;
	const bool recursive = p_arguments.has("recursive") ? (bool)p_arguments["recursive"] : true;
	collect_files(resolved.absolute, resolved.res_path, p_extensions, recursive, files);
	files.sort();

	Dictionary result;
	result[p_result_key] = files;
	result["folder"] = resolved.res_path;
	result["truncated"] = files.size() >= MAX_LISTED_FILES;
	return result;
}

static Dictionary listing_schema(const String &p_folder_description) {
	Dictionary properties;
	properties["folder"] = MCPSchema::string_property(p_folder_description, "res://");
	properties["recursive"] = MCPSchema::bool_property("Descend into subfolders.", true);
	return MCPSchema::object_schema(properties);
}

static Dictionary listing_output_schema(const String &p_key, const String &p_description) {
	Dictionary properties;
	properties[p_key] = MCPSchema::array_property(p_description, MCPSchema::string_property("Project path."));
	properties["folder"] = MCPSchema::string_property("Folder that was searched.");
	properties["truncated"] = MCPSchema::bool_property("True when the result hit the listing limit.");
	return MCPSchema::object_schema(properties);
}

class ListScenesTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_ListScenes"; }
	virtual String get_description() const override {
		return "List the scene files (.tscn/.scn) in the project, optionally under a specific folder.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }
	virtual Dictionary get_input_schema() const override {
		return listing_schema("Project folder to search, as a res:// path.");
	}
	virtual Dictionary get_output_schema() const override {
		return listing_output_schema("scenes", "Scene paths.");
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		Vector<String> extensions;
		extensions.push_back("tscn");
		extensions.push_back("scn");
		return list_folder(p_arguments, extensions, "scenes", r_error);
	}
};

class ListAssetsTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_ListAssets"; }
	virtual String get_description() const override {
		return "List project files, optionally filtered by extension (for example [\"png\", \"tres\"]).";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["folder"] = MCPSchema::string_property("Project folder to search, as a res:// path.", "res://");
		properties["recursive"] = MCPSchema::bool_property("Descend into subfolders.", true);
		properties["extensions"] = MCPSchema::array_property(
				"Lower-case extensions to include, without the dot. Empty means every file.",
				MCPSchema::string_property("Extension."));
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary get_output_schema() const override {
		return listing_output_schema("assets", "Asset paths.");
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		Vector<String> extensions;
		if (p_arguments.has("extensions")) {
			const Array requested = p_arguments["extensions"];
			for (int i = 0; i < requested.size(); i++) {
				extensions.push_back(String(requested[i]).to_lower().trim_prefix("."));
			}
		}
		return list_folder(p_arguments, extensions, "assets", r_error);
	}
};

class ReadTextFileTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_ReadTextFile"; }
	virtual String get_description() const override {
		return "Read a UTF-8 text file from the project. Paths are confined to the project directory.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("File to read, as a res:// or project-relative path.");
		Vector<String> required;
		required.push_back("path");
		return MCPSchema::object_schema(properties, required);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Resolved res:// path.");
		properties["text"] = MCPSchema::string_property("File contents.");
		properties["truncated"] = MCPSchema::bool_property("True when the file was longer than the read limit.");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPPaths::Resolved resolved;
		String error;
		if (!MCPPaths::resolve_existing(p_arguments["path"], resolved, error)) {
			r_error.set(MCPToolError::NOT_FOUND, error);
			return Dictionary();
		}
		if (resolved.is_directory) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, vformat("'%s' is a folder, not a file", resolved.res_path));
			return Dictionary();
		}

		Ref<FileAccess> file = FileAccess::open(resolved.absolute, FileAccess::READ);
		if (file.is_null()) {
			r_error.set(MCPToolError::FAILED, vformat("could not open '%s' for reading", resolved.res_path));
			return Dictionary();
		}
		const uint64_t length = file->get_length();
		const bool truncated = length > (uint64_t)MAX_READ_BYTES;
		const Vector<uint8_t> bytes = file->get_buffer(truncated ? MAX_READ_BYTES : (int64_t)length);

		Dictionary result;
		result["path"] = resolved.res_path;
		result["text"] = String::utf8((const char *)bytes.ptr(), bytes.size());
		result["truncated"] = truncated;
		return result;
	}
};

class WriteTextFileTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_WriteTextFile"; }
	virtual String get_description() const override {
		return "Write a UTF-8 text file inside the project and refresh the editor's filesystem "
			   "view. Use the scene tools instead of writing .tscn/.tres files directly.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_FILES; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("File to write, as a res:// or project-relative path.");
		properties["text"] = MCPSchema::string_property("New file contents.");
		properties["create_directories"] = MCPSchema::bool_property("Create missing parent folders.", false);
		Vector<String> required;
		required.push_back("path");
		required.push_back("text");
		return MCPSchema::object_schema(properties, required);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Resolved res:// path.");
		properties["bytes_written"] = MCPSchema::integer_property("Number of bytes written.");
		properties["created"] = MCPSchema::bool_property("True when the file did not exist before.");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPPaths::Resolved resolved;
		String error;
		if (!MCPPaths::resolve(p_arguments["path"], resolved, error)) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, error);
			return Dictionary();
		}
		if (resolved.is_directory) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, vformat("'%s' is a folder", resolved.res_path));
			return Dictionary();
		}

		const bool created = !resolved.exists;
		if (created && (bool)p_arguments["create_directories"]) {
			Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
			if (dir.is_valid()) {
				dir->make_dir_recursive(resolved.absolute.get_base_dir());
			}
		}

		const String text = p_arguments["text"];
		Ref<FileAccess> file = FileAccess::open(resolved.absolute, FileAccess::WRITE);
		if (file.is_null()) {
			r_error.set(MCPToolError::FAILED,
					vformat("could not open '%s' for writing%s", resolved.res_path,
							created ? " (its parent folder may not exist; pass create_directories)" : ""));
			return Dictionary();
		}
		file->store_string(text);
		file->flush();
		const int64_t bytes_written = text.utf8().length();
		file.unref();

		// Make the change visible to the editor immediately, so a following tool call
		// or a human looking at the FileSystem dock sees the same project state.
		// Null in headless runs and unit tests, where there is nothing to refresh.
		if (EditorFileSystem::get_singleton()) {
			EditorFileSystem::get_singleton()->update_file(resolved.res_path);
		}

		Dictionary result;
		result["path"] = resolved.res_path;
		result["bytes_written"] = bytes_written;
		result["created"] = created;
		return result;
	}
};

class SearchProjectTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_SearchProject"; }
	virtual String get_description() const override {
		return "Search project text files for a literal substring and return matching lines.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["query"] = MCPSchema::string_property("Literal text to look for.");
		properties["folder"] = MCPSchema::string_property("Folder to search, as a res:// path.", "res://");
		properties["extensions"] = MCPSchema::array_property(
				"Extensions to search. Defaults to common text formats.",
				MCPSchema::string_property("Extension."));
		properties["case_sensitive"] = MCPSchema::bool_property("Match case exactly.", false);
		Vector<String> required;
		required.push_back("query");
		return MCPSchema::object_schema(properties, required);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary match_properties;
		match_properties["path"] = MCPSchema::string_property("File containing the match.");
		match_properties["line"] = MCPSchema::integer_property("1-based line number.");
		match_properties["text"] = MCPSchema::string_property("Matching line.");

		Dictionary properties;
		properties["matches"] = MCPSchema::array_property("Matches.", MCPSchema::object_schema(match_properties));
		properties["truncated"] = MCPSchema::bool_property("True when the match limit was reached.");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPPaths::Resolved resolved;
		String error;
		const String folder = p_arguments["folder"];
		if (!MCPPaths::resolve(folder, resolved, error)) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, error);
			return Dictionary();
		}
		if (!resolved.exists || !resolved.is_directory) {
			r_error.set(MCPToolError::NOT_FOUND, vformat("'%s' is not a folder in this project", folder));
			return Dictionary();
		}

		const String query = p_arguments["query"];
		if (query.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, "query must not be empty");
			return Dictionary();
		}
		const bool case_sensitive = p_arguments["case_sensitive"];

		Vector<String> extensions;
		if (p_arguments.has("extensions")) {
			const Array requested = p_arguments["extensions"];
			for (int i = 0; i < requested.size(); i++) {
				extensions.push_back(String(requested[i]).to_lower().trim_prefix("."));
			}
		}
		if (extensions.is_empty()) {
			const char *defaults[] = { "gd", "cs", "tscn", "tres", "cfg", "json", "md", "txt", "gdshader", "import" };
			for (const char *extension : defaults) {
				extensions.push_back(extension);
			}
		}

		Array files;
		collect_files(resolved.absolute, resolved.res_path, extensions, true, files);
		files.sort();

		const String needle = case_sensitive ? query : query.to_lower();
		Array matches;
		bool truncated = false;
		for (int i = 0; i < files.size() && !truncated; i++) {
			const String res_path = files[i];
			MCPPaths::Resolved file_resolved;
			if (!MCPPaths::resolve(res_path, file_resolved, error)) {
				continue;
			}
			Ref<FileAccess> file = FileAccess::open(file_resolved.absolute, FileAccess::READ);
			if (file.is_null() || file->get_length() > (uint64_t)MAX_READ_BYTES) {
				continue;
			}
			int line_number = 0;
			while (!file->eof_reached()) {
				const String line = file->get_line();
				line_number++;
				if (!(case_sensitive ? line : line.to_lower()).contains(needle)) {
					continue;
				}
				Dictionary match;
				match["path"] = res_path;
				match["line"] = line_number;
				// Long lines are clipped: a minified file must not flood the response.
				match["text"] = line.length() > 400 ? (line.substr(0, 400) + "...") : line;
				matches.push_back(match);
				if (matches.size() >= MAX_SEARCH_MATCHES) {
					truncated = true;
					break;
				}
			}
		}

		Dictionary result;
		result["matches"] = matches;
		result["truncated"] = truncated;
		return result;
	}
};

} // namespace

void mcp_register_project_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);

	registry->register_tool(Ref<MCPTool>(memnew(ListScenesTool)));
	registry->register_tool(Ref<MCPTool>(memnew(ListAssetsTool)));
	registry->register_tool(Ref<MCPTool>(memnew(ReadTextFileTool)));
	registry->register_tool(Ref<MCPTool>(memnew(WriteTextFileTool)));
	registry->register_tool(Ref<MCPTool>(memnew(SearchProjectTool)));
}
