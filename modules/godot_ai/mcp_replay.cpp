/**************************************************************************/
/*  mcp_replay.cpp                                                        */
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

#include "mcp_replay.h"

String MCPReplayPlan::verdict_to_string(Verdict p_verdict) {
	switch (p_verdict) {
		case VERDICT_PASSED:
			return "passed";
		case VERDICT_FAILED:
			return "failed";
		case VERDICT_INDETERMINATE:
			return "indeterminate";
		default:
			return "running";
	}
}

bool MCPReplayPlan::load(const Array &p_events, const Array &p_assertions,
		int64_t p_recorded_start, String &r_error) {
	events.clear();
	assertions.clear();
	recorded_start = p_recorded_start;
	expected_span = 0;

	if (p_events.is_empty()) {
		r_error = "this session recorded no input, so there is nothing to replay";
		return false;
	}

	for (int i = 0; i < p_events.size(); i++) {
		if (p_events[i].get_type() != Variant::DICTIONARY) {
			r_error = vformat("trace record %d is not an object", i);
			return false;
		}
		const Dictionary record = p_events[i];
		// get() rather than operator[]: a missing key read through a const Dictionary
		// inserts a null, which would then look like a real field on the way back out.
		const Variant frame = record.get("frame", Variant());
		if (frame.get_type() != Variant::INT && frame.get_type() != Variant::FLOAT) {
			r_error = vformat("trace record %d has no usable 'frame'", i);
			return false;
		}
		ScheduledEvent scheduled;
		scheduled.offset = (int64_t)frame - recorded_start;
		if (scheduled.offset < 0) {
			// A record from before the session started. Clamp rather than refuse: the
			// recorder clears the trace at start, so this only happens for an event that
			// was already in flight, and dropping it would silently change the sequence.
			scheduled.offset = 0;
		}
		scheduled.event = record;
		events.push_back(scheduled);
		expected_span = MAX(expected_span, scheduled.offset);
	}

	for (int i = 0; i < p_assertions.size(); i++) {
		if (p_assertions[i].get_type() != Variant::DICTIONARY) {
			r_error = vformat("assertion %d is not an object", i);
			return false;
		}
		const Dictionary record = p_assertions[i];
		ScheduledAssertion scheduled;
		const Variant frame = record.get("frame", Variant());
		scheduled.offset = frame.get_type() == Variant::INT || frame.get_type() == Variant::FLOAT
				? MAX((int64_t)0, (int64_t)frame - recorded_start)
				: expected_span;
		scheduled.node_path = record.get("node_path", String());
		scheduled.property = record.get("property", String());
		scheduled.expected = record.get("value", Variant());
		if (scheduled.node_path.is_empty() || scheduled.property.is_empty()) {
			r_error = vformat("assertion %d names no node path or no property", i);
			return false;
		}
		assertions.push_back(scheduled);
		expected_span = MAX(expected_span, scheduled.offset);
	}

	// Oldest first. The trace is written in order, but an assertion captured out of band
	// need not be, and observe() relies on the ordering.
	events.sort_custom<ScheduledEventSorter>();
	assertions.sort_custom<ScheduledAssertionSorter>();
	return true;
}

void MCPReplayPlan::start(int64_t p_live_start_frame) {
	live_start = p_live_start_frame;
	last_observed = p_live_start_frame;
	started = true;
	failed = false;
	max_drift = 0;
	delivered_late = false;
	first_divergence = -1;
}

Array MCPReplayPlan::observe(int64_t p_live_frame, Vector<DueAssertion> &r_due) {
	Array to_inject;
	r_due.clear();
	if (!started || failed) {
		return to_inject;
	}

	const int64_t elapsed = p_live_frame - live_start;
	last_observed = p_live_frame;

	for (int i = 0; i < events.size(); i++) {
		ScheduledEvent &scheduled = events.write[i];
		if (scheduled.injected || scheduled.offset > elapsed) {
			continue;
		}
		// How late this event is. Zero is on time; anything large means the game ran on
		// while we were not looking, and it will see this input at a different point in
		// its own sequence than the recording did.
		const int64_t lateness = elapsed - scheduled.offset;
		max_drift = MAX(max_drift, lateness);
		if (lateness > drift_tolerance) {
			delivered_late = true;
		}
		scheduled.injected = true;
		to_inject.push_back(scheduled.event);
	}

	for (int i = 0; i < assertions.size(); i++) {
		const ScheduledAssertion &scheduled = assertions[i];
		if (scheduled.checked || scheduled.offset > elapsed) {
			continue;
		}
		DueAssertion due;
		due.node_path = scheduled.node_path;
		due.property = scheduled.property;
		due.expected = scheduled.expected;
		due.frame = live_start + scheduled.offset;
		due.index = i;
		r_due.push_back(due);
	}
	return to_inject;
}

void MCPReplayPlan::report_assertion(int p_index, const Variant &p_observed) {
	if (p_index < 0 || p_index >= assertions.size()) {
		return;
	}
	ScheduledAssertion &scheduled = assertions.write[p_index];
	if (scheduled.checked) {
		return;
	}
	scheduled.checked = true;
	scheduled.observed = p_observed;
	// Variant's own equality, so an int 3 and a float 3.0 compare equal the way the rest
	// of the engine treats them. A property whose type changed between record and replay
	// is a divergence and this reports it as one.
	scheduled.matched = (scheduled.expected == p_observed);
	if (!scheduled.matched && first_divergence < 0) {
		// First divergence ends the run. Everything after it is downstream of a game
		// already in a state the recording never visited.
		first_divergence = p_index;
		failed = true;
	}
}

bool MCPReplayPlan::is_finished() const {
	if (failed) {
		return true;
	}
	for (int i = 0; i < events.size(); i++) {
		if (!events[i].injected) {
			return false;
		}
	}
	for (int i = 0; i < assertions.size(); i++) {
		if (!assertions[i].checked) {
			return false;
		}
	}
	return true;
}

MCPReplayPlan::Verdict MCPReplayPlan::get_verdict() const {
	if (failed) {
		return VERDICT_FAILED;
	}
	if (!is_finished()) {
		return VERDICT_RUNNING;
	}
	// S6: a run whose pacing drifted has not proven the game still behaves, only that it
	// did not visibly break. Saying "passed" here would be the single most damaging lie
	// this tool could tell, because it is the answer a green suite is built on.
	if (delivered_late) {
		return VERDICT_INDETERMINATE;
	}
	return VERDICT_PASSED;
}

Dictionary MCPReplayPlan::to_report() const {
	Dictionary report;
	const Verdict verdict = get_verdict();
	report["verdict"] = verdict_to_string(verdict);

	int injected = 0;
	for (int i = 0; i < events.size(); i++) {
		injected += events[i].injected ? 1 : 0;
	}
	int checked = 0;
	int matched = 0;
	for (int i = 0; i < assertions.size(); i++) {
		checked += assertions[i].checked ? 1 : 0;
		matched += (assertions[i].checked && assertions[i].matched) ? 1 : 0;
	}

	report["events_total"] = events.size();
	report["events_injected"] = injected;
	report["assertions_total"] = assertions.size();
	report["assertions_checked"] = checked;
	report["assertions_matched"] = matched;
	report["expected_span_frames"] = expected_span;
	report["max_drift_frames"] = max_drift;
	report["drift_tolerance_frames"] = drift_tolerance;
	report["last_frame"] = last_observed;

	if (first_divergence >= 0) {
		const ScheduledAssertion &scheduled = assertions[first_divergence];
		Dictionary divergence;
		divergence["node_path"] = scheduled.node_path;
		divergence["property"] = scheduled.property;
		divergence["expected"] = scheduled.expected;
		divergence["observed"] = scheduled.observed;
		divergence["frame"] = live_start + scheduled.offset;
		divergence["assertion_index"] = first_divergence;
		report["first_divergence"] = divergence;
	}

	if (verdict == VERDICT_INDETERMINATE) {
		report["note"] = vformat(
				"The game fell %d frames behind the recording, past the %d-frame tolerance, so "
				"input reached it at a different point in its own sequence than when this "
				"session was recorded. That makes the run unreliable rather than failed: it is "
				"reported as indeterminate and must not be counted as a pass.",
				max_drift, drift_tolerance);
	} else if (verdict == VERDICT_FAILED) {
		report["note"] = "Reported at the first divergence. Later assertions were not checked, "
						 "because past this point the game is in a state the recording never "
						 "visited and anything they said would be downstream noise.";
	}
	return report;
}

void MCPReplayPlan::set_drift_tolerance(int64_t p_frames) {
	drift_tolerance = MAX((int64_t)0, p_frames);
}
