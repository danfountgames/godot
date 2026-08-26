/**************************************************************************/
/*  mcp_activity.cpp                                                      */
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

#include "mcp_activity.h"

#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/variant/variant.h"

// State lives in function-local statics rather than namespace-scope ones.
//
// `Array` is a Variant-family type: a namespace-scope instance is constructed before the
// engine's memory subsystem exists and destroyed after it is gone, and the ordering
// against the rest of the engine's static initialisation is not defined. A full-suite
// run segfaulted once inside an unrelated tool test with that arrangement in place and
// then would not reproduce, which is exactly the shape that hazard takes. Function-local
// statics are constructed on first use, after the engine is up.
//
// The mutex is a plain wrapper and would have been safe either way; it is here so the
// lock and the data it guards cannot get different lifetimes.
static Mutex &activity_mutex() {
	static Mutex mutex;
	return mutex;
}

static Array &activity_records() {
	static Array records;
	return records;
}

static int64_t &activity_next_sequence() {
	static int64_t sequence = 0;
	return sequence;
}

static int &activity_capacity() {
	static int capacity = MCPActivity::DEFAULT_CAPACITY;
	return capacity;
}

namespace {

// Argument keys whose *value* is a node path rather than a file path. Checked before
// the res:// test, because a node path never carries a scheme and would otherwise fall
// through to no subject at all.
const char *NODE_PATH_KEYS[] = {
	"node_path", "node", "parent", "parent_path", "target", "target_path",
	"from", "to", "path_in_scene", nullptr
};

bool is_node_path_key(const String &p_key) {
	for (int i = 0; NODE_PATH_KEYS[i]; i++) {
		if (p_key == NODE_PATH_KEYS[i]) {
			return true;
		}
	}
	return false;
}

bool looks_like_project_path(const String &p_value) {
	return p_value.begins_with("res://") || p_value.begins_with("user://");
}

// A node path as this interface uses them: slash-separated, no scheme, no extension,
// and short enough to be a path rather than prose. Deliberately conservative - a false
// negative costs a missing highlight, a false positive puts a sentence in the dock.
bool looks_like_node_path(const String &p_value) {
	if (p_value.is_empty() || p_value.length() > 256) {
		return false;
	}
	if (p_value.contains("://") || p_value.contains(" ")) {
		return false;
	}
	return true;
}

void push_subject(Array &r_subjects, const String &p_kind, const String &p_path) {
	if (p_path.is_empty()) {
		return;
	}
	// The same path can arrive under two keys ("from"/"to" on a move that did not move);
	// the dock should highlight it once.
	for (int i = 0; i < r_subjects.size(); i++) {
		const Dictionary existing = r_subjects[i];
		if (String(existing.get("path", String())) == p_path &&
				String(existing.get("kind", String())) == p_kind) {
			return;
		}
	}
	Dictionary subject;
	subject["kind"] = p_kind;
	subject["path"] = p_path;
	r_subjects.push_back(subject);
}

} // namespace

Array MCPActivity::extract_subjects(const Dictionary &p_arguments) {
	Array subjects;
	const Array keys = p_arguments.keys();
	for (int i = 0; i < keys.size(); i++) {
		const String key = keys[i];
		// get() rather than operator[]: reading a missing key through a const Dictionary
		// inserts a null, and these arguments are about to be schema-validated elsewhere.
		const Variant value = p_arguments.get(key, Variant());
		if (value.get_type() != Variant::STRING && value.get_type() != Variant::STRING_NAME) {
			continue;
		}
		const String text = value;
		if (looks_like_project_path(text)) {
			push_subject(subjects, "file", text);
		} else if (is_node_path_key(key) && looks_like_node_path(text)) {
			push_subject(subjects, "node", text);
		}
	}
	return subjects;
}

MCPActivity::Id MCPActivity::begin(const String &p_client, const String &p_tool,
		MCPCapability p_capability, const String &p_summary, const Array &p_subjects) {
	MutexLock lock(activity_mutex());

	activity_next_sequence()++;
	Dictionary record;
	record["sequence"] = activity_next_sequence();
	record["client"] = p_client;
	record["tool"] = p_tool;
	record["capability"] = mcp_capability_to_string(p_capability);
	record["summary"] = p_summary;
	record["subjects"] = p_subjects.duplicate();
	record["started"] = Time::get_singleton()->get_datetime_string_from_system(true);
	record["outcome"] = "running";
	record["detail"] = String();
	record["checkpoint"] = String();
	record["duration_ms"] = 0;
	// Not part of the wire format; how finish() computes a duration without trusting the
	// wall clock, which a user can change mid-session.
	record["_started_ticks"] = (int64_t)OS::get_singleton()->get_ticks_msec();

	activity_records().push_back(record);
	while (activity_records().size() > activity_capacity()) {
		activity_records().remove_at(0);
	}
	return (Id)activity_next_sequence();
}

int MCPActivity::_find(Id p_id) {
	for (int i = activity_records().size() - 1; i >= 0; i--) {
		const Dictionary record = activity_records()[i];
		if ((int64_t)record.get("sequence", 0) == (int64_t)p_id) {
			return i;
		}
	}
	return -1;
}

void MCPActivity::finish(Id p_id, const String &p_outcome, const String &p_detail,
		const String &p_checkpoint) {
	if (p_id == INVALID_ID) {
		return;
	}
	MutexLock lock(activity_mutex());

	const int index = _find(p_id);
	if (index < 0) {
		// Aged out of the buffer while it ran. Nothing to correct, and inventing a record
		// here would put a finished call *after* newer ones in the stream.
		return;
	}
	Dictionary record = activity_records()[index];
	const int64_t started = record.get("_started_ticks", 0);
	record["outcome"] = p_outcome;
	record["detail"] = p_detail;
	record["checkpoint"] = p_checkpoint;
	record["duration_ms"] = (int64_t)OS::get_singleton()->get_ticks_msec() - started;
}

void MCPActivity::refuse(const String &p_client, const String &p_tool, MCPCapability p_capability,
		const String &p_summary, const Array &p_subjects, const String &p_reason) {
	const Id id = begin(p_client, p_tool, p_capability, p_summary, p_subjects);
	finish(id, "refused", p_reason, String());
}

Array MCPActivity::snapshot(int64_t p_after_sequence, int p_limit) {
	MutexLock lock(activity_mutex());

	Array out;
	if (p_limit <= 0) {
		return out;
	}
	for (int i = 0; i < activity_records().size(); i++) {
		const Dictionary record = activity_records()[i];
		if ((int64_t)record.get("sequence", 0) <= p_after_sequence) {
			continue;
		}
		Dictionary copy = record.duplicate(true);
		copy.erase("_started_ticks");
		out.push_back(copy);
		if (out.size() >= p_limit) {
			break;
		}
	}
	return out;
}

int64_t MCPActivity::get_latest_sequence() {
	MutexLock lock(activity_mutex());
	if (activity_records().is_empty()) {
		return 0;
	}
	const Dictionary newest = activity_records()[activity_records().size() - 1];
	return newest.get("sequence", 0);
}

bool MCPActivity::has_running() {
	MutexLock lock(activity_mutex());
	for (int i = activity_records().size() - 1; i >= 0; i--) {
		const Dictionary record = activity_records()[i];
		if (String(record.get("outcome", String())) == "running") {
			return true;
		}
	}
	return false;
}

void MCPActivity::clear() {
	MutexLock lock(activity_mutex());
	activity_records().clear();
	// Sequence numbers are not reset: a dock polling "everything after 41" must not be
	// handed a different record 42 after a clear.
}

void MCPActivity::set_capacity(int p_capacity) {
	if (p_capacity < 1) {
		return;
	}
	MutexLock lock(activity_mutex());
	activity_capacity() = p_capacity;
	while (activity_records().size() > activity_capacity()) {
		activity_records().remove_at(0);
	}
}

int MCPActivity::get_capacity() {
	MutexLock lock(activity_mutex());
	return activity_capacity();
}
