/**************************************************************************/
/*  test_mcp_checkpoints.h                                                */
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

#ifndef TEST_MCP_CHECKPOINTS_H
#define TEST_MCP_CHECKPOINTS_H

#include "modules/godot_ai/mcp_checkpoints.h"
#include "modules/godot_ai/mcp_paths.h"
#include "modules/godot_ai/mcp_tool_registry.h"
#include "modules/godot_ai/tests/test_mcp_fs_helpers.h"
#include "modules/godot_ai/tools/mcp_builtin_tools.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

#include "tests/test_macros.h"

namespace TestMCPCheckpoints {

// A scratch project plus a scratch checkpoint store, so restores are exercised
// against real files.
class CheckpointFixture {
	String base;
	String root;
	String store;

public:
	explicit CheckpointFixture(const String &p_suffix) {
		base = OS::get_singleton()->get_cache_path().path_join(
				"godot_ai_test_ckpt_" + p_suffix + "_" + itos(OS::get_singleton()->get_process_id()));
		root = base.path_join("project");
		store = base.path_join("store");

		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		dir->make_dir_recursive(root);
		dir->make_dir_recursive(store);

		MCPPaths::set_project_root_override(root);
		MCPCheckpoints::set_root_override(store);
	}
	~CheckpointFixture() {
		MCPPaths::clear_project_root_override();
		MCPCheckpoints::clear_root_override();
		// Remove the whole scratch tree in one go; both subdirectories live under it.
		mcp_test_remove_tree(store);
		mcp_test_remove_tree(root);
		mcp_test_remove_tree(base);
	}

	void write(const String &p_relative, const String &p_contents) const {
		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		dir->make_dir_recursive(root.path_join(p_relative).get_base_dir());
		Ref<FileAccess> file = FileAccess::open(root.path_join(p_relative), FileAccess::WRITE);
		if (file.is_valid()) {
			file->store_string(p_contents);
		}
	}
	String read(const String &p_relative) const {
		return FileAccess::get_file_as_string(root.path_join(p_relative));
	}
	bool exists(const String &p_relative) const {
		return FileAccess::exists(root.path_join(p_relative));
	}
};

static Vector<String> paths_of(const String &p_a, const String &p_b = String()) {
	Vector<String> paths;
	paths.push_back(p_a);
	if (!p_b.is_empty()) {
		paths.push_back(p_b);
	}
	return paths;
}

TEST_CASE("[godot_ai] A checkpoint restores previous file contents") {
	CheckpointFixture fixture("restore");
	fixture.write("notes.txt", "original contents");

	String error;
	const String id = MCPCheckpoints::create("Godot_WriteTextFile", "Godot_WriteTextFile(path=notes.txt)",
			paths_of("res://notes.txt"), error);
	REQUIRE_FALSE(id.is_empty());

	// Simulate the tool doing its work.
	fixture.write("notes.txt", "changed by a tool");
	CHECK(fixture.read("notes.txt") == "changed by a tool");

	int restored = 0;
	int removed = 0;
	REQUIRE(MCPCheckpoints::restore(id, restored, removed, error));
	CHECK(restored == 1);
	CHECK(removed == 0);
	// The whole point: byte-for-byte what was there before.
	CHECK(fixture.read("notes.txt") == "original contents");
}

TEST_CASE("[godot_ai] Restoring removes files the tool created") {
	CheckpointFixture fixture("created");

	String error;
	const String id = MCPCheckpoints::create("Godot_WriteTextFile", "Godot_WriteTextFile(path=new.txt)",
			paths_of("res://new.txt"), error);
	REQUIRE_FALSE(id.is_empty());

	fixture.write("new.txt", "created by a tool");
	CHECK(fixture.exists("new.txt"));

	int restored = 0;
	int removed = 0;
	REQUIRE(MCPCheckpoints::restore(id, restored, removed, error));
	CHECK(restored == 0);
	CHECK(removed == 1);
	// A file that did not exist before must not exist after a restore.
	CHECK_FALSE(fixture.exists("new.txt"));
}

TEST_CASE("[godot_ai] Checkpoint manifests describe what they cover") {
	CheckpointFixture fixture("manifest");
	fixture.write("a.txt", "aaa");

	String error;
	const String id = MCPCheckpoints::create("Godot_WriteTextFile", "Godot_WriteTextFile(path=a.txt)",
			paths_of("res://a.txt", "res://b.txt"), error);
	REQUIRE_FALSE(id.is_empty());

	const Array checkpoints = MCPCheckpoints::list();
	REQUIRE(checkpoints.size() == 1);
	const Dictionary manifest = checkpoints[0];
	CHECK(String(manifest["id"]) == id);
	CHECK(String(manifest["tool"]) == "Godot_WriteTextFile");
	CHECK(String(manifest["invocation"]).contains("a.txt"));

	const Array files = manifest["files"];
	REQUIRE(files.size() == 2);
	// One file existed and one did not; a restore has to treat them differently.
	bool saw_existing = false;
	bool saw_absent = false;
	for (int i = 0; i < files.size(); i++) {
		const Dictionary entry = files[i];
		if (String(entry["path"]) == "res://a.txt") {
			saw_existing = (bool)entry["existed"];
		} else if (String(entry["path"]) == "res://b.txt") {
			saw_absent = !(bool)entry["existed"];
		}
	}
	CHECK(saw_existing);
	CHECK(saw_absent);
}

TEST_CASE("[godot_ai] Checkpoint edge cases") {
	CheckpointFixture fixture("edges");
	String error;

	SUBCASE("no paths means no checkpoint") {
		CHECK(MCPCheckpoints::create("Godot_ManageNode", "Godot_ManageNode()", Vector<String>(), error).is_empty());
		CHECK(error.is_empty());
		CHECK(MCPCheckpoints::list().is_empty());
	}

	SUBCASE("paths outside the project are not snapshotted") {
		CHECK(MCPCheckpoints::create("Godot_WriteTextFile", "x", paths_of("res://../../etc/passwd"), error).is_empty());
		CHECK(MCPCheckpoints::list().is_empty());
	}

	SUBCASE("restoring an unknown checkpoint is reported") {
		int restored = 0;
		int removed = 0;
		CHECK_FALSE(MCPCheckpoints::restore("does-not-exist", restored, removed, error));
		CHECK(error.contains("no checkpoint"));
	}

	SUBCASE("old checkpoints are pruned") {
		fixture.write("a.txt", "aaa");
		for (int i = 0; i < 4; i++) {
			MCPCheckpoints::create("Godot_WriteTextFile", "x", paths_of("res://a.txt"), error);
		}
		CHECK(MCPCheckpoints::list().size() >= 1);
		MCPCheckpoints::prune(2);
		CHECK(MCPCheckpoints::list().size() <= 2);
	}
}

TEST_CASE("[godot_ai] Mutating file tools declare what they will write") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	if (!registry->has_tool("Godot_WriteTextFile")) {
		mcp_register_project_tools();
	}

	// If a mutating tool stopped declaring its target, the protocol layer would
	// snapshot nothing and the user would lose their way back without being told.
	const Ref<MCPTool> tool = registry->get_tool("Godot_WriteTextFile");
	REQUIRE(tool.is_valid());
	CHECK(tool->is_mutating());

	Dictionary arguments;
	arguments["path"] = "res://notes.txt";
	const Vector<String> paths = tool->get_checkpoint_paths(arguments);
	REQUIRE(paths.size() == 1);
	CHECK(paths[0] == "res://notes.txt");

	// A read-only tool declares nothing and is never snapshotted.
	const Ref<MCPTool> reader = registry->get_tool("Godot_ReadTextFile");
	REQUIRE(reader.is_valid());
	CHECK_FALSE(reader->is_mutating());
	CHECK(reader->get_checkpoint_paths(arguments).is_empty());
}

TEST_CASE("[godot_ai] get_checkpoint_paths must not mutate the arguments it inspects") {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	if (!registry->has_tool("Godot_WriteTextFile")) {
		mcp_register_project_tools();
	}

	// Dictionary::operator[] inserts a null for a missing key even through a const
	// reference. get_checkpoint_paths() runs *before* schema validation, so a tool
	// that reads an optional argument without has() poisons the arguments and the
	// call is then rejected as wrongly typed. This caught exactly that bug.
	for (const String &name : registry->get_tool_names()) {
		const Ref<MCPTool> tool = registry->get_tool(name);
		if (tool.is_null() || !tool->is_mutating()) {
			continue;
		}
		Dictionary arguments;
		const int before = arguments.size();
		tool->get_checkpoint_paths(arguments);
		CHECK_MESSAGE(arguments.size() == before,
				vformat("%s::get_checkpoint_paths() added keys to the arguments", name).utf8().get_data());
	}
}

} // namespace TestMCPCheckpoints

#endif // TEST_MCP_CHECKPOINTS_H
