/**************************************************************************/
/*  test_mcp_replay.h                                                     */
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

#ifndef TEST_MCP_REPLAY_H
#define TEST_MCP_REPLAY_H

#include "modules/godot_ai/mcp_replay.h"

#include "tests/test_macros.h"

namespace TestMCPReplay {

static Dictionary event_at(int64_t p_frame, const String &p_kind) {
	Dictionary event;
	event["frame"] = p_frame;
	event["kind"] = p_kind;
	return event;
}

static Dictionary assertion_at(int64_t p_frame, const String &p_property, const Variant &p_value) {
	Dictionary assertion;
	assertion["frame"] = p_frame;
	assertion["node_path"] = "Main/Player";
	assertion["property"] = p_property;
	assertion["value"] = p_value;
	return assertion;
}

TEST_CASE("[godot_ai] Replay schedules events by frame offset, not absolute frame") {
	// Recorded starting at frame 1000; replayed against a game that happens to be at 50.
	// What must survive the move is the *spacing*.
	Array events;
	events.push_back(event_at(1000, "key"));
	events.push_back(event_at(1005, "pointer"));
	events.push_back(event_at(1020, "key"));

	MCPReplayPlan plan;
	String error;
	REQUIRE(plan.load(events, Array(), 1000, error));
	plan.start(50);

	Vector<MCPReplayPlan::DueAssertion> due;

	// Frame 50: only the event recorded at the very start is due.
	CHECK(plan.observe(50, due).size() == 1);
	// Frame 54: nothing new yet - the next is five frames in.
	CHECK(plan.observe(54, due).is_empty());
	// Frame 55: the second event.
	CHECK(plan.observe(55, due).size() == 1);
	CHECK_FALSE(plan.is_finished());
	// Frame 70: the third, and the trace is done.
	CHECK(plan.observe(70, due).size() == 1);
	CHECK(plan.is_finished());
	CHECK(plan.get_verdict() == MCPReplayPlan::VERDICT_PASSED);
}

TEST_CASE("[godot_ai] Replay passes when every assertion matches") {
	Array events;
	events.push_back(event_at(10, "key"));
	Array assertions;
	assertions.push_back(assertion_at(12, "health", 100));

	MCPReplayPlan plan;
	String error;
	REQUIRE(plan.load(events, assertions, 10, error));
	plan.start(0);

	Vector<MCPReplayPlan::DueAssertion> due;
	plan.observe(0, due);
	CHECK(due.is_empty());

	plan.observe(2, due);
	REQUIRE(due.size() == 1);
	CHECK(due[0].property == "health");
	plan.report_assertion(due[0].index, 100);

	CHECK(plan.is_finished());
	CHECK(plan.get_verdict() == MCPReplayPlan::VERDICT_PASSED);

	const Dictionary report = plan.to_report();
	CHECK(String(report["verdict"]) == "passed");
	CHECK((int)report["assertions_matched"] == 1);
	CHECK_FALSE(report.has("first_divergence"));
}

TEST_CASE("[godot_ai] Replay reports the first divergence and stops there") {
	Array events;
	events.push_back(event_at(0, "key"));
	Array assertions;
	assertions.push_back(assertion_at(1, "health", 100));
	assertions.push_back(assertion_at(2, "score", 50));

	MCPReplayPlan plan;
	String error;
	REQUIRE(plan.load(events, assertions, 0, error));
	plan.start(0);

	Vector<MCPReplayPlan::DueAssertion> due;
	plan.observe(1, due);
	REQUIRE(due.size() == 1);
	plan.report_assertion(due[0].index, 80); // Diverges: expected 100.

	// Everything after the first divergence is downstream of a state the recording never
	// visited, so the run ends here rather than collecting more noise.
	CHECK(plan.is_finished());
	CHECK(plan.get_verdict() == MCPReplayPlan::VERDICT_FAILED);

	const Dictionary report = plan.to_report();
	CHECK(String(report["verdict"]) == "failed");
	REQUIRE(report.has("first_divergence"));
	const Dictionary divergence = report["first_divergence"];
	CHECK(String(divergence["property"]) == "health");
	CHECK((int)divergence["expected"] == 100);
	CHECK((int)divergence["observed"] == 80);
	// The second assertion was never checked, and the report must not imply it passed.
	CHECK((int)report["assertions_checked"] == 1);
	CHECK((int)report["assertions_matched"] == 0);
}

TEST_CASE("[godot_ai] Replay that drifts past its tolerance is indeterminate, never passed") {
	// S6, and the requirement most worth protecting: a run whose events arrived late has
	// not proven the game still behaves. Calling it a pass is the one lie that would make
	// a green suite meaningless.
	Array events;
	events.push_back(event_at(0, "key"));
	events.push_back(event_at(10, "key"));

	MCPReplayPlan plan;
	String error;
	REQUIRE(plan.load(events, Array(), 0, error));
	plan.set_drift_tolerance(4);
	plan.start(0);

	Vector<MCPReplayPlan::DueAssertion> due;
	plan.observe(0, due);
	// The game jumped from frame 0 to frame 40: the event due at offset 10 is 30 frames
	// late. It still gets injected, but the run can no longer claim to reproduce anything.
	CHECK(plan.observe(40, due).size() == 1);

	CHECK(plan.is_finished());
	CHECK(plan.get_verdict() == MCPReplayPlan::VERDICT_INDETERMINATE);

	const Dictionary report = plan.to_report();
	CHECK(String(report["verdict"]) == "indeterminate");
	CHECK((int64_t)report["max_drift_frames"] == 30);
	CHECK(String(report["note"]).contains("must not be counted as a pass"));
}

TEST_CASE("[godot_ai] Replay tolerates ordinary jitter without going indeterminate") {
	Array events;
	events.push_back(event_at(0, "key"));
	events.push_back(event_at(10, "key"));

	MCPReplayPlan plan;
	String error;
	REQUIRE(plan.load(events, Array(), 0, error));
	plan.set_drift_tolerance(4);
	plan.start(0);

	Vector<MCPReplayPlan::DueAssertion> due;
	plan.observe(0, due);
	// Three frames late is scheduling jitter, not a different game.
	plan.observe(13, due);
	CHECK(plan.get_verdict() == MCPReplayPlan::VERDICT_PASSED);
}

TEST_CASE("[godot_ai] A divergence outranks drift in the report") {
	Array events;
	events.push_back(event_at(0, "key"));
	Array assertions;
	assertions.push_back(assertion_at(10, "health", 100));

	MCPReplayPlan plan;
	String error;
	REQUIRE(plan.load(events, assertions, 0, error));
	plan.set_drift_tolerance(1);
	plan.start(0);

	Vector<MCPReplayPlan::DueAssertion> due;
	plan.observe(0, due);
	plan.observe(60, due);
	REQUIRE(due.size() == 1);
	plan.report_assertion(due[0].index, 7);

	// The run drifted *and* diverged. "Failed" is the more actionable answer: an
	// assertion that came back 7 instead of 100 is a real difference to look at,
	// regardless of pacing.
	CHECK(plan.get_verdict() == MCPReplayPlan::VERDICT_FAILED);
}

TEST_CASE("[godot_ai] Replay refuses a trace it cannot use") {
	MCPReplayPlan plan;
	String error;

	SUBCASE("an empty trace") {
		CHECK_FALSE(plan.load(Array(), Array(), 0, error));
		CHECK(error.contains("nothing to replay"));
	}

	SUBCASE("an event with no frame") {
		Array events;
		Dictionary event;
		event["kind"] = "key";
		events.push_back(event);
		CHECK_FALSE(plan.load(events, Array(), 0, error));
		CHECK(error.contains("frame"));
	}

	SUBCASE("an assertion with no target") {
		Array events;
		events.push_back(event_at(0, "key"));
		Array assertions;
		Dictionary assertion;
		assertion["frame"] = 1;
		assertions.push_back(assertion);
		CHECK_FALSE(plan.load(events, assertions, 0, error));
	}
}

TEST_CASE("[godot_ai] Replay orders an out-of-order trace before running it") {
	Array events;
	events.push_back(event_at(20, "third"));
	events.push_back(event_at(0, "first"));
	events.push_back(event_at(10, "second"));

	MCPReplayPlan plan;
	String error;
	REQUIRE(plan.load(events, Array(), 0, error));
	plan.start(0);

	Vector<MCPReplayPlan::DueAssertion> due;
	const Array first = plan.observe(0, due);
	REQUIRE(first.size() == 1);
	CHECK(String(Dictionary(first[0])["kind"]) == "first");

	// Asking once at the end must still hand them back in recorded order, not in the
	// order they happened to sit in the file.
	const Array rest = plan.observe(100, due);
	REQUIRE(rest.size() == 2);
	CHECK(String(Dictionary(rest[0])["kind"]) == "second");
	CHECK(String(Dictionary(rest[1])["kind"]) == "third");
}

TEST_CASE("[godot_ai] An event recorded before the session start is not dropped") {
	// The recorder clears the trace at start, so this only happens to an event already in
	// flight. Dropping it would silently change the sequence the game sees.
	Array events;
	events.push_back(event_at(95, "in-flight"));
	events.push_back(event_at(100, "first"));

	MCPReplayPlan plan;
	String error;
	REQUIRE(plan.load(events, Array(), 100, error));
	plan.start(0);

	Vector<MCPReplayPlan::DueAssertion> due;
	CHECK(plan.observe(0, due).size() == 2);
}

TEST_CASE("[godot_ai] Reporting the same assertion twice does not overwrite the first answer") {
	Array events;
	events.push_back(event_at(0, "key"));
	Array assertions;
	assertions.push_back(assertion_at(0, "health", 100));

	MCPReplayPlan plan;
	String error;
	REQUIRE(plan.load(events, assertions, 0, error));
	plan.start(0);

	Vector<MCPReplayPlan::DueAssertion> due;
	plan.observe(0, due);
	REQUIRE(due.size() == 1);
	plan.report_assertion(due[0].index, 42); // Diverges.
	plan.report_assertion(due[0].index, 100); // A late duplicate must not erase that.

	CHECK(plan.get_verdict() == MCPReplayPlan::VERDICT_FAILED);
	const Dictionary divergence = Dictionary(plan.to_report())["first_divergence"];
	CHECK((int)divergence["observed"] == 42);
}

TEST_CASE("[godot_ai] An assertion with no frame is checked at the end of the trace") {
	Array events;
	events.push_back(event_at(0, "key"));
	events.push_back(event_at(30, "key"));
	Array assertions;
	Dictionary assertion;
	assertion["node_path"] = "Main/Player";
	assertion["property"] = "health";
	assertion["value"] = 100;
	assertions.push_back(assertion); // No frame.

	MCPReplayPlan plan;
	String error;
	REQUIRE(plan.load(events, assertions, 0, error));
	plan.start(0);

	Vector<MCPReplayPlan::DueAssertion> due;
	plan.observe(0, due);
	CHECK(due.is_empty()); // Not due at the start.
	plan.observe(30, due);
	CHECK(due.size() == 1); // Due once the trace has played out.
}

TEST_CASE("[godot_ai] Loading a second trace forgets the first run entirely") {
	// The crash this pins down: the same tool object replays one session after another,
	// and the first run's verdict was surviving load(). A failed run followed by a session
	// with no assertions left `failed` true and the divergence index at 0, so the next
	// poll called the new run finished and to_report() reached into an assertion list that
	// load() had just emptied. That took the whole editor down.
	Array first_events;
	first_events.push_back(event_at(0, "key"));
	Array first_assertions;
	first_assertions.push_back(assertion_at(0, "press_count", 5));

	MCPReplayPlan plan;
	String error;
	REQUIRE(plan.load(first_events, first_assertions, 0, error));
	plan.start(0);
	Vector<MCPReplayPlan::DueAssertion> due;
	plan.observe(0, due);
	REQUIRE(due.size() == 1);
	plan.report_assertion(due[0].index, 6); // Diverges: the run fails.
	REQUIRE(plan.get_verdict() == MCPReplayPlan::VERDICT_FAILED);

	// A second session, with no assertions at all - which is what a retroactive capture
	// produces, because nobody was there to record one.
	Array second_events;
	second_events.push_back(event_at(100, "pointer"));
	REQUIRE(plan.load(second_events, Array(), 100, error));

	// Before start(), and this is the window the tool actually polls in.
	CHECK_FALSE(plan.is_finished());
	CHECK(plan.get_verdict() == MCPReplayPlan::VERDICT_RUNNING);
	const Dictionary report = plan.to_report();
	CHECK_FALSE(report.has("first_divergence"));
	CHECK((int)report["assertions_total"] == 0);
	CHECK((int)report["events_injected"] == 0);

	// And it then runs to a clean pass, rather than inheriting the failure.
	plan.start(0);
	CHECK(plan.observe(0, due).size() == 1);
	CHECK(plan.is_finished());
	CHECK(plan.get_verdict() == MCPReplayPlan::VERDICT_PASSED);
}

TEST_CASE("[godot_ai] A loaded plan is not finished until it has been started") {
	Array events;
	events.push_back(event_at(0, "key"));

	MCPReplayPlan plan;
	String error;
	REQUIRE(plan.load(events, Array(), 0, error));
	// Nothing has been injected and nothing can have been: the plan has no live frame to
	// schedule against yet.
	CHECK_FALSE(plan.is_finished());
	plan.start(0);
	CHECK_FALSE(plan.is_finished());
	Vector<MCPReplayPlan::DueAssertion> due;
	plan.observe(0, due);
	CHECK(plan.is_finished());
}

TEST_CASE("[godot_ai] A speed multiplier compresses the schedule and says what it costs") {
	Array events;
	events.push_back(event_at(0, "key"));
	events.push_back(event_at(60, "key"));
	events.push_back(event_at(120, "key"));

	MCPReplayPlan plan;
	plan.set_speed(2.0);
	String error;
	REQUIRE(plan.load(events, Array(), 0, error));
	CHECK(plan.get_expected_span() == 60);

	plan.start(0);
	Vector<MCPReplayPlan::DueAssertion> due;
	CHECK(plan.observe(0, due).size() == 1);
	CHECK(plan.observe(29, due).is_empty());
	CHECK(plan.observe(30, due).size() == 1);
	CHECK(plan.observe(60, due).size() == 1);
	CHECK(plan.is_finished());

	const Dictionary report = plan.to_report();
	CHECK(double(report["speed"]) == doctest::Approx(2.0));
	// The honesty is in the artifact, not only in the tool's description. A pass at 2x is
	// a pass over a sequence nobody performed.
	CHECK(String(report["speed_note"]).contains("nobody performed"));
	CHECK(String(report["speed_note"]).contains("1x"));
}

TEST_CASE("[godot_ai] A replay at 1x says nothing about speed at all") {
	Array events;
	events.push_back(event_at(0, "key"));
	events.push_back(event_at(30, "key"));

	MCPReplayPlan plan;
	String error;
	REQUIRE(plan.load(events, Array(), 0, error));
	plan.start(0);
	Vector<MCPReplayPlan::DueAssertion> due;
	plan.observe(0, due);
	plan.observe(30, due);

	const Dictionary report = plan.to_report();
	CHECK(double(report["speed"]) == doctest::Approx(1.0));
	// No note, because there is nothing to disclaim. A caveat attached to every run is a
	// caveat nobody reads on the run that needed it.
	CHECK_FALSE(report.has("speed_note"));
}

TEST_CASE("[godot_ai] Compressing a schedule never puts two moments on one frame") {
	// Rounding down is what turns a press-then-release into a press-and-release: no
	// hardware produces that, and a game polling once per frame cannot see it at all.
	Array events;
	events.push_back(event_at(0, "key"));
	events.push_back(event_at(1, "key"));
	events.push_back(event_at(2, "key"));

	MCPReplayPlan plan;
	plan.set_speed(8.0);
	String error;
	REQUIRE(plan.load(events, Array(), 0, error));
	plan.start(0);

	Vector<MCPReplayPlan::DueAssertion> due;
	CHECK(plan.observe(0, due).size() == 1);
	// Both remaining events are one frame apart at minimum, not collapsed onto frame 0.
	CHECK(plan.observe(1, due).size() == 2);
}

TEST_CASE("[godot_ai] An absurd speed is clamped rather than obeyed") {
	MCPReplayPlan plan;
	plan.set_speed(1000.0);
	CHECK(plan.get_speed() == doctest::Approx(8.0));
	plan.set_speed(0.0);
	CHECK(plan.get_speed() == doctest::Approx(0.1));
	plan.set_speed(-3.0);
	CHECK(plan.get_speed() == doctest::Approx(0.1));
}

TEST_CASE("[godot_ai] A slowed replay stretches the schedule") {
	// The other direction is the useful one for a game that is genuinely too slow to keep
	// up: giving it twice as many frames between events is not cheating, because the
	// recording's own spacing was never a requirement on the game's frame rate.
	Array events;
	events.push_back(event_at(0, "key"));
	events.push_back(event_at(30, "key"));

	MCPReplayPlan plan;
	plan.set_speed(0.5);
	String error;
	REQUIRE(plan.load(events, Array(), 0, error));
	CHECK(plan.get_expected_span() == 60);
}

} // namespace TestMCPReplay

#endif // TEST_MCP_REPLAY_H
