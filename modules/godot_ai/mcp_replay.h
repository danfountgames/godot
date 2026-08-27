/**************************************************************************/
/*  mcp_replay.h                                                          */
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

#ifndef MCP_REPLAY_H
#define MCP_REPLAY_H

#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

// The decision-making half of replaying a session, with no game attached.
//
// Replay is a loop: ask the running game what frame it is on, inject whatever the trace
// says happened by then, check whatever assertions have come due, and decide whether
// what came back still matches. Every part of that except talking to the game is
// arithmetic and comparison - so it lives here, where it can be tested without a game,
// a display, or a second process.
//
// The tool (`tools/mcp_session_tools.cpp`) owns the socket and the clock. This owns the
// decisions. That split is what makes S4 and S6 testable at all.
//
// **Frames, not milliseconds.** A recorded event happens at an absolute process frame;
// replay maps it onto `start_frame + (recorded_frame - recorded_start)`. Wall-clock
// replay is not reproducible and this deliberately cannot express it.
//
// **S6, the requirement that matters.** A replay whose frames drift out of step with the
// recording has not proven anything, and reporting it as a pass would be worse than
// reporting nothing. `observe()` tracks how far the game's frame counter has moved
// against how far the trace expected it to, and any run that skips past events without
// being able to deliver them on time ends `indeterminate` rather than `passed`.
class MCPReplayPlan {
public:
	enum Verdict {
		VERDICT_RUNNING,
		VERDICT_PASSED,
		VERDICT_FAILED, // An assertion diverged.
		VERDICT_INDETERMINATE, // Frame pacing drifted; the run proves nothing.
	};

	static String verdict_to_string(Verdict p_verdict);

	// One assertion that has come due and needs checking against the live game.
	struct DueAssertion {
		String node_path;
		String property;
		Variant expected;
		int64_t frame = 0;
		int index = -1;
	};

	// How many frames the game may fall behind the schedule before the run is called
	// indeterminate. One or two frames is ordinary scheduling jitter; a large jump means
	// events were delivered late enough that the game saw a different sequence.
	static const int64_t DEFAULT_DRIFT_TOLERANCE = 8;

	// `p_events` and `p_assertions` are what MCPSessions read back. `p_recorded_start` is
	// the session's start_frame. Returns false and fills r_error when the trace cannot be
	// replayed at all - no events, or events with no usable frame.
	bool load(const Array &p_events, const Array &p_assertions, int64_t p_recorded_start, String &r_error);

	// Binds the loaded trace to the frame the live game is on now.
	void start(int64_t p_live_start_frame);

	// Feeds the game's current frame in. Returns the events that should be injected now,
	// oldest first; fills r_due with the assertions that have come due.
	Array observe(int64_t p_live_frame, Vector<DueAssertion> &r_due);

	// Records the outcome of an assertion that observe() handed out. A mismatch is the
	// end of the run: everything after the first divergence is downstream noise.
	void report_assertion(int p_index, const Variant &p_observed);

	// True once every event has been injected and every assertion checked, or once the
	// run has failed.
	bool is_finished() const;

	Verdict get_verdict() const;

	// The full report: verdict, counts, drift, and the first divergence if there was one.
	Dictionary to_report() const;

	void set_drift_tolerance(int64_t p_frames);

	// Compresses the schedule: 2.0 replays the trace in half the frames.
	//
	// **A sped-up replay is a different input sequence, not a faster one.** Halving the
	// gap between two presses is a thing no hand did and no hardware produced, and a game
	// polling `is_action_just_pressed` in `_process`, or measuring a gesture per frame, or
	// running a cooldown, can legitimately behave differently. That is not a defect in the
	// game and must never be reported as one, so a run at any speed but 1 carries the
	// multiplier in its report and says what it does not prove. Set before `load()`, which
	// is where the schedule is built.
	void set_speed(double p_speed);
	double get_speed() const { return speed; }

	int64_t get_expected_span() const { return expected_span; }

private:
	struct ScheduledEvent {
		int64_t offset = 0; // Frames after the recorded start.
		Dictionary event;
		bool injected = false;
	};
	struct ScheduledAssertion {
		int64_t offset = 0;
		String node_path;
		String property;
		Variant expected;
		bool checked = false;
		bool matched = false;
		Variant observed;
	};

	// A recorded frame offset, compressed by the speed multiplier.
	int64_t _scaled(int64_t p_offset) const;

	// Oldest first. observe() walks both in order and relies on it.
	struct ScheduledEventSorter {
		bool operator()(const ScheduledEvent &a, const ScheduledEvent &b) const {
			return a.offset < b.offset;
		}
	};
	struct ScheduledAssertionSorter {
		bool operator()(const ScheduledAssertion &a, const ScheduledAssertion &b) const {
			return a.offset < b.offset;
		}
	};

	Vector<ScheduledEvent> events;
	Vector<ScheduledAssertion> assertions;

	int64_t recorded_start = 0;
	int64_t live_start = 0;
	int64_t expected_span = 0;
	int64_t max_drift = 0;
	int64_t drift_tolerance = DEFAULT_DRIFT_TOLERANCE;
	double speed = 1.0;
	int64_t last_observed = -1;

	bool started = false;
	bool failed = false;
	int first_divergence = -1;
	// Set when an event's scheduled frame had already passed by the time we were asked.
	bool delivered_late = false;
};

#endif // MCP_REPLAY_H
