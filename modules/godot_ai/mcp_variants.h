/**************************************************************************/
/*  mcp_variants.h                                                        */
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

#ifndef MCP_VARIANTS_H
#define MCP_VARIANTS_H

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

// A live tuning workspace: several values for one property, tried against a running game.
//
// Deliberately **not** general "AI-generated alternatives". The gesture this supports is
// the one a designer already makes by hand and badly: change the jump height, play it,
// change it again, play it again, lose track of which of the four numbers felt right, and
// end up typing whichever one is still in the box. So a variant set is one node, one
// property, and a handful of named candidate values that can be flipped between while the
// game keeps running - with the original captured first, so discarding puts it back.
//
// The last act is promotion, which already exists (`Godot_PromoteRuntimeValue`): the value
// that felt right becomes the authored value. Without that, as the user put it, live
// tuning is theatre because the last act is manual.
//
// **What this measures, and what it refuses to claim.** It knows how long each candidate
// was the live value, because it holds the clock. It does not know whether the game was in
// the relevant state at the time, or whether anyone was watching. So it reports the
// milliseconds rather than a verdict, and when a choice is made over candidates that were
// each live for under half a second it says the run recorded a choice and not a
// comparison. That is a fact about the session, not a judgement about the designer.
//
// Pure: no editor, no game, no clock of its own. The caller passes the time in. That is
// what makes the interesting rules - what may be kept, what a brief look is worth -
// testable without any of the machinery around them.
class MCPVariants {
public:
	// A candidate value was live for less than this, so nobody can have felt it. Half a
	// second is about thirty frames: long enough to see a number change, not long enough
	// to play anything. Chosen to be defensible rather than exact, and reported rather
	// than enforced - refusing here would be this class overruling the person tuning.
	static constexpr int64_t BRIEF_MSEC = 500;

	// The name reserved for the value the property held before any of this started.
	static const char *ORIGINAL_NAME;

	struct Candidate {
		String name;
		// The value as the caller gave it, handed to the runtime unchanged. `set_property`
		// already knows how to turn a JSON `[64, 32]` into the Vector2 the property holds,
		// so nothing here has to re-solve that - and a tuning set that could not tune a
		// position would be missing half the cases worth tuning.
		Variant value;
		// The same value printed, which is what a name defaults to and what a reader sees.
		String text;
		int times_applied = 0;
		int64_t msec_live = 0;
		// What the agent or the user observed while this one was live. The whole point of
		// the exercise is a judgement, and a set of numbers with no judgements attached is
		// a list of numbers.
		String note;
	};

	// True while a set is open.
	bool is_open() const { return open; }

	String get_runtime_path() const { return runtime_path; }
	String get_property() const { return property; }
	String get_current() const { return current; }
	String get_original_text() const { return original_text; }
	int get_candidate_count() const { return candidates.size(); }

	// Opens a set. `p_candidates` is an Array of either plain values (whose printed form
	// becomes their name) or `{name, value}` objects. Refuses fewer than two candidates:
	// one value is not a choice, and calling it one would turn Godot_SetRuntimeProperty
	// into a ceremony.
	bool open_set(const String &p_runtime_path, const String &p_property,
			const Variant &p_original_value, const String &p_original_text,
			const Array &p_candidates, String &r_error);

	// Makes a candidate the live one, closing out the previous one's time. `original` is
	// always a valid name: comparing a candidate against what was there before is the
	// comparison that matters most.
	bool switch_to(const String &p_name, int64_t p_now_msec, String &r_error);

	// Records an observation against the live candidate, or a named one.
	bool add_note(const String &p_name, const String &p_note, String &r_error);

	// Whether this candidate may be kept, and why not when it may not.
	bool can_keep(const String &p_name, String &r_error) const;

	// A named candidate, by name. `r_found` says whether there was one, because a
	// candidate's value may legitimately be null.
	Variant value_for(const String &p_name, bool &r_found) const;

	// The printed form of a named candidate. Empty when there is no such name.
	String text_for(const String &p_name) const;

	// Stops the clock without deciding anything. Call before reading a final summary.
	void close(int64_t p_now_msec);

	// Everything the reply and the record need: each candidate with its time and notes,
	// which is live, and what the set is honest about.
	Dictionary summary() const;

	// One sentence about how much of a comparison this was. Never flattering: a set whose
	// alternatives were each live for a moment says so.
	String describe_comparison() const;

	// Forgets the set entirely.
	void reset();

private:
	Vector<Candidate> candidates;
	String runtime_path;
	String property;
	Variant original_value;
	String original_text;
	String current;
	int64_t current_since_msec = 0;
	bool open = false;

	int _index_of(const String &p_name) const;
	void _bank_time(int64_t p_now_msec);
};

#endif // MCP_VARIANTS_H
