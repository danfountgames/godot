/**************************************************************************/
/*  mcp_ask_user_tool.cpp                                                 */
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

#include "mcp_builtin_tools.h"

#include "../mcp_deferred.h"
#include "../mcp_tool_registry.h"

#include "core/object/callable_mp.h"
#include "core/variant/array.h"
#include "editor/editor_node.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "servers/display/display_server.h"

namespace {

// The dialog owns the token: whichever way it closes, exactly one answer is sent.
//
// This is why deferred responses exist. A modal needs the main loop to keep running,
// so the tool cannot block waiting for it - it returns a token and the service sends
// the response when this dialog resolves it.
class MCPAskUserDialog : public AcceptDialog {
	GDCLASS(MCPAskUserDialog, AcceptDialog);

	MCPDeferred::Token token = MCPDeferred::INVALID_TOKEN;
	LineEdit *free_text = nullptr;
	bool answered = false;

	void _answer(const String &p_answer) {
		if (answered) {
			return;
		}
		answered = true;
		Dictionary result;
		result["answer"] = p_answer;
		result["cancelled"] = false;
		MCPDeferred::complete(token, result);
		hide();
		queue_free();
	}

	void _cancelled() {
		if (answered) {
			return;
		}
		answered = true;
		// A refusal to answer is an answer, and the agent has to be able to tell it
		// apart from a reply that happened to be empty.
		Dictionary result;
		result["answer"] = String();
		result["cancelled"] = true;
		MCPDeferred::complete(token, result);
		queue_free();
	}

	void _submit_free_text() {
		_answer(free_text ? free_text->get_text() : String());
	}

protected:
	void _notification(int p_what) {
		switch (p_what) {
			case NOTIFICATION_READY: {
				set_process(true);
			} break;

			case NOTIFICATION_PROCESS: {
				// The client's request has a deadline this dialog knows nothing about.
				// Once it passes the service answers the token without us, and every
				// button here goes inert - so the dialog has to leave with it. Left up,
				// it is worse than no dialog: it invites an answer, accepts the click,
				// and does nothing.
				if (!answered && !MCPDeferred::is_pending(token)) {
					answered = true;
					hide();
					queue_free();
				}
			} break;

			case NOTIFICATION_PREDELETE: {
				if (!answered) {
					// The editor is going away with the question still open.
					MCPDeferred::fail(token, MCPToolError::INVALID_STATE, "the question was dismissed");
				}
			} break;
		}
	}

public:
	MCPAskUserDialog(MCPDeferred::Token p_token, const String &p_question, const String &p_context, const Array &p_choices) :
			token(p_token) {
		set_title(TTR("Godot AI asks"));
		set_min_size(Size2(460, 0));

		VBoxContainer *layout = memnew(VBoxContainer);
		add_child(layout);

		Label *question = memnew(Label);
		question->set_text(p_question);
		question->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		layout->add_child(question);

		if (!p_context.is_empty()) {
			Label *context = memnew(Label);
			context->set_text(p_context);
			context->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
			context->set_modulate(Color(1, 1, 1, 0.7));
			layout->add_child(context);
		}

		if (p_choices.is_empty()) {
			free_text = memnew(LineEdit);
			free_text->set_placeholder(TTR("Your answer"));
			free_text->connect("text_submitted", callable_mp(this, &MCPAskUserDialog::_answer));
			layout->add_child(free_text);
			set_ok_button_text(TTR("Answer"));
			get_ok_button()->connect("pressed", callable_mp(this, &MCPAskUserDialog::_submit_free_text));
		} else {
			// With fixed choices the dialog's own OK button would be a second, unlabelled
			// way to answer, so it is hidden and each choice is a button.
			get_ok_button()->hide();
			for (int i = 0; i < p_choices.size(); i++) {
				const String choice = p_choices[i];
				Button *button = memnew(Button);
				button->set_text(choice);
				button->connect("pressed", callable_mp(this, &MCPAskUserDialog::_answer).bind(choice));
				layout->add_child(button);
			}
		}

		set_close_on_escape(true);
		connect("canceled", callable_mp(this, &MCPAskUserDialog::_cancelled));
	}
};

class AskUserTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_AskUser"; }
	virtual String get_description() const override {
		return "Ask the user a question in the editor and wait for their answer. Use this "
			   "when a decision is genuinely theirs to make; the call blocks until they "
			   "answer, dismiss the dialog, or the timeout elapses.";
	}
	// Asking a question changes nothing; the risk is the interruption, not the reach.
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["question"] = MCPSchema::string_property("The question to put to the user.");
		properties["context"] = MCPSchema::string_property("Optional detail shown under the question.", "");
		properties["choices"] = MCPSchema::array_property(
				"Fixed answers to offer as buttons. Omit for a free-text answer.",
				MCPSchema::string_property("A choice."));
		properties["timeout_seconds"] = MCPSchema::integer_property(
				"How long to wait before giving up.", 300);
		Vector<String> required;
		required.push_back("question");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["answer"] = MCPSchema::string_property("What the user answered, empty when cancelled.");
		properties["cancelled"] = MCPSchema::bool_property("True when the user dismissed the question.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!EditorNode::get_singleton()) {
			r_error.set(MCPToolError::UNSUPPORTED,
					"there is no editor here to ask; run without --headless to use this tool");
			return Dictionary();
		}
		// Arguments are checked before anything about the environment: a call with no
		// question is wrong whoever is watching, and telling the caller that their
		// call was malformed is more use than telling them nobody was home.
		const String question = String(p_arguments["question"]).strip_edges();
		if (question.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, "question must not be empty");
			return Dictionary();
		}

		// An editor is not a person. `--headless --editor` has a complete EditorNode
		// and nobody looking at it, so without this the dialog opened on the dummy
		// display server and the caller waited out the full timeout - five minutes of
		// a CI job spent asking a question no one could see.
		DisplayServer *display = DisplayServer::get_singleton();
		if (!display || display->get_name() == "headless") {
			r_error.set(MCPToolError::UNSUPPORTED,
					"this editor is running headless, so nobody can answer the question; "
					"decide from what you can observe or leave the decision in the report");
			return Dictionary();
		}

		// `get()`, not subscript: reading a missing optional key off a const Dictionary
		// inserts a null, and a missing timeout would then come back as 0 - a question
		// the user gets one second to answer.
		const int timeout = MAX(1, (int)p_arguments.get("timeout_seconds", 300));
		const MCPDeferred::Token token = MCPDeferred::begin((double)timeout);

		MCPAskUserDialog *dialog = memnew(MCPAskUserDialog(token, question,
				p_arguments.get("context", String()), p_arguments.get("choices", Array())));
		EditorNode::get_singleton()->get_gui_base()->add_child(dialog);
		dialog->popup_centered();

		// The answer arrives later; the service holds the client's request until then.
		return MCPDeferred::make_deferred_result(token);
	}
};

} // namespace

void mcp_register_ask_user_tool() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);

	registry->register_tool(Ref<MCPTool>(memnew(AskUserTool)));
}
