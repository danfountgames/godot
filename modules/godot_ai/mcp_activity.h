/**************************************************************************/
/*  mcp_activity.h                                                        */
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

#ifndef MCP_ACTIVITY_H
#define MCP_ACTIVITY_H

#include "mcp_types.h"

#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

// What the agent is doing, while it does it.
//
// The audit log (`mcp_audit.h`) already records every call and every refusal, but it is
// append-only on disk and written after the fact: it answers "what happened" and cannot
// answer "what is happening". This is the live half - a bounded in-memory stream the
// Activity dock renders and `Godot_GetActivity` reads back.
//
// Deliberately *not* a second persistence layer. The audit log stays the durable record;
// this buffer is lost when the editor exits, which is requirement E6.
//
// Records move through three states:
//
//   begin()  -> outcome "running", duration unknown
//   finish() -> "ok" | "failed", with a duration
//   refuse() -> "refused", complete on arrival (the tool never ran)
//
// One state is honest rather than tidy: a tool that answers asynchronously finishes as
// "deferred", because the protocol hands the caller a token and this layer is not told
// when that token resolves. See EXPERIENCE_LEDGER.md, row E1.
class MCPActivity {
public:
	// Identifies one in-flight record. Zero is never returned by begin().
	typedef uint64_t Id;
	// constexpr, not const: a plain static const is ODR-used the moment a caller binds
	// it to a reference (a doctest CHECK does), and would then need an out-of-line
	// definition. constexpr statics are implicitly inline in C++17.
	static constexpr Id INVALID_ID = 0;

	// How many records are kept before the oldest are dropped.
	static constexpr int DEFAULT_CAPACITY = 512;

	// Opens a record for a call that is about to run. Pair with finish().
	static Id begin(const String &p_client, const String &p_tool, MCPCapability p_capability,
			const String &p_summary, const Array &p_subjects);

	// Closes a record opened by begin(). `p_outcome` is "ok", "failed" or "deferred";
	// `p_detail` carries the error message when it is not "ok". A checkpoint id, when
	// one was taken, is what makes the record revertible from the dock.
	static void finish(Id p_id, const String &p_outcome, const String &p_detail,
			const String &p_checkpoint);

	// Records a call that was refused before it ran, so the stream shows attempts and
	// not only successes - the same reason the audit log records refusals.
	static void refuse(const String &p_client, const String &p_tool, MCPCapability p_capability,
			const String &p_summary, const Array &p_subjects, const String &p_reason);

	// Records newer than `p_after_sequence`, oldest first, at most `p_limit` of them.
	// Pass 0 to read from the start of what is still buffered.
	static Array snapshot(int64_t p_after_sequence = 0, int p_limit = DEFAULT_CAPACITY);

	// Sequence number of the newest record, or 0 when the stream is empty. Lets a poller
	// (the dock) ask "has anything happened" without copying the buffer.
	static int64_t get_latest_sequence();

	// True while at least one record is still "running".
	static bool has_running();

	// Drops every record. The audit log is untouched.
	static void clear();

	// Test seam and dock setting. Values below 1 are ignored.
	static void set_capacity(int p_capacity);
	static int get_capacity();

	// Best-effort extraction of the concrete things a call touches, from its own
	// arguments: `res://` and `user://` values become file subjects, values that parse as
	// node paths under a node-ish key become node subjects (requirement E3).
	//
	// Heuristic by construction - it reads arguments it does not own. A tool that knows
	// better overrides MCPTool::get_activity_subjects() and this is never consulted.
	static Array extract_subjects(const Dictionary &p_arguments);

private:
	// The buffer, its sequence counter and its mutex live in function-local statics in
	// the .cpp - an `Array` at namespace scope is a Variant-family object constructed
	// before the engine's memory subsystem and destroyed after it. See the comment there.

	// Index into the record buffer for an id, or -1. Caller holds the mutex.
	static int _find(Id p_id);
};

#endif // MCP_ACTIVITY_H
