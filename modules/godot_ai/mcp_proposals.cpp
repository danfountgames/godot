/**************************************************************************/
/*  mcp_proposals.cpp                                                     */
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

#include "mcp_proposals.h"

#include "core/variant/variant.h"

String MCPProposals::risk_to_string(Risk p_risk) {
	switch (p_risk) {
		case RISK_MECHANICAL:
			return "mechanical";
		case RISK_SUBSTANTIAL:
			return "substantial";
		case RISK_IRREVERSIBLE:
			return "irreversible";
	}
	return "substantial";
}

MCPProposals::Risk MCPProposals::risk_from_string(const String &p_text) {
	if (p_text == "mechanical") {
		return RISK_MECHANICAL;
	}
	if (p_text == "irreversible") {
		return RISK_IRREVERSIBLE;
	}
	// Anything unrecognised is treated as the middle rather than the safest reading. A
	// typo must not quietly downgrade a decision.
	return RISK_SUBSTANTIAL;
}

bool MCPProposals::is_destructive(const String &p_tool, const Dictionary &p_arguments) {
	const String tool = p_tool.to_lower();
	if (tool.contains("delete") || tool.contains("remove") || tool.contains("erase")) {
		return true;
	}
	// Every tool in this module spells a destructive variant in its `action` argument, so
	// this is a reading of the interface rather than a guess about it. get() rather than
	// operator[]: a missing key read through a const Dictionary inserts a null.
	const String action = String(p_arguments.get("action", String())).to_lower();
	return action == "delete" || action == "remove" || action == "erase" || action == "clear";
}

MCPProposals::Risk MCPProposals::classify(MCPCapability p_capability,
		const Vector<String> &p_files, const String &p_tool, const Dictionary &p_arguments,
		String &r_reason) {
	if (p_capability == MCP_CAP_DANGEROUS_EXEC) {
		// Deny-by-default elsewhere; if one ever reaches a plan, it is never batched.
		r_reason = "runs with the dangerous_exec capability, which nothing here can undo";
		return RISK_IRREVERSIBLE;
	}

	if (!mcp_capability_is_mutating(p_capability)) {
		r_reason = "changes nothing in the project";
		return RISK_MECHANICAL;
	}

	const bool destructive = is_destructive(p_tool, p_arguments);

	if (p_capability == MCP_CAP_EDIT_SCENE) {
		// A scene edit lives in the editor's undo history until the scene is saved, which
		// is why these tools legitimately declare no checkpoint files. Reading "no files"
		// as "nothing can put it back" would misclassify the most ordinary edit there is.
		if (destructive) {
			r_reason = "removes something from the scene; undoable, but worth seeing on its own";
			return RISK_SUBSTANTIAL;
		}
		r_reason = "a scene edit, held in the editor's undo history until the scene is saved";
		return RISK_MECHANICAL;
	}

	if (p_files.is_empty()) {
		// A mutating tool that declares no files it may write is telling the protocol layer
		// there is nothing to snapshot. For a scene edit that is true and handled above;
		// for anything else it means no checkpoint will exist afterwards.
		r_reason = "writes something that no checkpoint will capture, so there is nothing to "
				   "restore from";
		return RISK_IRREVERSIBLE;
	}

	if (destructive) {
		r_reason = vformat("deletes %d file(s), which a checkpoint can restore - but only if "
						   "somebody notices in time",
				p_files.size());
		return RISK_SUBSTANTIAL;
	}

	if (p_files.size() > 1) {
		r_reason = vformat("writes %d files at once; approving it as one of a batch would hide "
						   "which of them you agreed to",
				p_files.size());
		return RISK_SUBSTANTIAL;
	}

	r_reason = vformat("writes '%s', behind a checkpoint", p_files[0]);
	return RISK_MECHANICAL;
}

String MCPProposals::group_key(const Item &p_item) {
	switch (p_item.risk) {
		case RISK_MECHANICAL:
			// One key for all of them, whatever they touch. This is the whole of "not 40
			// separate approvals": forty reversible, narrow edits are one decision.
			return "mechanical";
		case RISK_SUBSTANTIAL: {
			// Keyed by subject, so every edit to one scene is one decision and the edits to
			// another scene are a different one. A plan that reworks two scenes should not
			// make you agree to both at once.
			if (!p_item.files.is_empty()) {
				return "substantial:" + p_item.files[0];
			}
			for (int i = 0; i < p_item.subjects.size(); i++) {
				const Dictionary subject = p_item.subjects[i];
				const String path = subject.get("path", String());
				if (!path.is_empty()) {
					return "substantial:" + path;
				}
			}
			return "substantial:" + p_item.tool;
		}
		case RISK_IRREVERSIBLE:
			// Never shared. Its own index is in the key precisely so that two of them
			// cannot collapse into one checkbox.
			return vformat("irreversible:%d", p_item.index);
	}
	return vformat("item:%d", p_item.index);
}

Array MCPProposals::build_groups(const Vector<Item> &p_items) {
	Array groups;
	// Order of first appearance, so the plan reads in the order it was written rather than
	// in an order sorted by something the reader cannot see.
	Vector<String> seen;

	for (int i = 0; i < p_items.size(); i++) {
		const Item &item = p_items[i];
		const String key = item.group.is_empty() ? group_key(item) : item.group;

		int at = -1;
		for (int j = 0; j < seen.size(); j++) {
			if (seen[j] == key) {
				at = j;
				break;
			}
		}

		if (at < 0) {
			seen.push_back(key);
			Dictionary group;
			group["key"] = key;
			group["risk"] = risk_to_string(item.risk);
			group["items"] = Array();
			group["files"] = Array();
			group["subjects"] = Array();
			groups.push_back(group);
			at = seen.size() - 1;
		}

		Dictionary group = groups[at];
		Array indices = group["items"];
		indices.push_back(item.index);

		Array files = group["files"];
		for (int f = 0; f < item.files.size(); f++) {
			if (!files.has(item.files[f])) {
				files.push_back(item.files[f]);
			}
		}
		Array subjects = group["subjects"];
		for (int s = 0; s < item.subjects.size(); s++) {
			if (!subjects.has(item.subjects[s])) {
				subjects.push_back(item.subjects[s]);
			}
		}
	}

	for (int i = 0; i < groups.size(); i++) {
		Dictionary group = groups[i];
		group["description"] = describe_group(group["items"],
				risk_from_string(group["risk"]), p_items);
	}
	return groups;
}

String MCPProposals::describe_group(const Array &p_group_items, Risk p_risk,
		const Vector<Item> &p_items) {
	const int count = p_group_items.size();
	if (count == 0) {
		return String();
	}

	// A single item speaks for itself; repeating a count and a category over the top of it
	// would be noise.
	if (count == 1) {
		const int index = p_group_items[0];
		for (int i = 0; i < p_items.size(); i++) {
			if (p_items[i].index != index) {
				continue;
			}
			if (p_risk == RISK_IRREVERSIBLE) {
				return vformat("%s — nothing can put this back (%s)", p_items[i].description,
						p_items[i].risk_reason);
			}
			return p_items[i].description;
		}
		return String();
	}

	switch (p_risk) {
		case RISK_MECHANICAL:
			return vformat("%d reversible changes, each confined to one thing", count);
		case RISK_SUBSTANTIAL:
			return vformat("%d changes to the same thing, reversible together but not "
						   "separately once applied",
					count);
		case RISK_IRREVERSIBLE:
			// Cannot happen - irreversible items are keyed by their own index - but a
			// wrong answer here would be a batched approval for something unbatchable, so
			// it says what it is rather than guessing.
			return vformat("%d changes that cannot be undone", count);
	}
	return String();
}

Dictionary MCPProposals::to_dictionary(const String &p_title, const Vector<Item> &p_items) {
	Array items;
	int mechanical = 0;
	int substantial = 0;
	int irreversible = 0;

	for (int i = 0; i < p_items.size(); i++) {
		const Item &item = p_items[i];
		Dictionary entry;
		entry["index"] = item.index;
		entry["description"] = item.description;
		entry["tool"] = item.tool;
		entry["arguments"] = item.arguments;
		entry["risk"] = risk_to_string(item.risk);
		entry["risk_reason"] = item.risk_reason;
		entry["group"] = item.group.is_empty() ? group_key(item) : item.group;
		entry["state"] = item.state;
		Array files;
		for (int f = 0; f < item.files.size(); f++) {
			files.push_back(item.files[f]);
		}
		entry["files"] = files;
		entry["subjects"] = item.subjects;
		items.push_back(entry);

		switch (item.risk) {
			case RISK_MECHANICAL:
				mechanical++;
				break;
			case RISK_SUBSTANTIAL:
				substantial++;
				break;
			case RISK_IRREVERSIBLE:
				irreversible++;
				break;
		}
	}

	Dictionary result;
	result["title"] = p_title;
	result["items"] = items;
	result["groups"] = build_groups(p_items);
	result["item_count"] = p_items.size();
	result["mechanical_count"] = mechanical;
	result["substantial_count"] = substantial;
	result["irreversible_count"] = irreversible;
	return result;
}
