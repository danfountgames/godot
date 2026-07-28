/**************************************************************************/
/*  test_mcp_audit.h                                                      */
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

#ifndef TEST_MCP_AUDIT_H
#define TEST_MCP_AUDIT_H

#include "modules/godot_ai/mcp_audit.h"
#include "modules/godot_ai/mcp_service.h"
#include "modules/godot_ai/tests/test_mcp_fs_helpers.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"

#include "tests/test_macros.h"

namespace TestMCPAudit {

class AuditFixture {
	String directory;
	String log_path;

public:
	explicit AuditFixture(const String &p_suffix) {
		directory = OS::get_singleton()->get_cache_path().path_join(
				"godot_ai_test_audit_" + p_suffix + "_" + itos(OS::get_singleton()->get_process_id()));
		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		dir->make_dir_recursive(directory);
		log_path = directory.path_join("audit.log");
		MCPAudit::set_log_path_override(log_path);
	}
	~AuditFixture() {
		MCPAudit::clear_log_path_override();
		mcp_test_remove_tree(directory);
	}

	String read() const { return FileAccess::get_file_as_string(log_path); }
	bool exists() const { return FileAccess::exists(log_path); }

	Array entries() const {
		Array out;
		for (const String &line : read().split("\n", false)) {
			const Variant parsed = JSON::parse_string(line);
			if (parsed.get_type() == Variant::DICTIONARY) {
				out.push_back(parsed);
			}
		}
		return out;
	}
};

TEST_CASE("[godot_ai] The audit log records allowed and refused calls alike") {
	AuditFixture fixture("record");

	MCPAudit::record("claude-code", "Godot_WriteTextFile",
			"Godot_WriteTextFile(path=\"res://a.txt\")", true, "ok");
	MCPAudit::record("claude-code", "Godot_ManageNode",
			"Godot_ManageNode(action=\"delete\")", false, "the user declined");

	REQUIRE(fixture.exists());
	const Array entries = fixture.entries();
	REQUIRE(entries.size() == 2);

	const Dictionary allowed = entries[0];
	CHECK(String(allowed["client"]) == "claude-code");
	CHECK(String(allowed["tool"]) == "Godot_WriteTextFile");
	CHECK((bool)allowed["allowed"]);
	CHECK(String(allowed["invocation"]).contains("res://a.txt"));
	CHECK_FALSE(String(allowed["time"]).is_empty());

	// A refusal is the more interesting half of an audit trail: it is the record of
	// what an agent tried to do and was stopped from doing.
	const Dictionary refused = entries[1];
	CHECK_FALSE((bool)refused["allowed"]);
	CHECK(String(refused["reason"]) == "the user declined");
}

TEST_CASE("[godot_ai] The audit log appends rather than overwriting") {
	AuditFixture fixture("append");

	for (int i = 0; i < 5; i++) {
		MCPAudit::record("client", "Godot_ListScenes", vformat("call %d", i), true, "ok");
	}
	// Losing earlier entries would make the trail useless for exactly the case it
	// exists for: reviewing what happened while you were not watching.
	CHECK(fixture.entries().size() == 5);
	CHECK(fixture.read().contains("call 0"));
	CHECK(fixture.read().contains("call 4"));
}

TEST_CASE("[godot_ai] Audit entries stay one JSON object per line") {
	AuditFixture fixture("lines");

	// A multi-line argument must not break the line-per-entry format.
	MCPAudit::record("client", "Godot_WriteTextFile",
			"Godot_WriteTextFile(text=\"first\nsecond\")", true, "ok");
	const PackedStringArray lines = fixture.read().split("\n", false);
	CHECK(lines.size() == 1);
	CHECK(fixture.entries().size() == 1);
}

TEST_CASE("[godot_ai] Client approval is denied by default and opt-in for automation") {
	// Without editor settings and without the automation flag, an unknown client is
	// refused: the deny-by-default rule cannot depend on a UI being present.
	const String previous = OS::get_singleton()->get_environment("GODOT_AI_AUTO_APPROVE");
	OS::get_singleton()->set_environment("GODOT_AI_AUTO_APPROVE", "");
	CHECK_FALSE(MCPService::is_client_approved("some-client"));

	OS::get_singleton()->set_environment("GODOT_AI_AUTO_APPROVE", "1");
	CHECK(MCPService::is_client_approved("some-client"));

	// Only the exact opt-in counts; a stray value must not open the door.
	OS::get_singleton()->set_environment("GODOT_AI_AUTO_APPROVE", "yes");
	CHECK_FALSE(MCPService::is_client_approved("some-client"));

	OS::get_singleton()->set_environment("GODOT_AI_AUTO_APPROVE", previous);
}

} // namespace TestMCPAudit

#endif // TEST_MCP_AUDIT_H
