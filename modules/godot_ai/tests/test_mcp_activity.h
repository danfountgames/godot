/**************************************************************************/
/*  test_mcp_activity.h                                                   */
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

#ifndef TEST_MCP_ACTIVITY_H
#define TEST_MCP_ACTIVITY_H

#include "modules/godot_ai/mcp_activity.h"

#include "core/variant/array.h"
#include "core/variant/dictionary.h"

#include "tests/test_macros.h"

namespace TestMCPActivity {

// The buffer is process-wide, so every case starts from a known state and puts the
// capacity back afterwards. Sequence numbers deliberately survive clear(), so cases
// must not assert on absolute sequence values across a reset.
struct ActivityFixture {
	int previous_capacity = 0;

	ActivityFixture() {
		previous_capacity = MCPActivity::get_capacity();
		MCPActivity::clear();
	}
	~ActivityFixture() {
		MCPActivity::clear();
		MCPActivity::set_capacity(previous_capacity);
	}
};

TEST_CASE("[godot_ai] Activity opens a record as running and closes it with a duration") {
	ActivityFixture fixture;

	const MCPActivity::Id id = MCPActivity::begin(
			"test-client", "Godot_ReadTextFile", MCP_CAP_READ_PROJECT,
			"Godot_ReadTextFile(path=\"res://main.gd\")", Array());
	CHECK(id != MCPActivity::INVALID_ID);
	CHECK(MCPActivity::has_running());

	Array records = MCPActivity::snapshot();
	REQUIRE(records.size() == 1);
	Dictionary running = records[0];
	CHECK(String(running["outcome"]) == "running");
	CHECK(String(running["client"]) == "test-client");
	CHECK(String(running["tool"]) == "Godot_ReadTextFile");
	CHECK(String(running["capability"]) == "read_project");
	// The tick stamp is bookkeeping for the duration; it must not reach a client.
	CHECK_FALSE(running.has("_started_ticks"));

	MCPActivity::finish(id, "ok", String(), "chk-1");
	CHECK_FALSE(MCPActivity::has_running());

	records = MCPActivity::snapshot();
	REQUIRE(records.size() == 1);
	Dictionary done = records[0];
	CHECK(String(done["outcome"]) == "ok");
	CHECK(String(done["checkpoint"]) == "chk-1");
	CHECK((int64_t)done["duration_ms"] >= 0);
}

TEST_CASE("[godot_ai] Activity records a refusal as a complete record, not a running one") {
	ActivityFixture fixture;

	MCPActivity::refuse("test-client", "Godot_DeleteProjectFile", MCP_CAP_EDIT_FILES,
			"Godot_DeleteProjectFile(path=\"res://main.gd\")", Array(),
			"'edit_files' is denied for this session");

	CHECK_FALSE(MCPActivity::has_running());
	const Array records = MCPActivity::snapshot();
	REQUIRE(records.size() == 1);
	const Dictionary record = records[0];
	CHECK(String(record["outcome"]) == "refused");
	CHECK(String(record["detail"]).contains("denied"));
}

TEST_CASE("[godot_ai] Activity polling by sequence returns only what is new") {
	ActivityFixture fixture;

	MCPActivity::finish(MCPActivity::begin("c", "Godot_A", MCP_CAP_READ_PROJECT, "a", Array()),
			"ok", String(), String());
	const int64_t seen = MCPActivity::get_latest_sequence();
	MCPActivity::finish(MCPActivity::begin("c", "Godot_B", MCP_CAP_READ_PROJECT, "b", Array()),
			"ok", String(), String());

	const Array fresh = MCPActivity::snapshot(seen);
	REQUIRE(fresh.size() == 1);
	const Dictionary record = fresh[0];
	CHECK(String(record["tool"]) == "Godot_B");

	// Polling with the newest sequence returns nothing rather than repeating the tail.
	CHECK(MCPActivity::snapshot(MCPActivity::get_latest_sequence()).is_empty());
}

TEST_CASE("[godot_ai] Activity drops the oldest records past its capacity") {
	ActivityFixture fixture;
	MCPActivity::set_capacity(3);

	for (int i = 0; i < 5; i++) {
		MCPActivity::finish(
				MCPActivity::begin("c", "Godot_Tool" + itos(i), MCP_CAP_READ_PROJECT, "s", Array()),
				"ok", String(), String());
	}

	const Array records = MCPActivity::snapshot();
	REQUIRE(records.size() == 3);
	const Dictionary oldest = records[0];
	const Dictionary newest = records[2];
	CHECK(String(oldest["tool"]) == "Godot_Tool2");
	CHECK(String(newest["tool"]) == "Godot_Tool4");
}

TEST_CASE("[godot_ai] Activity finishing an evicted record does not resurrect it") {
	ActivityFixture fixture;
	MCPActivity::set_capacity(2);

	const MCPActivity::Id first = MCPActivity::begin("c", "Godot_First", MCP_CAP_READ_PROJECT, "s", Array());
	MCPActivity::begin("c", "Godot_Second", MCP_CAP_READ_PROJECT, "s", Array());
	MCPActivity::begin("c", "Godot_Third", MCP_CAP_READ_PROJECT, "s", Array());

	// `first` has been pushed out of the buffer. Closing it must be a no-op rather than
	// appending a finished record after two newer ones.
	MCPActivity::finish(first, "ok", String(), String());

	const Array records = MCPActivity::snapshot();
	REQUIRE(records.size() == 2);
	const Dictionary a = records[0];
	const Dictionary b = records[1];
	CHECK(String(a["tool"]) == "Godot_Second");
	CHECK(String(b["tool"]) == "Godot_Third");
}

TEST_CASE("[godot_ai] Activity clear keeps sequence numbers moving forward") {
	ActivityFixture fixture;

	MCPActivity::finish(MCPActivity::begin("c", "Godot_A", MCP_CAP_READ_PROJECT, "a", Array()),
			"ok", String(), String());
	const int64_t before = MCPActivity::get_latest_sequence();
	MCPActivity::clear();
	CHECK(MCPActivity::get_latest_sequence() == 0);

	MCPActivity::finish(MCPActivity::begin("c", "Godot_B", MCP_CAP_READ_PROJECT, "b", Array()),
			"ok", String(), String());
	// A poller holding `before` must not be handed a *different* record with that number.
	CHECK(MCPActivity::get_latest_sequence() > before);
}

TEST_CASE("[godot_ai] Activity extracts file and node subjects from arguments") {
	Dictionary arguments;
	arguments["path"] = "res://scenes/main.tscn";
	arguments["node_path"] = "Main/Player/Sprite";
	arguments["text"] = "some file contents that are not a path at all";
	arguments["count"] = 3;

	const Array subjects = MCPActivity::extract_subjects(arguments);
	REQUIRE(subjects.size() == 2);

	bool saw_file = false;
	bool saw_node = false;
	for (int i = 0; i < subjects.size(); i++) {
		const Dictionary subject = subjects[i];
		if (String(subject["kind"]) == "file" && String(subject["path"]) == "res://scenes/main.tscn") {
			saw_file = true;
		}
		if (String(subject["kind"]) == "node" && String(subject["path"]) == "Main/Player/Sprite") {
			saw_node = true;
		}
	}
	CHECK(saw_file);
	CHECK(saw_node);
}

TEST_CASE("[godot_ai] Activity subject extraction does not mutate the arguments it reads") {
	// Dictionary::operator[] inserts a null for a missing key even through a const
	// reference, which then fails schema validation. Every reader in this module has to
	// use get(); this pins it for the extractor.
	Dictionary arguments;
	arguments["path"] = "res://main.gd";
	const int before = arguments.size();

	MCPActivity::extract_subjects(arguments);

	CHECK(arguments.size() == before);
}

TEST_CASE("[godot_ai] Activity does not mistake prose for a node path") {
	Dictionary arguments;
	// A "target" that is a sentence, which the node-path keys would otherwise accept.
	arguments["target"] = "the enemy that spawns after the first wave";
	arguments["parent"] = "Main/Enemies";

	const Array subjects = MCPActivity::extract_subjects(arguments);
	REQUIRE(subjects.size() == 1);
	const Dictionary subject = subjects[0];
	CHECK(String(subject["path"]) == "Main/Enemies");
}

TEST_CASE("[godot_ai] Activity reports the same path under two keys once") {
	Dictionary arguments;
	arguments["from"] = "Main/Player";
	arguments["to"] = "Main/Player";

	const Array subjects = MCPActivity::extract_subjects(arguments);
	CHECK(subjects.size() == 1);
}

TEST_CASE("[godot_ai] A one-line summary prefers the intention over the tool name") {
	// The terminal panel shows this because it hides the Activity dock by sharing the
	// bottom strip with it. A user reading one line wants what the agent is trying to
	// do, not the name of whichever primitive that decomposed into.
	Dictionary record;
	record["tool"] = "Godot_ManageNode";
	record["summary"] = "create Node2D under /root";
	record["intent"] = "moving the spawn point";
	record["outcome"] = "running";
	CHECK(MCPActivity::describe_record(record) == String::utf8("moving the spawn point…"));

	record.erase("intent");
	CHECK(MCPActivity::describe_record(record) == String::utf8("create Node2D under /root…"));

	record.erase("summary");
	CHECK(MCPActivity::describe_record(record) == String::utf8("Godot_ManageNode…"));
}

TEST_CASE("[godot_ai] The summary says which way a record ended") {
	Dictionary record;
	record["intent"] = "renaming the enemy";

	record["outcome"] = "ok";
	CHECK(MCPActivity::describe_record(record) == "renaming the enemy");

	record["outcome"] = "refused";
	CHECK(MCPActivity::describe_record(record) == "Refused: renaming the enemy");

	record["outcome"] = "deferred";
	CHECK(MCPActivity::describe_record(record) == "Waiting: renaming the enemy");

	// A failure without a reason is still a failure worth showing; with one, the
	// reason is the useful half.
	record["outcome"] = "failed";
	CHECK(MCPActivity::describe_record(record) == "Failed: renaming the enemy");
	record["detail"] = "no node at that path";
	CHECK(MCPActivity::describe_record(record) == "Failed: renaming the enemy - no node at that path");
}

TEST_CASE("[godot_ai] A record with nothing to say reads as idle") {
	CHECK(MCPActivity::describe_record(Dictionary()) == "Idle.");

	// Not the same as an empty record, and it must not render as a bare ellipsis.
	Dictionary blank;
	blank["outcome"] = "running";
	blank["tool"] = "   ";
	CHECK(MCPActivity::describe_record(blank) == "Idle.");
}

} // namespace TestMCPActivity

#endif // TEST_MCP_ACTIVITY_H
