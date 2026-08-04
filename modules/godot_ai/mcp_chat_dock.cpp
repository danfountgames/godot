/**************************************************************************/
/*  mcp_chat_dock.cpp                                                     */
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

#include "mcp_chat_dock.h"

#include "mcp_service.h"

#include "core/object/callable_mp.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/rich_text_label.h"

MCPChatDock::MCPChatDock(MCPService *p_service) :
		service(p_service) {
	set_name(TTR("AI Chat"));
	set_v_size_flags(SIZE_EXPAND_FILL);

	chat.instantiate();
	chat->set_service(p_service);
	chat->set_storage_path(MCPChat::default_storage_path());
	// A conversation the editor forgot on restart would be a notepad, not a chat.
	chat->load();
	chat->connect("changed", callable_mp(this, &MCPChatDock::_on_chat_changed));

	transcript = memnew(RichTextLabel);
	transcript->set_v_size_flags(SIZE_EXPAND_FILL);
	transcript->set_use_bbcode(true);
	transcript->set_selection_enabled(true);
	transcript->set_focus_mode(FOCUS_NONE);
	add_child(transcript);

	status = memnew(Label);
	status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	add_child(status);

	HBoxContainer *row = memnew(HBoxContainer);
	add_child(row);

	input = memnew(LineEdit);
	input->set_placeholder(TTR("Ask about this project"));
	input->set_h_size_flags(SIZE_EXPAND_FILL);
	input->connect("text_submitted", callable_mp(this, &MCPChatDock::_send).unbind(1));
	row->add_child(input);

	send_button = memnew(Button);
	send_button->set_text(TTR("Send"));
	send_button->connect("pressed", callable_mp(this, &MCPChatDock::_send));
	row->add_child(send_button);

	cancel_button = memnew(Button);
	cancel_button->set_text(TTR("Cancel"));
	cancel_button->connect("pressed", callable_mp(this, &MCPChatDock::_cancel));
	row->add_child(cancel_button);

	HBoxContainer *tools = memnew(HBoxContainer);
	add_child(tools);

	attach_button = memnew(Button);
	attach_button->set_text(TTR("Attach Edited Scene"));
	attach_button->connect("pressed", callable_mp(this, &MCPChatDock::_attach_edited_scene));
	tools->add_child(attach_button);

	clear_button = memnew(Button);
	clear_button->set_text(TTR("Clear"));
	clear_button->connect("pressed", callable_mp(this, &MCPChatDock::_clear));
	tools->add_child(clear_button);

	refresh();
}

void MCPChatDock::_notification(int p_what) {
	if (p_what == NOTIFICATION_VISIBILITY_CHANGED && is_visible_in_tree()) {
		refresh();
	}
}

void MCPChatDock::_on_chat_changed() {
	refresh();
}

void MCPChatDock::_send() {
	const String text = input->get_text();
	String error;
	if (!chat->send(text, error)) {
		// send() already leaves a note in the conversation for anything the user needs
		// to act on; the status line carries the rest.
		status->set_text(error);
		refresh();
		return;
	}
	input->clear();
	refresh();
}

void MCPChatDock::_cancel() {
	chat->cancel();
	refresh();
}

void MCPChatDock::_clear() {
	chat->clear();
	refresh();
}

void MCPChatDock::_attach_edited_scene() {
	if (!EditorNode::get_singleton() || !EditorInterface::get_singleton()) {
		return;
	}
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	const String path = root ? root->get_scene_file_path() : String();
	if (path.is_empty()) {
		status->set_text(TTR("No saved scene is open to attach."));
		return;
	}
	chat->stage_attachment(path);
	refresh();
}

void MCPChatDock::focus_input() {
	show();
	if (input) {
		input->grab_focus();
	}
}

void MCPChatDock::refresh() {
	transcript->clear();
	for (const MCPChatMessage &message : chat->get_messages()) {
		switch (message.role) {
			case MCPChatMessage::ROLE_USER:
				transcript->push_bold();
				transcript->add_text(TTR("You: "));
				transcript->pop();
				transcript->add_text(message.text);
				if (!message.attachments.is_empty()) {
					transcript->add_text("\n");
					transcript->push_italics();
					transcript->add_text(vformat(TTR("attached: %s"), String(", ").join(message.attachments)));
					transcript->pop();
				}
				break;
			case MCPChatMessage::ROLE_ASSISTANT:
				transcript->push_bold();
				transcript->add_text(TTR("Assistant: "));
				transcript->pop();
				transcript->add_text(message.text);
				break;
			case MCPChatMessage::ROLE_NOTE:
				transcript->push_italics();
				transcript->add_text(message.text);
				transcript->pop();
				break;
		}
		transcript->add_text("\n\n");
	}

	const bool waiting = chat->is_waiting();
	send_button->set_disabled(waiting);
	input->set_editable(!waiting);
	cancel_button->set_disabled(!waiting);

	String state;
	if (waiting) {
		state = TTR("Waiting for the client to answer...");
	} else if (service && !service->has_sampling_client()) {
		// The honest version of "it is not working": the editor has no model, and says
		// where one comes from.
		state = TTR("No connected client offers sampling. This editor has no model of its "
					"own - it borrows one from an attached MCP client.");
	} else {
		state = TTR("Ready.");
	}
	const PackedStringArray staged = chat->get_staged_attachments();
	if (!staged.is_empty()) {
		state += "\n" + vformat(TTR("Attached to the next message: %s"), String(", ").join(staged));
	}
	status->set_text(state);
}
