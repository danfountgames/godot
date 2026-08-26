/**************************************************************************/
/*  mcp_sessions.h                                                        */
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

#ifndef MCP_SESSIONS_H
#define MCP_SESSIONS_H

#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

// Recorded play sessions, on disk.
//
// A session is a *trace* - the input the editor injected into the running game, one
// record per event, indexed by the game's process frame - plus *assertions* captured
// alongside it. Replaying the trace and re-checking the assertions is how a change gets
// tested against how the game actually plays rather than against a unit's idea of it.
//
// **Scope, stated once so no description has to lie about it.** The trace covers input
// this editor injected. It does not observe a human playing the game window: the only
// thing writing to the runtime trace is the four `_send_*` handlers in
// `mcp_runtime_agent.cpp`. Recording a designer's own play needs a hook in the game's
// input pipeline that does not exist yet - see S1b in the experience spec.
//
// Storage mirrors the profiler export (`mcp_profiler_recorder.cpp`), which already had
// to solve this: JSON Lines under `user://`, because the debugger channel silently drops
// messages over 8 MiB and a session of any length exceeds anything worth passing inline.
//
//   user://godot_ai_sessions/<slug>/meta.json     one object: name, frames, counts, verdict
//   user://godot_ai_sessions/<slug>/trace.jsonl   one input record per line
//   user://godot_ai_sessions/<slug>/asserts.jsonl one assertion per line
//
// Frames, not milliseconds. Wall-clock replay is not reproducible; the process frame is
// the only index both sides can agree on.
class MCPSessions {
public:
	// Result of a store operation that can fail for a reason worth telling the caller.
	struct Result {
		bool ok = false;
		String error;
		static Result good() {
			Result r;
			r.ok = true;
			return r;
		}
		static Result bad(const String &p_error) {
			Result r;
			r.error = p_error;
			return r;
		}
	};

	// Turns a caller's name into a directory-safe slug. Empty or entirely unusable names
	// are rejected rather than silently renamed, because a caller that cannot predict the
	// slug cannot find its own session again.
	static String slugify(const String &p_name, String &r_error);

	// Absolute directory for a slug. Does not create it.
	static String get_session_dir(const String &p_slug);

	// Root under which all sessions live.
	static String get_root();

	// Creates (or truncates) a session and writes its opening metadata.
	static Result begin(const String &p_slug, const String &p_name, int64_t p_start_frame,
			const Dictionary &p_context);

	// Appends input records. Each must carry a `frame`; records without one are refused,
	// because a trace indexed by nothing cannot be replayed.
	static Result append_events(const String &p_slug, const Array &p_events);

	// Appends assertions: { node_path, property, value, frame }.
	static Result append_assertions(const String &p_slug, const Array &p_assertions);

	// Finalises metadata. `p_verdict` is free-form ("recorded", "passed", "failed",
	// "indeterminate") and is what `list()` reports back.
	static Result finish(const String &p_slug, int64_t p_end_frame, const String &p_verdict,
			const Dictionary &p_summary);

	// Records how a replay of this session went, without touching the recording's own
	// frames. A replay reports on a session; it does not re-record it.
	static Result set_replay_result(const String &p_slug, const String &p_verdict,
			const Dictionary &p_report);

	// One entry per recorded session, newest first, each the session's metadata plus its
	// on-disk sizes. Never fails: a missing root is zero sessions, not an error.
	static Array list();

	// Metadata for one session. Empty when it does not exist.
	static Dictionary read_meta(const String &p_slug);

	// The trace, oldest first. `p_limit` of 0 reads all of it.
	static Array read_events(const String &p_slug, int p_limit = 0);

	// The assertions, in capture order.
	static Array read_assertions(const String &p_slug);

	static bool exists(const String &p_slug);

	// Removes a session directory and everything in it. Refuses any path that is not
	// under the session root - the same rule the test helpers enforce, for the same
	// reason (a recursive delete that starts in the wrong place once erased this
	// repository, DEC-0006).
	static Result remove(const String &p_slug);

	// Test seam: redirects the root without touching user://.
	static void set_root_override(const String &p_absolute);
	static void clear_root_override();

private:
	static Result _append_lines(const String &p_slug, const String &p_file, const Array &p_records);
	static Array _read_lines(const String &p_absolute, int p_limit);
};

#endif // MCP_SESSIONS_H
