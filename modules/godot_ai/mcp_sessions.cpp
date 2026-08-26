/**************************************************************************/
/*  mcp_sessions.cpp                                                      */
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

#include "mcp_sessions.h"

#include "mcp_paths.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/time.h"

namespace {

const char *SESSIONS_DIR = "godot_ai_sessions";
const char *META_FILE = "meta.json";
const char *TRACE_FILE = "trace.jsonl";
const char *ASSERT_FILE = "asserts.jsonl";

// Bounded so a runaway recording cannot fill the user's disk. A session this long is
// not a regression test anyone will run.
const int MAX_RECORDS_PER_FILE = 200000;

String &root_override() {
	static String override;
	return override;
}

// Appends one JSON object per line. Godot's FileAccess has no append-open that also
// creates, so a missing file is created and an existing one is opened at its end.
Error open_for_append(const String &p_absolute, Ref<FileAccess> &r_file) {
	Error error = OK;
	if (FileAccess::exists(p_absolute)) {
		r_file = FileAccess::open(p_absolute, FileAccess::READ_WRITE, &error);
		if (r_file.is_valid()) {
			r_file->seek_end();
		}
	} else {
		r_file = FileAccess::open(p_absolute, FileAccess::WRITE, &error);
	}
	return r_file.is_null() ? (error == OK ? FAILED : error) : OK;
}

int count_lines(const String &p_absolute) {
	if (!FileAccess::exists(p_absolute)) {
		return 0;
	}
	Ref<FileAccess> file = FileAccess::open(p_absolute, FileAccess::READ);
	if (file.is_null()) {
		return 0;
	}
	int lines = 0;
	while (!file->eof_reached()) {
		if (!file->get_line().strip_edges().is_empty()) {
			lines++;
		}
	}
	return lines;
}

} // namespace

String MCPSessions::get_root() {
	if (!root_override().is_empty()) {
		return root_override();
	}
	return MCPPaths::get_user_root().path_join(SESSIONS_DIR);
}

void MCPSessions::set_root_override(const String &p_absolute) {
	root_override() = p_absolute;
}

void MCPSessions::clear_root_override() {
	root_override() = String();
}

String MCPSessions::slugify(const String &p_name, String &r_error) {
	String slug;
	const String trimmed = p_name.strip_edges();
	for (int i = 0; i < trimmed.length(); i++) {
		const char32_t c = trimmed[i];
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
			// '_' and '-' are kept as themselves: a caller that names a session
			// "boss_fight-2" must get that slug back, or it cannot find its own session.
			slug += String::chr(c);
		} else if (c >= 'A' && c <= 'Z') {
			slug += String::chr(c + ('a' - 'A'));
		} else if (c == ' ' || c == '.' || c == '/' || c == '\\') {
			// Path separators become word breaks rather than vanishing. Dropping them
			// would turn "etc/passwd" into "etcpasswd" - two names silently colliding
			// into one, which is worse than an ugly slug. Runs collapse, so no "a--b".
			if (!slug.is_empty() && !slug.ends_with("-")) {
				slug += "-";
			}
		}
		// Everything else is dropped: a slug is a directory name, and this one is built
		// from a string a model chose.
	}
	while (slug.ends_with("-")) {
		slug = slug.substr(0, slug.length() - 1);
	}
	if (slug.is_empty()) {
		r_error = vformat("'%s' has no characters usable in a session name; use letters, "
						  "digits, '-' or '_'",
				p_name);
		return String();
	}
	if (slug.length() > 64) {
		slug = slug.substr(0, 64);
		while (slug.ends_with("-")) {
			slug = slug.substr(0, slug.length() - 1);
		}
	}
	return slug;
}

String MCPSessions::get_session_dir(const String &p_slug) {
	return get_root().path_join(p_slug);
}

bool MCPSessions::exists(const String &p_slug) {
	return FileAccess::exists(get_session_dir(p_slug).path_join(META_FILE));
}

MCPSessions::Result MCPSessions::begin(const String &p_slug, const String &p_name,
		int64_t p_start_frame, const Dictionary &p_context) {
	if (p_slug.is_empty()) {
		return Result::bad("a session needs a name");
	}
	const String dir = get_session_dir(p_slug);
	Ref<DirAccess> access = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (access.is_null()) {
		return Result::bad("could not reach the filesystem to create the session");
	}
	if (access->make_dir_recursive(dir) != OK) {
		return Result::bad(vformat("could not create '%s'", dir));
	}

	// Truncate any previous recording under this name. Re-recording a session is the
	// normal way to update a regression test, and half of an old trace mixed into a new
	// one is worse than either.
	for (const char *name : { TRACE_FILE, ASSERT_FILE }) {
		const String path = dir.path_join(name);
		if (FileAccess::exists(path)) {
			Ref<FileAccess> truncate = FileAccess::open(path, FileAccess::WRITE);
			if (truncate.is_null()) {
				return Result::bad(vformat("could not clear the previous '%s'", name));
			}
		}
	}

	Dictionary meta;
	meta["name"] = p_name;
	meta["slug"] = p_slug;
	meta["start_frame"] = p_start_frame;
	meta["end_frame"] = -1;
	meta["started"] = Time::get_singleton()->get_datetime_string_from_system(true);
	meta["verdict"] = "recording";
	meta["context"] = p_context.duplicate(true);
	// Says out loud what the trace is, so nothing downstream has to infer it.
	meta["input_source"] = "editor_injected";
	meta["note"] = "Frames are the game's process frames. This trace covers input the "
				   "editor injected; it does not observe a human playing the window.";

	Ref<FileAccess> file = FileAccess::open(dir.path_join(META_FILE), FileAccess::WRITE);
	if (file.is_null()) {
		return Result::bad(vformat("could not write the session metadata in '%s'", dir));
	}
	file->store_string(JSON::stringify(meta, "  "));
	return Result::good();
}

MCPSessions::Result MCPSessions::_append_lines(const String &p_slug, const String &p_file,
		const Array &p_records) {
	if (!exists(p_slug)) {
		return Result::bad(vformat("no session '%s' has been started", p_slug));
	}
	if (p_records.is_empty()) {
		return Result::good();
	}
	const String path = get_session_dir(p_slug).path_join(p_file);
	if (count_lines(path) + p_records.size() > MAX_RECORDS_PER_FILE) {
		return Result::bad(vformat("session '%s' would exceed %d records in '%s'; stop the "
								   "recording and start another",
				p_slug, MAX_RECORDS_PER_FILE, p_file));
	}

	Ref<FileAccess> file;
	if (open_for_append(path, file) != OK) {
		return Result::bad(vformat("could not append to '%s'", path));
	}
	for (int i = 0; i < p_records.size(); i++) {
		const Variant record = p_records[i];
		if (record.get_type() != Variant::DICTIONARY) {
			return Result::bad(vformat("record %d is not an object", i));
		}
		// One line per record and no embedded newlines, or the reader loses the framing.
		file->store_line(JSON::stringify(record));
	}
	return Result::good();
}

MCPSessions::Result MCPSessions::append_events(const String &p_slug, const Array &p_events) {
	for (int i = 0; i < p_events.size(); i++) {
		const Variant event = p_events[i];
		if (event.get_type() != Variant::DICTIONARY) {
			return Result::bad(vformat("event %d is not an object", i));
		}
		// get() rather than operator[]: reading a missing key through a const Dictionary
		// inserts a null, which would then be written to the trace as a real field.
		const Dictionary as_dict = event;
		if (as_dict.get("frame", Variant()).get_type() == Variant::NIL) {
			return Result::bad(vformat(
					"event %d has no 'frame'; a trace indexed by nothing cannot be replayed", i));
		}
	}
	return _append_lines(p_slug, TRACE_FILE, p_events);
}

MCPSessions::Result MCPSessions::append_assertions(const String &p_slug, const Array &p_assertions) {
	for (int i = 0; i < p_assertions.size(); i++) {
		const Variant assertion = p_assertions[i];
		if (assertion.get_type() != Variant::DICTIONARY) {
			return Result::bad(vformat("assertion %d is not an object", i));
		}
		const Dictionary as_dict = assertion;
		if (String(as_dict.get("node_path", String())).is_empty()) {
			return Result::bad(vformat("assertion %d has no 'node_path'", i));
		}
		if (String(as_dict.get("property", String())).is_empty()) {
			return Result::bad(vformat("assertion %d has no 'property'", i));
		}
	}
	return _append_lines(p_slug, ASSERT_FILE, p_assertions);
}

MCPSessions::Result MCPSessions::finish(const String &p_slug, int64_t p_end_frame,
		const String &p_verdict, const Dictionary &p_summary) {
	Dictionary meta = read_meta(p_slug);
	if (meta.is_empty()) {
		return Result::bad(vformat("no session '%s' has been started", p_slug));
	}
	const String dir = get_session_dir(p_slug);
	meta["end_frame"] = p_end_frame;
	meta["finished"] = Time::get_singleton()->get_datetime_string_from_system(true);
	meta["verdict"] = p_verdict;
	meta["event_count"] = count_lines(dir.path_join(TRACE_FILE));
	meta["assertion_count"] = count_lines(dir.path_join(ASSERT_FILE));
	const int64_t start = meta.get("start_frame", 0);
	meta["frame_span"] = p_end_frame >= start ? (p_end_frame - start) : 0;
	if (!p_summary.is_empty()) {
		meta["summary"] = p_summary.duplicate(true);
	}

	Ref<FileAccess> file = FileAccess::open(dir.path_join(META_FILE), FileAccess::WRITE);
	if (file.is_null()) {
		return Result::bad(vformat("could not update the session metadata in '%s'", dir));
	}
	file->store_string(JSON::stringify(meta, "  "));
	return Result::good();
}

Dictionary MCPSessions::read_meta(const String &p_slug) {
	const String path = get_session_dir(p_slug).path_join(META_FILE);
	if (!FileAccess::exists(path)) {
		return Dictionary();
	}
	const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(path));
	return parsed.get_type() == Variant::DICTIONARY ? Dictionary(parsed) : Dictionary();
}

Array MCPSessions::_read_lines(const String &p_absolute, int p_limit) {
	Array out;
	if (!FileAccess::exists(p_absolute)) {
		return out;
	}
	Ref<FileAccess> file = FileAccess::open(p_absolute, FileAccess::READ);
	if (file.is_null()) {
		return out;
	}
	while (!file->eof_reached()) {
		const String line = file->get_line().strip_edges();
		if (line.is_empty()) {
			continue;
		}
		const Variant parsed = JSON::parse_string(line);
		if (parsed.get_type() == Variant::DICTIONARY) {
			out.push_back(parsed);
		}
		if (p_limit > 0 && out.size() >= p_limit) {
			break;
		}
	}
	return out;
}

Array MCPSessions::read_events(const String &p_slug, int p_limit) {
	return _read_lines(get_session_dir(p_slug).path_join(TRACE_FILE), p_limit);
}

Array MCPSessions::read_assertions(const String &p_slug) {
	return _read_lines(get_session_dir(p_slug).path_join(ASSERT_FILE), 0);
}

Array MCPSessions::list() {
	Array out;
	const String root = get_root();
	Ref<DirAccess> access = DirAccess::open(root);
	if (access.is_null()) {
		// No root means no sessions. That is an answer, not a failure.
		return out;
	}
	access->list_dir_begin();
	for (String entry = access->get_next(); !entry.is_empty(); entry = access->get_next()) {
		if (!access->current_is_dir() || entry.begins_with(".")) {
			continue;
		}
		Dictionary meta = read_meta(entry);
		if (meta.is_empty()) {
			continue;
		}
		const String dir = root.path_join(entry);
		meta["event_count"] = count_lines(dir.path_join(TRACE_FILE));
		meta["assertion_count"] = count_lines(dir.path_join(ASSERT_FILE));
		meta["directory"] = String("user://") + SESSIONS_DIR + "/" + entry;

		// Newest first, by the timestamp written at begin(). A plain string compare is
		// right: the format is ISO-8601, which sorts lexicographically. Inserted in place
		// rather than sorted afterwards - a project has tens of sessions, not thousands,
		// and this keeps the ordering rule next to the field it reads.
		const String started = meta.get("started", String());
		int at = out.size();
		for (int i = 0; i < out.size(); i++) {
			if (started > String(Dictionary(out[i]).get("started", String()))) {
				at = i;
				break;
			}
		}
		out.insert(at, meta);
	}
	access->list_dir_end();
	return out;
}

MCPSessions::Result MCPSessions::remove(const String &p_slug) {
	const String root = get_root();
	const String dir = get_session_dir(p_slug);
	// Confinement, deliberately paranoid: a recursive delete that started in the wrong
	// place once erased this entire repository (DEC-0006). A slug that escapes the root
	// is refused rather than normalised.
	if (p_slug.is_empty() || p_slug.contains("..") || p_slug.contains("/") || p_slug.contains("\\")) {
		return Result::bad(vformat("'%s' is not a session name", p_slug));
	}
	if (!dir.begins_with(root)) {
		return Result::bad("refusing to delete outside the session root");
	}
	if (!exists(p_slug)) {
		return Result::bad(vformat("no session '%s'", p_slug));
	}
	Ref<DirAccess> access = DirAccess::open(dir);
	if (access.is_null()) {
		return Result::bad(vformat("could not open '%s'", dir));
	}
	if (access->erase_contents_recursive() != OK) {
		return Result::bad(vformat("could not clear '%s'", dir));
	}
	Ref<DirAccess> parent = DirAccess::open(root);
	if (parent.is_valid()) {
		parent->remove(p_slug);
	}
	return Result::good();
}
