/**************************************************************************/
/*  mcp_proposal_tools.cpp                                                */
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

// Proposing a plan and getting it decided.
//
// **This tool applies nothing.** Every guarantee the module makes is enforced in the
// protocol's call path: the user's hold, the permission decision, the checkpoint, the
// audit record. A tool that ran other tools would bypass all four, so this one validates
// the plan, groups it by risk, asks the user, and hands back what was approved. The agent
// then makes those calls the ordinary way, each with its own check and its own record.
//
// It is also why the approval here is worth something: the user is agreeing to a list of
// concrete calls with their real arguments, which is what they will get.

#include "mcp_builtin_tools.h"

#include "../mcp_deferred.h"
#include "../mcp_proposals.h"
#include "../mcp_schema.h"
#include "../mcp_tool_registry.h"

#include "core/object/callable_mp.h"

#ifdef TOOLS_ENABLED
#include "editor/editor_node.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/separator.h"
#endif

namespace {

#ifdef TOOLS_ENABLED

// One dialog, one decision per group.
//
// The user's instruction was "not 40 separate approvals", and this is where that is
// either honoured or not: forty mechanical edits arrive as one checkbox, and the delete
// among them arrives as its own. A dialog that listed forty checkboxes would have met the
// letter of D1 and missed the whole point of it.
class MCPProposalDialog : public AcceptDialog {
	GDCLASS(MCPProposalDialog, AcceptDialog);

	MCPDeferred::Token token = MCPDeferred::INVALID_TOKEN;
	Dictionary plan;
	Vector<CheckBox *> boxes;
	bool answered = false;

	void _decide(bool p_approved_any) {
		if (answered) {
			return;
		}
		answered = true;

		Array approved_groups;
		Array approved_items;
		Array rejected_items;
		const Array groups = plan["groups"];
		for (int i = 0; i < groups.size() && i < boxes.size(); i++) {
			const Dictionary group = groups[i];
			const Array indices = group["items"];
			const bool ticked = p_approved_any && boxes[i]->is_pressed();
			if (ticked) {
				approved_groups.push_back(group["key"]);
			}
			for (int j = 0; j < indices.size(); j++) {
				(ticked ? approved_items : rejected_items).push_back(indices[j]);
			}
		}

		Dictionary result = plan.duplicate(true);
		result["approved_groups"] = approved_groups;
		result["approved_items"] = approved_items;
		result["rejected_items"] = rejected_items;
		result["decided"] = true;
		result["cancelled"] = !p_approved_any;

		// The calls to make, in the order they were proposed. Handed back rather than run:
		// see the note at the top of this file.
		Array calls;
		const Array items = plan["items"];
		for (int i = 0; i < items.size(); i++) {
			const Dictionary item = items[i];
			if (!approved_items.has(item["index"])) {
				continue;
			}
			Dictionary call;
			call["index"] = item["index"];
			call["tool"] = item["tool"];
			call["arguments"] = item["arguments"];
			call["description"] = item["description"];
			calls.push_back(call);
		}
		result["calls"] = calls;
		result["next"] = calls.is_empty()
				? "Nothing was approved. Do not make any of these calls; say what was declined "
				  "and stop, or propose something different."
				: "Make exactly these calls, in this order, and nothing else from the plan. "
				  "Each one still goes through its own permission check and checkpoint.";

		MCPDeferred::complete(token, result);
		hide();
		queue_free();
	}

	void _approve() { _decide(true); }
	void _cancelled() { _decide(false); }

protected:
	void _notification(int p_what) {
		switch (p_what) {
			case NOTIFICATION_READY: {
				set_process(true);
			} break;

			case NOTIFICATION_PROCESS: {
				// The client's request has a deadline this dialog knows nothing about. Once
				// it passes the service answers the token without us and every button here
				// goes inert - so the dialog leaves with it rather than sitting there
				// inviting a decision it can no longer deliver.
				if (!answered && !MCPDeferred::is_pending(token)) {
					answered = true;
					hide();
					queue_free();
				}
			} break;

			case NOTIFICATION_PREDELETE: {
				if (!answered) {
					MCPDeferred::fail(token, MCPToolError::INVALID_STATE,
							"the editor closed the plan without a decision; nothing was approved");
				}
			} break;
		}
	}

public:
	// Wide enough to read, and every wrapping label is told this width explicitly.
	//
	// Not cosmetic. A Label with autowrap reports its minimum *width* as its longest word,
	// and then its minimum *height* as the height that text needs at that width - which for
	// a paragraph is enormous. Left to itself this dialog computed a minimum height of 1698
	// pixels on an 800-pixel screen and put its own buttons a thousand pixels below the
	// bottom of the display, where nothing could press them. Every wrapping label below
	// carries this width so it wraps where it is actually drawn.
	static constexpr int CONTENT_WIDTH = 540;

	static Label *wrapped(const String &p_text) {
		Label *label = memnew(Label);
		label->set_text(p_text);
		label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		label->set_custom_minimum_size(Size2(CONTENT_WIDTH, 0));
		return label;
	}

	MCPProposalDialog(MCPDeferred::Token p_token, const Dictionary &p_plan) :
			token(p_token), plan(p_plan) {
		set_title(TTR("Godot AI proposes a change"));
		set_min_size(Size2(CONTENT_WIDTH + 40, 0));
		// A ceiling as well as a floor, so a plan of forty changes cannot grow the window
		// off the screen however carefully the labels are sized.
		set_max_size(Size2i(CONTENT_WIDTH + 80, 640));

		VBoxContainer *layout = memnew(VBoxContainer);
		add_child(layout);

		layout->add_child(wrapped(plan.get("title", String())));

		Label *summary = wrapped(vformat(TTR("%d change(s). Tick what you want done; anything "
											 "left unticked will not happen."),
				(int)plan.get("item_count", 0)));
		summary->set_modulate(Color(1, 1, 1, 0.7));
		layout->add_child(summary);

		layout->add_child(memnew(HSeparator));

		ScrollContainer *scroll = memnew(ScrollContainer);
		scroll->set_custom_minimum_size(Size2(CONTENT_WIDTH, 260));
		scroll->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
		scroll->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		layout->add_child(scroll);

		VBoxContainer *list = memnew(VBoxContainer);
		list->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		list->set_custom_minimum_size(Size2(CONTENT_WIDTH, 0));
		scroll->add_child(list);

		const Array groups = plan["groups"];
		const Array items = plan["items"];
		for (int i = 0; i < groups.size(); i++) {
			const Dictionary group = groups[i];
			const String risk = group["risk"];

			CheckBox *box = memnew(CheckBox);
			box->set_text(String(group["description"]));
			box->set_custom_minimum_size(Size2(CONTENT_WIDTH, 0));
			box->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
			// Reversible, narrow changes start ticked; anything else starts unticked. A
			// dialog that pre-approves what it cannot take back is not asking.
			box->set_pressed(risk == "mechanical");
			list->add_child(box);
			boxes.push_back(box);

			// Every call, spelled out under its group. The approval is worth something only
			// if what was agreed to is visible: "12 changes" is not a thing anyone can
			// consent to.
			const Array indices = group["items"];
			for (int j = 0; j < indices.size(); j++) {
				for (int k = 0; k < items.size(); k++) {
					const Dictionary item = items[k];
					if ((int)item["index"] != (int)indices[j]) {
						continue;
					}
					Label *line = wrapped(vformat("    %s  —  %s", String(item["description"]),
							String(item["tool"])));
					line->set_modulate(Color(1, 1, 1, 0.75));
					list->add_child(line);

					if (risk != "mechanical") {
						Label *why = wrapped(vformat("        %s", String(item["risk_reason"])));
						why->set_modulate(Color(1, 0.85, 0.6, 0.9));
						list->add_child(why);
					}
				}
			}
			if (i + 1 < groups.size()) {
				list->add_child(memnew(HSeparator));
			}
		}

		// The two actions live in the dialog's *body*, not in AcceptDialog's own button
		// bar, and its OK button is hidden.
		//
		// Not a style choice. A click on the built-in bar never reached this dialog under a
		// bare Xvfb - the window is where X says it is, the button is where Godot says it
		// is, the coordinates agree exactly, and the press simply does not arrive. Buttons
		// in the body are the shape Godot_AskUser already uses and the one thing here that
		// demonstrably works, so an automated check can press this dialog the same way a
		// person would rather than the feature going unverified.
		get_ok_button()->hide();
		layout->add_child(memnew(HSeparator));

		HBoxContainer *actions = memnew(HBoxContainer);
		actions->set_alignment(BoxContainer::ALIGNMENT_END);
		layout->add_child(actions);

		Button *decline = memnew(Button);
		decline->set_text(TTR("Leave It"));
		decline->connect("pressed", callable_mp(this, &MCPProposalDialog::_cancelled));
		actions->add_child(decline);

		Button *apply = memnew(Button);
		apply->set_text(TTR("Apply Ticked"));
		apply->connect("pressed", callable_mp(this, &MCPProposalDialog::_approve));
		actions->add_child(apply);

		set_close_on_escape(true);
		connect("canceled", callable_mp(this, &MCPProposalDialog::_cancelled));
	}
};

#endif // TOOLS_ENABLED

class ProposeChangeTool : public MCPTool {
	// Builds the plan, refusing anything that could not actually be run.
	//
	// Validated here rather than at apply time on purpose: discovering that item 37 names
	// a tool that does not exist, after thirty-six edits have already happened, is the
	// failure this whole tool is meant to prevent.
	bool _build(const String &p_title, const Array &p_changes, Vector<MCPProposals::Item> &r_items,
			String &r_error) {
		MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
		if (!registry) {
			r_error = "the tool registry is not available";
			return false;
		}

		for (int i = 0; i < p_changes.size(); i++) {
			if (Variant(p_changes[i]).get_type() != Variant::DICTIONARY) {
				r_error = vformat("change %d is not an object", i);
				return false;
			}
			const Dictionary change = p_changes[i];

			MCPProposals::Item item;
			item.index = i;
			item.description = String(change.get("description", String())).strip_edges();
			item.tool = String(change.get("tool", String())).strip_edges();
			item.arguments = change.get("arguments", Dictionary());

			if (item.description.is_empty()) {
				// A plan is for a person to read. An unlabelled call is not a proposal, it
				// is a request to approve something nobody described.
				r_error = vformat("change %d has no 'description'; a plan a person cannot read "
								  "is not a plan",
						i);
				return false;
			}
			if (item.tool.is_empty()) {
				r_error = vformat("change %d has no 'tool'", i);
				return false;
			}

			const Ref<MCPTool> tool = registry->get_tool(item.tool);
			if (tool.is_null()) {
				r_error = vformat("change %d names '%s', which is not a tool", i, item.tool);
				return false;
			}

			// The same schema discovery advertises, so a plan cannot be approved for a call
			// the schema would then reject. Schema-level and no further: a rule a tool
			// applies only when it runs - Godot_ManageNode wanting a `path` for a rename
			// but not for every action - is not in the schema and is not caught here. The
			// tool's description says so rather than letting a green plan imply more.
			Variant validated;
			String schema_error;
			if (!MCPSchema::validate(item.arguments, tool->get_input_schema(), validated,
						schema_error)) {
				r_error = vformat("change %d (%s): %s", i, item.tool, schema_error);
				return false;
			}
			if (validated.get_type() == Variant::DICTIONARY) {
				// Defaults filled in, so the plan shows the arguments the call will really
				// carry rather than the ones that were typed.
				item.arguments = validated;
			}

			item.files = tool->get_checkpoint_paths(item.arguments);
			item.subjects = tool->get_activity_subjects(item.arguments);
			item.risk = MCPProposals::classify(tool->get_capability(), item.files, item.tool,
					item.arguments, item.risk_reason);
			item.group = MCPProposals::group_key(item);
			r_items.push_back(item);
		}
		return true;
	}

public:
	virtual String get_tool_name() const override { return "Godot_ProposeChange"; }
	virtual String get_description() const override {
		return "Put a plan to the user before doing any of it. Takes an ordered list of changes, "
			   "each with a description and the exact tool call that would perform it, checks "
			   "every one against the real tool and its real schema, groups them by how risky "
			   "they are, and shows them in the editor for a decision. Reversible narrow changes "
			   "are one tick together; anything that cannot be undone is asked about on its own. "
			   "It applies nothing: it returns the approved calls for you to make normally, so "
			   "each keeps its own permission check, checkpoint and audit record. Use it for "
			   "design work - a refactor, a batch rename, a cleanup - where being told afterwards "
			   "is too late. The check is schema-level: it catches a tool that does not exist, a "
			   "missing required argument and a wrongly typed one, but not a rule a tool only "
			   "applies when it runs, so an approved call can still fail.";
	}
	// Proposing changes nothing; the plan is a question. The changes themselves are made
	// afterwards by their own tools, under their own capabilities.
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary change_properties;
		change_properties["description"] = MCPSchema::string_property(
				"What this change does, in words a person can judge. Required.");
		change_properties["tool"] = MCPSchema::string_property(
				"The tool that would perform it, such as Godot_ManageNode. Required.");
		change_properties["arguments"] = MCPSchema::object_schema(
				Dictionary(), Vector<String>(), true);
		Vector<String> change_required;
		change_required.push_back("description");
		change_required.push_back("tool");

		Dictionary properties;
		properties["title"] = MCPSchema::string_property(
				"What the plan as a whole is for, in one line.");
		properties["changes"] = MCPSchema::array_property(
				"The changes, in the order they would be made.",
				MCPSchema::object_schema(change_properties, change_required));
		properties["timeout_seconds"] = MCPSchema::integer_property(
				"How long to wait for a decision.", 300);
		properties["dry_run"] = MCPSchema::bool_property(
				"Build and classify the plan without asking anybody. Use it to check a plan is "
				"well-formed before putting it to the user.",
				false);
		Vector<String> required;
		required.push_back("title");
		required.push_back("changes");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["title"] = MCPSchema::string_property("The plan's title.");
		properties["items"] = MCPSchema::array_property("Every change, with its risk and group.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["groups"] = MCPSchema::array_property(
				"The decisions the user is actually offered, one per group.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["item_count"] = MCPSchema::integer_property("How many changes were proposed.");
		properties["irreversible_count"] = MCPSchema::integer_property(
				"How many of them nothing could put back.");
		properties["decided"] = MCPSchema::bool_property("True once the user has answered.");
		properties["approved_items"] = MCPSchema::array_property("Indices the user approved.",
				MCPSchema::integer_property("An item index."));
		properties["rejected_items"] = MCPSchema::array_property("Indices the user did not.",
				MCPSchema::integer_property("An item index."));
		properties["calls"] = MCPSchema::array_property(
				"The approved calls to make, in order. Making anything else is not what was "
				"agreed to.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		// get() rather than operator[]: a missing key read through a const Dictionary
		// inserts a null, which schema validation then rejects as wrongly typed.
		const String title = String(p_arguments.get("title", String())).strip_edges();
		const Array changes = p_arguments.get("changes", Array());
		if (changes.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					"a proposal needs at least one change; there is nothing to decide otherwise");
			return Dictionary();
		}

		Vector<MCPProposals::Item> items;
		String build_error;
		if (!_build(title, changes, items, build_error)) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, build_error);
			return Dictionary();
		}

		Dictionary plan = MCPProposals::to_dictionary(title, items);

		if ((bool)p_arguments.get("dry_run", false)) {
			plan["decided"] = false;
			plan["dry_run"] = true;
			plan["next"] = "Nobody has been asked. Call again without dry_run to put this to the "
						   "user.";
			return plan;
		}

#ifdef TOOLS_ENABLED
		if (!EditorNode::get_singleton()) {
			r_error.set(MCPToolError::UNSUPPORTED,
					"there is no editor here to show the plan in; run without --headless, or "
					"pass dry_run to build and check the plan without asking anybody");
			return Dictionary();
		}

		const int timeout = MAX(1, (int)p_arguments.get("timeout_seconds", 300));
		const MCPDeferred::Token token = MCPDeferred::begin((double)timeout,
				"nobody decided on the plan in time; nothing was approved and nothing was done");

		MCPProposalDialog *dialog = memnew(MCPProposalDialog(token, plan));
		EditorNode::get_singleton()->get_gui_base()->add_child(dialog);
		dialog->popup_centered();
		return MCPDeferred::make_deferred_result(token);
#else
		r_error.set(MCPToolError::UNSUPPORTED,
				"showing a plan needs a running editor; pass dry_run to build and check one "
				"without asking anybody");
		return Dictionary();
#endif
	}
};

} // namespace

void mcp_register_proposal_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->register_tool(Ref<MCPTool>(memnew(ProposeChangeTool)));
}
