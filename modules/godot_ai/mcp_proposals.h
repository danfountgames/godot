/**************************************************************************/
/*  mcp_proposals.h                                                       */
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

#ifndef MCP_PROPOSALS_H
#define MCP_PROPOSALS_H

#include "mcp_types.h"

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

// A plan of concrete edits, shown before any of them happen.
//
// The requirement (D1) is that design work is *proposed and chosen*, not executed and
// reported. The failure this prevents is the one everybody has had: an agent explains
// what it did, and half of it was not what you wanted.
//
// **What this does not do: apply anything.** Every guarantee this module makes lives in
// the protocol's call path - the user's hold is checked there, the permission decision is
// made there, the checkpoint is taken there, the audit record is written there. A tool
// that ran other tools would bypass all four, so this one does not. It validates the plan,
// classifies it, asks the user about it, and hands back what was approved; the calls are
// then made the ordinary way, each with its own check, checkpoint and record.
//
// **Grouping is the point.** The user's instruction was "not 40 separate approvals". So an
// item's risk is computed from what it can actually do - its capability, whether anything
// snapshots it, whether it destroys something - and items of the same risk over the same
// subject are approved together. Forty renames become one decision; the single delete in
// the middle of them stays its own.
class MCPProposals {
public:
	enum Risk {
		// Reversible and narrow: undo or a checkpoint puts it back, and it touches one
		// thing. Safe to approve as a batch.
		RISK_MECHANICAL,
		// Reversible, but broad or destructive enough that a batch approval would hide
		// which part you agreed to.
		RISK_SUBSTANTIAL,
		// Nothing puts it back. Asked about on its own, always.
		RISK_IRREVERSIBLE,
	};

	static String risk_to_string(Risk p_risk);
	static Risk risk_from_string(const String &p_text);

	// One planned edit.
	struct Item {
		int index = 0;
		String description;
		String tool;
		Dictionary arguments;
		Risk risk = RISK_MECHANICAL;
		String risk_reason;
		// Files the tool declared it may write, and the nodes and files it touches.
		Vector<String> files;
		Array subjects;
		String group;
		// "proposed" until the user decides; then "approved" or "rejected".
		String state = "proposed";
	};

	// How risky an invocation is, from what it can actually do.
	//
	// Computed rather than declared per tool: a tool author who has to remember to mark
	// something dangerous will one day forget, and the capability class and the checkpoint
	// declaration are both already mandatory and both already say most of it.
	static Risk classify(MCPCapability p_capability, const Vector<String> &p_files,
			const String &p_tool, const Dictionary &p_arguments, String &r_reason);

	// True when this call destroys something rather than adding or changing it. Read from
	// the tool's name and its `action` argument, which is how every tool in this module
	// spells it.
	static bool is_destructive(const String &p_tool, const Dictionary &p_arguments);

	// Which approval an item belongs to. Two items with the same key are one decision.
	//
	// Mechanical items share a single key, whatever they touch: that is what turns forty
	// renames into one approval. Substantial items are keyed by their subject, so the
	// edits to one scene travel together and the edits to another do not. Irreversible
	// items are keyed by their own index and therefore never share.
	static String group_key(const Item &p_item);

	// The groups a plan resolves into, in the order they were first seen: one entry of
	// `{ key, risk, description, items: [index], files, subjects }` each.
	static Array build_groups(const Vector<Item> &p_items);

	// A sentence naming what the user is being asked to decide. Says the count and the
	// worst risk in it, because "12 changes" and "12 changes, one of which deletes a file"
	// are different questions.
	static String describe_group(const Array &p_group_items, Risk p_risk,
			const Vector<Item> &p_items);

	// The whole plan as a reply: items, groups, and the counts a reader needs first.
	static Dictionary to_dictionary(const String &p_title, const Vector<Item> &p_items);
};

#endif // MCP_PROPOSALS_H
