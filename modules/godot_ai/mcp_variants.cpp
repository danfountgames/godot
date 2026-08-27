/**************************************************************************/
/*  mcp_variants.cpp                                                      */
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

#include "mcp_variants.h"

#include "core/variant/variant.h"

const char *MCPVariants::ORIGINAL_NAME = "original";

int MCPVariants::_index_of(const String &p_name) const {
	for (int i = 0; i < candidates.size(); i++) {
		if (candidates[i].name == p_name) {
			return i;
		}
	}
	return -1;
}

void MCPVariants::_bank_time(int64_t p_now_msec) {
	const int index = _index_of(current);
	if (index < 0) {
		return;
	}
	// `times_applied`, not a zero timestamp, is what says whether the clock is running. A
	// set opens on the original without applying it, so there is a candidate that is
	// current and has never been live - and zero is a perfectly valid tick count, so using
	// it as the "not started" marker would throw away the first interval of any session
	// that happened to begin at it.
	if (candidates[index].times_applied == 0) {
		return;
	}
	if (p_now_msec > current_since_msec) {
		candidates.write[index].msec_live += p_now_msec - current_since_msec;
	}
	current_since_msec = p_now_msec;
}

bool MCPVariants::open_set(const String &p_runtime_path, const String &p_property,
		const Variant &p_original_value, const String &p_original_text,
		const Array &p_candidates, String &r_error) {
	if (open) {
		r_error = vformat("already tuning '%s.%s'; there is one running game, so there is one "
						  "tuning set. Keep one or discard the set first.",
				runtime_path, property);
		return false;
	}
	if (p_candidates.size() < 2) {
		// One value is not a choice. Allowing it would turn Godot_SetRuntimeProperty into a
		// ceremony and produce a "comparison" with nothing to compare.
		r_error = "a tuning set needs at least two values; with one there is nothing to "
				  "choose between, so use Godot_SetRuntimeProperty instead";
		return false;
	}

	reset();
	// The value that was there before, always available to switch back to. Comparing a
	// candidate against what the game already had is the comparison that matters most, and
	// it is the one a designer forgets to make.
	Candidate original;
	original.name = ORIGINAL_NAME;
	original.value = p_original_value;
	original.text = p_original_text;
	candidates.push_back(original);

	for (int i = 0; i < p_candidates.size(); i++) {
		Candidate candidate;
		const Variant entry = p_candidates[i];
		bool has_value = false;
		if (entry.get_type() == Variant::DICTIONARY) {
			const Dictionary as_dict = entry;
			// get() and has() rather than operator[]: a missing key read through a const
			// Dictionary inserts a null, which would then look like a real field.
			has_value = as_dict.has("value");
			candidate.value = as_dict.get("value", Variant());
			candidate.name = String(as_dict.get("name", String())).strip_edges();
		} else {
			has_value = true;
			candidate.value = entry;
		}
		if (!has_value) {
			r_error = vformat("candidate %d has no 'value'", i);
			reset();
			return false;
		}
		candidate.text = String(candidate.value);
		if (candidate.text.is_empty()) {
			// An empty printed form leaves nothing to name it by and nothing to show. That
			// is not a value anybody meant to tune to.
			r_error = vformat("candidate %d's value is empty", i);
			reset();
			return false;
		}
		if (candidate.name.is_empty()) {
			// The value itself is a perfectly good name for a number, and a name nobody
			// chose is one nobody has to remember.
			candidate.name = candidate.text;
		}
		if (candidate.name == ORIGINAL_NAME) {
			r_error = vformat("'%s' is reserved for the value the property already held", ORIGINAL_NAME);
			reset();
			return false;
		}
		if (_index_of(candidate.name) >= 0) {
			// Two candidates under one name cannot be told apart afterwards, which defeats
			// the point of naming them.
			r_error = vformat("two candidates are both called '%s'", candidate.name);
			reset();
			return false;
		}
		candidates.push_back(candidate);
	}

	runtime_path = p_runtime_path;
	property = p_property;
	original_value = p_original_value;
	original_text = p_original_text;
	current = ORIGINAL_NAME;
	current_since_msec = 0;
	open = true;
	return true;
}

bool MCPVariants::switch_to(const String &p_name, int64_t p_now_msec, String &r_error) {
	if (!open) {
		r_error = "no tuning set is open";
		return false;
	}
	const int index = _index_of(p_name);
	if (index < 0) {
		String names;
		for (int i = 0; i < candidates.size(); i++) {
			names += (i > 0 ? ", " : "") + candidates[i].name;
		}
		r_error = vformat("no candidate called '%s'; this set holds %s", p_name, names);
		return false;
	}

	_bank_time(p_now_msec);
	current = p_name;
	current_since_msec = p_now_msec;
	candidates.write[index].times_applied++;
	return true;
}

bool MCPVariants::add_note(const String &p_name, const String &p_note, String &r_error) {
	if (!open) {
		r_error = "no tuning set is open";
		return false;
	}
	const String name = p_name.is_empty() ? current : p_name;
	const int index = _index_of(name);
	if (index < 0) {
		r_error = vformat("no candidate called '%s'", name);
		return false;
	}
	Candidate &candidate = candidates.write[index];
	// Appended rather than replaced: a second look at the same value is a second
	// observation, and overwriting the first would lose the change of mind that is the most
	// interesting thing in the record.
	candidate.note = candidate.note.is_empty() ? p_note : candidate.note + "; " + p_note;
	return true;
}

bool MCPVariants::can_keep(const String &p_name, String &r_error) const {
	if (!open) {
		r_error = "no tuning set is open";
		return false;
	}
	const int index = _index_of(p_name);
	if (index < 0) {
		r_error = vformat("no candidate called '%s'", p_name);
		return false;
	}
	if (candidates[index].times_applied == 0) {
		// Keeping a value that was never live is not tuning. It is editing the scene by a
		// longer route, and calling it the result of a comparison would be a claim about a
		// thing that did not happen.
		r_error = vformat("'%s' was never applied to the running game, so nothing about how it "
						  "plays has been observed. Switch to it first, or set the value "
						  "directly with Godot_SetSceneProperty if that is what you meant.",
				p_name);
		return false;
	}
	return true;
}

Variant MCPVariants::value_for(const String &p_name, bool &r_found) const {
	const int index = _index_of(p_name);
	r_found = index >= 0;
	return index < 0 ? Variant() : candidates[index].value;
}

String MCPVariants::text_for(const String &p_name) const {
	const int index = _index_of(p_name);
	return index < 0 ? String() : candidates[index].text;
}

void MCPVariants::close(int64_t p_now_msec) {
	_bank_time(p_now_msec);
}

Dictionary MCPVariants::summary() const {
	Dictionary result;
	result["runtime_path"] = runtime_path;
	result["property"] = property;
	result["original"] = original_text;
	result["current"] = current;

	Array entries;
	for (int i = 0; i < candidates.size(); i++) {
		const Candidate &candidate = candidates[i];
		Dictionary entry;
		entry["name"] = candidate.name;
		// Both forms. `value` is what the caller gave and what would be sent again, so it
		// can be fed straight back; `text` is what a person reads. Reporting only the
		// printed form would make a Vector2 candidate impossible to reuse from the reply.
		entry["value"] = candidate.value;
		entry["text"] = candidate.text;
		entry["times_applied"] = candidate.times_applied;
		entry["msec_live"] = candidate.msec_live;
		entry["tried"] = candidate.times_applied > 0;
		// Said per candidate as well as in the summary sentence, because this is what a
		// reader scanning the list needs in order to discount a row.
		entry["only_briefly"] = candidate.times_applied > 0 && candidate.msec_live < BRIEF_MSEC;
		if (!candidate.note.is_empty()) {
			entry["note"] = candidate.note;
		}
		entries.push_back(entry);
	}
	result["candidates"] = entries;
	result["comparison"] = describe_comparison();
	return result;
}

String MCPVariants::describe_comparison() const {
	int tried = 0;
	int properly_tried = 0;
	for (int i = 0; i < candidates.size(); i++) {
		if (candidates[i].times_applied == 0) {
			continue;
		}
		tried++;
		if (candidates[i].msec_live >= BRIEF_MSEC) {
			properly_tried++;
		}
	}

	if (tried == 0) {
		return "Nothing has been tried yet. Switch to a candidate to put it in the running "
			   "game.";
	}
	if (tried == 1) {
		return vformat("Only one of the %d values has been live, so there is nothing to compare "
					   "it against yet - not even the original.",
				candidates.size());
	}
	if (properly_tried < 2) {
		return vformat("%d values have been live, but %d of them for under %d ms - long enough "
					   "to see a number change and not long enough to play anything. Keeping one "
					   "now records a choice rather than a comparison.",
				tried, tried - properly_tried, (int)BRIEF_MSEC);
	}
	return vformat("%d of %d values have been live, %d of them for long enough to judge.",
			tried, candidates.size(), properly_tried);
}

void MCPVariants::reset() {
	candidates.clear();
	runtime_path = String();
	property = String();
	original_value = Variant();
	original_text = String();
	current = String();
	current_since_msec = 0;
	open = false;
}
