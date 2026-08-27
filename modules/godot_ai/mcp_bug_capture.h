/**************************************************************************/
/*  mcp_bug_capture.h                                                     */
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

#ifndef MCP_BUG_CAPTURE_H
#define MCP_BUG_CAPTURE_H

#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

// Capturing a bug *after* it has happened.
//
// Recording a session (`mcp_sessions.h`) has to be armed first: you say "start
// recording", play, and say "stop". That is the wrong shape for a bug, because you do not
// know it is a bug until it happens. By then the interesting input is in the past and the
// only thing an armed recorder would have caught is the reproduction you have not worked
// out yet.
//
// So the input is always being kept, in a bounded rolling buffer, and a capture is
// retroactive: "write out what just happened, under this name". The result is an ordinary
// session, which means `Godot_ReplaySession` can then re-run it - the capture is not a new
// artifact type, it is the existing one, produced from the other end.
//
// **Two buffers, because a crash destroys one of them.**
//
//   * The running game keeps its own trace (`g_trace` in `mcp_runtime_agent.cpp`), stamped
//     with the game's real process frame. It is authoritative while the game is alive: it
//     records what the game actually did with an event, not what the editor asked for.
//   * This class keeps the editor's mirror of the same events, written as they are
//     dispatched. It exists for exactly one case, which is the headline one: the game
//     process is *gone*, and its buffer went with it. The mirror survives, so "capture the
//     sequence that caused this crash" still has a sequence to capture.
//
// **Frames the mirror could not learn.** Each dispatched event is stamped with the game's
// frame when the game acknowledges it. The last event before a crash is never acknowledged
// - that is what crashing means - and it is also the most important one in the buffer, so
// dropping it is not an option and neither is inventing a frame and calling it measured.
// Such an event is placed by extrapolating from the frame rate this buffer actually
// observed, and is marked `frame_estimated` in the record, in the metadata, and in the
// tool's reply. A replay of an estimated tail is a reproduction attempt, not a proof.
class MCPBugCapture {
public:
	// Events kept before the oldest are dropped. A bug reproduction is the last handful of
	// interactions; a buffer that grew without limit would be a memory leak in the editor
	// for the sake of input nobody will replay.
	static constexpr int DEFAULT_CAPACITY = 512;

	// Identifies a dispatched event so its frame can be filled in when the game answers.
	typedef uint64_t Id;
	static constexpr Id INVALID_ID = 0;

	// True when this runtime command is input worth mirroring. Anything else - reading a
	// property, taking a screenshot - is a question, not a thing that happened to the game.
	static bool is_input_command(const String &p_command);

	// Maps `send_pointer` to the trace's `pointer`, and so on. Empty for anything else.
	static String kind_for_command(const String &p_command);

	// --- the mirror -----------------------------------------------------------

	// Records an input command as the editor dispatches it. Returns INVALID_ID for
	// anything that is not input.
	static Id record_dispatch(const String &p_command, const Dictionary &p_arguments,
			int64_t p_dispatch_msec);

	// Fills in the game's own frame for a dispatched event, from its reply. An event whose
	// reply carries no frame stays unacknowledged - there is nothing to learn from a reply
	// that did not say.
	static void record_acknowledgement(Id p_id, const Dictionary &p_payload);

	// Called when a game starts. Clears the mirror: the previous run's input is not part of
	// this run's reproduction, and keeping it would let a capture splice two games together.
	static void note_game_started();

	// Called when the game goes away. The buffer is deliberately kept - it is the only copy
	// of the trace now - but a capture taken afterwards says the game was already gone.
	static void note_game_stopped();

	// True when the last thing this mirror saw was a game that then stopped.
	static bool game_stopped_since_last_start();

	// Everything buffered, oldest first, each `{kind, frame?, dispatch_msec, acknowledged,
	// ...arguments}`.
	static Array snapshot();
	static void clear();
	static void set_capacity(int p_capacity);
	static int get_capacity();

	// --- assembly -------------------------------------------------------------
	//
	// Pure from here down: no game, no disk, no singletons. This is the part that decides
	// what a capture contains and what it is allowed to claim, so it is the part worth
	// testing without any of the machinery around it.

	struct Window {
		// Ready to hand to MCPSessions::append_events: every entry carries a frame, in
		// non-decreasing order.
		Array events;
		int considered = 0;
		int kept = 0;
		// Events whose frame had to be extrapolated because the game never acknowledged
		// them. Non-zero means the tail of this capture is an attempt, not a record.
		int estimated = 0;
		// Events dropped because no frame could be worked out for them at all.
		int dropped = 0;
		int64_t first_frame = 0;
		int64_t last_frame = 0;
	};

	// Chooses what a capture contains and gives every kept event a frame.
	//
	// `p_last_events` of 0 keeps everything in the window; `p_since_frame` of 0 imposes no
	// lower bound. Both may be given, and the narrower wins - "the last ten events, but
	// only since the level loaded" is a reasonable thing to ask for.
	static Window select(const Array &p_events, int p_last_events, int64_t p_since_frame);

	// One sentence a person can read that says what this capture can prove. Never
	// optimistic: an estimated tail and a dead game both make it into the text.
	static String describe_fidelity(const Window &p_window, const String &p_source,
			bool p_game_running);

	// The metadata a captured session carries beyond an ordinary recording: where the
	// events came from, what the caller said was wrong, and what the capture cannot claim.
	static Dictionary build_context(const Window &p_window, const String &p_source,
			bool p_game_running, const String &p_reason, const Array &p_errors);

private:
	// Frames per millisecond as this buffer actually observed it, or 0 when fewer than two
	// acknowledged events make the question unanswerable.
	static double _observed_frame_rate(const Array &p_events);
};

#endif // MCP_BUG_CAPTURE_H
