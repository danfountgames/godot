/**************************************************************************/
/*  mcp_user_data_tools.cpp                                               */
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

// Saves and settings: the `user://` directory.
//
// The project tools deliberately cannot reach here, and that was right - a tool that
// can wander outside the project is a tool that can wander anywhere. But it left an
// instruction in the game-production template impossible to follow: verify that
// progress survives a quit and relaunch, and that a corrupt save is handled rather
// than crashing. Both live in this directory and nowhere else.
//
// So it gets its own boundary and its own two capability classes. Two things make it
// more dangerous than the project directory, and shape what is here:
//
//   * **Nothing is watching it.** Project files are under version control; a player's
//     save is not. There is no checkpoint layer for this, so deletion demands an
//     explicit confirmation rather than a plausible-looking argument.
//   * **It is the player's data, not the developer's.** Reading it is `ask`, not
//     `allow`, even though reading the project is allowed by default.

#include "mcp_builtin_tools.h"

#include "../mcp_paths.h"
#include "../mcp_tool_registry.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/variant/array.h"

namespace {

// Enough for a save file or a settings file. Anything larger is a media asset that a
// caller should be reading some other way.
const int64_t MAX_USER_FILE_BYTES = 4 * 1024 * 1024;

// FileAccess has no static size query, so this opens and asks. Returns -1 when the
// file cannot be opened at all, which the callers treat as "not readable".
int64_t file_size(const String &p_absolute) {
	Ref<FileAccess> file = FileAccess::open(p_absolute, FileAccess::READ);
	return file.is_null() ? -1 : (int64_t)file->get_length();
}

class ListUserFilesTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_ListUserFiles"; }
	virtual String get_description() const override {
		return "List files in the game's user:// directory - where saves, settings and logs are "
			   "written. This is the player's data, not the project, and it is the one place "
			   "these tools can reach that no version control is watching.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_USER_DATA; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["folder"] = MCPSchema::string_property(
				"Folder to list, as a user:// path. Defaults to the root.", "user://");
		properties["recursive"] = MCPSchema::bool_property("Descend into subfolders.", true);
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["root"] = MCPSchema::string_property("Absolute path of the user directory.");
		properties["files"] = MCPSchema::array_property("Files found.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		return MCPSchema::object_schema(properties);
	}

	static void collect(const String &p_absolute, const String &p_prefix, bool p_recursive, Array &r_files) {
		Ref<DirAccess> access = DirAccess::open(p_absolute);
		if (access.is_null()) {
			return;
		}
		access->list_dir_begin();
		String name = access->get_next();
		while (!name.is_empty()) {
			if (name == "." || name == "..") {
				name = access->get_next();
				continue;
			}
			const String child = p_prefix.is_empty() ? name : p_prefix + "/" + name;
			if (access->current_is_dir()) {
				if (p_recursive) {
					collect(p_absolute.path_join(name), child, true, r_files);
				}
			} else {
				Dictionary entry;
				entry["path"] = "user://" + child;
				entry["size"] = file_size(p_absolute.path_join(name));
				r_files.push_back(entry);
			}
			name = access->get_next();
		}
		access->list_dir_end();
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		const String folder = p_arguments.has("folder") ? String(p_arguments["folder"]) : String("user://");
		MCPPaths::Resolved resolved;
		String error;
		if (!MCPPaths::resolve_user(folder, resolved, error)) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, error);
			return Dictionary();
		}

		Array files;
		// A missing user directory is not an error: a game that has never saved has
		// nothing here, and that is a fact worth returning rather than a failure.
		if (resolved.exists && resolved.is_directory) {
			const String prefix = resolved.res_path == "user://"
					? String()
					: resolved.res_path.trim_prefix("user://");
			collect(resolved.absolute, prefix, (bool)p_arguments["recursive"], files);
		}

		Dictionary result;
		result["root"] = MCPPaths::get_user_root();
		result["files"] = files;
		return result;
	}
};

class ReadUserFileTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_ReadUserFile"; }
	virtual String get_description() const override {
		return "Read a UTF-8 file from user://, such as a save or a settings file. Inspecting the "
			   "file is not the same as verifying that loading works - for that, restart the game "
			   "and check the state it comes back with.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_USER_DATA; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("File to read, as a user:// path.");
		Vector<String> required;
		required.push_back("path");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("File that was read.");
		properties["content"] = MCPSchema::string_property("File contents.");
		properties["text"] = MCPSchema::string_property(
				"The same contents under the name Godot_ReadTextFile uses, so the two read tools can be used interchangeably.");
		properties["size"] = MCPSchema::integer_property("Size in bytes.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPPaths::Resolved resolved;
		String error;
		if (!MCPPaths::resolve_user(p_arguments["path"], resolved, error)) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, error);
			return Dictionary();
		}
		if (!resolved.exists || resolved.is_directory) {
			r_error.set(MCPToolError::NOT_FOUND,
					vformat("'%s' is not a file in the user data directory", resolved.res_path));
			return Dictionary();
		}
		const int64_t size = file_size(resolved.absolute);
		if (size > MAX_USER_FILE_BYTES) {
			r_error.set(MCPToolError::INVALID_STATE,
					vformat("'%s' is %d bytes, larger than this tool will return", resolved.res_path, size));
			return Dictionary();
		}

		Error open_error = OK;
		const String content = FileAccess::get_file_as_string(resolved.absolute, &open_error);
		if (open_error != OK) {
			r_error.set(MCPToolError::FAILED, vformat("could not read '%s'", resolved.res_path));
			return Dictionary();
		}

		Dictionary result;
		result["path"] = resolved.res_path;
		result["content"] = content;
		result["size"] = size;
		return result;
	}
};

class WriteUserFileTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_WriteUserFile"; }
	virtual String get_description() const override {
		return "Write a file into user://. Its main honest use is making a save deliberately "
			   "wrong - truncated, malformed, from an older version - so that the game's "
			   "recovery path can be tested at all. Writing a save to reach a game state is "
			   "not the same as playing to it, and proves nothing about whether a player could.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_USER_DATA; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("File to write, as a user:// path.");
		properties["content"] = MCPSchema::string_property("Contents to write.");
		Vector<String> required;
		required.push_back("path");
		required.push_back("content");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("File that was written.");
		properties["size"] = MCPSchema::integer_property("Bytes written.");
		properties["replaced"] = MCPSchema::bool_property("True when a file was overwritten.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPPaths::Resolved resolved;
		String error;
		if (!MCPPaths::resolve_user(p_arguments["path"], resolved, error)) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, error);
			return Dictionary();
		}
		if (resolved.is_directory) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					vformat("'%s' is a directory", resolved.res_path));
			return Dictionary();
		}
		const bool replaced = resolved.exists;

		const String directory = resolved.absolute.get_base_dir();
		Ref<DirAccess> access = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (access.is_valid() && !access->dir_exists(directory)) {
			access->make_dir_recursive(directory);
		}

		Error open_error = OK;
		Ref<FileAccess> file = FileAccess::open(resolved.absolute, FileAccess::WRITE, &open_error);
		if (file.is_null()) {
			r_error.set(MCPToolError::FAILED, vformat("could not write '%s'", resolved.res_path));
			return Dictionary();
		}
		const String content = p_arguments["content"];
		file->store_string(content);
		file.unref();

		Dictionary result;
		result["path"] = resolved.res_path;
		result["size"] = file_size(resolved.absolute);
		result["replaced"] = replaced;
		return result;
	}
};

class DeleteUserFileTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_DeleteUserFile"; }
	virtual String get_description() const override {
		return "Delete a file from user://, for testing a first run or a lost save. Requires "
			   "confirm=true, because unlike the project there is no checkpoint and no version "
			   "control behind this - a player's save deleted here is gone.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_USER_DATA; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("File to delete, as a user:// path.");
		properties["confirm"] = MCPSchema::bool_property(
				"Must be true. Deliberate, because this cannot be undone.", false);
		Vector<String> required;
		required.push_back("path");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("File that was deleted.");
		properties["deleted"] = MCPSchema::bool_property("True when a file was removed.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!(bool)p_arguments["confirm"]) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					"deleting user data needs confirm=true; there is no checkpoint for this "
					"directory, so the deletion cannot be undone");
			return Dictionary();
		}
		MCPPaths::Resolved resolved;
		String error;
		if (!MCPPaths::resolve_user(p_arguments["path"], resolved, error)) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, error);
			return Dictionary();
		}
		if (resolved.is_directory) {
			// One file at a time, on purpose. A recursive delete rooted at a directory
			// is exactly the operation that has already destroyed a working tree once
			// in this repository's history.
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					vformat("'%s' is a directory; this tool deletes one file at a time", resolved.res_path));
			return Dictionary();
		}
		if (!resolved.exists) {
			r_error.set(MCPToolError::NOT_FOUND,
					vformat("'%s' does not exist", resolved.res_path));
			return Dictionary();
		}

		Ref<DirAccess> access = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (access.is_null() || access->remove(resolved.absolute) != OK) {
			r_error.set(MCPToolError::FAILED, vformat("could not delete '%s'", resolved.res_path));
			return Dictionary();
		}

		Dictionary result;
		result["path"] = resolved.res_path;
		result["deleted"] = true;
		return result;
	}
};

} // namespace

void mcp_register_user_data_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->register_tool(Ref<MCPTool>(memnew(ListUserFilesTool)));
	registry->register_tool(Ref<MCPTool>(memnew(ReadUserFileTool)));
	registry->register_tool(Ref<MCPTool>(memnew(WriteUserFileTool)));
	registry->register_tool(Ref<MCPTool>(memnew(DeleteUserFileTool)));
}
