/**************************************************************************/
/*  test_mcp_sessions.h                                                   */
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

#ifndef TEST_MCP_SESSIONS_H
#define TEST_MCP_SESSIONS_H

#include "modules/godot_ai/mcp_sessions.h"
#include "modules/godot_ai/tests/test_mcp_fs_helpers.h"

#include "core/io/dir_access.h"
#include "core/os/os.h"

#include "tests/test_macros.h"

namespace TestMCPSessions {

// Scratch root under the cache directory, with the godot_ai_test_ marker that
// mcp_test_remove_tree() insists on. Never under the project or the working directory:
// a fixture that got that wrong once erased this repository (DEC-0006).
class SessionFixture {
	String root;

public:
	explicit SessionFixture(const String &p_suffix) {
		root = OS::get_singleton()->get_cache_path().path_join(
				"godot_ai_test_sessions_" + p_suffix + "_" + itos(OS::get_singleton()->get_process_id()));
		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		dir->make_dir_recursive(root);
		MCPSessions::set_root_override(root);
	}
	~SessionFixture() {
		MCPSessions::clear_root_override();
		mcp_test_remove_tree(root);
	}
};

static Dictionary make_event(int64_t p_frame, const String &p_kind) {
	Dictionary event;
	event["frame"] = p_frame;
	event["kind"] = p_kind;
	return event;
}

TEST_CASE("[godot_ai] Sessions record a trace and read it back in order") {
	SessionFixture fixture("roundtrip");

	CHECK(MCPSessions::begin("jump-over-gap", "jump over gap", 100, Dictionary()).ok);
	CHECK(MCPSessions::exists("jump-over-gap"));

	Array events;
	events.push_back(make_event(101, "key"));
	events.push_back(make_event(104, "pointer"));
	CHECK(MCPSessions::append_events("jump-over-gap", events).ok);

	Array assertions;
	Dictionary assertion;
	assertion["node_path"] = "Main/Player";
	assertion["property"] = "position";
	assertion["value"] = Vector2(10, 20);
	assertion["frame"] = 104;
	assertions.push_back(assertion);
	CHECK(MCPSessions::append_assertions("jump-over-gap", assertions).ok);

	CHECK(MCPSessions::finish("jump-over-gap", 130, "recorded", Dictionary()).ok);

	const Array read_back = MCPSessions::read_events("jump-over-gap");
	REQUIRE(read_back.size() == 2);
	CHECK((int64_t)Dictionary(read_back[0])["frame"] == 101);
	CHECK(String(Dictionary(read_back[1])["kind"]) == "pointer");

	CHECK(MCPSessions::read_assertions("jump-over-gap").size() == 1);

	const Dictionary meta = MCPSessions::read_meta("jump-over-gap");
	CHECK(String(meta["verdict"]) == "recorded");
	CHECK((int64_t)meta["event_count"] == 2);
	CHECK((int64_t)meta["assertion_count"] == 1);
	CHECK((int64_t)meta["frame_span"] == 30);
	// The trace must say what it is, so nothing downstream infers a human played it.
	CHECK(String(meta["input_source"]) == "editor_injected");
}

TEST_CASE("[godot_ai] Sessions refuse an event with no frame") {
	SessionFixture fixture("noframe");
	REQUIRE(MCPSessions::begin("s", "s", 0, Dictionary()).ok);

	Array events;
	Dictionary event;
	event["kind"] = "key";
	events.push_back(event);

	// A trace indexed by nothing cannot be replayed, so this is refused at write time
	// rather than discovered at replay time.
	const MCPSessions::Result result = MCPSessions::append_events("s", events);
	CHECK_FALSE(result.ok);
	CHECK(result.error.contains("frame"));
	CHECK(MCPSessions::read_events("s").is_empty());
}

TEST_CASE("[godot_ai] Sessions refuse an assertion with no target") {
	SessionFixture fixture("noassert");
	REQUIRE(MCPSessions::begin("s", "s", 0, Dictionary()).ok);

	Array assertions;
	Dictionary missing_property;
	missing_property["node_path"] = "Main/Player";
	assertions.push_back(missing_property);
	CHECK_FALSE(MCPSessions::append_assertions("s", assertions).ok);

	Array no_node;
	Dictionary missing_node;
	missing_node["property"] = "health";
	no_node.push_back(missing_node);
	CHECK_FALSE(MCPSessions::append_assertions("s", no_node).ok);
}

TEST_CASE("[godot_ai] Sessions appending to a session that was never started fails") {
	SessionFixture fixture("nostart");
	Array events;
	events.push_back(make_event(1, "key"));
	const MCPSessions::Result result = MCPSessions::append_events("ghost", events);
	CHECK_FALSE(result.ok);
	CHECK(result.error.contains("ghost"));
}

TEST_CASE("[godot_ai] Sessions re-recording truncates the previous trace") {
	SessionFixture fixture("rerecord");
	REQUIRE(MCPSessions::begin("run", "run", 0, Dictionary()).ok);
	Array first;
	first.push_back(make_event(1, "key"));
	first.push_back(make_event(2, "key"));
	REQUIRE(MCPSessions::append_events("run", first).ok);
	REQUIRE(MCPSessions::read_events("run").size() == 2);

	// Re-recording is how a regression test is updated. Half an old trace mixed into a
	// new one would be worse than either.
	REQUIRE(MCPSessions::begin("run", "run", 500, Dictionary()).ok);
	CHECK(MCPSessions::read_events("run").is_empty());

	Array second;
	second.push_back(make_event(501, "pointer"));
	REQUIRE(MCPSessions::append_events("run", second).ok);
	const Array events = MCPSessions::read_events("run");
	REQUIRE(events.size() == 1);
	CHECK((int64_t)Dictionary(events[0])["frame"] == 501);
}

TEST_CASE("[godot_ai] Sessions list what has been recorded") {
	SessionFixture fixture("list");
	CHECK(MCPSessions::list().is_empty());

	REQUIRE(MCPSessions::begin("alpha", "alpha", 0, Dictionary()).ok);
	Array events;
	events.push_back(make_event(1, "key"));
	REQUIRE(MCPSessions::append_events("alpha", events).ok);
	REQUIRE(MCPSessions::finish("alpha", 10, "passed", Dictionary()).ok);

	const Array listed = MCPSessions::list();
	REQUIRE(listed.size() == 1);
	const Dictionary entry = listed[0];
	CHECK(String(entry["slug"]) == "alpha");
	CHECK(String(entry["verdict"]) == "passed");
	CHECK((int64_t)entry["event_count"] == 1);
	CHECK(String(entry["directory"]).begins_with("user://godot_ai_sessions/"));
}

TEST_CASE("[godot_ai] Session names become safe slugs") {
	String error;

	CHECK(MCPSessions::slugify("Jump Over Gap", error) == "jump-over-gap");
	CHECK(MCPSessions::slugify("boss_fight-2", error) == "boss_fight-2");
	// Path separators and traversal are dropped, not escaped: the slug is a directory
	// name built from a string a model chose.
	CHECK(MCPSessions::slugify("../../etc/passwd", error) == "etc-passwd");
	CHECK_FALSE(MCPSessions::slugify("../../etc/passwd", error).contains(".."));
	CHECK(MCPSessions::slugify("a///b", error) == "a-b");

    // A name with nothing usable is refused rather than silently renamed - a caller that
    // cannot predict the slug cannot find its session again.
	error = String();
	CHECK(MCPSessions::slugify("!!!", error).is_empty());
	CHECK_FALSE(error.is_empty());
}

TEST_CASE("[godot_ai] Session removal refuses to escape the session root") {
	SessionFixture fixture("remove");
	REQUIRE(MCPSessions::begin("keeper", "keeper", 0, Dictionary()).ok);

	// A recursive delete that started in the wrong place once erased this repository.
	for (const String &escape : { String(".."), String("../.."), String("a/b"), String() }) {
		const MCPSessions::Result result = MCPSessions::remove(escape);
		CHECK_FALSE(result.ok);
	}
	CHECK(MCPSessions::exists("keeper"));

	CHECK(MCPSessions::remove("keeper").ok);
	CHECK_FALSE(MCPSessions::exists("keeper"));
}

TEST_CASE("[godot_ai] Reading a session that does not exist is empty, not an error") {
	SessionFixture fixture("missing");
	CHECK(MCPSessions::read_meta("nope").is_empty());
	CHECK(MCPSessions::read_events("nope").is_empty());
	CHECK(MCPSessions::read_assertions("nope").is_empty());
	CHECK_FALSE(MCPSessions::exists("nope"));
}

TEST_CASE("[godot_ai] Session events survive a value containing a newline") {
	// The store is JSON Lines, so an embedded newline in a value would break the framing
	// if it were not escaped by the encoder.
	SessionFixture fixture("newline");
	REQUIRE(MCPSessions::begin("s", "s", 0, Dictionary()).ok);

	Array events;
	Dictionary event = make_event(1, "key");
	event["label"] = "line one\nline two";
	events.push_back(event);
	REQUIRE(MCPSessions::append_events("s", events).ok);

	const Array read_back = MCPSessions::read_events("s");
	REQUIRE(read_back.size() == 1);
	CHECK(String(Dictionary(read_back[0])["label"]) == "line one\nline two");
}

} // namespace TestMCPSessions

#endif // TEST_MCP_SESSIONS_H
