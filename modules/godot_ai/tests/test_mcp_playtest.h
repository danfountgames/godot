/**************************************************************************/
/*  test_mcp_playtest.h                                                   */
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

#ifndef TEST_MCP_PLAYTEST_H
#define TEST_MCP_PLAYTEST_H

#include "modules/godot_ai/mcp_playtest.h"
#include "modules/godot_ai/tests/test_mcp_fs_helpers.h"

#include "core/io/dir_access.h"
#include "core/os/os.h"

#include "tests/test_macros.h"

namespace TestMCPPlaytest {

// Scratch root under the cache directory, carrying the godot_ai_test_ marker that
// mcp_test_remove_tree() insists on. Never under the project or the working directory:
// a fixture that got that wrong once erased this repository (DEC-0006).
//
// A headless test process has no `user://` at all, which is why the store has a root
// override in the first place.
class PlaytestFixture {
	String root;

public:
	explicit PlaytestFixture(const String &p_suffix) {
		root = OS::get_singleton()->get_cache_path().path_join(
				"godot_ai_test_playtests_" + p_suffix + "_" + itos(OS::get_singleton()->get_process_id()));
		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		dir->make_dir_recursive(root);
		MCPPlaytest::set_root_override(root);
		MCPPlaytest::reset_for_tests();
	}
	~PlaytestFixture() {
		MCPPlaytest::reset_for_tests();
		MCPPlaytest::clear_root_override();
		mcp_test_remove_tree(root);
	}
};

static Dictionary record(int64_t p_sequence, const String &p_tool, const String &p_outcome = "ok") {
	Dictionary entry;
	entry["sequence"] = p_sequence;
	entry["tool"] = p_tool;
	entry["outcome"] = p_outcome;
	entry["detail"] = String();
	return entry;
}

static Dictionary frame_sample(int64_t p_frame, double p_milliseconds) {
	Dictionary sample;
	sample["frame"] = p_frame;
	sample["milliseconds"] = p_milliseconds;
	return sample;
}

TEST_CASE("[godot_ai] A playtest's window is the sequence range it claimed") {
	Array records;
	records.push_back(record(1, "Godot_OpenScene"));
	records.push_back(record(5, "Godot_SendKeyInput"));
	records.push_back(record(9, "Godot_SendKeyInput"));
	records.push_back(record(20, "Godot_WriteTextFile"));

	// Calls made before the playtest opened belong to whatever came before it, and calls
	// after it closed belong to whatever came next. A report that swept up either would
	// credit this run with work it did not do.
	const Array window = MCPPlaytest::activity_in_window(records, 5, 9);
	REQUIRE(window.size() == 2);
	CHECK(int64_t(Dictionary(window[0])["sequence"]) == 5);
	CHECK(int64_t(Dictionary(window[1])["sequence"]) == 9);
}

TEST_CASE("[godot_ai] A window with no upper bound runs to the end of the stream") {
	Array records;
	records.push_back(record(1, "Godot_OpenScene"));
	records.push_back(record(7, "Godot_SendKeyInput"));

	const Array window = MCPPlaytest::activity_in_window(records, 2, 0);
	REQUIRE(window.size() == 1);
	CHECK(int64_t(Dictionary(window[0])["sequence"]) == 7);
}

TEST_CASE("[godot_ai] Only the calls that could have driven the game count as input") {
	Array activity;
	activity.push_back(record(1, "Godot_GetRuntimeSceneTree"));
	activity.push_back(record(2, "Godot_SendKeyInput"));
	activity.push_back(record(3, "Godot_SendPointerInput"));
	activity.push_back(record(4, "Godot_SendActionInput"));
	activity.push_back(record(5, "Godot_CaptureViewport"));

	const Array inputs = MCPPlaytest::input_in_window(activity);
	CHECK(inputs.size() == 3);
}

TEST_CASE("[godot_ai] Problems are the errors and warnings, under either spelling") {
	Array messages;

	Dictionary error;
	error["severity"] = "error";
	error["text"] = "Null instance";
	messages.push_back(error);

	// Godot_ReadOutputLog calls the field `type`. Both are accepted so the report and
	// that tool can be wired together without either pretending to be the other.
	Dictionary warning;
	warning["type"] = "warning";
	warning["text"] = "Node not found";
	messages.push_back(warning);

	Dictionary ordinary;
	ordinary["type"] = "output";
	ordinary["text"] = "Player jumped";
	messages.push_back(ordinary);

	// An empty line is not a problem, however it is labelled.
	Dictionary blank;
	blank["severity"] = "error";
	blank["text"] = "   ";
	messages.push_back(blank);

	const Array problems = MCPPlaytest::problems_from_log(messages);
	REQUIRE(problems.size() == 2);
	CHECK(String(Dictionary(problems[0])["severity"]) == "error");
	CHECK(String(Dictionary(problems[1])["severity"]) == "warning");
}

TEST_CASE("[godot_ai] A spike is measured against how the game usually runs") {
	Array frames;
	for (int i = 0; i < 10; i++) {
		frames.push_back(frame_sample(i, 16.0));
	}
	frames.push_back(frame_sample(10, 100.0));

	const Array spikes = MCPPlaytest::spikes_from_frame_times(frames, 3.0);
	REQUIRE(spikes.size() == 1);
	const Dictionary spike = spikes[0];
	CHECK(int64_t(spike["frame"]) == 10);
	CHECK(double(spike["times_median"]) > 6.0);

	// A game that is uniformly slow has no spikes: 40ms every frame is a performance
	// problem, but it is not a *spike*, and reporting it as one would bury the real ones.
	Array uniform;
	for (int i = 0; i < 10; i++) {
		uniform.push_back(frame_sample(i, 40.0));
	}
	CHECK(MCPPlaytest::spikes_from_frame_times(uniform, 3.0).is_empty());
}

TEST_CASE("[godot_ai] Spike detection refuses inputs it cannot draw a conclusion from") {
	Array two;
	two.push_back(frame_sample(0, 16.0));
	two.push_back(frame_sample(1, 900.0));
	// Two samples have no meaningful middle.
	CHECK(MCPPlaytest::spikes_from_frame_times(two, 3.0).is_empty());

	Array many;
	for (int i = 0; i < 10; i++) {
		many.push_back(frame_sample(i, 16.0));
	}
	// A multiplier of one or less would call every frame a spike.
	CHECK(MCPPlaytest::spikes_from_frame_times(many, 1.0).is_empty());
	CHECK(MCPPlaytest::spikes_from_frame_times(many, 0.5).is_empty());
}

TEST_CASE("[godot_ai] A claimed success with no input at all is not a success") {
	// The check that catches a report written from the source rather than from the game.
	String reason;
	const MCPPlaytest::Verdict verdict =
			MCPPlaytest::reconcile_verdict(MCPPlaytest::VERDICT_REACHED, 0, 0, false, reason);
	CHECK(verdict == MCPPlaytest::VERDICT_INDETERMINATE);
	CHECK(reason.contains("no input"));
}

TEST_CASE("[godot_ai] A claimed success past a logged error is reported as indeterminate") {
	String reason;
	const MCPPlaytest::Verdict verdict =
			MCPPlaytest::reconcile_verdict(MCPPlaytest::VERDICT_REACHED, 2, 5, false, reason);
	CHECK(verdict == MCPPlaytest::VERDICT_INDETERMINATE);
	CHECK(reason.contains("2 error"));
}

TEST_CASE("[godot_ai] A clean run keeps the verdict it was given") {
	String reason;
	CHECK(MCPPlaytest::reconcile_verdict(MCPPlaytest::VERDICT_REACHED, 0, 4, false, reason) ==
			MCPPlaytest::VERDICT_REACHED);
	CHECK(reason.is_empty());

	CHECK(MCPPlaytest::reconcile_verdict(MCPPlaytest::VERDICT_NOT_REACHED, 0, 4, false, reason) ==
			MCPPlaytest::VERDICT_NOT_REACHED);
	CHECK(reason.is_empty());

	// 'blocked' is what a run says when something stopped it, so errors do not contradict it.
	CHECK(MCPPlaytest::reconcile_verdict(MCPPlaytest::VERDICT_BLOCKED, 3, 1, false, reason) ==
			MCPPlaytest::VERDICT_BLOCKED);
}

TEST_CASE("[godot_ai] 'Not reached' that ran out of time says only that it ran out of time") {
	String reason;
	const MCPPlaytest::Verdict verdict =
			MCPPlaytest::reconcile_verdict(MCPPlaytest::VERDICT_NOT_REACHED, 0, 30, true, reason);
	CHECK(verdict == MCPPlaytest::VERDICT_INDETERMINATE);
	CHECK(reason.contains("budget"));
}

TEST_CASE("[godot_ai] Finishing without stating a verdict is not a pass") {
	String reason;
	CHECK(MCPPlaytest::reconcile_verdict(MCPPlaytest::VERDICT_UNKNOWN, 0, 5, false, reason) ==
			MCPPlaytest::VERDICT_INDETERMINATE);
	CHECK(reason.contains("without stating a verdict"));
}

TEST_CASE("[godot_ai] Verdict names round-trip, and an unknown one is refused") {
	for (const MCPPlaytest::Verdict verdict : { MCPPlaytest::VERDICT_UNKNOWN,
				 MCPPlaytest::VERDICT_REACHED, MCPPlaytest::VERDICT_NOT_REACHED,
				 MCPPlaytest::VERDICT_BLOCKED, MCPPlaytest::VERDICT_INDETERMINATE }) {
		bool known = false;
		CHECK(MCPPlaytest::verdict_from_string(MCPPlaytest::verdict_to_string(verdict), known) == verdict);
		CHECK(known);
	}

	bool known = true;
	MCPPlaytest::verdict_from_string("mostly worked", known);
	CHECK_FALSE(known);
}

TEST_CASE("[godot_ai] The report separates what was recorded from what was claimed") {
	Dictionary meta;
	meta["goal"] = "reach the second room";

	Array activity;
	activity.push_back(record(1, "Godot_SendKeyInput"));
	activity.push_back(record(2, "Godot_GetRuntimeSceneTree"));

	Array inputs;
	inputs.push_back(record(1, "Godot_SendKeyInput"));

	Array problems;
	Dictionary problem;
	problem["severity"] = "warning";
	problem["text"] = "slow frame";
	problems.push_back(problem);

	Array observations;
	Dictionary observation;
	observation["note"] = "the door did not open";
	observations.push_back(observation);

	const Dictionary report = MCPPlaytest::build_report(meta, activity, inputs, problems, Array(),
			observations, MCPPlaytest::VERDICT_NOT_REACHED, String(), "the door stayed shut");

	CHECK(String(report["goal"]) == "reach the second room");
	CHECK(String(report["verdict"]) == "not_reached");
	CHECK(String(report["summary"]) == "the door stayed shut");

	const Dictionary counts = report["counts"];
	CHECK(int(counts["calls"]) == 2);
	CHECK(int(counts["inputs"]) == 1);
	CHECK(int(counts["problems"]) == 1);
	CHECK(int(counts["observations"]) == 1);

	// The whole activity list lives beside the report rather than inside it: a long
	// playtest makes thousands of records and the debugger channel drops anything over
	// 8 MiB, which is the lesson the session store already learned.
	CHECK(report.has("activity_file"));
	CHECK_FALSE(report.has("activity"));

	// And the report says which half is evidence, so a reader does not have to guess.
	CHECK(String(report["evidence_note"]).contains("agent's conclusion"));
}

TEST_CASE("[godot_ai] A report carries the reason its verdict was changed") {
	Dictionary meta;
	const Dictionary report = MCPPlaytest::build_report(meta, Array(), Array(), Array(), Array(),
			Array(), MCPPlaytest::VERDICT_INDETERMINATE, "no input was injected", "all good");
	CHECK(String(report["verdict"]) == "indeterminate");
	CHECK(String(report["verdict_reason"]) == "no input was injected");
	// The claim is kept beside the conclusion rather than replaced by it.
	CHECK(String(report["summary"]) == "all good");
}

TEST_CASE("[godot_ai] A playtest name becomes a directory-safe slug") {
	String error;
	CHECK(MCPPlaytest::slugify("Reach the Second Room", error) == "reach-the-second-room");
	CHECK(MCPPlaytest::slugify("boss_fight-2", error) == "boss_fight-2");

	// Separators become word breaks rather than vanishing, so two goals cannot collide.
	CHECK(MCPPlaytest::slugify("etc/passwd", error) == "etc-passwd");
	CHECK(MCPPlaytest::slugify("../../escape", error) == "escape");

	error = String();
	CHECK(MCPPlaytest::slugify("!!!", error).is_empty());
	CHECK_FALSE(error.is_empty());
}

TEST_CASE("[godot_ai] Two playtests cannot be open at once") {
	PlaytestFixture fixture("twoatonce");
	CHECK_FALSE(MCPPlaytest::is_running());

	const MCPPlaytest::Result first = MCPPlaytest::begin("first", "reach the room", 60, "", Dictionary());
	REQUIRE_MESSAGE(first.ok, first.error);
	CHECK(MCPPlaytest::is_running());
	CHECK(MCPPlaytest::get_active_slug() == "first");

	// Two overlapping windows would each claim the same activity records and neither
	// report would be true.
	const MCPPlaytest::Result second = MCPPlaytest::begin("second", "reach the other room", 60, "", Dictionary());
	CHECK_FALSE(second.ok);
	CHECK(second.error.contains("already running"));

}

TEST_CASE("[godot_ai] A report says whether anything measured the frame times") {
	// "No spikes" and "nobody was looking" read identically in a report and mean
	// completely different things. The first is a finding; the second is a gap. Until the
	// frame-time recorder was wired in, every live report said the first while meaning the
	// second, and the ledger had to carry a row admitting it.
	PlaytestFixture fixture("coverage");
	REQUIRE(MCPPlaytest::begin("unmeasured", "reach the room", 60, "", Dictionary()).ok);

	Dictionary unmeasured;
	REQUIRE(MCPPlaytest::finish(MCPPlaytest::VERDICT_NOT_REACHED, "nothing happened",
			unmeasured).ok);
	const Dictionary no_coverage = unmeasured["frame_coverage"];
	CHECK(bool(no_coverage["measured"]) == false);
	CHECK((int)no_coverage["samples"] == 0);
	CHECK(String(no_coverage["note"]).contains("nothing was looking"));
	CHECK(Array(unmeasured["spikes"]).is_empty());
}

TEST_CASE("[godot_ai] Frame times from the game reach the report as spikes") {
	PlaytestFixture fixture("spikey");
	REQUIRE(MCPPlaytest::begin("measured", "reach the room", 60, "", Dictionary()).ok);

	Array frames;
	for (int i = 0; i < 30; i++) {
		frames.push_back(frame_sample(i, 16.0));
	}
	frames.push_back(frame_sample(30, 120.0));

	Dictionary report;
	REQUIRE(MCPPlaytest::finish(MCPPlaytest::VERDICT_NOT_REACHED, "it stuttered", report,
			frames).ok);

	const Dictionary coverage = report["frame_coverage"];
	CHECK(bool(coverage["measured"]));
	CHECK((int)coverage["samples"] == 31);
	CHECK_FALSE(coverage.has("note"));

	const Array spikes = report["spikes"];
	REQUIRE(spikes.size() == 1);
	CHECK((int64_t)Dictionary(spikes[0])["frame"] == 30);
	CHECK((int)Dictionary(report["counts"])["spikes"] == 1);
}

TEST_CASE("[godot_ai] A playtest refuses to open without a goal") {
	PlaytestFixture fixture("nogoal");
	const MCPPlaytest::Result result = MCPPlaytest::begin("nameless", "   ", 60, "", Dictionary());
	CHECK_FALSE(result.ok);
	CHECK(result.error.contains("goal"));
	CHECK_FALSE(MCPPlaytest::is_running());
}

TEST_CASE("[godot_ai] Observations need a playtest, and something to say") {
	PlaytestFixture fixture("observe");

	const MCPPlaytest::Result before = MCPPlaytest::observe("something", "note");
	CHECK_FALSE(before.ok);

	REQUIRE(MCPPlaytest::begin("observed", "reach the room", 60, "", Dictionary()).ok);
	CHECK(MCPPlaytest::observe("the door did not open", "problem").ok);
	CHECK_FALSE(MCPPlaytest::observe("  ", "note").ok);

}

TEST_CASE("[godot_ai] Stopping early keeps what was collected and says it is partial") {
	PlaytestFixture fixture("stopearly");
	REQUIRE(MCPPlaytest::begin("stopped-early", "reach the room", 60, "", Dictionary()).ok);
	REQUIRE(MCPPlaytest::observe("got as far as the corridor", "progress").ok);

	Dictionary report;
	const MCPPlaytest::Result result = MCPPlaytest::abandon("the user asked to stop", report);
	REQUIRE_MESSAGE(result.ok, result.error);

	CHECK(bool(report["partial"]));
	CHECK(String(report["verdict"]) == "indeterminate");
	CHECK(String(report["summary"]).contains("the user asked to stop"));
	// The observation survives the stop: partial results are still results.
	CHECK(int(Dictionary(report["counts"])["observations"]) == 1);
	CHECK_FALSE(MCPPlaytest::is_running());
}

} // namespace TestMCPPlaytest

#endif // TEST_MCP_PLAYTEST_H
