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

static void ensure_capture_tools_registered() {
	if (!MCPToolRegistry::get_singleton()->has_tool("Godot_CaptureInspectorProperty")) {
		mcp_register_capture_tools();
	}
}

static Dictionary call(const String &p_tool, const Dictionary &p_arguments, MCPToolError &r_error) {
	return MCPToolRegistry::get_singleton()->call_tool(p_tool, p_arguments, r_error);
}

TEST_CASE("[godot_ai] semantic documentation capture tools declare and enforce their targets") {
	ensure_capture_tools_registered();
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	REQUIRE(registry->has_tool("Godot_CaptureInspectorProperty"));
	REQUIRE(registry->has_tool("Godot_CaptureSceneTreeNode"));

	const Dictionary inspector_descriptor = registry->get_tool_descriptor("Godot_CaptureInspectorProperty");
	const Dictionary inspector_schema = inspector_descriptor["inputSchema"];
	const Dictionary inspector_properties = inspector_schema["properties"];
	CHECK(inspector_properties.has("resource"));
	CHECK(inspector_properties.has("scene"));
	CHECK(inspector_properties.has("node_path"));
	CHECK(inspector_properties.has("context_above"));
	CHECK(inspector_properties.has("context_below"));

	MCPToolError error;
	Dictionary arguments;
	arguments["resource"] = "res://fixture.tres";
	arguments["property_chain"] = Array();
	call("Godot_CaptureInspectorProperty", arguments, error);
	CHECK(error.kind == MCPToolError::INVALID_ARGUMENTS);
	CHECK(error.message.contains("at least"));

	error.clear();
	Array chain;
	chain.push_back("text");
	arguments["property_chain"] = chain;
	arguments["scene"] = "res://scene.tscn";
	call("Godot_CaptureInspectorProperty", arguments, error);
	CHECK(error.kind == MCPToolError::INVALID_ARGUMENTS);
	CHECK(error.message.contains("exactly one"));

	const Ref<MCPTool> scene_tree_tool = registry->get_tool("Godot_CaptureSceneTreeNode");
	REQUIRE(scene_tree_tool.is_valid());
	Dictionary checkpoint_arguments;
	checkpoint_arguments["path"] = "res://docs/tree.png";
	const Vector<String> checkpoint_paths = scene_tree_tool->get_checkpoint_paths(checkpoint_arguments);
	REQUIRE(checkpoint_paths.size() == 1);
	CHECK(checkpoint_paths[0] == "res://docs/tree.png");
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

TEST_CASE("[godot_ai] Godot_DeleteProjectFile insists on confirmation and a checkpoint") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	if (!registry->has_tool("Godot_DeleteProjectFile")) {
		return;
	}

	const Dictionary meta = registry->get_tool_descriptor("Godot_DeleteProjectFile")["_meta"];
	CHECK(String(meta["capability"]) == "edit_files");
	CHECK((bool)meta["mutating"]);

	SUBCASE("it declares the file it will delete, so the checkpoint layer can save it") {
		// Unlike user:// data, a deleted project file *is* recoverable - but only because the
		// tool names it before writing. A tool that deleted an undeclared path would be
		// unrecoverable, and MCPTool forbids exactly that.
		Dictionary arguments;
		arguments["path"] = "res://scenes/main.tscn";
		const Vector<String> declared =
				registry->get_tool("Godot_DeleteProjectFile")->get_checkpoint_paths(arguments);
		CHECK(declared.size() == 1);
		CHECK(declared[0] == "res://scenes/main.tscn");
	}

	SUBCASE("a path outside the project is refused") {
		Dictionary arguments;
		arguments["path"] = "res://../../etc/passwd";
		arguments["confirm"] = true;
		MCPToolError error;
		call("Godot_DeleteProjectFile", arguments, error);
		CHECK(error.kind != MCPToolError::NONE);
	}
}

TEST_CASE("[godot_ai] Godot_ScanFilesystem is a file-writing operation, not a read") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	if (!registry->has_tool("Godot_ScanFilesystem")) {
		return;
	}
	// A rescan rewrites the editor's global class cache under .godot/. read_project would be
	// the comfortable answer and the wrong one, so it is pinned here.
	const Dictionary meta = registry->get_tool_descriptor("Godot_ScanFilesystem")["_meta"];
	CHECK(String(meta["capability"]) == "edit_files");

	SUBCASE("without an editor it reports unsupported rather than crashing") {
		MCPToolError error;
		call("Godot_ScanFilesystem", Dictionary(), error);
		CHECK(error.kind == MCPToolError::UNSUPPORTED);
	}
}

TEST_CASE("[godot_ai] every input tool accepts frames_per_step") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	// Pacing is not a pointer-only idea: a typed string, a gamepad axis sweep and a touch drag
	// are all gestures whose frames matter. If one tool were left out, that gesture would
	// silently be the one delivered in a single frame.
	const char *tools[] = { "Godot_SendPointerInput", "Godot_SendTouchInput",
							"Godot_SendKeyInput", "Godot_SendGamepadInput" };
	for (int i = 0; i < 4; i++) {
		if (!registry->has_tool(tools[i])) {
			continue;
		}
		const Dictionary schema = registry->get_tool_descriptor(tools[i])["inputSchema"];
		const Dictionary properties = schema["properties"];
		CHECK_MESSAGE(properties.has("frames_per_step"),
				vformat("%s does not accept frames_per_step", tools[i]));
	}
}

TEST_CASE("[godot_ai] Godot_SendTouchInput offers a swept drag, like the pointer does") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	if (!registry->has_tool("Godot_SendTouchInput")) {
		return;
	}
	// The gap this closes: a consuming project needed a paced touch drag, found only
	// relative_x/relative_y, and wrote a shell script to assemble one call at a time. The tool
	// should have made that unnecessary, so the schema is pinned.
	const Dictionary schema = registry->get_tool_descriptor("Godot_SendTouchInput")["inputSchema"];
	const Dictionary properties = schema["properties"];
	CHECK(properties.has("to_x"));
	CHECK(properties.has("to_y"));
	CHECK(properties.has("steps"));
	// And the by-hand form stays, for a caller assembling a gesture itself.
	CHECK(properties.has("relative_x"));
}

} // namespace TestMCPTools

#endif // TEST_MCP_TOOLS_H
