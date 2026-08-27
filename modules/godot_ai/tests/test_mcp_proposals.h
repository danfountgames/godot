/**************************************************************************/
/*  test_mcp_proposals.h                                                  */
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

#ifndef TEST_MCP_PROPOSALS_H
#define TEST_MCP_PROPOSALS_H

#include "modules/godot_ai/mcp_proposals.h"

#include "tests/test_macros.h"

namespace TestMCPProposals {

static Vector<String> files(const String &p_a = String(), const String &p_b = String()) {
	Vector<String> out;
	if (!p_a.is_empty()) {
		out.push_back(p_a);
	}
	if (!p_b.is_empty()) {
		out.push_back(p_b);
	}
	return out;
}

static Dictionary action(const String &p_action) {
	Dictionary arguments;
	arguments["action"] = p_action;
	return arguments;
}

static MCPProposals::Item item(int p_index, MCPProposals::Risk p_risk,
		const String &p_tool = "Godot_ManageNode", const Vector<String> &p_files = Vector<String>()) {
	MCPProposals::Item entry;
	entry.index = p_index;
	entry.risk = p_risk;
	entry.tool = p_tool;
	entry.files = p_files;
	entry.description = vformat("change %d", p_index);
	entry.group = MCPProposals::group_key(entry);
	return entry;
}

// --- classification --------------------------------------------------------------

TEST_CASE("[godot_ai] A read changes nothing and is never a decision") {
	String reason;
	CHECK(MCPProposals::classify(MCP_CAP_READ_PROJECT, files(), "Godot_ListScenes", Dictionary(),
				  reason) == MCPProposals::RISK_MECHANICAL);
	CHECK(reason.contains("changes nothing"));
	CHECK(MCPProposals::classify(MCP_CAP_READ_RUNTIME, files(), "Godot_GetRuntimeProperty",
				  Dictionary(), reason) == MCPProposals::RISK_MECHANICAL);
}

TEST_CASE("[godot_ai] A scene edit is mechanical because the undo history holds it") {
	// The nuance that matters most here. Scene tools legitimately declare no checkpoint
	// files - the change lives in the editor's undo history until the scene is saved - so
	// reading "no files" as "nothing can put it back" would misclassify the most ordinary
	// edit there is, and put forty renames behind forty separate approvals.
	String reason;
	CHECK(MCPProposals::classify(MCP_CAP_EDIT_SCENE, files(), "Godot_ManageNode",
				  action("rename"), reason) == MCPProposals::RISK_MECHANICAL);
	CHECK(reason.contains("undo history"));
}

TEST_CASE("[godot_ai] Deleting from a scene is undoable, and still worth seeing on its own") {
	String reason;
	CHECK(MCPProposals::classify(MCP_CAP_EDIT_SCENE, files(), "Godot_ManageNode",
				  action("delete"), reason) == MCPProposals::RISK_SUBSTANTIAL);
	CHECK(reason.contains("undoable"));
}

TEST_CASE("[godot_ai] A file write with nothing to snapshot is irreversible") {
	// A mutating tool that declares no checkpoint paths is telling the protocol layer there
	// is nothing to snapshot. For a scene edit that is true and handled above; for anything
	// else it means no checkpoint will exist afterwards, and this must never be batched.
	String reason;
	CHECK(MCPProposals::classify(MCP_CAP_EDIT_FILES, files(), "Godot_WriteUserFile", Dictionary(),
				  reason) == MCPProposals::RISK_IRREVERSIBLE);
	CHECK(reason.contains("nothing to restore"));
}

TEST_CASE("[godot_ai] One file behind a checkpoint is mechanical, and says which file") {
	String reason;
	CHECK(MCPProposals::classify(MCP_CAP_EDIT_FILES, files("res://scripts/player.gd"),
				  "Godot_WriteTextFile", Dictionary(), reason) == MCPProposals::RISK_MECHANICAL);
	CHECK(reason.contains("res://scripts/player.gd"));
}

TEST_CASE("[godot_ai] Several files at once is substantial, however reversible") {
	// Not because it might fail, but because approving it inside a batch would hide which
	// of the files you agreed to.
	String reason;
	CHECK(MCPProposals::classify(MCP_CAP_EDIT_FILES, files("res://a.gd", "res://b.gd"),
				  "Godot_WriteTextFile", Dictionary(), reason) == MCPProposals::RISK_SUBSTANTIAL);
	CHECK(reason.contains("2 files"));
	CHECK(reason.contains("hide"));
}

TEST_CASE("[godot_ai] dangerous_exec is never batched, whatever else is true of it") {
	String reason;
	CHECK(MCPProposals::classify(MCP_CAP_DANGEROUS_EXEC, files("res://a.gd"), "Godot_Whatever",
				  Dictionary(), reason) == MCPProposals::RISK_IRREVERSIBLE);
	CHECK(reason.contains("dangerous_exec"));
}

TEST_CASE("[godot_ai] Destructive is read from the tool's own spelling of it") {
	CHECK(MCPProposals::is_destructive("Godot_DeleteUserFile", Dictionary()));
	CHECK(MCPProposals::is_destructive("Godot_ManageNode", action("delete")));
	CHECK(MCPProposals::is_destructive("Godot_ManageNode", action("remove")));
	CHECK(MCPProposals::is_destructive("Godot_Anything", action("clear")));
	CHECK_FALSE(MCPProposals::is_destructive("Godot_ManageNode", action("create")));
	CHECK_FALSE(MCPProposals::is_destructive("Godot_ManageNode", Dictionary()));
	// And reading a missing key must not have inserted one, which is the bug this codebase
	// keeps a regression test for everywhere it reads an optional argument.
	Dictionary empty;
	MCPProposals::is_destructive("Godot_ManageNode", empty);
	CHECK(empty.is_empty());
}

// --- grouping --------------------------------------------------------------------

TEST_CASE("[godot_ai] Forty mechanical changes are one decision") {
	// The whole of the user's instruction: "not 40 separate approvals".
	Vector<MCPProposals::Item> items;
	for (int i = 0; i < 40; i++) {
		items.push_back(item(i, MCPProposals::RISK_MECHANICAL));
	}
	const Array groups = MCPProposals::build_groups(items);
	REQUIRE(groups.size() == 1);
	CHECK(Array(Dictionary(groups[0])["items"]).size() == 40);
	CHECK(String(Dictionary(groups[0])["description"]).contains("40 reversible changes"));
}

TEST_CASE("[godot_ai] The one irreversible change among them stays its own decision") {
	Vector<MCPProposals::Item> items;
	items.push_back(item(0, MCPProposals::RISK_MECHANICAL));
	items.push_back(item(1, MCPProposals::RISK_IRREVERSIBLE, "Godot_DeleteUserFile"));
	items.push_back(item(2, MCPProposals::RISK_MECHANICAL));

	const Array groups = MCPProposals::build_groups(items);
	REQUIRE(groups.size() == 2);
	// The mechanical pair travelled together...
	CHECK(Array(Dictionary(groups[0])["items"]).size() == 2);
	// ...and the delete did not join them.
	CHECK(String(Dictionary(groups[1])["risk"]) == "irreversible");
	CHECK(Array(Dictionary(groups[1])["items"]).size() == 1);
}

TEST_CASE("[godot_ai] Two irreversible changes never collapse into one tick") {
	Vector<MCPProposals::Item> items;
	items.push_back(item(0, MCPProposals::RISK_IRREVERSIBLE, "Godot_DeleteUserFile"));
	items.push_back(item(1, MCPProposals::RISK_IRREVERSIBLE, "Godot_DeleteUserFile"));

	const Array groups = MCPProposals::build_groups(items);
	// Same tool, same risk, and still two decisions - because agreeing to delete one thing
	// is not agreeing to delete another.
	CHECK(groups.size() == 2);
}

TEST_CASE("[godot_ai] Substantial changes group by what they touch, not by their risk") {
	Vector<MCPProposals::Item> items;
	items.push_back(item(0, MCPProposals::RISK_SUBSTANTIAL, "Godot_WriteTextFile",
			files("res://scenes/main.tscn")));
	items.push_back(item(1, MCPProposals::RISK_SUBSTANTIAL, "Godot_WriteTextFile",
			files("res://scenes/main.tscn")));
	items.push_back(item(2, MCPProposals::RISK_SUBSTANTIAL, "Godot_WriteTextFile",
			files("res://scenes/menu.tscn")));

	const Array groups = MCPProposals::build_groups(items);
	// A plan that reworks two scenes should not make you agree to both at once.
	REQUIRE(groups.size() == 2);
	CHECK(Array(Dictionary(groups[0])["items"]).size() == 2);
	CHECK(Array(Dictionary(groups[1])["items"]).size() == 1);
	CHECK(String(Dictionary(groups[0])["key"]).contains("main.tscn"));
}

TEST_CASE("[godot_ai] Groups appear in the order the plan was written") {
	Vector<MCPProposals::Item> items;
	items.push_back(item(0, MCPProposals::RISK_IRREVERSIBLE, "Godot_DeleteUserFile"));
	items.push_back(item(1, MCPProposals::RISK_MECHANICAL));

	const Array groups = MCPProposals::build_groups(items);
	REQUIRE(groups.size() == 2);
	// Not sorted by severity: the reader is following a plan, and reordering it would make
	// the numbered items and the checkboxes disagree.
	CHECK(String(Dictionary(groups[0])["risk"]) == "irreversible");
	CHECK(String(Dictionary(groups[1])["risk"]) == "mechanical");
}

TEST_CASE("[godot_ai] A group collects the files and subjects of everything in it") {
	Vector<MCPProposals::Item> items;
	MCPProposals::Item first = item(0, MCPProposals::RISK_MECHANICAL, "Godot_WriteTextFile",
			files("res://a.gd"));
	MCPProposals::Item second = item(1, MCPProposals::RISK_MECHANICAL, "Godot_WriteTextFile",
			files("res://b.gd"));
	items.push_back(first);
	items.push_back(second);

	const Array groups = MCPProposals::build_groups(items);
	REQUIRE(groups.size() == 1);
	const Array group_files = Dictionary(groups[0])["files"];
	CHECK(group_files.size() == 2);
	CHECK(group_files.has("res://a.gd"));
	CHECK(group_files.has("res://b.gd"));
}

// --- what the plan says about itself -----------------------------------------------

TEST_CASE("[godot_ai] A single change speaks for itself rather than being counted") {
	Vector<MCPProposals::Item> items;
	MCPProposals::Item only = item(0, MCPProposals::RISK_MECHANICAL);
	only.description = "rename Player to Hero";
	items.push_back(only);

	const Array groups = MCPProposals::build_groups(items);
	CHECK(String(Dictionary(groups[0])["description"]) == "rename Player to Hero");
}

TEST_CASE("[godot_ai] A lone irreversible change says so in the line the user reads") {
	Vector<MCPProposals::Item> items;
	MCPProposals::Item only = item(0, MCPProposals::RISK_IRREVERSIBLE, "Godot_DeleteUserFile");
	only.description = "delete the old save file";
	only.risk_reason = "writes something that no checkpoint will capture";
	items.push_back(only);

	const Array groups = MCPProposals::build_groups(items);
	const String description = Dictionary(groups[0])["description"];
	CHECK(description.contains("delete the old save file"));
	// The consequence, not just the category: "irreversible" is a word, and "nothing can
	// put this back" is a fact about what happens if you tick it.
	CHECK(description.contains("nothing can put this back"));
}

TEST_CASE("[godot_ai] The plan reports its counts, with the irreversible ones separate") {
	Vector<MCPProposals::Item> items;
	items.push_back(item(0, MCPProposals::RISK_MECHANICAL));
	items.push_back(item(1, MCPProposals::RISK_MECHANICAL));
	items.push_back(item(2, MCPProposals::RISK_SUBSTANTIAL, "Godot_WriteTextFile",
			files("res://a.gd")));
	items.push_back(item(3, MCPProposals::RISK_IRREVERSIBLE, "Godot_DeleteUserFile"));

	const Dictionary plan = MCPProposals::to_dictionary("tidy the project", items);
	CHECK(String(plan["title"]) == "tidy the project");
	CHECK((int)plan["item_count"] == 4);
	CHECK((int)plan["mechanical_count"] == 2);
	CHECK((int)plan["substantial_count"] == 1);
	CHECK((int)plan["irreversible_count"] == 1);
	// Three decisions from four changes, which is the number that matters.
	CHECK(Array(plan["groups"]).size() == 3);
	CHECK(Array(plan["items"]).size() == 4);
}

TEST_CASE("[godot_ai] An unknown risk name reads as the middle, never as the safest") {
	// A typo must not quietly downgrade a decision into the batch.
	CHECK(MCPProposals::risk_from_string("nonsense") == MCPProposals::RISK_SUBSTANTIAL);
	CHECK(MCPProposals::risk_from_string("mechanical") == MCPProposals::RISK_MECHANICAL);
	CHECK(MCPProposals::risk_from_string("irreversible") == MCPProposals::RISK_IRREVERSIBLE);
	CHECK(MCPProposals::risk_to_string(MCPProposals::RISK_SUBSTANTIAL) == "substantial");
}

} // namespace TestMCPProposals

#endif // TEST_MCP_PROPOSALS_H
