/**************************************************************************/
/*  mcp_chat.cpp                                                          */
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

#include "mcp_chat.h"

#include "mcp_paths.h"
#include "mcp_service.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"

#ifdef TOOLS_ENABLED
#include "editor/file_system/editor_paths.h"
#endif

namespace {

// A whole scene pasted into a prompt is mostly noise, and a long one crowds out the
// conversation. Attachments are excerpts, and say so when they are cut.
const int64_t MAX_ATTACHMENT_BYTES = 64 * 1024;

} // namespace

String MCPChatMessage::role_to_string(Role p_role) {
	switch (p_role) {
		case ROLE_ASSISTANT:
			return "assistant";
		case ROLE_NOTE:
			return "note";
		default:
			return "user";
	}
}

MCPChatMessage::Role MCPChatMessage::role_from_string(const String &p_role) {
	if (p_role == "assistant") {
		return ROLE_ASSISTANT;
	}
	if (p_role == "note") {
		return ROLE_NOTE;
	}
	return ROLE_USER;
}

Dictionary MCPChatMessage::to_dictionary() const {
	Dictionary out;
	out["role"] = role_to_string(role);
	out["text"] = text;
	out["timestamp"] = timestamp;
	if (!attachments.is_empty()) {
		out["attachments"] = attachments;
	}
	return out;
}

MCPChatMessage MCPChatMessage::from_dictionary(const Dictionary &p_source) {
	MCPChatMessage message;
	message.role = role_from_string(p_source.get("role", "user"));
	message.text = p_source.get("text", String());
	message.timestamp = p_source.get("timestamp", 0.0);
	if (p_source.has("attachments")) {
		message.attachments = p_source["attachments"];
	}
	return message;
}

String MCPChat::default_storage_path() {
#ifdef TOOLS_ENABLED
	if (EditorPaths::get_singleton()) {
		return EditorPaths::get_singleton()->get_project_settings_dir().path_join("godot_ai_chat.json");
	}
#endif
	return String();
}

void MCPChat::_bind_methods() {
	ADD_SIGNAL(MethodInfo("changed"));
}

void MCPChat::_emit_changed() {
	emit_signal(SNAME("changed"));
}

MCPChatMessage MCPChat::get_message(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, messages.size(), MCPChatMessage());
	return messages[p_index];
}

void MCPChat::stage_attachment(const String &p_path) {
	if (p_path.is_empty() || staged_attachments.has(p_path)) {
		return;
	}
	staged_attachments.push_back(p_path);
	_emit_changed();
}

void MCPChat::clear_staged_attachments() {
	if (staged_attachments.is_empty()) {
		return;
	}
	staged_attachments.clear();
	_emit_changed();
}

void MCPChat::add_note(const String &p_text) {
	MCPChatMessage note;
	note.role = MCPChatMessage::ROLE_NOTE;
	note.text = p_text;
	note.timestamp = OS::get_singleton() ? OS::get_singleton()->get_unix_time() : 0.0;
	messages.push_back(note);
	save();
	_emit_changed();
}

Dictionary MCPChat::build_sampling_request(const String &p_prompt, const PackedStringArray &p_attachments) const {
	Array conversation;

	// Prior turns, so the client's model sees the conversation rather than one line.
	// Notes are the editor talking to the user and are left out on purpose.
	for (const MCPChatMessage &message : messages) {
		if (message.role == MCPChatMessage::ROLE_NOTE) {
			continue;
		}
		Dictionary content;
		content["type"] = "text";
		content["text"] = message.text;
		Dictionary entry;
		entry["role"] = MCPChatMessage::role_to_string(message.role);
		entry["content"] = content;
		conversation.push_back(entry);
	}

	String prompt = p_prompt;
	for (const String &attachment : p_attachments) {
		MCPPaths::Resolved resolved;
		String error;
		// Attachments go through the same confinement as every other path: a chat box
		// is not a way out of the project.
		if (!MCPPaths::resolve_existing(attachment, resolved, error)) {
			prompt += vformat("\n\n[attachment %s could not be read: %s]", attachment, error);
			continue;
		}
		Error open_error = OK;
		const String contents = FileAccess::get_file_as_string(resolved.absolute, &open_error);
		if (open_error != OK) {
			prompt += vformat("\n\n[attachment %s could not be read]", attachment);
			continue;
		}
		if (contents.length() > MAX_ATTACHMENT_BYTES) {
			prompt += vformat("\n\n--- %s (first %d bytes of %d) ---\n%s",
					attachment, MAX_ATTACHMENT_BYTES, contents.length(),
					contents.substr(0, MAX_ATTACHMENT_BYTES));
		} else {
			prompt += vformat("\n\n--- %s ---\n%s", attachment, contents);
		}
	}

	Dictionary content;
	content["type"] = "text";
	content["text"] = prompt;
	Dictionary entry;
	entry["role"] = "user";
	entry["content"] = content;
	conversation.push_back(entry);

	Dictionary params;
	params["messages"] = conversation;
	params["maxTokens"] = 2048;
	params["systemPrompt"] =
			"You are answering inside the Godot editor's AI panel. The user is working on "
			"the project this editor has open. You also have the editor's Godot_ tools "
			"available through this same connection - use them to look before you answer.";
	return params;
}

bool MCPChat::send(const String &p_prompt, String &r_error) {
	const String prompt = p_prompt.strip_edges();
	if (prompt.is_empty()) {
		r_error = "nothing to send";
		return false;
	}
	if (state == STATE_WAITING) {
		r_error = "a previous message is still being answered";
		return false;
	}
	if (!service) {
		r_error = "the AI service is not running";
		add_note(r_error);
		return false;
	}

	const Dictionary params = build_sampling_request(prompt, staged_attachments);

	// Record the turn before sending, so a client that answers instantly cannot
	// deliver into a conversation that does not contain the question yet.
	MCPChatMessage message;
	message.role = MCPChatMessage::ROLE_USER;
	message.text = prompt;
	message.attachments = staged_attachments;
	message.timestamp = OS::get_singleton() ? OS::get_singleton()->get_unix_time() : 0.0;
	messages.push_back(message);
	staged_attachments.clear();

	const int64_t request = service->send_sampling_request(params, this);
	if (request == 0) {
		r_error = "no connected client offered sampling; the editor has no model of its "
				  "own, so attach an MCP client that does";
		add_note(r_error);
		save();
		_emit_changed();
		return false;
	}

	pending_request = request;
	state = STATE_WAITING;
	save();
	_emit_changed();
	return true;
}

bool MCPChat::cancel() {
	if (state != STATE_WAITING) {
		return false;
	}
	if (service) {
		service->cancel_sampling_request(pending_request);
	}
	pending_request = 0;
	state = STATE_IDLE;
	add_note("cancelled");
	return true;
}

void MCPChat::accept_response(int64_t p_request, const Dictionary &p_result) {
	// A late answer to a cancelled turn is dropped: the user said stop, and a reply
	// appearing afterwards would be indistinguishable from one they did not cancel.
	if (p_request != pending_request || state != STATE_WAITING) {
		return;
	}
	pending_request = 0;
	state = STATE_IDLE;

	String text;
	if (p_result.has("content")) {
		const Variant content = p_result["content"];
		if (content.get_type() == Variant::DICTIONARY) {
			const Dictionary block = content;
			text = block.get("text", String());
		} else if (content.get_type() == Variant::ARRAY) {
			const Array blocks = content;
			for (int i = 0; i < blocks.size(); i++) {
				const Dictionary block = blocks[i];
				if (String(block.get("type", "")) == "text") {
					text += String(block.get("text", String()));
				}
			}
		}
	}
	if (text.is_empty()) {
		text = "(the client answered with no text)";
	}

	MCPChatMessage message;
	message.role = MCPChatMessage::ROLE_ASSISTANT;
	message.text = text;
	message.timestamp = OS::get_singleton() ? OS::get_singleton()->get_unix_time() : 0.0;
	messages.push_back(message);
	save();
	_emit_changed();
}

void MCPChat::accept_failure(int64_t p_request, const String &p_message) {
	if (p_request != pending_request || state != STATE_WAITING) {
		return;
	}
	pending_request = 0;
	state = STATE_IDLE;
	add_note(p_message);
}

void MCPChat::clear() {
	messages.clear();
	staged_attachments.clear();
	state = STATE_IDLE;
	pending_request = 0;
	save();
	_emit_changed();
}

Error MCPChat::save() const {
	if (storage_path.is_empty()) {
		return ERR_UNCONFIGURED;
	}
	Array entries;
	for (const MCPChatMessage &message : messages) {
		entries.push_back(message.to_dictionary());
	}
	Dictionary document;
	document["version"] = 1;
	document["messages"] = entries;

	const String directory = storage_path.get_base_dir();
	if (!directory.is_empty()) {
		Ref<DirAccess> access = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (access.is_valid() && !access->dir_exists(directory)) {
			access->make_dir_recursive(directory);
		}
	}

	Error error = OK;
	Ref<FileAccess> file = FileAccess::open(storage_path, FileAccess::WRITE, &error);
	if (file.is_null()) {
		return error;
	}
	file->store_string(JSON::stringify(document, "  "));
	return OK;
}

Error MCPChat::load() {
	messages.clear();
	state = STATE_IDLE;
	pending_request = 0;
	if (storage_path.is_empty() || !FileAccess::exists(storage_path)) {
		return ERR_FILE_NOT_FOUND;
	}
	Error error = OK;
	const String text = FileAccess::get_file_as_string(storage_path, &error);
	if (error != OK) {
		return error;
	}
	const Variant parsed = JSON::parse_string(text);
	if (parsed.get_type() != Variant::DICTIONARY) {
		return ERR_FILE_CORRUPT;
	}
	const Dictionary document = parsed;
	if (!document.has("messages")) {
		return ERR_FILE_CORRUPT;
	}
	const Array entries = document["messages"];
	for (int i = 0; i < entries.size(); i++) {
		if (entries[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		messages.push_back(MCPChatMessage::from_dictionary(entries[i]));
	}
	_emit_changed();
	return OK;
}
