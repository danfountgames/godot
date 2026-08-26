/**************************************************************************/
/*  mcp_activity_dock.cpp                                                 */
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

#include "mcp_activity_dock.h"

#include "mcp_activity.h"
#include "mcp_agent_state.h"
#include "mcp_checkpoints.h"

#include "core/object/callable_mp.h"

#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "scene/gui/separator.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"

namespace {

// One line per record, short enough to scan. The full text goes in the detail pane.
String summarise(const Dictionary &p_record) {
	const String intent = p_record.get("intent", String());
	if (!intent.is_empty()) {
		// What the agent said it was doing beats the tool name, which is why it is asked
		// for. "Measuring the current jump height" reads; "Godot_GetRuntimeProperty(...)"
		// does not.
		return intent;
	}
	const String summary = p_record.get("summary", String());
	return summary.is_empty() ? String(p_record.get("tool", String())) : summary;
}

String subjects_line(const Dictionary &p_record) {
	const Array subjects = p_record.get("subjects", Array());
	String out;
	for (int i = 0; i < subjects.size(); i++) {
		const Dictionary subject = subjects[i];
		if (!out.is_empty()) {
			out += ", ";
		}
		out += String(subject.get("path", String()));
	}
	return out;
}

} // namespace

MCPActivityDock::MCPActivityDock() {
	set_name("Agent Activity");
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);

	// --- what is it trying to achieve, and what is it doing now ---------------
	goal_label = memnew(Label);
	goal_label->set_theme_type_variation("HeaderSmall");
	goal_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	add_child(goal_label);

	activity_label = memnew(Label);
	activity_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	add_child(activity_label);

	// --- can I stop it --------------------------------------------------------
	HBoxContainer *controls = memnew(HBoxContainer);
	add_child(controls);

	state_label = memnew(Label);
	state_label->set_h_size_flags(SIZE_EXPAND_FILL);
	controls->add_child(state_label);

	pause_button = memnew(Button);
	pause_button->set_text(TTR("Pause"));
	pause_button->set_tooltip_text(
			TTR("Refuse the agent's next tool call until you resume. Reads that explain what it "
				"already did keep working."));
	pause_button->connect(SceneStringName(pressed), callable_mp(this, &MCPActivityDock::_pause_pressed));
	controls->add_child(pause_button);

	stop_button = memnew(Button);
	stop_button->set_text(TTR("Stop"));
	stop_button->set_tooltip_text(TTR("Stop the agent acting. Needs an explicit resume."));
	stop_button->connect(SceneStringName(pressed), callable_mp(this, &MCPActivityDock::_stop_pressed));
	controls->add_child(stop_button);

	resume_button = memnew(Button);
	resume_button->set_text(TTR("Resume"));
	resume_button->connect(SceneStringName(pressed), callable_mp(this, &MCPActivityDock::_resume_pressed));
	controls->add_child(resume_button);

	add_child(memnew(HSeparator));

	// --- what has it been doing ----------------------------------------------
	records = memnew(Tree);
	records->set_h_size_flags(SIZE_EXPAND_FILL);
	records->set_v_size_flags(SIZE_EXPAND_FILL);
	records->set_hide_root(true);
	records->set_columns(2);
	records->set_column_titles_visible(true);
	records->set_column_title(0, TTR("What happened"));
	records->set_column_title(1, TTR("Result"));
	records->set_column_expand(0, true);
	records->set_column_expand(1, false);
	records->set_column_custom_minimum_width(1, 90);
	records->create_item();
	records->connect("item_selected", callable_mp(this, &MCPActivityDock::_selection_changed));
	add_child(records);

	// --- what did it touch, what changed --------------------------------------
	detail = memnew(RichTextLabel);
	detail->set_custom_minimum_size(Size2(0, 110));
	detail->set_h_size_flags(SIZE_EXPAND_FILL);
	detail->set_selection_enabled(true);
	add_child(detail);

	HBoxContainer *actions = memnew(HBoxContainer);
	add_child(actions);

	reveal_button = memnew(Button);
	reveal_button->set_text(TTR("Reveal"));
	reveal_button->set_tooltip_text(
			TTR("Select what this call touched in the Scene and FileSystem docks."));
	reveal_button->connect(SceneStringName(pressed), callable_mp(this, &MCPActivityDock::_reveal_pressed));
	actions->add_child(reveal_button);

	diff_button = memnew(Button);
	diff_button->set_text(TTR("What Changed"));
	diff_button->set_tooltip_text(TTR("List the files this call's checkpoint captured."));
	diff_button->connect(SceneStringName(pressed), callable_mp(this, &MCPActivityDock::_diff_pressed));
	actions->add_child(diff_button);

	revert_button = memnew(Button);
	revert_button->set_text(TTR("Revert This"));
	revert_button->set_tooltip_text(
			TTR("Put back the files this one call changed, using the checkpoint taken before it "
				"ran. Later calls are not undone."));
	revert_button->connect(SceneStringName(pressed), callable_mp(this, &MCPActivityDock::_revert_pressed));
	actions->add_child(revert_button);

	set_process(true);
	refresh();
}

Dictionary MCPActivityDock::_selected_record() const {
	if (selected_sequence <= 0) {
		return Dictionary();
	}
	// One short read rather than caching the record: it can change under us when a
	// deferred call finishes, and a stale copy would offer a revert for the wrong state.
	const Array recent = MCPActivity::snapshot(selected_sequence - 1, 1);
	if (recent.is_empty()) {
		return Dictionary();
	}
	const Dictionary record = recent[0];
	return (int64_t)record.get("sequence", 0) == selected_sequence ? record : Dictionary();
}

void MCPActivityDock::_pause_pressed() {
	MCPAgentControl::pause();
	refresh();
}

void MCPActivityDock::_stop_pressed() {
	MCPAgentControl::stop();
	refresh();
}

void MCPActivityDock::_resume_pressed() {
	MCPAgentControl::resume();
	refresh();
}

void MCPActivityDock::_selection_changed() {
	TreeItem *item = records->get_selected();
	selected_sequence = item ? (int64_t)item->get_metadata(0) : 0;

	const Dictionary record = _selected_record();
	selected_checkpoint = record.get("checkpoint", String());

	String text;
	if (record.is_empty()) {
		text = TTR("Select a call to see what it touched.");
	} else {
		text = vformat("[b]%s[/b]\n", String(record.get("tool", String())));
		text += vformat("%s\n", String(record.get("summary", String())));
		const String intent = record.get("intent", String());
		if (!intent.is_empty()) {
			text += vformat("\n%s: %s\n", TTR("Said it was"), intent);
		}
		const String touched = subjects_line(record);
		text += vformat("\n%s: %s\n", TTR("Touched"),
				touched.is_empty() ? String(TTR("nothing it named")) : touched);
		text += vformat("%s: %s", TTR("Outcome"), String(record.get("outcome", String())));
		const String detail_text = record.get("detail", String());
		if (!detail_text.is_empty()) {
			text += vformat(" - %s", detail_text);
		}
		if (selected_checkpoint.is_empty()) {
			text += vformat("\n%s", TTR("No checkpoint: this call changed no files."));
		}
	}
	detail->set_text(text);
	_update_selection_controls();
}

void MCPActivityDock::_reveal_pressed() {
	const Dictionary record = _selected_record();
	const Array subjects = record.get("subjects", Array());
	EditorInterface *interface = EditorInterface::get_singleton();
	if (!interface) {
		return;
	}
	for (int i = 0; i < subjects.size(); i++) {
		const Dictionary subject = subjects[i];
		const String kind = subject.get("kind", String());
		const String path = subject.get("path", String());
		if (kind == "file" && path.begins_with("res://")) {
			interface->select_file(path);
		} else if (kind == "node") {
			// The edited scene, not the running game: this reveals what the call touched
			// in the project, and a runtime node may not exist any more.
			Node *root = interface->get_edited_scene_root();
			Node *node = root ? root->get_node_or_null(NodePath(path)) : nullptr;
			if (!node && root) {
				// Records often carry the path as the agent wrote it, which may include
				// the scene root. Try again below it.
				node = root->get_node_or_null(NodePath(path.trim_prefix(String(root->get_name()) + "/")));
			}
			if (node) {
				interface->edit_node(node);
			}
		}
	}
}

void MCPActivityDock::_diff_pressed() {
	if (selected_checkpoint.is_empty()) {
		detail->set_text(TTR("This call took no checkpoint, so it changed no files."));
		return;
	}
	String text = vformat("[b]%s[/b] %s\n", TTR("Checkpoint"), selected_checkpoint);
	bool found = false;
	for (const Variant &entry : MCPCheckpoints::list()) {
		const Dictionary checkpoint = entry;
		if (String(checkpoint.get("id", String())) != selected_checkpoint) {
			continue;
		}
		found = true;
		const Array files = checkpoint.get("files", Array());
		for (int i = 0; i < files.size(); i++) {
			const Dictionary file = files[i];
			const bool existed = file.get("existed", false);
			text += vformat("\n%s  (%s)", String(file.get("path", String())),
					existed ? TTR("changed") : TTR("created by this call"));
		}
		if (files.is_empty()) {
			text += vformat("\n%s", TTR("The checkpoint recorded no files."));
		}
		break;
	}
	if (!found) {
		text += vformat("\n%s", TTR("That checkpoint is no longer on disk; it may have been pruned."));
	}
	detail->set_text(text);
}

void MCPActivityDock::_revert_pressed() {
	if (selected_checkpoint.is_empty()) {
		return;
	}
	int restored = 0;
	int removed = 0;
	String error;
	if (!MCPCheckpoints::restore(selected_checkpoint, restored, removed, error)) {
		detail->set_text(vformat("%s: %s", TTR("Could not revert"), error));
		return;
	}
	// Say exactly what happened, including that this is one call and not a session undo.
	detail->set_text(vformat(
			TTR("Reverted %d file(s), removed %d the call had created. Only this call was undone; "
				"anything done after it still stands."),
			restored, removed));
	refresh();
}

void MCPActivityDock::_update_selection_controls() {
	const bool has_record = selected_sequence > 0;
	const bool has_checkpoint = has_record && !selected_checkpoint.is_empty();
	reveal_button->set_disabled(!has_record);
	diff_button->set_disabled(!has_record);
	revert_button->set_disabled(!has_checkpoint);
}

void MCPActivityDock::_update_header() {
	const String goal = MCPAgentIntent::get_goal();
	const String activity = MCPAgentIntent::get_activity();
	goal_label->set_text(goal.is_empty() ? String(TTR("No goal declared")) : goal);

	if (!activity.is_empty()) {
		activity_label->set_text(activity);
	} else if (MCPActivity::has_running()) {
		activity_label->set_text(TTR("Working."));
	} else {
		activity_label->set_text(TTR("Idle."));
	}

	const MCPAgentControl::State state = MCPAgentControl::get_state();
	String state_text = MCPAgentControl::state_to_string(state);
	const String reason = MCPAgentControl::get_reason();
	if (!reason.is_empty()) {
		state_text += vformat(" - %s", reason);
	}
	state_label->set_text(state_text);

	// Only one of these is ever the useful next action.
	const bool running = state == MCPAgentControl::STATE_RUNNING;
	pause_button->set_disabled(!running);
	stop_button->set_disabled(state == MCPAgentControl::STATE_STOPPED);
	resume_button->set_disabled(running);
}

void MCPActivityDock::_append_new_records() {
	// Append rather than rebuild: a rebuild every poll would throw away the user's
	// selection and scroll position twice a second, which makes the dock unusable
	// exactly when there is something to read.
	const Array fresh = MCPActivity::snapshot(last_sequence, MCPActivity::DEFAULT_CAPACITY);
	TreeItem *root = records->get_root();
	if (!root) {
		return;
	}
	for (int i = 0; i < fresh.size(); i++) {
		const Dictionary record = fresh[i];
		TreeItem *item = records->create_item(root);
		item->set_text(0, summarise(record));
		item->set_text(1, record.get("outcome", String()));
		item->set_metadata(0, record.get("sequence", 0));
		item->set_tooltip_text(0, record.get("summary", String()));
		last_sequence = MAX(last_sequence, (int64_t)record.get("sequence", 0));
	}
	if (!fresh.is_empty()) {
		// A record whose outcome changed after it was added - a call that finished, or a
		// deferred one - would otherwise keep its old text.
		refresh();
	}
}

void MCPActivityDock::refresh() {
	_update_header();

	// Outcomes change in place, so walk what is already shown and update it. Cheap: the
	// buffer is bounded and this only touches rows that are visible at all.
	TreeItem *root = records->get_root();
	if (root) {
		const Array current = MCPActivity::snapshot(0, MCPActivity::DEFAULT_CAPACITY);
		HashMap<int64_t, Dictionary> by_sequence;
		for (int i = 0; i < current.size(); i++) {
			const Dictionary record = current[i];
			by_sequence.insert((int64_t)record.get("sequence", 0), record);
		}
		for (TreeItem *item = root->get_first_child(); item; item = item->get_next()) {
			const int64_t sequence = item->get_metadata(0);
			HashMap<int64_t, Dictionary>::Iterator found = by_sequence.find(sequence);
			if (found) {
				item->set_text(0, summarise(found->value));
				item->set_text(1, found->value.get("outcome", String()));
			}
		}
	}
	_update_selection_controls();
}

void MCPActivityDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_PROCESS: {
			// Polled rather than signalled. Records are opened and closed from the
			// protocol layer, from deferred completions and from refusals; a missed
			// signal would leave the dock claiming a call is still running when it is
			// not, which is the one thing it must not do.
			poll_accumulator += get_process_delta_time();
			if (poll_accumulator >= 0.5) {
				poll_accumulator = 0.0;
				if (MCPActivity::get_latest_sequence() > last_sequence) {
					_append_new_records();
				} else {
					refresh();
				}
			}
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			refresh();
		} break;
	}
}
