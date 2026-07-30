/**************************************************************************/
/*  test_mcp_tools.h                                                      */
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

#ifndef TEST_MCP_TOOLS_H
#define TEST_MCP_TOOLS_H

#include "modules/godot_ai/mcp_tool_registry.h"
#include "modules/godot_ai/tests/test_mcp_fs_helpers.h"
#include "modules/godot_ai/tools/mcp_builtin_tools.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

#include "tests/test_macros.h"

namespace TestMCPTools {

// Project fixture for the filesystem-backed tools. Cleanup goes through the guarded
// helper, never a working-directory-relative recursive erase.
class ToolProjectFixture {
	String root;

public:
	explicit ToolProjectFixture(const String &p_suffix) {
		root = OS::get_singleton()->get_cache_path().path_join(
				"godot_ai_test_tools_" + p_suffix + "_" + itos(OS::get_singleton()->get_process_id()));
		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		dir->make_dir_recursive(root.path_join("scenes/levels"));
		MCPPaths::set_project_root_override(root);

		write("scenes/main.tscn", "[gd_scene format=3]\n\n[node name=\"Main\" type=\"Node2D\"]\n");
		write("scenes/levels/level_1.tscn", "[gd_scene format=3]\n\n[node name=\"Level\" type=\"Node2D\"]\n");
		write("notes.txt", "hello from a project text file\nsecond line with Sprite2D\n");
		write("scripts.gd", "extends Node\n\nfunc _ready():\n\tprint(\"ready\")\n");
	}
	~ToolProjectFixture() {
		MCPPaths::clear_project_root_override();
		mcp_test_remove_tree(root);
	}

	void write(const String &p_relative, const String &p_contents) const {
		Ref<FileAccess> file = FileAccess::open(root.path_join(p_relative), FileAccess::WRITE);
		if (file.is_valid()) {
			file->store_string(p_contents);
		}
	}
	bool exists(const String &p_relative) const { return FileAccess::exists(root.path_join(p_relative)); }
	String read(const String &p_relative) const { return FileAccess::get_file_as_string(root.path_join(p_relative)); }
};

// The project tools are registered once for the whole suite; registering them twice
// would be rejected as duplicates.
static void ensure_project_tools_registered() {
	if (!MCPToolRegistry::get_singleton()->has_tool("Godot_ListScenes")) {
		mcp_register_project_tools();
	}
}

static Dictionary call(const String &p_tool, const Dictionary &p_arguments, MCPToolError &r_error) {
	return MCPToolRegistry::get_singleton()->call_tool(p_tool, p_arguments, r_error);
}

TEST_CASE("[godot_ai] Godot_ListScenes returns usable res:// paths") {
	ensure_project_tools_registered();
	ToolProjectFixture fixture("list");
	MCPToolError error;

	const Dictionary result = call("Godot_ListScenes", Dictionary(), error);
	REQUIRE_FALSE(error.has_error());
	const Array scenes = result["scenes"];
	REQUIRE(scenes.size() == 2);

	// Regression: joining a child onto "res://" once produced "res:/scenes/...", a
	// path that looks plausible but fails every subsequent lookup.
	for (int i = 0; i < scenes.size(); i++) {
		const String path = scenes[i];
		CHECK(path.begins_with("res://"));
		CHECK_FALSE(path.contains("res:/scenes"));

		// The advertised path must round-trip: another tool has to be able to use it.
		MCPToolError read_error;
		Dictionary read_arguments;
		read_arguments["path"] = path;
		call("Godot_ReadTextFile", read_arguments, read_error);
		CHECK_FALSE(read_error.has_error());
	}
	CHECK_FALSE((bool)result["truncated"]);
}

TEST_CASE("[godot_ai] Godot_ListScenes honours folder and recursion") {
	ensure_project_tools_registered();
	ToolProjectFixture fixture("folder");
	MCPToolError error;

	SUBCASE("a subfolder narrows the result") {
		Dictionary arguments;
		arguments["folder"] = "res://scenes/levels";
		const Array scenes = call("Godot_ListScenes", arguments, error)["scenes"];
		REQUIRE(scenes.size() == 1);
		CHECK(String(scenes[0]) == "res://scenes/levels/level_1.tscn");
	}

	SUBCASE("recursive false stays at one level") {
		Dictionary arguments;
		arguments["folder"] = "res://scenes";
		arguments["recursive"] = false;
		const Array scenes = call("Godot_ListScenes", arguments, error)["scenes"];
		REQUIRE(scenes.size() == 1);
		CHECK(String(scenes[0]) == "res://scenes/main.tscn");
	}

	SUBCASE("a missing folder is reported, not silently empty") {
		Dictionary arguments;
		arguments["folder"] = "res://does_not_exist";
		call("Godot_ListScenes", arguments, error);
		CHECK(error.kind == MCPToolError::NOT_FOUND);
	}

	SUBCASE("a folder outside the project is refused") {
		Dictionary arguments;
		arguments["folder"] = "res://../..";
		call("Godot_ListScenes", arguments, error);
		CHECK(error.kind == MCPToolError::INVALID_ARGUMENTS);
	}
}

TEST_CASE("[godot_ai] Godot_ListAssets filters by extension") {
	ensure_project_tools_registered();
	ToolProjectFixture fixture("assets");
	MCPToolError error;

	Dictionary arguments;
	Array extensions;
	extensions.push_back("gd");
	arguments["extensions"] = extensions;
	const Array assets = call("Godot_ListAssets", arguments, error)["assets"];
	REQUIRE_FALSE(error.has_error());
	REQUIRE(assets.size() == 1);
	CHECK(String(assets[0]) == "res://scripts.gd");
}

TEST_CASE("[godot_ai] Godot_ReadTextFile") {
	ensure_project_tools_registered();
	ToolProjectFixture fixture("read");
	MCPToolError error;

	SUBCASE("reads a project file") {
		Dictionary arguments;
		arguments["path"] = "res://notes.txt";
		const Dictionary result = call("Godot_ReadTextFile", arguments, error);
		CHECK_FALSE(error.has_error());
		CHECK(String(result["text"]).begins_with("hello from a project text file"));
		CHECK_FALSE((bool)result["truncated"]);
	}

	SUBCASE("refuses a directory") {
		Dictionary arguments;
		arguments["path"] = "res://scenes";
		call("Godot_ReadTextFile", arguments, error);
		CHECK(error.kind == MCPToolError::INVALID_ARGUMENTS);
	}

	SUBCASE("refuses a path outside the project") {
		Dictionary arguments;
		arguments["path"] = "res://../../etc/passwd";
		call("Godot_ReadTextFile", arguments, error);
		CHECK(error.kind == MCPToolError::NOT_FOUND);
		CHECK(error.message.contains("outside the project"));
	}

	SUBCASE("reports a missing file") {
		Dictionary arguments;
		arguments["path"] = "res://missing.txt";
		call("Godot_ReadTextFile", arguments, error);
		CHECK(error.kind == MCPToolError::NOT_FOUND);
	}
}

TEST_CASE("[godot_ai] Godot_WriteTextFile") {
	ensure_project_tools_registered();
	ToolProjectFixture fixture("write");
	MCPToolError error;

	SUBCASE("creates a new file and reports it as created") {
		Dictionary arguments;
		arguments["path"] = "res://new_file.txt";
		arguments["text"] = "written by a tool";
		const Dictionary result = call("Godot_WriteTextFile", arguments, error);
		REQUIRE_FALSE(error.has_error());
		CHECK((bool)result["created"]);
		CHECK((int)result["bytes_written"] == 17);
		CHECK(fixture.read("new_file.txt") == "written by a tool");
	}

	SUBCASE("overwriting reports created false") {
		Dictionary arguments;
		arguments["path"] = "res://notes.txt";
		arguments["text"] = "replaced";
		const Dictionary result = call("Godot_WriteTextFile", arguments, error);
		REQUIRE_FALSE(error.has_error());
		CHECK_FALSE((bool)result["created"]);
		CHECK(fixture.read("notes.txt") == "replaced");
	}

	SUBCASE("refuses to write outside the project") {
		Dictionary arguments;
		arguments["path"] = "../escaped.txt";
		arguments["text"] = "nope";
		call("Godot_WriteTextFile", arguments, error);
		CHECK(error.kind == MCPToolError::INVALID_ARGUMENTS);
	}

	SUBCASE("a missing parent folder fails unless create_directories is set") {
		Dictionary arguments;
		arguments["path"] = "res://deep/nested/file.txt";
		arguments["text"] = "content";
		call("Godot_WriteTextFile", arguments, error);
		CHECK(error.kind == MCPToolError::FAILED);

		MCPToolError create_error;
		arguments["create_directories"] = true;
		call("Godot_WriteTextFile", arguments, create_error);
		CHECK_FALSE(create_error.has_error());
		CHECK(fixture.exists("deep/nested/file.txt"));
	}
}

TEST_CASE("[godot_ai] Godot_SearchProject") {
	ensure_project_tools_registered();
	ToolProjectFixture fixture("search");
	MCPToolError error;

	SUBCASE("finds a literal match with its line number") {
		Dictionary arguments;
		arguments["query"] = "Sprite2D";
		const Dictionary result = call("Godot_SearchProject", arguments, error);
		REQUIRE_FALSE(error.has_error());
		const Array matches = result["matches"];
		// Regression: a broken res:// join made every file unreadable, so search
		// silently returned nothing at all.
		REQUIRE(matches.size() == 1);
		const Dictionary match = matches[0];
		CHECK(String(match["path"]) == "res://notes.txt");
		CHECK((int)match["line"] == 2);
		CHECK(String(match["text"]).contains("Sprite2D"));
	}

	SUBCASE("is case-insensitive by default and exact when asked") {
		Dictionary arguments;
		arguments["query"] = "sprite2d";
		CHECK(((Array)call("Godot_SearchProject", arguments, error)["matches"]).size() == 1);

		arguments["case_sensitive"] = true;
		CHECK(((Array)call("Godot_SearchProject", arguments, error)["matches"]).size() == 0);
	}

	SUBCASE("an empty query is rejected") {
		Dictionary arguments;
		arguments["query"] = "";
		call("Godot_SearchProject", arguments, error);
		CHECK(error.kind == MCPToolError::INVALID_ARGUMENTS);
	}
}

TEST_CASE("[godot_ai] Scene tools declare edit_scene and refuse without an editor") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	if (!registry->has_tool("Godot_ManageNode")) {
		mcp_register_scene_tools();
	}

	// Structural editing is a mutating capability, so a read-only session and a
	// deny policy can both refuse it without the tool having to check.
	const Dictionary descriptor = registry->get_tool_descriptor("Godot_ManageNode");
	const Dictionary meta = descriptor["_meta"];
	CHECK(String(meta["capability"]) == "edit_scene");
	CHECK((bool)meta["mutating"]);

	const Dictionary undo_meta = registry->get_tool_descriptor("Godot_UndoLastAction")["_meta"];
	CHECK(String(undo_meta["capability"]) == "edit_scene");

	SUBCASE("the action enum is enforced by the schema") {
		Dictionary arguments;
		arguments["action"] = "explode";
		MCPToolError error;
		call("Godot_ManageNode", arguments, error);
		CHECK(error.kind == MCPToolError::INVALID_ARGUMENTS);
		CHECK(error.message.contains("create"));
	}

	SUBCASE("a missing action is reported") {
		MCPToolError error;
		call("Godot_ManageNode", Dictionary(), error);
		CHECK(error.kind == MCPToolError::INVALID_ARGUMENTS);
		CHECK(error.message.contains("action"));
	}

	SUBCASE("without an editor it reports unsupported rather than crashing") {
		Dictionary arguments;
		arguments["action"] = "create";
		arguments["type"] = "Node2D";
		MCPToolError error;
		call("Godot_ManageNode", arguments, error);
		CHECK(error.kind == MCPToolError::UNSUPPORTED);
	}
}

TEST_CASE("[godot_ai] Editor tools refuse to run without an editor") {
	// The doctest binary has no EditorInterface, which is exactly the state these
	// tools must report clearly rather than crash in.
	if (MCPToolRegistry::get_singleton()->has_tool("Godot_OpenScene")) {
		MCPToolError error;
		Dictionary arguments;
		arguments["path"] = "res://scenes/main.tscn";
		call("Godot_OpenScene", arguments, error);
		CHECK(error.kind == MCPToolError::UNSUPPORTED);
	}
}

TEST_CASE("[godot_ai] Godot_CloseScene is declared as a scene mutation") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	if (!registry->has_tool("Godot_CloseScene")) {
		return;
	}

	// Closing a tab drops the editor's in-memory copy, and with discard_unsaved it
	// destroys edits. Declaring it read_project would let a read-only session throw
	// away a user's work, so this is pinned rather than left to reviewer memory.
	const Dictionary meta = registry->get_tool_descriptor("Godot_CloseScene")["_meta"];
	CHECK(String(meta["capability"]) == "edit_scene");
	CHECK((bool)meta["mutating"]);

	SUBCASE("path is optional, so a bare call is schema-valid and fails on state") {
		// It must reach the editor check rather than being rejected for a missing
		// argument: closing "the current scene" is the common case.
		MCPToolError error;
		call("Godot_CloseScene", Dictionary(), error);
		CHECK(error.kind == MCPToolError::UNSUPPORTED);
	}

	SUBCASE("without an editor it reports unsupported rather than crashing") {
		Dictionary arguments;
		arguments["path"] = "res://scenes/main.tscn";
		MCPToolError error;
		call("Godot_CloseScene", arguments, error);
		CHECK(error.kind == MCPToolError::UNSUPPORTED);
	}

	SUBCASE("a path outside the project is refused") {
		Dictionary arguments;
		arguments["path"] = "res://../../etc/passwd";
		MCPToolError error;
		call("Godot_CloseScene", arguments, error);
		// The editor check runs first in this binary, so either refusal is correct -
		// what must not happen is the path being accepted.
		CHECK((error.kind == MCPToolError::UNSUPPORTED || error.kind == MCPToolError::INVALID_ARGUMENTS));
	}

	SUBCASE("it declares no checkpoint paths") {
		// Closing a tab writes no file. Declaring one here would snapshot a scene the
		// tool never touches, and MCPTool forbids writing an undeclared path - not
		// declaring one it does not write is the other half of that contract.
		CHECK(registry->get_tool("Godot_CloseScene")->get_checkpoint_paths(Dictionary()).is_empty());
	}
}

} // namespace TestMCPTools

#endif // TEST_MCP_TOOLS_H
