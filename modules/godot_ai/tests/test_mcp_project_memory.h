/**************************************************************************/
/*  test_mcp_project_memory.h                                             */
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

#ifndef TEST_MCP_PROJECT_MEMORY_H
#define TEST_MCP_PROJECT_MEMORY_H

#include "modules/godot_ai/mcp_paths.h"
#include "modules/godot_ai/mcp_project_memory.h"
#include "modules/godot_ai/tests/test_mcp_fs_helpers.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

#include "tests/test_macros.h"

namespace TestMCPProjectMemory {

// A throwaway project to keep memory in. Same shape as the path fixture: scratch
// under the cache directory, torn down through the guarded delete.
class MemoryFixture {
	String root;

public:
	explicit MemoryFixture(const String &p_suffix) {
		root = OS::get_singleton()->get_cache_path().path_join(
				"godot_ai_test_mem_" + p_suffix + "_" + itos(OS::get_singleton()->get_process_id()));
		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		dir->make_dir_recursive(root);
		MCPPaths::set_project_root_override(root);
	}
	~MemoryFixture() {
		MCPPaths::clear_project_root_override();
		mcp_test_remove_tree(root);
	}

	String get_root() const { return root; }
};

TEST_CASE("[godot_ai] A note written to project memory reads back") {
	MemoryFixture fixture("roundtrip");

	MCPProjectMemory::Note written;
	String error;
	REQUIRE(MCPProjectMemory::write("player-movement", "How the player moves",
			"Movement lives in res://player/player.gd and is authoritative.", written, error));
	CHECK(written.name == "player-movement");
	CHECK_FALSE(written.updated.is_empty());

	MCPProjectMemory::Note read;
	REQUIRE(MCPProjectMemory::read("player-movement", read, error));
	CHECK(read.subject == "How the player moves");
	CHECK(read.body.contains("res://player/player.gd"));
	CHECK(read.updated == written.updated);
}

TEST_CASE("[godot_ai] Memory lives in the project as readable markdown") {
	MemoryFixture fixture("onDisk");

	MCPProjectMemory::Note note;
	String error;
	REQUIRE(MCPProjectMemory::write("spawning", "Enemy spawning", "SpawnManager owns it.", note, error));

	// The store is files a human can read, diff and correct - that is the whole
	// reason it is not a database.
	const String path = fixture.get_root().path_join(".godot_ai/memory/spawning.md");
	REQUIRE(FileAccess::exists(path));
	const String text = FileAccess::get_file_as_string(path);
	CHECK(text.begins_with("---\n"));
	CHECK(text.contains("subject: Enemy spawning"));
	CHECK(text.contains("SpawnManager owns it."));
}

TEST_CASE("[godot_ai] A name cannot escape the memory folder") {
	MemoryFixture fixture("escape");

	// The defence is that a name is reduced to [a-z0-9-] before it is ever a path,
	// so traversal is not rejected so much as unrepresentable.
	CHECK(MCPProjectMemory::slugify("../../etc/passwd") == "etc-passwd");
	CHECK(MCPProjectMemory::slugify("/absolute/path") == "absolute-path");
	CHECK(MCPProjectMemory::slugify("Player Movement!") == "player-movement");
	CHECK(MCPProjectMemory::slugify("  spaced  out  ") == "spaced-out");
	CHECK(MCPProjectMemory::slugify("C:\\Windows") == "c-windows");
	CHECK(MCPProjectMemory::slugify("...").is_empty());

	String res_path;
	String error;
	REQUIRE(MCPProjectMemory::note_path("../../escape", res_path, error));
	CHECK(res_path == "res://.godot_ai/memory/escape.md");

	// A name with nothing usable in it is refused rather than silently becoming
	// some default filename.
	CHECK_FALSE(MCPProjectMemory::note_path("///", res_path, error));
	CHECK_FALSE(error.is_empty());
}

TEST_CASE("[godot_ai] Recall is an index, and the index summarises") {
	MemoryFixture fixture("index");

	MCPProjectMemory::Note note;
	String error;
	String long_body;
	for (int i = 0; i < 40; i++) {
		long_body += "the frame budget is sixteen milliseconds ";
	}
	REQUIRE(MCPProjectMemory::write("perf", "Performance", long_body, note, error));

	const Vector<MCPProjectMemory::Note> listed = MCPProjectMemory::list(error);
	REQUIRE(listed.size() == 1);

	// Returning every body on every recall is how a memory store becomes context
	// poisoning, so the index carries a clipped summary and nothing else.
	const Dictionary indexed = listed[0].to_dictionary(false);
	CHECK_FALSE(indexed.has("body"));
	REQUIRE(indexed.has("summary"));
	CHECK(String(indexed["summary"]).length() <= MCPProjectMemory::SUMMARY_CHARS);
	CHECK(String(indexed["summary"]).begins_with("the frame budget"));

	const Dictionary full = listed[0].to_dictionary(true);
	CHECK(full.has("body"));
	CHECK(String(full["body"]).length() > MCPProjectMemory::SUMMARY_CHARS);
}

TEST_CASE("[godot_ai] The index is ordered by recency, and undated notes sort last") {
	MemoryFixture fixture("order");

	MCPProjectMemory::Note note;
	String error;
	REQUIRE(MCPProjectMemory::write("first", "First", "one", note, error));
	REQUIRE(MCPProjectMemory::write("second", "Second", "two", note, error));

	// A hand-written note with no timestamp is a real case - the format invites
	// editing - and an unknown age must not read as the newest.
	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	dir->make_dir_recursive(fixture.get_root().path_join(".godot_ai/memory"));
	Ref<FileAccess> file = FileAccess::open(
			fixture.get_root().path_join(".godot_ai/memory/handwritten.md"), FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_string("---\nsubject: Written by a person\n---\n\nno timestamp here\n");
	file->close();

	const Vector<MCPProjectMemory::Note> listed = MCPProjectMemory::list(error);
	REQUIRE(listed.size() == 3);
	CHECK(listed[listed.size() - 1].name == "handwritten");
}

TEST_CASE("[godot_ai] A full store refuses a new note instead of evicting one") {
	MemoryFixture fixture("full");

	MCPProjectMemory::Note note;
	String error;
	for (int i = 0; i < MCPProjectMemory::MAX_NOTES; i++) {
		REQUIRE(MCPProjectMemory::write("note-" + itos(i), "Subject " + itos(i), "body", note, error));
	}

	// Evicting the oldest would make the store quietly forget the thing you most
	// relied on it for, so the write fails and the message says what to do.
	CHECK_FALSE(MCPProjectMemory::write("one-too-many", "Subject", "body", note, error));
	CHECK(error.contains("limit"));

	// Replacing an existing note always works: consolidating is the way out.
	CHECK(MCPProjectMemory::write("note-0", "Merged", "several facts, merged", note, error));
}

TEST_CASE("[godot_ai] An over-long note is refused with the size named") {
	MemoryFixture fixture("toolong");

	String body;
	while (body.length() <= MCPProjectMemory::MAX_NOTE_CHARS) {
		body += "a transcript of everything that just happened. ";
	}

	MCPProjectMemory::Note note;
	String error;
	CHECK_FALSE(MCPProjectMemory::write("essay", "An essay", body, note, error));
	CHECK(error.contains(itos(MCPProjectMemory::MAX_NOTE_CHARS)));
}

TEST_CASE("[godot_ai] Writing the same name replaces rather than duplicating") {
	MemoryFixture fixture("replace");

	MCPProjectMemory::Note note;
	String error;
	REQUIRE(MCPProjectMemory::write("physics", "Physics", "gravity is 980", note, error));
	REQUIRE(MCPProjectMemory::write("physics", "Physics", "gravity is 1200 since the jump rework", note, error));

	CHECK(MCPProjectMemory::list(error).size() == 1);

	MCPProjectMemory::Note read;
	REQUIRE(MCPProjectMemory::read("physics", read, error));
	CHECK(read.body.contains("1200"));
	CHECK_FALSE(read.body.contains("980"));
}

TEST_CASE("[godot_ai] Forgetting removes the note, and forgetting twice says so") {
	MemoryFixture fixture("forget");

	MCPProjectMemory::Note note;
	String error;
	REQUIRE(MCPProjectMemory::write("stale", "No longer true", "the old way", note, error));

	CHECK(MCPProjectMemory::erase("stale", error));
	CHECK(MCPProjectMemory::list(error).is_empty());

	CHECK_FALSE(MCPProjectMemory::erase("stale", error));
	CHECK(error.contains("stale"));
}

TEST_CASE("[godot_ai] An empty store is not an error") {
	MemoryFixture fixture("empty");

	// A project nobody has told anything yet is the common case, and it must read as
	// an empty list rather than as a missing folder.
	String error;
	CHECK(MCPProjectMemory::list(error).is_empty());
	CHECK(error.is_empty());
}

TEST_CASE("[godot_ai] A note that no longer parses is listed with its problem") {
	MemoryFixture fixture("broken");

	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	dir->make_dir_recursive(fixture.get_root().path_join(".godot_ai/memory"));
	Ref<FileAccess> file = FileAccess::open(
			fixture.get_root().path_join(".godot_ai/memory/mangled.md"), FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_string("somebody deleted the frontmatter\n");
	file->close();

	// Silently skipping it would leave the user wondering where their note went.
	String error;
	const Vector<MCPProjectMemory::Note> listed = MCPProjectMemory::list(error);
	REQUIRE(listed.size() == 1);
	CHECK(listed[0].name == "mangled");
	CHECK(listed[0].subject.contains("unreadable"));
}

TEST_CASE("[godot_ai] Serialisation survives a subject containing a colon") {
	MCPProjectMemory::Note note;
	note.name = "boss";
	note.subject = "Boss fight: phase two";
	note.updated = "2026-08-30T12:00:00Z";
	note.body = "Phase two starts at 50% health.";

	MCPProjectMemory::Note parsed;
	String error;
	REQUIRE(MCPProjectMemory::parse(MCPProjectMemory::serialize(note), parsed, error));
	CHECK(parsed.subject == "Boss fight: phase two");
	CHECK(parsed.updated == "2026-08-30T12:00:00Z");
	CHECK(parsed.body == "Phase two starts at 50% health.");
}

TEST_CASE("[godot_ai] A note without a subject is rejected on parse") {
	MCPProjectMemory::Note parsed;
	String error;
	CHECK_FALSE(MCPProjectMemory::parse("---\nupdated: 2026-01-01T00:00:00Z\n---\n\nbody\n", parsed, error));
	CHECK(error.contains("subject"));

	CHECK_FALSE(MCPProjectMemory::parse("no frontmatter at all\n", parsed, error));
	CHECK(error.contains("frontmatter"));

	CHECK_FALSE(MCPProjectMemory::parse("---\nsubject: Never closed\n", parsed, error));
	CHECK(error.contains("closed"));
}

TEST_CASE("[godot_ai] An empty body is refused, since forgetting is the other tool") {
	MemoryFixture fixture("emptybody");

	MCPProjectMemory::Note note;
	String error;
	CHECK_FALSE(MCPProjectMemory::write("blank", "A subject", "   ", note, error));
	CHECK(error.contains("forget"));

	CHECK_FALSE(MCPProjectMemory::write("blank", "  ", "a body", note, error));
	CHECK(error.contains("subject"));
}

} // namespace TestMCPProjectMemory

#endif // TEST_MCP_PROJECT_MEMORY_H
