/**************************************************************************/
/*  test_mcp_skills.h                                                     */
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

#ifndef TEST_MCP_SKILLS_H
#define TEST_MCP_SKILLS_H

#include "modules/godot_ai/mcp_skills.h"
#include "modules/godot_ai/tests/test_mcp_fs_helpers.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/version.h"

#include "tests/test_macros.h"

namespace TestMCPSkills {

// Builds a skill tree under the cache directory and points discovery at it.
class SkillFixture {
	String root;

public:
	explicit SkillFixture(const String &p_suffix) {
		root = OS::get_singleton()->get_cache_path().path_join(
				"godot_ai_test_skills_" + p_suffix + "_" + itos(OS::get_singleton()->get_process_id()));
		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		dir->make_dir_recursive(root);

		Vector<String> roots;
		roots.push_back(root);
		MCPSkills::set_roots_override(roots);
		MCPSkills::set_allow_override(Vector<String>());
	}
	~SkillFixture() {
		MCPSkills::clear_roots_override();
		MCPSkills::clear_allow_override();
		mcp_test_remove_tree(root);
	}

	void add(const String &p_folder, const String &p_contents) const {
		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		dir->make_dir_recursive(root.path_join(p_folder));
		Ref<FileAccess> file = FileAccess::open(root.path_join(p_folder).path_join("SKILL.md"), FileAccess::WRITE);
		if (file.is_valid()) {
			file->store_string(p_contents);
		}
	}

	void add_resource(const String &p_folder, const String &p_relative, const String &p_contents) const {
		const String path = root.path_join(p_folder).path_join(p_relative);
		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		dir->make_dir_recursive(path.get_base_dir());
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
		if (file.is_valid()) {
			file->store_string(p_contents);
		}
	}

	void allow(const String &p_name) const {
		Vector<String> allowed;
		allowed.push_back(p_name);
		MCPSkills::set_allow_override(allowed);
	}
};

static const char *VALID_SKILL =
		"---\n"
		"name: scene-cleanup\n"
		"description: Tidy up the edited scene.\n"
		"enabled: true\n"
		"tools:\n"
		"  - Godot_GetEditedSceneTree\n"
		"  - Godot_ManageNode\n"
		"---\n"
		"\n"
		"You are a Godot scene-maintenance specialist.\n"
		"\n"
		"Read `references/naming.md` for conventions.\n";

TEST_CASE("[godot_ai] Skill frontmatter parsing") {
	MCPSkill skill;
	String body;
	String error;

	SUBCASE("a well-formed skill parses fully") {
		REQUIRE(MCPSkills::parse(VALID_SKILL, skill, body, error));
		CHECK(skill.name == "scene-cleanup");
		CHECK(skill.description == "Tidy up the edited scene.");
		CHECK(skill.enabled);
		REQUIRE(skill.tools.size() == 2);
		CHECK(skill.tools[0] == "Godot_GetEditedSceneTree");
		CHECK(skill.tools[1] == "Godot_ManageNode");
		// The body is the instruction text, with the frontmatter removed.
		CHECK(body.begins_with("You are a Godot scene-maintenance specialist."));
		CHECK_FALSE(body.contains("name: scene-cleanup"));
	}

	SUBCASE("a file without frontmatter is rejected") {
		CHECK_FALSE(MCPSkills::parse("just some markdown\n", skill, body, error));
		CHECK(error.contains("frontmatter"));
	}

	SUBCASE("an unterminated frontmatter block is rejected") {
		CHECK_FALSE(MCPSkills::parse("---\nname: broken\n", skill, body, error));
		CHECK(error.contains("never closed"));
	}

	SUBCASE("frontmatter without a name is rejected") {
		CHECK_FALSE(MCPSkills::parse("---\ndescription: nameless\n---\nbody\n", skill, body, error));
		CHECK(error.contains("name"));
	}

	SUBCASE("a malformed line is reported with its line number") {
		CHECK_FALSE(MCPSkills::parse("---\nname: x\nthis line is nonsense\n---\n", skill, body, error));
		CHECK(error.contains("line 3"));
	}

	SUBCASE("enabled: false is honoured") {
		REQUIRE(MCPSkills::parse("---\nname: off\nenabled: false\n---\nbody\n", skill, body, error));
		CHECK_FALSE(skill.enabled);
	}

	SUBCASE("quoted values keep their content") {
		REQUIRE(MCPSkills::parse("---\nname: \"quoted name\"\ndescription: 'single'\n---\n", skill, body, error));
		CHECK(skill.name == "quoted name");
		CHECK(skill.description == "single");
	}
}

TEST_CASE("[godot_ai] Skill editor-version gating") {
	CHECK(MCPSkills::version_satisfied("", 4, 3));
	CHECK(MCPSkills::version_satisfied(">=4.3", 4, 3));
	CHECK(MCPSkills::version_satisfied(">=4.2", 4, 3));
	CHECK_FALSE(MCPSkills::version_satisfied(">=4.6", 4, 3));
	CHECK_FALSE(MCPSkills::version_satisfied(">4.3", 4, 3));
	CHECK(MCPSkills::version_satisfied("<=4.3", 4, 3));
	CHECK(MCPSkills::version_satisfied("<5", 4, 3));
	CHECK(MCPSkills::version_satisfied("==4.3", 4, 3));
	CHECK_FALSE(MCPSkills::version_satisfied("==4.2", 4, 3));
	// A bare version means "at least this".
	CHECK(MCPSkills::version_satisfied("4.0", 4, 3));
	// An unreadable constraint must not silently pass.
	CHECK_FALSE(MCPSkills::version_satisfied("banana", 4, 3));
}

TEST_CASE("[godot_ai] Skill discovery") {
	SkillFixture fixture("discover");
	fixture.add("cleanup", VALID_SKILL);
	fixture.add("broken", "no frontmatter here\n");
	fixture.add("future", "---\nname: future-skill\nrequired_editor_version: \">=99.0\"\n---\nbody\n");

	const Vector<MCPSkill> skills = MCPSkills::discover();
	CHECK(skills.size() == 3);

	bool found_cleanup = false;
	bool found_broken = false;
	bool found_future = false;
	for (const MCPSkill &skill : skills) {
		if (skill.name == "scene-cleanup") {
			found_cleanup = true;
			CHECK(skill.problem.is_empty());
			// Discovered, but not trusted yet.
			CHECK_FALSE(skill.allowed);
			CHECK_FALSE(skill.is_usable());
		} else if (skill.name == "broken") {
			found_broken = true;
			// A broken skill is reported rather than hidden.
			CHECK_FALSE(skill.problem.is_empty());
		} else if (skill.name == "future-skill") {
			found_future = true;
			CHECK_FALSE(skill.version_supported);
			CHECK_FALSE(skill.is_usable());
		}
	}
	CHECK(found_cleanup);
	CHECK(found_broken);
	CHECK(found_future);
}

TEST_CASE("[godot_ai] A directory without SKILL.md is not a skill") {
	SkillFixture fixture("empty");
	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	fixture.add_resource("not_a_skill", "readme.md", "nothing here");
	CHECK(MCPSkills::discover().is_empty());
}

TEST_CASE("[godot_ai] Skills are untrusted until the user allows them") {
	SkillFixture fixture("trust");
	fixture.add("cleanup", VALID_SKILL);

	String body;
	String error;

	SUBCASE("instructions are refused while the skill is denied") {
		const MCPSkill skill = MCPSkills::discover()[0];
		CHECK_FALSE(MCPSkills::read_instructions(skill, body, error));
		CHECK(error.contains("has not been allowed"));
	}

	SUBCASE("allowing the skill makes it readable") {
		fixture.allow("scene-cleanup");
		const MCPSkill skill = MCPSkills::discover()[0];
		CHECK(skill.allowed);
		CHECK(skill.is_usable());
		REQUIRE(MCPSkills::read_instructions(skill, body, error));
		CHECK(body.begins_with("You are a Godot scene-maintenance specialist."));
	}

	SUBCASE("a disabled skill stays unusable even when allowed") {
		fixture.add("disabled", "---\nname: disabled-skill\nenabled: false\n---\nbody\n");
		Vector<String> allowed;
		allowed.push_back("disabled-skill");
		MCPSkills::set_allow_override(allowed);

		for (const MCPSkill &skill : MCPSkills::discover()) {
			if (skill.name == "disabled-skill") {
				CHECK_FALSE(skill.is_usable());
				CHECK_FALSE(MCPSkills::read_instructions(skill, body, error));
				CHECK(error.contains("disabled"));
			}
		}
	}
}

TEST_CASE("[godot_ai] Supporting resources load on demand and stay in the skill folder") {
	SkillFixture fixture("resources");
	fixture.add("cleanup", VALID_SKILL);
	fixture.add_resource("cleanup", "references/naming.md", "use PascalCase for nodes");
	fixture.allow("scene-cleanup");

	const MCPSkill skill = MCPSkills::discover()[0];
	String contents;
	String error;

	SUBCASE("a sibling resource is readable") {
		REQUIRE(MCPSkills::read_resource(skill, "references/naming.md", contents, error));
		CHECK(contents == "use PascalCase for nodes");
	}

	SUBCASE("a missing resource is reported") {
		CHECK_FALSE(MCPSkills::read_resource(skill, "references/missing.md", contents, error));
		CHECK(error.contains("no resource"));
	}

	SUBCASE("traversal out of the skill folder is refused") {
		CHECK_FALSE(MCPSkills::read_resource(skill, "../../../etc/passwd", contents, error));
		CHECK(error.contains("outside the skill"));
	}

	SUBCASE("resources of a denied skill are refused") {
		MCPSkills::set_allow_override(Vector<String>());
		const MCPSkill denied = MCPSkills::discover()[0];
		CHECK_FALSE(MCPSkills::read_resource(denied, "references/naming.md", contents, error));
		CHECK(error.contains("not been allowed"));
	}
}

TEST_CASE("[godot_ai] Duplicate skill names keep the first and flag the rest") {
	SkillFixture fixture("duplicate");
	fixture.add("a_first", VALID_SKILL);
	fixture.add("b_second", VALID_SKILL);

	const Vector<MCPSkill> skills = MCPSkills::discover();
	REQUIRE(skills.size() == 2);
	int flagged = 0;
	for (const MCPSkill &skill : skills) {
		if (!skill.problem.is_empty()) {
			flagged++;
			CHECK(skill.problem.contains("already found"));
		}
	}
	// Exactly one of the two is usable; the other cannot silently shadow it.
	CHECK(flagged == 1);
}

TEST_CASE("[godot_ai] Skill status decides what the approvals UI offers") {
	MCPSkill skill;
	skill.name = "example";
	skill.root_kind = "project";
	bool can_toggle = false;
	bool needs_decision = false;

	SUBCASE("a usable but unallowed skill is the one that needs a decision") {
		skill.allowed = false;
		const String status = mcp_skill_status_text(skill, can_toggle, needs_decision);
		CHECK(status.contains("Not allowed"));
		CHECK(status.contains("project"));
		CHECK(can_toggle);
		CHECK(needs_decision);
	}

	SUBCASE("an allowed skill can be revoked but needs no decision") {
		skill.allowed = true;
		const String status = mcp_skill_status_text(skill, can_toggle, needs_decision);
		CHECK(status.contains("Allowed"));
		CHECK(can_toggle);
		CHECK_FALSE(needs_decision);
	}

	SUBCASE("a broken skill explains itself and offers no button") {
		skill.problem = "frontmatter is missing a 'name'";
		const String status = mcp_skill_status_text(skill, can_toggle, needs_decision);
		CHECK(status == "frontmatter is missing a 'name'");
		// Offering "Allow" for something that cannot load would be a lie.
		CHECK_FALSE(can_toggle);
		CHECK_FALSE(needs_decision);
	}

	SUBCASE("a self-disabled skill offers no button") {
		skill.enabled = false;
		const String status = mcp_skill_status_text(skill, can_toggle, needs_decision);
		CHECK(status == "Disabled by the skill itself");
		CHECK_FALSE(can_toggle);
		CHECK_FALSE(needs_decision);
	}

	SUBCASE("a version-gated skill says which version it wants") {
		skill.version_supported = false;
		skill.required_editor_version = ">=99.0";
		const String status = mcp_skill_status_text(skill, can_toggle, needs_decision);
		CHECK(status.contains(">=99.0"));
		CHECK_FALSE(can_toggle);
	}
}

} // namespace TestMCPSkills

#endif // TEST_MCP_SKILLS_H
