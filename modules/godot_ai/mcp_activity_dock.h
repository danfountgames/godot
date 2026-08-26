/**************************************************************************/
/*  mcp_activity_dock.h                                                   */
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

#ifndef MCP_ACTIVITY_DOCK_H
#define MCP_ACTIVITY_DOCK_H

#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/tree.h"

// The Agent Activity dock.
//
// Deliberately small. It answers six questions and stops:
//
//   1. What is the agent trying to achieve?   - the declared goal
//   2. What is it doing right now?            - the declared activity, and the in-flight call
//   3. What is it touching?                   - the nodes and files of the selected record
//   4. What has changed?                      - the checkpoint behind the selected record
//   5. Can I stop it?                         - Pause and Stop, enforced in the protocol
//   6. Can I undo *that* change?              - Revert, on the selected record's checkpoint
//
// No scrubber, no timeline, no animation. Those are worth building once the workflow they
// would decorate has been proven, and not before.
//
// It is a *presentation*, not a store. Everything it shows comes from `MCPActivity`,
// `MCPAgentIntent` and `MCPAgentControl`, all of which are readable without an editor and
// exposed as tools - so the same state is testable headlessly and consumable by anything
// else that wants it. Adding a fact to the dock means adding it to the stream first.
class MCPActivityDock : public VBoxContainer {
	GDCLASS(MCPActivityDock, VBoxContainer);

	Label *goal_label = nullptr;
	Label *activity_label = nullptr;
	Label *state_label = nullptr;
	Button *pause_button = nullptr;
	Button *stop_button = nullptr;
	Button *resume_button = nullptr;
	Tree *records = nullptr;
	RichTextLabel *detail = nullptr;
	Button *reveal_button = nullptr;
	Button *diff_button = nullptr;
	Button *revert_button = nullptr;

	// Highest sequence already in the tree, so a poll appends rather than rebuilds. A
	// rebuild would lose the user's selection and scroll position every half second.
	int64_t last_sequence = 0;
	double poll_accumulator = 0.0;

	// The record the user has selected, by sequence. Kept rather than reading the Tree,
	// so the buttons still know their subject while the tree is being appended to.
	int64_t selected_sequence = 0;
	String selected_checkpoint;

	void _pause_pressed();
	void _stop_pressed();
	void _resume_pressed();
	void _selection_changed();
	void _reveal_pressed();
	void _diff_pressed();
	void _revert_pressed();

	void _append_new_records();
	void _update_header();
	void _update_selection_controls();
	Dictionary _selected_record() const;

protected:
	void _notification(int p_what);

public:
	// Re-reads everything. Cheap; called on a timer and after the user acts.
	void refresh();

	MCPActivityDock();
};

#endif // MCP_ACTIVITY_DOCK_H
