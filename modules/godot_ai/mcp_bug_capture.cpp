/**************************************************************************/
/*  mcp_bug_capture.cpp                                                   */
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

#include "mcp_bug_capture.h"

#include "core/math/math_funcs.h"
#include "core/os/mutex.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

namespace {

// The buffer and its counters live in function-local statics rather than at namespace
// scope. A `Dictionary` at namespace scope is a Variant-family object constructed before
// the engine's memory subsystem is up and destroyed after it has gone down, which is the
// lifetime hazard that produced a shutdown crash earlier in this module's life.
struct Mirror {
	LocalVector<Dictionary> events;
	MCPBugCapture::Id next_id = 1;
	int capacity = MCPBugCapture::DEFAULT_CAPACITY;
	bool stopped = false;
	Mutex mutex;
};

Mirror &mirror() {
	static Mirror instance;
	return instance;
}

} // namespace

bool MCPBugCapture::is_input_command(const String &p_command) {
	return !kind_for_command(p_command).is_empty();
}

String MCPBugCapture::kind_for_command(const String &p_command) {
	if (p_command == "send_pointer") {
		return "pointer";
	}
	if (p_command == "send_key") {
		return "key";
	}
	if (p_command == "send_touch") {
		return "touch";
	}
	if (p_command == "send_gamepad") {
		return "gamepad";
	}
	return String();
}

MCPBugCapture::Id MCPBugCapture::record_dispatch(const String &p_command,
		const Dictionary &p_arguments, int64_t p_dispatch_msec) {
	const String kind = kind_for_command(p_command);
	if (kind.is_empty()) {
		return INVALID_ID;
	}

	Mirror &state = mirror();
	MutexLock lock(state.mutex);

	Dictionary entry = p_arguments.duplicate();
	entry["kind"] = kind;
	entry["dispatch_msec"] = p_dispatch_msec;
	entry["acknowledged"] = false;
	const Id id = state.next_id++;
	entry["_mirror_id"] = (int64_t)id;

	if ((int)state.events.size() >= state.capacity) {
		state.events.remove_at(0);
	}
	state.events.push_back(entry);
	return id;
}

void MCPBugCapture::record_acknowledgement(Id p_id, const Dictionary &p_payload) {
	if (p_id == INVALID_ID || !p_payload.has("frame")) {
		return;
	}
	Mirror &state = mirror();
	MutexLock lock(state.mutex);
	for (uint32_t i = state.events.size(); i > 0; i--) {
		Dictionary &entry = state.events[i - 1];
		if ((int64_t)entry.get("_mirror_id", 0) != (int64_t)p_id) {
			continue;
		}
		entry["frame"] = (int64_t)p_payload["frame"];
		entry["acknowledged"] = true;
		return;
	}
}

void MCPBugCapture::note_game_started() {
	Mirror &state = mirror();
	MutexLock lock(state.mutex);
	state.events.clear();
	state.stopped = false;
}

void MCPBugCapture::note_game_stopped() {
	Mirror &state = mirror();
	MutexLock lock(state.mutex);
	state.stopped = true;
}

bool MCPBugCapture::game_stopped_since_last_start() {
	Mirror &state = mirror();
	MutexLock lock(state.mutex);
	return state.stopped;
}

Array MCPBugCapture::snapshot() {
	Mirror &state = mirror();
	MutexLock lock(state.mutex);
	Array out;
	for (uint32_t i = 0; i < state.events.size(); i++) {
		out.push_back(state.events[i].duplicate());
	}
	return out;
}

void MCPBugCapture::clear() {
	Mirror &state = mirror();
	MutexLock lock(state.mutex);
	state.events.clear();
	state.stopped = false;
}

void MCPBugCapture::set_capacity(int p_capacity) {
	if (p_capacity < 1) {
		return;
	}
	Mirror &state = mirror();
	MutexLock lock(state.mutex);
	state.capacity = p_capacity;
	while ((int)state.events.size() > state.capacity) {
		state.events.remove_at(0);
	}
}

int MCPBugCapture::get_capacity() {
	Mirror &state = mirror();
	MutexLock lock(state.mutex);
	return state.capacity;
}

// ------------------------------------------------------------------- assembly ---

double MCPBugCapture::_observed_frame_rate(const Array &p_events) {
	// Two acknowledged events, as far apart as the buffer allows. Frames per millisecond
	// measured over the whole window rather than over the last pair: a single pair can
	// straddle a stall and produce a rate the run never ran at.
	int64_t first_frame = 0;
	int64_t first_msec = 0;
	int64_t last_frame = 0;
	int64_t last_msec = 0;
	bool have_first = false;
	bool have_last = false;

	for (int i = 0; i < p_events.size(); i++) {
		const Dictionary event = p_events[i];
		if (!(bool)event.get("acknowledged", false) || !event.has("frame") ||
				!event.has("dispatch_msec")) {
			continue;
		}
		const int64_t frame = event["frame"];
		const int64_t msec = event["dispatch_msec"];
		if (!have_first) {
			first_frame = frame;
			first_msec = msec;
			have_first = true;
		}
		last_frame = frame;
		last_msec = msec;
		have_last = true;
	}

	if (!have_first || !have_last || last_msec <= first_msec || last_frame < first_frame) {
		return 0.0;
	}
	return (double)(last_frame - first_frame) / (double)(last_msec - first_msec);
}

MCPBugCapture::Window MCPBugCapture::select(const Array &p_events, int p_last_events,
		int64_t p_since_frame) {
	Window window;
	window.considered = p_events.size();

	const double rate = _observed_frame_rate(p_events);

	// Pass one: give every event a frame, so the "last N" count is taken over events that
	// can actually be replayed rather than over ones that are about to be dropped.
	Array framed;
	int64_t carried_frame = 0;
	int64_t carried_msec = 0;
	bool have_carried = false;

	for (int i = 0; i < p_events.size(); i++) {
		Dictionary event = Dictionary(p_events[i]).duplicate();
		event.erase("_mirror_id");

		if (event.has("frame")) {
			carried_frame = event["frame"];
			have_carried = true;
			if (event.has("dispatch_msec")) {
				carried_msec = event["dispatch_msec"];
			}
			framed.push_back(event);
			continue;
		}

		// No frame: the game never acknowledged this one. It is almost always the event
		// the game died on, which makes it the single most valuable record in the buffer -
		// so it is placed rather than dropped, and marked so nothing downstream can read
		// the placement as a measurement.
		if (!have_carried) {
			// Nothing acknowledged before it either. There is no anchor to extrapolate
			// from, and a frame invented out of nothing would be a lie with no data behind
			// it at all.
			window.dropped++;
			continue;
		}
		int64_t offset = 1;
		if (rate > 0.0 && event.has("dispatch_msec")) {
			const int64_t elapsed = (int64_t)event["dispatch_msec"] - carried_msec;
			if (elapsed > 0) {
				offset = MAX((int64_t)1, (int64_t)Math::round(elapsed * rate));
			}
		}
		carried_frame += offset;
		if (event.has("dispatch_msec")) {
			carried_msec = event["dispatch_msec"];
		}
		event["frame"] = carried_frame;
		event["frame_estimated"] = true;
		framed.push_back(event);
	}

	// Pass two: the window the caller asked for. Both bounds may be given and the narrower
	// one wins, because "the last ten events, but only since the level loaded" is a
	// reasonable thing to ask and neither bound should quietly widen the other.
	int start = 0;
	if (p_since_frame > 0) {
		for (int i = 0; i < framed.size(); i++) {
			const Dictionary event = framed[i];
			if ((int64_t)event.get("frame", 0) >= p_since_frame) {
				start = i;
				break;
			}
			start = i + 1;
		}
	}
	if (p_last_events > 0) {
		start = MAX(start, framed.size() - p_last_events);
	}

	for (int i = start; i < framed.size(); i++) {
		const Dictionary event = framed[i];
		window.events.push_back(event);
		if ((bool)event.get("frame_estimated", false)) {
			window.estimated++;
		}
	}
	window.kept = window.events.size();
	if (window.kept > 0) {
		window.first_frame = Dictionary(window.events[0]).get("frame", 0);
		window.last_frame = Dictionary(window.events[window.kept - 1]).get("frame", 0);
	}
	return window;
}

String MCPBugCapture::describe_fidelity(const Window &p_window, const String &p_source,
		bool p_game_running) {
	if (p_window.kept == 0) {
		return "Nothing was captured: no input had been injected into the game, so there is "
			   "no sequence to replay. A bug nobody's input produced cannot be reproduced by "
			   "replaying input.";
	}

	String text;
	if (p_source == "runtime_trace") {
		text = vformat("%d event(s) from the running game's own trace, every one stamped with "
					   "the frame the game processed it on.",
				p_window.kept);
	} else {
		text = vformat("%d event(s) from the editor's mirror of what it dispatched. The game "
					   "process is gone and took its own trace with it, so this is a record of "
					   "what was sent rather than of what the game did with it.",
				p_window.kept);
	}

	if (p_window.estimated > 0) {
		text += vformat(" %d of them were never acknowledged - which is what a crash looks "
						"like from here - so their frames are extrapolated from the rate this "
						"buffer observed. Replaying that tail is a reproduction attempt, not a "
						"proof: a run that does not reproduce the bug has not disproved it.",
				p_window.estimated);
	}
	if (p_window.dropped > 0) {
		text += vformat(" %d event(s) were dropped because nothing before them was ever "
						"acknowledged, leaving no anchor to place them against.",
				p_window.dropped);
	}
	if (!p_game_running) {
		text += " Replaying this needs a game running: press play, then call "
				"Godot_ReplaySession.";
	}
	return text;
}

Dictionary MCPBugCapture::build_context(const Window &p_window, const String &p_source,
		bool p_game_running, const String &p_reason, const Array &p_errors) {
	Dictionary context;
	context["captured_retroactively"] = true;
	context["source"] = p_source;
	context["game_running_at_capture"] = p_game_running;
	context["events_considered"] = p_window.considered;
	context["events_kept"] = p_window.kept;
	context["frames_estimated"] = p_window.estimated;
	context["events_dropped"] = p_window.dropped;
	context["first_frame"] = p_window.first_frame;
	context["last_frame"] = p_window.last_frame;
	if (!p_reason.strip_edges().is_empty()) {
		context["reason"] = p_reason;
	}
	if (!p_errors.is_empty()) {
		context["errors"] = p_errors;
	}
	context["fidelity"] = describe_fidelity(p_window, p_source, p_game_running);
	// Said in the artifact itself, not only in the reply that created it. Somebody will
	// read this file months later with none of the conversation around it.
	context["replay_level"] = p_window.estimated > 0 ? "attempt" : "general";
	return context;
}
