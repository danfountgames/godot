/**************************************************************************/
/*  mcp_chat.h                                                            */
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

// The conversation behind the editor's chat dock.
//
// The interesting decision here is where the model comes from. The editor has no API
// key, no vendor account and no business acquiring either - so it does not call a
// model at all. It asks the *client* to, using MCP's `sampling/createMessage`: the
// agent already connected to this editor has a model, and sampling exists precisely so
// a server can borrow it. That keeps credentials where they already are, keeps the
// user's "which model" decision where they already made it, and means the chat works
// with whatever client is attached rather than one this fork happened to bundle.
//
// This class is the conversation and its rules, with no UI in it: what a message is,
// how a turn is sent, cancelled and answered, and how the whole thing survives closing
// the editor. The dock renders it.

#ifndef MCP_CHAT_H
#define MCP_CHAT_H

#include "core/object/ref_counted.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class MCPService;

// One turn in the conversation.
struct MCPChatMessage {
	enum Role {
		ROLE_USER,
		ROLE_ASSISTANT,
		// Not part of the model conversation: how the editor tells the user something
		// happened (no client attached, a turn cancelled, a request refused).
		ROLE_NOTE,
	};

	Role role = ROLE_USER;
	String text;
	// Project files the user attached to this turn, as res:// paths. Their contents
	// are read when the turn is sent, never stored here - a conversation transcript
	// that embedded whole scenes would grow without bound and go stale immediately.
	PackedStringArray attachments;
	double timestamp = 0.0;

	Dictionary to_dictionary() const;
	static MCPChatMessage from_dictionary(const Dictionary &p_source);
	static String role_to_string(Role p_role);
	static Role role_from_string(const String &p_role);
};

class MCPChat : public RefCounted {
	GDCLASS(MCPChat, RefCounted);

public:
	enum State {
		STATE_IDLE,
		STATE_WAITING, // A turn is out with the client and has not answered yet.
	};

private:
	Vector<MCPChatMessage> messages;
	State state = STATE_IDLE;
	int64_t pending_request = 0;
	String storage_path;

	// Attachments chosen for the turn being composed, not yet sent.
	PackedStringArray staged_attachments;

	MCPService *service = nullptr;

	void _emit_changed();

public:
	// Where the transcript is kept. Under the project's own settings directory, not in
	// the project itself: a conversation is not a project asset, and nobody wants it
	// imported or committed by accident.
	static String default_storage_path();

	void set_service(MCPService *p_service) { service = p_service; }
	void set_storage_path(const String &p_path) { storage_path = p_path; }
	String get_storage_path() const { return storage_path; }

	const Vector<MCPChatMessage> &get_messages() const { return messages; }
	int get_message_count() const { return messages.size(); }
	MCPChatMessage get_message(int p_index) const;
	State get_state() const { return state; }
	bool is_waiting() const { return state == STATE_WAITING; }

	// Attachments for the turn being composed.
	void stage_attachment(const String &p_path);
	void clear_staged_attachments();
	PackedStringArray get_staged_attachments() const { return staged_attachments; }

	void add_note(const String &p_text);

	// Composes the `sampling/createMessage` params for the whole conversation so far.
	// Exposed so the shape can be tested without a client attached.
	Dictionary build_sampling_request(const String &p_prompt, const PackedStringArray &p_attachments) const;

	// Sends a turn. Returns false and leaves a note in the conversation when there is
	// nothing to send it to - no client attached, or none that offered sampling.
	bool send(const String &p_prompt, String &r_error);

	// Cancels the turn in flight, telling the client to stop. Returns false when there
	// was nothing to cancel.
	bool cancel();

	// Called by the service when the client answers or fails a turn.
	void accept_response(int64_t p_request, const Dictionary &p_result);
	void accept_failure(int64_t p_request, const String &p_message);

	void clear();

	Error save() const;
	Error load();

protected:
	static void _bind_methods();
};

#endif // MCP_CHAT_H
