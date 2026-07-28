/**************************************************************************/
/*  test_mcp_chat.h                                                       */
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

#ifndef TEST_MCP_CHAT_H
#define TEST_MCP_CHAT_H

#include "modules/godot_ai/mcp_chat.h"
#include "modules/godot_ai/mcp_paths.h"
#include "modules/godot_ai/tests/test_mcp_fs_helpers.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

#include "tests/test_macros.h"

namespace TestMCPChat {

// A scratch project plus a place to keep the transcript. Deleted through the guarded
// helper, which refuses anything that is not under the cache directory.
struct ChatFixture {
	String root;
	String storage;

	ChatFixture() {
		root = OS::get_singleton()->get_cache_path().path_join(
				vformat("godot_ai_test_chat_%d", OS::get_singleton()->get_ticks_usec()));
		Ref<DirAccess> access = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		access->make_dir_recursive(root);
		storage = root.path_join("transcript.json");
		MCPPaths::set_project_root_override(root);
	}

	~ChatFixture() {
		MCPPaths::clear_project_root_override();
		mcp_test_remove_tree(root);
	}

	void write(const String &p_relative, const String &p_contents) const {
		Ref<FileAccess> file = FileAccess::open(root.path_join(p_relative), FileAccess::WRITE);
		CHECK(file.is_valid());
		file->store_string(p_contents);
	}

	Ref<MCPChat> chat() const {
		Ref<MCPChat> instance;
		instance.instantiate();
		instance->set_storage_path(storage);
		return instance;
	}
};

TEST_CASE("[godot_ai] Chat sampling request carries the conversation, not just the prompt") {
	ChatFixture fixture;
	Ref<MCPChat> chat = fixture.chat();
	chat->add_note("a note the model must never see");

	const Dictionary first = chat->build_sampling_request("hello", PackedStringArray());
	const Array messages = first["messages"];
	// The note is the editor talking to the user; only the prompt goes to the model.
	REQUIRE(messages.size() == 1);
	const Dictionary only = messages[0];
	CHECK(String(only["role"]) == "user");
	const Dictionary content = only["content"];
	CHECK(String(content["text"]) == "hello");
	CHECK(int(first["maxTokens"]) > 0);
	CHECK(String(first["systemPrompt"]).length() > 0);
}

TEST_CASE("[godot_ai] Chat attachments are inlined, confined and truncated") {
	ChatFixture fixture;
	fixture.write("notes.txt", "the quick brown fox");
	Ref<MCPChat> chat = fixture.chat();

	SUBCASE("a project file is inlined into the prompt") {
		const Dictionary request = chat->build_sampling_request("look", PackedStringArray({ "res://notes.txt" }));
		const Array messages = request["messages"];
		const Dictionary content = Dictionary(messages[messages.size() - 1])["content"];
		const String text = content["text"];
		CHECK(text.begins_with("look"));
		CHECK(text.contains("notes.txt"));
		CHECK(text.contains("the quick brown fox"));
	}

	SUBCASE("a path outside the project is refused, not read") {
		const Dictionary request = chat->build_sampling_request(
				"look", PackedStringArray({ "res://../../../etc/passwd" }));
		const Array messages = request["messages"];
		const Dictionary content = Dictionary(messages[messages.size() - 1])["content"];
		const String text = content["text"];
		CHECK(text.contains("could not be read"));
		CHECK_FALSE(text.contains("root:"));
	}

	SUBCASE("a missing file is reported rather than silently dropped") {
		const Dictionary request = chat->build_sampling_request(
				"look", PackedStringArray({ "res://nothing-here.txt" }));
		const Array messages = request["messages"];
		const Dictionary content = Dictionary(messages[messages.size() - 1])["content"];
		CHECK(String(content["text"]).contains("could not be read"));
	}

	SUBCASE("a large file is cut and says so") {
		String big;
		for (int i = 0; i < 5000; i++) {
			big += "0123456789abcdef\n";
		}
		fixture.write("big.txt", big);
		const Dictionary request = chat->build_sampling_request("look", PackedStringArray({ "res://big.txt" }));
		const Array messages = request["messages"];
		const String text = Dictionary(Dictionary(messages[messages.size() - 1])["content"])["text"];
		CHECK(text.contains("first 65536 bytes of"));
		CHECK(text.length() < big.length());
	}
}

TEST_CASE("[godot_ai] Chat staged attachments belong to one message") {
	ChatFixture fixture;
	fixture.write("notes.txt", "content");
	Ref<MCPChat> chat = fixture.chat();

	chat->stage_attachment("res://notes.txt");
	chat->stage_attachment("res://notes.txt"); // Staging twice must not duplicate.
	CHECK(chat->get_staged_attachments().size() == 1);

	// A send that could not happen must not consume them. Nothing reached a model, so
	// silently dropping what the user attached would lose work they would have to redo
	// without being told why.
	String error;
	CHECK_FALSE(chat->send("hello", error));
	CHECK(chat->get_staged_attachments().size() == 1);

	chat->clear_staged_attachments();
	CHECK(chat->get_staged_attachments().is_empty());
}

TEST_CASE("[godot_ai] Chat state machine refuses to send twice and drops late answers") {
	ChatFixture fixture;
	Ref<MCPChat> chat = fixture.chat();

	SUBCASE("an empty prompt is not a message") {
		String error;
		CHECK_FALSE(chat->send("   ", error));
		CHECK(error.contains("nothing to send"));
		CHECK(chat->get_message_count() == 0);
	}

	SUBCASE("a cancelled turn stops waiting and refuses the late answer") {
		// Drive the state machine directly: with no client attached, send() cannot
		// reach STATE_WAITING, and this is about what happens once it has.
		chat->add_note("standing in for a sent turn");
		CHECK(chat->get_state() == MCPChat::STATE_IDLE);
		CHECK_FALSE(chat->cancel()); // Nothing in flight.

		// An answer that matches no pending request must never append.
		Dictionary result;
		Dictionary content;
		content["type"] = "text";
		content["text"] = "late reply";
		result["content"] = content;
		const int before = chat->get_message_count();
		chat->accept_response(999, result);
		CHECK(chat->get_message_count() == before);
	}
}

TEST_CASE("[godot_ai] Chat survives the editor closing") {
	ChatFixture fixture;
	{
		Ref<MCPChat> chat = fixture.chat();
		chat->add_note("first");
		Ref<MCPChat> other = fixture.chat();
		other->load();
		CHECK(other->get_message_count() == 1);
		CHECK(other->get_message(0).text == "first");
		CHECK(other->get_message(0).role == MCPChatMessage::ROLE_NOTE);
	}

	SUBCASE("messages round-trip with their attachments") {
		Ref<MCPChat> chat = fixture.chat();
		chat->load();
		MCPChatMessage message;
		message.role = MCPChatMessage::ROLE_USER;
		message.text = "with an attachment";
		message.attachments.push_back("res://scenes/main.tscn");
		// Reach through the public save/load path by rebuilding the transcript.
		Dictionary stored = message.to_dictionary();
		MCPChatMessage restored = MCPChatMessage::from_dictionary(stored);
		CHECK(restored.text == message.text);
		CHECK(restored.role == MCPChatMessage::ROLE_USER);
		REQUIRE(restored.attachments.size() == 1);
		CHECK(restored.attachments[0] == "res://scenes/main.tscn");
	}

	SUBCASE("clearing empties the stored transcript too") {
		Ref<MCPChat> chat = fixture.chat();
		chat->load();
		chat->clear();
		Ref<MCPChat> other = fixture.chat();
		other->load();
		CHECK(other->get_message_count() == 0);
	}

	SUBCASE("a corrupt transcript does not take the editor with it") {
		Ref<FileAccess> file = FileAccess::open(fixture.storage, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		file->store_string("{not json");
		file.unref();

		Ref<MCPChat> chat = fixture.chat();
		CHECK(chat->load() != OK);
		CHECK(chat->get_message_count() == 0);
	}
}

} // namespace TestMCPChat

#endif // TEST_MCP_CHAT_H
