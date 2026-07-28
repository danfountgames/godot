/**************************************************************************/
/*  test_mcp_paths.h                                                      */
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

#ifndef TEST_MCP_PATHS_H
#define TEST_MCP_PATHS_H

#include "modules/godot_ai/mcp_paths.h"
#include "modules/godot_ai/mcp_permissions.h"
#include "modules/godot_ai/tests/test_mcp_fs_helpers.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

#include "tests/test_macros.h"

namespace TestMCPPaths {

// Builds a throwaway project directory under the cache path and points the path
// boundary at it, so the confinement rules are exercised against a real filesystem.
// Cleanup goes through mcp_test_remove_tree(), never a CWD-relative recursive erase.
class ProjectFixture {
	String root;

public:
	explicit ProjectFixture(const String &p_suffix) {
		root = OS::get_singleton()->get_cache_path().path_join(
				"godot_ai_test_" + p_suffix + "_" + itos(OS::get_singleton()->get_process_id()));
		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		dir->make_dir_recursive(root.path_join("scenes"));
		MCPPaths::set_project_root_override(root);
	}
	~ProjectFixture() {
		MCPPaths::clear_project_root_override();
		mcp_test_remove_tree(root);
	}

	String get_root() const { return root; }

	void write(const String &p_relative, const String &p_contents) const {
		Ref<FileAccess> file = FileAccess::open(root.path_join(p_relative), FileAccess::WRITE);
		if (file.is_valid()) {
			file->store_string(p_contents);
		}
	}
};

TEST_CASE("[godot_ai] Project-relative paths resolve inside the project") {
	ProjectFixture fixture("resolve");
	fixture.write("scenes/main.tscn", "[gd_scene]");

	MCPPaths::Resolved resolved;
	String error;

	SUBCASE("res:// paths resolve") {
		REQUIRE(MCPPaths::resolve("res://scenes/main.tscn", resolved, error));
		CHECK(resolved.res_path == "res://scenes/main.tscn");
		CHECK(resolved.exists);
		CHECK_FALSE(resolved.is_directory);
	}

	SUBCASE("bare relative paths resolve") {
		REQUIRE(MCPPaths::resolve("scenes/main.tscn", resolved, error));
		CHECK(resolved.res_path == "res://scenes/main.tscn");
	}

	SUBCASE("directories are reported as such") {
		REQUIRE(MCPPaths::resolve("res://scenes", resolved, error));
		CHECK(resolved.is_directory);
	}

	SUBCASE("a missing file resolves but is marked absent") {
		REQUIRE(MCPPaths::resolve("res://scenes/missing.tscn", resolved, error));
		CHECK_FALSE(resolved.exists);
		CHECK_FALSE(MCPPaths::resolve_existing("res://scenes/missing.tscn", resolved, error));
		CHECK(error.contains("does not exist"));
	}
}

TEST_CASE("[godot_ai] Paths outside the project are refused") {
	ProjectFixture fixture("escape");
	MCPPaths::Resolved resolved;
	String error;

	SUBCASE("parent traversal is refused") {
		CHECK_FALSE(MCPPaths::resolve("res://../../etc/passwd", resolved, error));
		CHECK(error.contains("outside the project"));
	}

	SUBCASE("traversal hidden mid-path is refused") {
		CHECK_FALSE(MCPPaths::resolve("scenes/../../outside.txt", resolved, error));
	}

	SUBCASE("absolute paths outside the project are refused") {
		CHECK_FALSE(MCPPaths::resolve("/etc/passwd", resolved, error));
	}

	SUBCASE("other schemes are refused") {
		CHECK_FALSE(MCPPaths::resolve("user://settings.cfg", resolved, error));
		CHECK(error.contains("scheme"));
	}

	SUBCASE("an empty path is refused") {
		CHECK_FALSE(MCPPaths::resolve("   ", resolved, error));
	}
}

TEST_CASE("[godot_ai] Symbolic links cannot escape the project") {
	ProjectFixture fixture("symlink");

	// A link inside the project pointing at a directory outside it.
	const String outside = OS::get_singleton()->get_cache_path().path_join(
			"godot_ai_outside_" + itos(OS::get_singleton()->get_process_id()));
	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	REQUIRE(dir.is_valid());
	dir->make_dir_recursive(outside);
	{
		Ref<FileAccess> file = FileAccess::open(outside.path_join("secret.txt"), FileAccess::WRITE);
		if (file.is_valid()) {
			file->store_string("secret");
		}
	}

	const String link = fixture.get_root().path_join("escape_link");
	if (dir->create_link(outside, link) != OK) {
		// Filesystems without symlink support cannot exercise this rule.
		WARN("Skipping symlink escape check: this filesystem does not support links.");
	} else {
		MCPPaths::Resolved resolved;
		String error;
		CHECK_FALSE(MCPPaths::resolve("res://escape_link/secret.txt", resolved, error));
		CHECK(error.contains("symbolic link"));

		// A link that stays inside the project is still usable.
		const String inside_link = fixture.get_root().path_join("inside_link");
		if (dir->create_link(fixture.get_root().path_join("scenes"), inside_link) == OK) {
			CHECK(MCPPaths::resolve("res://inside_link", resolved, error));
		}
	}

	mcp_test_remove_tree(outside);
}

TEST_CASE("[godot_ai] Path resolution needs an open project") {
	MCPPaths::clear_project_root_override();
	MCPPaths::Resolved resolved;
	String error;
	// The doctest binary has no project open, so this is the real no-project path.
	if (MCPPaths::get_project_root().is_empty()) {
		CHECK_FALSE(MCPPaths::resolve("res://anything.tscn", resolved, error));
		CHECK(error.contains("no Godot project"));
	}
}

TEST_CASE("[godot_ai] The guarded test delete refuses paths outside its scratch area") {
	// This guard exists because an earlier fixture deleted the repository; keep it
	// covered so the protection cannot regress unnoticed.
	ERR_PRINT_OFF;
	CHECK_FALSE(mcp_test_remove_tree("/"));
	CHECK_FALSE(mcp_test_remove_tree("/etc"));
	CHECK_FALSE(mcp_test_remove_tree(OS::get_singleton()->get_cache_path().path_join("not_a_test_dir")));
	CHECK_FALSE(mcp_test_remove_tree("relative/godot_ai_test_x"));
	ERR_PRINT_ON;
}

TEST_CASE("[godot_ai] Capability policy defaults follow the security model") {
	MCPPermissions::clear_policy_overrides();

	CHECK(MCPPermissions::get_default_policy(MCP_CAP_READ_PROJECT) == MCP_POLICY_ALLOW);
	CHECK(MCPPermissions::get_default_policy(MCP_CAP_READ_RUNTIME) == MCP_POLICY_ALLOW);
	CHECK(MCPPermissions::get_default_policy(MCP_CAP_EDIT_FILES) == MCP_POLICY_ASK);
	CHECK(MCPPermissions::get_default_policy(MCP_CAP_EDIT_SCENE) == MCP_POLICY_ASK);
	CHECK(MCPPermissions::get_default_policy(MCP_CAP_RUN_PROJECT) == MCP_POLICY_ASK);
	CHECK(MCPPermissions::get_default_policy(MCP_CAP_DANGEROUS_EXEC) == MCP_POLICY_DENY);

	CHECK(mcp_capability_is_mutating(MCP_CAP_EDIT_FILES));
	CHECK(mcp_capability_is_mutating(MCP_CAP_EDIT_SCENE));
	CHECK_FALSE(mcp_capability_is_mutating(MCP_CAP_READ_PROJECT));
	// Running the game changes nothing persistent.
	CHECK_FALSE(mcp_capability_is_mutating(MCP_CAP_RUN_PROJECT));
}

TEST_CASE("[godot_ai] Permission evaluation") {
	MCPPermissions::clear_policy_overrides();
	MCPSession session;
	session.client_approved = true;

	SUBCASE("an unapproved client is denied everything") {
		MCPSession unapproved;
		unapproved.client_approved = false;
		const MCPPermissions::Decision decision = MCPPermissions::evaluate(unapproved, MCP_CAP_READ_PROJECT, "Godot_ListScenes");
		CHECK(decision.outcome == MCPPermissions::OUTCOME_DENY);
		CHECK(decision.reason.contains("not been approved"));
	}

	SUBCASE("dangerous_exec is denied even when a client asks to auto-approve") {
		session.approval_mode = MCP_POLICY_ALLOW;
		MCPPermissions::set_policy_override(MCP_CAP_DANGEROUS_EXEC, MCP_POLICY_ALLOW);
		CHECK(MCPPermissions::evaluate(session, MCP_CAP_DANGEROUS_EXEC, "Anything").outcome == MCPPermissions::OUTCOME_DENY);
	}

	SUBCASE("approval mode allow upgrades ask to allow") {
		session.approval_mode = MCP_POLICY_ALLOW;
		MCPPermissions::set_policy_override(MCP_CAP_EDIT_FILES, MCP_POLICY_ASK);
		CHECK(MCPPermissions::evaluate(session, MCP_CAP_EDIT_FILES, "Godot_WriteTextFile").outcome == MCPPermissions::OUTCOME_ALLOW);
	}

	SUBCASE("approval mode allow cannot override a deny policy") {
		session.approval_mode = MCP_POLICY_ALLOW;
		MCPPermissions::set_policy_override(MCP_CAP_EDIT_FILES, MCP_POLICY_DENY);
		CHECK(MCPPermissions::evaluate(session, MCP_CAP_EDIT_FILES, "Godot_WriteTextFile").outcome == MCPPermissions::OUTCOME_DENY);
	}

	SUBCASE("approval mode deny turns ask into deny") {
		session.approval_mode = MCP_POLICY_DENY;
		MCPPermissions::set_policy_override(MCP_CAP_RUN_PROJECT, MCP_POLICY_ASK);
		CHECK(MCPPermissions::evaluate(session, MCP_CAP_RUN_PROJECT, "Godot_PlayCurrentScene").outcome == MCPPermissions::OUTCOME_DENY);
	}

	SUBCASE("read-only sessions may still read and run") {
		session.read_only = true;
		MCPPermissions::set_policy_override(MCP_CAP_READ_PROJECT, MCP_POLICY_ALLOW);
		MCPPermissions::set_policy_override(MCP_CAP_RUN_PROJECT, MCP_POLICY_ALLOW);
		CHECK(MCPPermissions::evaluate(session, MCP_CAP_READ_PROJECT, "Godot_ListScenes").outcome == MCPPermissions::OUTCOME_ALLOW);
		CHECK(MCPPermissions::evaluate(session, MCP_CAP_RUN_PROJECT, "Godot_PlayCurrentScene").outcome == MCPPermissions::OUTCOME_ALLOW);
	}

	SUBCASE("read-only sessions never mutate") {
		session.read_only = true;
		session.approval_mode = MCP_POLICY_ALLOW;
		MCPPermissions::set_policy_override(MCP_CAP_EDIT_SCENE, MCP_POLICY_ALLOW);
		const MCPPermissions::Decision decision = MCPPermissions::evaluate(session, MCP_CAP_EDIT_SCENE, "Godot_SaveScene");
		CHECK(decision.outcome == MCPPermissions::OUTCOME_DENY);
		CHECK(decision.reason.contains("read-only"));
	}

	MCPPermissions::clear_policy_overrides();
}

} // namespace TestMCPPaths

#endif // TEST_MCP_PATHS_H
