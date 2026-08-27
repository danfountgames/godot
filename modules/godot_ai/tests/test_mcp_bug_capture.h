/**************************************************************************/
/*  test_mcp_bug_capture.h                                                */
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

#ifndef TEST_MCP_BUG_CAPTURE_H
#define TEST_MCP_BUG_CAPTURE_H

#include "modules/godot_ai/mcp_bug_capture.h"
#include "modules/godot_ai/mcp_replay.h"

#include "tests/test_macros.h"

namespace TestMCPBugCapture {

// An acknowledged event: the game came back and said which frame it landed on.
static Dictionary acknowledged(const String &p_kind, int64_t p_frame, int64_t p_msec) {
	Dictionary event;
	event["kind"] = p_kind;
	event["frame"] = p_frame;
	event["dispatch_msec"] = p_msec;
	event["acknowledged"] = true;
	return event;
}

// One the game never answered. In a real buffer this is the event it died on.
static Dictionary unacknowledged(const String &p_kind, int64_t p_msec) {
	Dictionary event;
	event["kind"] = p_kind;
	event["dispatch_msec"] = p_msec;
	event["acknowledged"] = false;
	return event;
}

// --- the mirror ---------------------------------------------------------------

TEST_CASE("[godot_ai] The mirror keeps input commands and ignores questions") {
	MCPBugCapture::clear();

	Dictionary click;
	click["x"] = 10;
	click["y"] = 20;
	CHECK(MCPBugCapture::record_dispatch("send_pointer", click, 1000) !=
			MCPBugCapture::INVALID_ID);
	// Reading a property is a question about the game, not a thing that happened to it,
	// and replaying it would prove nothing.
	CHECK(MCPBugCapture::record_dispatch("get_property", Dictionary(), 1001) ==
			MCPBugCapture::INVALID_ID);
	CHECK(MCPBugCapture::record_dispatch("capture", Dictionary(), 1002) ==
			MCPBugCapture::INVALID_ID);

	const Array buffered = MCPBugCapture::snapshot();
	REQUIRE(buffered.size() == 1);
	const Dictionary only = buffered[0];
	CHECK(String(only["kind"]) == "pointer");
	CHECK((int)only["x"] == 10);
	CHECK((bool)only["acknowledged"] == false);

	MCPBugCapture::clear();
}

TEST_CASE("[godot_ai] Every input command maps onto the trace kind replay expects") {
	// The mapping is what lets a mirrored capture be replayed by the same code path as a
	// recorded one. A kind replay does not recognise is silently skipped at replay time,
	// so a wrong name here would produce a session that runs and does nothing.
	CHECK(MCPBugCapture::kind_for_command("send_pointer") == "pointer");
	CHECK(MCPBugCapture::kind_for_command("send_key") == "key");
	CHECK(MCPBugCapture::kind_for_command("send_touch") == "touch");
	CHECK(MCPBugCapture::kind_for_command("send_gamepad") == "gamepad");
	CHECK(MCPBugCapture::kind_for_command("ping").is_empty());
	CHECK(MCPBugCapture::is_input_command("send_key"));
	CHECK_FALSE(MCPBugCapture::is_input_command("runtime_errors"));
}

TEST_CASE("[godot_ai] An acknowledgement fills in the frame the game reported") {
	MCPBugCapture::clear();

	const MCPBugCapture::Id id = MCPBugCapture::record_dispatch("send_key", Dictionary(), 500);
	Dictionary reply;
	reply["frame"] = 4242;
	MCPBugCapture::record_acknowledgement(id, reply);

	const Dictionary event = MCPBugCapture::snapshot()[0];
	CHECK((int64_t)event["frame"] == 4242);
	CHECK((bool)event["acknowledged"]);

	MCPBugCapture::clear();
}

TEST_CASE("[godot_ai] A reply with no frame in it teaches the mirror nothing") {
	MCPBugCapture::clear();

	const MCPBugCapture::Id id = MCPBugCapture::record_dispatch("send_key", Dictionary(), 500);
	// An older game, or a command whose reply simply does not carry one. Guessing would be
	// worse than staying unacknowledged: select() knows how to place an event with no
	// frame and says so, and a wrong frame it was told is one it will believe.
	MCPBugCapture::record_acknowledgement(id, Dictionary());

	const Dictionary event = MCPBugCapture::snapshot()[0];
	CHECK_FALSE(event.has("frame"));
	CHECK_FALSE((bool)event["acknowledged"]);

	MCPBugCapture::clear();
}

TEST_CASE("[godot_ai] The mirror is bounded and drops the oldest first") {
	MCPBugCapture::clear();
	const int original = MCPBugCapture::get_capacity();
	MCPBugCapture::set_capacity(3);

	for (int i = 0; i < 5; i++) {
		Dictionary arguments;
		arguments["index"] = i;
		MCPBugCapture::record_dispatch("send_key", arguments, 100 + i);
	}

	const Array buffered = MCPBugCapture::snapshot();
	REQUIRE(buffered.size() == 3);
	CHECK((int)Dictionary(buffered[0])["index"] == 2);
	CHECK((int)Dictionary(buffered[2])["index"] == 4);

	MCPBugCapture::set_capacity(original);
	MCPBugCapture::clear();
}

TEST_CASE("[godot_ai] A new game clears the mirror, a stopped game does not") {
	MCPBugCapture::clear();
	MCPBugCapture::record_dispatch("send_key", Dictionary(), 100);

	// The game dying is the moment the mirror matters most: it is now the only copy of the
	// trace, so it must survive.
	MCPBugCapture::note_game_stopped();
	CHECK(MCPBugCapture::snapshot().size() == 1);
	CHECK(MCPBugCapture::game_stopped_since_last_start());

	// A new run is a new reproduction. Keeping the previous run's input would let a capture
	// splice two games together and present the result as one sequence.
	MCPBugCapture::note_game_started();
	CHECK(MCPBugCapture::snapshot().is_empty());
	CHECK_FALSE(MCPBugCapture::game_stopped_since_last_start());

	MCPBugCapture::clear();
}

// --- selection ----------------------------------------------------------------

TEST_CASE("[godot_ai] A capture of acknowledged events estimates nothing") {
	Array events;
	events.push_back(acknowledged("key", 100, 1000));
	events.push_back(acknowledged("pointer", 160, 2000));

	const MCPBugCapture::Window window = MCPBugCapture::select(events, 0, 0);
	CHECK(window.kept == 2);
	CHECK(window.estimated == 0);
	CHECK(window.dropped == 0);
	CHECK(window.first_frame == 100);
	CHECK(window.last_frame == 160);
	CHECK_FALSE(Dictionary(window.events[0]).has("frame_estimated"));
}

TEST_CASE("[godot_ai] The event the game died on is placed, not dropped") {
	// The whole point of a retroactive capture. The last event before a crash is never
	// acknowledged - that is what crashing means - and it is also the one that caused the
	// bug, so a capture that drops it captures everything except the interesting part.
	Array events;
	events.push_back(acknowledged("key", 100, 1000));
	events.push_back(acknowledged("key", 160, 2000)); // 60 frames per 1000 ms.
	events.push_back(unacknowledged("pointer", 2500));

	const MCPBugCapture::Window window = MCPBugCapture::select(events, 0, 0);
	REQUIRE(window.kept == 3);
	CHECK(window.estimated == 1);
	CHECK(window.dropped == 0);

	const Dictionary last = window.events[2];
	CHECK((bool)last["frame_estimated"]);
	// 500 ms after frame 160 at the rate this buffer actually ran at.
	CHECK((int64_t)last["frame"] == 190);
}

TEST_CASE("[godot_ai] An estimated frame is never presented as a measured one") {
	Array events;
	events.push_back(acknowledged("key", 100, 1000));
	events.push_back(unacknowledged("key", 1100));

	const MCPBugCapture::Window window = MCPBugCapture::select(events, 0, 0);
	const Dictionary measured = window.events[0];
	const Dictionary guessed = window.events[1];
	CHECK_FALSE(measured.has("frame_estimated"));
	CHECK((bool)guessed["frame_estimated"]);

	// And it says so in the artifact as well as in the reply, because somebody will read
	// the session months later with none of this conversation around it.
	const Dictionary context =
			MCPBugCapture::build_context(window, "editor_mirror", false, "it froze", Array());
	CHECK((int)context["frames_estimated"] == 1);
	CHECK(String(context["replay_level"]) == "attempt");
	CHECK(String(context["fidelity"]).contains("extrapolated"));
	CHECK(String(context["reason"]) == "it froze");
}

TEST_CASE("[godot_ai] With no rate to measure, an unacknowledged event still keeps its order") {
	// One acknowledged event gives an anchor but no rate. Placing the next one a single
	// frame later is a floor, not a guess at the truth - and it is still marked estimated,
	// so nothing downstream can mistake it for a measurement.
	Array events;
	events.push_back(acknowledged("key", 700, 1000));
	events.push_back(unacknowledged("key", 1400));

	const MCPBugCapture::Window window = MCPBugCapture::select(events, 0, 0);
	REQUIRE(window.kept == 2);
	CHECK((int64_t)Dictionary(window.events[1])["frame"] == 701);
	CHECK((bool)Dictionary(window.events[1])["frame_estimated"]);
}

TEST_CASE("[godot_ai] An event with nothing acknowledged before it is dropped and counted") {
	// No anchor at all. A frame invented from nothing would be a claim with no data behind
	// it, so the event goes - but the count says it went, because a capture that silently
	// loses its first events is a capture that silently changes the reproduction.
	Array events;
	events.push_back(unacknowledged("key", 1000));
	events.push_back(acknowledged("key", 300, 1100));

	const MCPBugCapture::Window window = MCPBugCapture::select(events, 0, 0);
	CHECK(window.kept == 1);
	CHECK(window.dropped == 1);
	CHECK(window.considered == 2);
	CHECK(String(MCPBugCapture::describe_fidelity(window, "editor_mirror", false))
					.contains("dropped"));
}

TEST_CASE("[godot_ai] last_events keeps the tail, which is where a bug lives") {
	Array events;
	for (int i = 0; i < 10; i++) {
		events.push_back(acknowledged("key", 100 + i, 1000 + i * 10));
	}

	const MCPBugCapture::Window window = MCPBugCapture::select(events, 3, 0);
	CHECK(window.considered == 10);
	CHECK(window.kept == 3);
	CHECK(window.first_frame == 107);
	CHECK(window.last_frame == 109);
}

TEST_CASE("[godot_ai] since_frame drops everything before the moment named") {
	Array events;
	events.push_back(acknowledged("key", 100, 1000));
	events.push_back(acknowledged("key", 200, 2000));
	events.push_back(acknowledged("key", 300, 3000));

	const MCPBugCapture::Window window = MCPBugCapture::select(events, 0, 200);
	CHECK(window.kept == 2);
	CHECK(window.first_frame == 200);
}

TEST_CASE("[godot_ai] Given both bounds the narrower one wins") {
	// "The last ten events, but only since the level loaded" is a reasonable thing to ask,
	// and neither bound should quietly widen the other.
	Array events;
	for (int i = 0; i < 10; i++) {
		events.push_back(acknowledged("key", 100 + i * 10, 1000 + i * 100));
	}

	// since_frame is the narrower: it starts at index 8.
	CHECK(MCPBugCapture::select(events, 5, 180).kept == 2);
	// last_events is the narrower: since_frame would have allowed six.
	CHECK(MCPBugCapture::select(events, 2, 140).kept == 2);
}

TEST_CASE("[godot_ai] An empty buffer captures nothing and says why") {
	const MCPBugCapture::Window window = MCPBugCapture::select(Array(), 0, 0);
	CHECK(window.kept == 0);
	CHECK(window.considered == 0);
	const String fidelity = MCPBugCapture::describe_fidelity(window, "runtime_trace", true);
	CHECK(fidelity.contains("Nothing was captured"));
	// Not a failure, and not silence either: a bug reached without injected input cannot be
	// reproduced by replaying input, and the caller has to be told that rather than handed
	// an empty session that replays as a pass.
	CHECK(fidelity.contains("no sequence to replay"));
}

// --- what the capture claims ----------------------------------------------------

TEST_CASE("[godot_ai] A mirror capture never claims to know what the game did") {
	Array events;
	events.push_back(acknowledged("key", 100, 1000));
	const MCPBugCapture::Window window = MCPBugCapture::select(events, 0, 0);

	const String from_game = MCPBugCapture::describe_fidelity(window, "runtime_trace", true);
	CHECK(from_game.contains("the game processed it on"));

	const String from_mirror = MCPBugCapture::describe_fidelity(window, "editor_mirror", false);
	CHECK(from_mirror.contains("what was sent"));
	CHECK(from_mirror.contains("press play"));
}

TEST_CASE("[godot_ai] The context records where the events came from and what was said") {
	Array events;
	events.push_back(acknowledged("key", 100, 1000));
	events.push_back(acknowledged("key", 120, 1300));
	const MCPBugCapture::Window window = MCPBugCapture::select(events, 0, 0);

	Array errors;
	Dictionary error;
	error["message"] = "Invalid access to property 'health' on a null instance";
	errors.push_back(error);

	const Dictionary context = MCPBugCapture::build_context(window, "runtime_trace", true,
			"the player fell through the floor", errors);
	CHECK((bool)context["captured_retroactively"]);
	CHECK(String(context["source"]) == "runtime_trace");
	CHECK((bool)context["game_running_at_capture"]);
	CHECK((int)context["events_kept"] == 2);
	CHECK((int64_t)context["first_frame"] == 100);
	CHECK((int64_t)context["last_frame"] == 120);
	CHECK(Array(context["errors"]).size() == 1);
	// Nothing was estimated, so this capture is a record rather than an attempt.
	CHECK(String(context["replay_level"]) == "general");
}

// --- the join with replay --------------------------------------------------------

TEST_CASE("[godot_ai] A captured window loads as a replay plan") {
	// The claim the whole feature rests on: a retroactive capture is an ordinary session,
	// so the existing replay code runs it with no special case. If select() ever produced
	// an event replay could not schedule, this is what would catch it.
	Array events;
	events.push_back(acknowledged("key", 1000, 5000));
	events.push_back(acknowledged("pointer", 1060, 6000));
	events.push_back(unacknowledged("key", 6500));

	const MCPBugCapture::Window window = MCPBugCapture::select(events, 0, 0);
	REQUIRE(window.kept == 3);

	MCPReplayPlan plan;
	String error;
	REQUIRE_MESSAGE(plan.load(window.events, Array(), window.first_frame, error), error);

	// Replayed against a game that has just started: what survives is the spacing.
	plan.start(0);
	Vector<MCPReplayPlan::DueAssertion> due;
	CHECK(plan.observe(0, due).size() == 1);
	CHECK(plan.observe(59, due).is_empty());
	CHECK(plan.observe(60, due).size() == 1);
}

TEST_CASE("[godot_ai] Capture bookkeeping does not travel back into the game as arguments") {
	// select() writes `frame_estimated` into the record on purpose - the file has to say
	// which frames were guessed. Replay strips it, along with the mirror's own dispatch
	// bookkeeping, so none of it is handed back to the runtime as an input argument.
	MCPBugCapture::clear();

	Dictionary click;
	click["x"] = 30;
	click["y"] = 40;
	const MCPBugCapture::Id first = MCPBugCapture::record_dispatch("send_pointer", click, 1000);
	Dictionary reply;
	reply["frame"] = 100;
	MCPBugCapture::record_acknowledgement(first, reply);
	MCPBugCapture::record_dispatch("send_pointer", click, 1500);

	const Array buffered = MCPBugCapture::snapshot();
	REQUIRE(buffered.size() == 2);
	// The mirror's own correlation id is real in the buffer...
	CHECK(Dictionary(buffered[0]).has("_mirror_id"));

	const MCPBugCapture::Window window = MCPBugCapture::select(buffered, 0, 0);
	REQUIRE(window.kept == 2);
	// ...and gone from the record that gets written, where it would be a field of the
	// trace that means nothing to anyone reading it.
	CHECK_FALSE(Dictionary(window.events[0]).has("_mirror_id"));
	CHECK(Dictionary(window.events[1]).has("frame_estimated"));
	// The arguments themselves survive intact - replaying re-sends exactly these.
	CHECK((int)Dictionary(window.events[0])["x"] == 30);

	MCPBugCapture::clear();
}

} // namespace TestMCPBugCapture

#endif // TEST_MCP_BUG_CAPTURE_H
