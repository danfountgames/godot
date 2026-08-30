/**************************************************************************/
/*  mcp_playtest.h                                                        */
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

#ifndef MCP_PLAYTEST_H
#define MCP_PLAYTEST_H

#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

// A playtest: a stated goal, a bounded window of play, and a report about what happened.
//
// The report is the product; the play is the means. Everything in it already existed as
// a primitive - input injection, the activity stream, the output log, screenshots,
// checkpoints - and nothing put them together and said whether the goal was reached.
// This is that assembly, and it is deliberately the first half of the P tranche: the
// second half moves *who drives the loop* into the editor, which changes how a playtest
// is run but not what it produces.
//
// Two boundaries worth stating because they are easy to get wrong.
//
// **A playtest does not start the game.** Launching needs `run_project` and playing
// needs `simulate_input`, and a tool declares one capability class. Rather than hold
// authority the permission model cannot see, `Godot_StartPlaytest` asks for a game that
// is already running - the same decision DEC-0010 made for replay, for the same reason.
//
// **The evidence is collected, not claimed.** The agent supplies the goal, its own
// observations and a verdict; everything else in the report is read back from what the
// editor actually recorded during the window. An agent that says it pressed Jump and
// never did produces a report whose activity section says so.
//
// Storage mirrors the session store, which had to solve the same problem:
//
//   user://godot_ai_playtests/<slug>/report.json      the assembled report
//   user://godot_ai_playtests/<slug>/activity.jsonl   every call made during the window
class MCPPlaytest {
public:
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

	// What a finished playtest concluded. Free-form verdicts invite "mostly worked", so
	// this is a closed set and the caller says which one.
	enum Verdict {
		VERDICT_UNKNOWN, // Still running, or finished without one being given.
		VERDICT_REACHED, // The goal was reached.
		VERDICT_NOT_REACHED, // The goal was not reached, and nothing broke.
		VERDICT_BLOCKED, // Something prevented the attempt: a crash, an error, no game.
		VERDICT_INDETERMINATE, // The run does not support a conclusion either way.
	};

	static String verdict_to_string(Verdict p_verdict);
	static Verdict verdict_from_string(const String &p_text, bool &r_known);

	// Directory-safe slug for a goal, so a caller can find its own report again.
	static String slugify(const String &p_goal, String &r_error);

	static String get_root();
	static String get_playtest_dir(const String &p_slug);

	// Test seam, matching the session store's. A headless test process has no `user://`
	// at all, so without this the store could only be exercised through an editor - and
	// the assembly rules are exactly the part that should not need one.
	static void set_root_override(const String &p_absolute_root);
	static void clear_root_override();

	// --- the live session -----------------------------------------------------

	// Opens a playtest. One at a time: two overlapping windows would each claim the same
	// activity records and neither report would be true.
	static Result begin(const String &p_slug, const String &p_goal, int p_budget_seconds,
			const String &p_oracle, const Dictionary &p_context);

	static bool is_running();
	static String get_active_slug();

	// The agent's own account of what it saw or did. Kept separate from the collected
	// evidence in the report, because one is a claim and the other is a record.
	static Result observe(const String &p_note, const String &p_kind);

	// True once the wall-clock budget has passed. A playtest is not stopped
	// automatically - the caller decides what to do about it - but the report says
	// whether it ran over.
	static bool is_over_budget();
	static int get_elapsed_seconds();

	// Closes the playtest and writes the report. `p_verdict` and `p_summary` are the
	// caller's conclusion; everything else is assembled from what was recorded.
	// `p_frame_times` is the game's own per-frame cost over the window, as
	// `{frame, milliseconds}`; spikes are detected from it. Defaulted so a caller with no
	// running game to ask still gets a report - one that honestly reports no spikes
	// because nothing measured any, rather than because none happened.
	static Result finish(Verdict p_verdict, const String &p_summary, Dictionary &r_report,
			const Array &p_frame_times = Array());

	// Abandons the playtest without a conclusion, keeping what was collected. This is
	// what a stop looks like: partial results, said to be partial (requirement P4).
	static Result abandon(const String &p_reason, Dictionary &r_report,
			const Array &p_frame_times = Array());

	// The report as it stands, live or finished.
	static Dictionary get_report(const String &p_slug, String &r_error);
	static Array list();

	// Test seam: forget the in-memory session without writing anything.
	static void reset_for_tests();

private:
	static Result _write(const String &p_slug, const Dictionary &p_report, const Array &p_activity);

public:

	// --- assembly, testable without an editor ---------------------------------

	// Activity records that fall inside a playtest's window, and a count of the ones
	// that failed or were refused. Sequence numbers, not timestamps: the activity stream
	// is ordered by sequence and a clock is not needed to say "during".
	static Array activity_in_window(const Array &p_records, int64_t p_first_sequence,
			int64_t p_last_sequence);

	// The subset of activity that actually drove the game: the input tools. This is what
	// makes an agent's claim about what it pressed checkable.
	static Array input_in_window(const Array &p_activity);

	// Log messages that are errors or warnings, normalised to { severity, text }.
	static Array problems_from_log(const Array &p_messages);

	// Frames whose time exceeded `p_multiplier` times the median, as
	// { frame, milliseconds, times_median }. A multiplier rather than an absolute
	// threshold, because a spike is relative to how the game usually runs.
	static Array spikes_from_frame_times(const Array &p_frame_times, double p_multiplier);

	// The verdict a caller's claim is allowed to become, given the evidence.
	//
	// This is the honest core of the report. An agent that reports success while the
	// game logged an error is not lying on purpose - it did not see the error - but the
	// report must not repeat the claim unqualified. A claimed success with problems
	// recorded becomes INDETERMINATE and the report says why.
	// Writes to the running game, as opposed to injected input. Both count as having
	// acted; a read does not.
	static Array runtime_actions_in_window(const Array &p_activity);

	static Verdict reconcile_verdict(Verdict p_claimed, int p_problem_count,
			int p_input_count, bool p_over_budget, String &r_reason,
			int p_runtime_action_count = 0);

	// The whole report object, from its parts. Pure, so the shape is pinned by tests
	// rather than by whatever the tool happened to build that day.
	static Dictionary build_report(const Dictionary &p_meta, const Array &p_activity,
			const Array &p_inputs, const Array &p_problems, const Array &p_spikes,
			const Array &p_observations, Verdict p_verdict, const String &p_verdict_reason,
			const String &p_summary);
};

#endif // MCP_PLAYTEST_H
