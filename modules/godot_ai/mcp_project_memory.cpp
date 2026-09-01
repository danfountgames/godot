/**************************************************************************/
/*  mcp_project_memory.cpp                                                */
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

#include "mcp_project_memory.h"

#include "mcp_paths.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/time.h"

const char *MCPProjectMemory::MEMORY_DIR = ".godot_ai/memory";

namespace {

// Sorts most-recently-updated first. `updated` is ISO-8601 UTC with fixed field
// widths, so lexicographic order is chronological order; a note with no timestamp
// (hand-written, or from before the field existed) sorts last rather than first,
// because an unknown age is not a recent one.
struct NoteRecency {
	bool operator()(const MCPProjectMemory::Note &a, const MCPProjectMemory::Note &b) const {
		if (a.updated.is_empty() != b.updated.is_empty()) {
			return b.updated.is_empty();
		}
		if (a.updated != b.updated) {
			return a.updated > b.updated;
		}
		return a.name < b.name;
	}
};

static String memory_dir_absolute(String &r_error) {
	MCPPaths::Resolved resolved;
	if (!MCPPaths::resolve(MCPProjectMemory::MEMORY_DIR, resolved, r_error)) {
		return String();
	}
	return resolved.absolute;
}

} // namespace

String MCPProjectMemory::Note::summary() const {
	for (const String &line : body.split("\n")) {
		const String trimmed = line.strip_edges();
		if (trimmed.is_empty()) {
			continue;
		}
		if (trimmed.length() <= SUMMARY_CHARS) {
			return trimmed;
		}
		return trimmed.substr(0, SUMMARY_CHARS - 1) + String::utf8("…");
	}
	return String();
}

Dictionary MCPProjectMemory::Note::to_dictionary(bool p_with_body) const {
	Dictionary out;
	out["name"] = name;
	out["subject"] = subject;
	out["updated"] = updated;
	if (p_with_body) {
		out["body"] = body;
	} else {
		out["summary"] = summary();
	}
	return out;
}

String MCPProjectMemory::slugify(const String &p_text) {
	String out;
	bool pending_dash = false;
	for (int i = 0; i < p_text.length(); i++) {
		const char32_t c = p_text[i];
		const bool digit = c >= '0' && c <= '9';
		const bool lower = c >= 'a' && c <= 'z';
		const bool upper = c >= 'A' && c <= 'Z';
		if (digit || lower || upper) {
			// A separator only becomes a dash once something follows it, so the
			// result never starts or ends with one.
			if (pending_dash && !out.is_empty()) {
				out += "-";
			}
			pending_dash = false;
			out += String::chr(upper ? (c + 32) : c);
		} else {
			pending_dash = true;
		}
	}
	return out;
}

String MCPProjectMemory::serialize(const Note &p_note) {
	String out = "---\n";
	out += "subject: " + p_note.subject + "\n";
	if (!p_note.updated.is_empty()) {
		out += "updated: " + p_note.updated + "\n";
	}
	out += "---\n\n";
	out += p_note.body;
	if (!out.ends_with("\n")) {
		out += "\n";
	}
	return out;
}

bool MCPProjectMemory::parse(const String &p_text, Note &r_note, String &r_error) {
	const Vector<String> lines = p_text.split("\n");
	if (lines.is_empty() || lines[0].strip_edges() != "---") {
		r_error = "the note does not start with a '---' frontmatter block";
		return false;
	}

	int i = 1;
	bool closed = false;
	for (; i < lines.size(); i++) {
		const String line = lines[i].strip_edges();
		if (line == "---") {
			closed = true;
			i++;
			break;
		}
		if (line.is_empty()) {
			continue;
		}
		const int colon = line.find_char(':');
		if (colon <= 0) {
			r_error = vformat("frontmatter line '%s' is not 'key: value'", line);
			return false;
		}
		// Split on the first colon only: a subject is prose and may contain more.
		const String key = line.substr(0, colon).strip_edges();
		const String value = line.substr(colon + 1).strip_edges();
		if (key == "subject") {
			r_note.subject = value;
		} else if (key == "updated") {
			r_note.updated = value;
		}
	}
	if (!closed) {
		r_error = "the frontmatter block is never closed with '---'";
		return false;
	}

	// One blank line after the block is part of the format, not of the body.
	if (i < lines.size() && lines[i].strip_edges().is_empty()) {
		i++;
	}
	String body;
	for (int j = i; j < lines.size(); j++) {
		if (j > i) {
			body += "\n";
		}
		body += lines[j];
	}
	r_note.body = body.strip_edges();

	if (r_note.subject.is_empty()) {
		r_error = "frontmatter is missing a 'subject'";
		return false;
	}
	return true;
}

bool MCPProjectMemory::note_path(const String &p_name, String &r_res_path, String &r_error) {
	const String slug = slugify(p_name);
	if (slug.is_empty()) {
		r_error = vformat("'%s' has no letters or digits, so it cannot name a note", p_name);
		return false;
	}
	r_res_path = String("res://") + MEMORY_DIR + "/" + slug + ".md";
	return true;
}

Vector<MCPProjectMemory::Note> MCPProjectMemory::list(String &r_error) {
	Vector<Note> notes;
	const String dir_path = memory_dir_absolute(r_error);
	if (dir_path.is_empty()) {
		return notes;
	}
	if (!DirAccess::dir_exists_absolute(dir_path)) {
		// An empty store is the normal state of a project nobody has told anything
		// yet, not a failure.
		return notes;
	}

	Ref<DirAccess> dir = DirAccess::open(dir_path);
	if (dir.is_null()) {
		r_error = vformat("the memory folder '%s' could not be opened", dir_path);
		return notes;
	}

	Vector<String> files;
	dir->list_dir_begin();
	for (String entry = dir->get_next(); !entry.is_empty(); entry = dir->get_next()) {
		if (dir->current_is_dir() || !entry.ends_with(".md")) {
			continue;
		}
		files.push_back(entry);
	}
	dir->list_dir_end();

	for (const String &entry : files) {
		Error open_error = OK;
		const String text = FileAccess::get_file_as_string(dir_path.path_join(entry), &open_error);
		if (open_error != OK) {
			continue;
		}
		Note note;
		String parse_error;
		if (!parse(text, note, parse_error)) {
			// A hand-edited note that no longer parses should be visible rather than
			// silently missing, so it is listed with the problem as its subject.
			note.name = entry.get_basename();
			note.subject = vformat("(unreadable: %s)", parse_error);
			note.body = String();
			notes.push_back(note);
			continue;
		}
		note.name = entry.get_basename();
		notes.push_back(note);
	}

	notes.sort_custom<NoteRecency>();
	return notes;
}

bool MCPProjectMemory::read(const String &p_name, Note &r_note, String &r_error) {
	String res_path;
	if (!note_path(p_name, res_path, r_error)) {
		return false;
	}
	MCPPaths::Resolved resolved;
	if (!MCPPaths::resolve(res_path, resolved, r_error)) {
		return false;
	}
	if (!resolved.exists) {
		r_error = vformat("nothing is remembered under '%s'", slugify(p_name));
		return false;
	}
	Error open_error = OK;
	const String text = FileAccess::get_file_as_string(resolved.absolute, &open_error);
	if (open_error != OK) {
		r_error = vformat("the note '%s' could not be read", slugify(p_name));
		return false;
	}
	if (!parse(text, r_note, r_error)) {
		return false;
	}
	r_note.name = slugify(p_name);
	return true;
}

bool MCPProjectMemory::write(const String &p_name, const String &p_subject, const String &p_body, Note &r_note, String &r_error) {
	const String slug = slugify(p_name);
	String res_path;
	if (!note_path(p_name, res_path, r_error)) {
		return false;
	}

	const String subject = p_subject.strip_edges();
	if (subject.is_empty()) {
		r_error = "a note needs a subject: one line saying what it is about";
		return false;
	}
	const String body = p_body.strip_edges();
	if (body.is_empty()) {
		r_error = "a note with no body remembers nothing; use the forget action to remove one";
		return false;
	}
	if (body.length() > MAX_NOTE_CHARS) {
		r_error = vformat(
				"this note is %d characters and the limit is %d. Memory holds standing facts about the "
				"project, not transcripts - split it, or keep the part that will still be true next month.",
				body.length(), MAX_NOTE_CHARS);
		return false;
	}

	MCPPaths::Resolved resolved;
	if (!MCPPaths::resolve(res_path, resolved, r_error)) {
		return false;
	}

	if (!resolved.exists) {
		String list_error;
		const Vector<Note> existing = list(list_error);
		if (existing.size() >= MAX_NOTES) {
			// Refusing beats evicting. A store that drops the oldest note to make room
			// is a store that quietly forgets the thing you most relied on it for.
			String names;
			for (int i = 0; i < existing.size() && i < 5; i++) {
				names += (i ? ", " : "") + existing[existing.size() - 1 - i].name;
			}
			r_error = vformat(
					"the project's memory already holds %d notes, which is the limit. Forget or merge "
					"something first - the least recently updated are: %s",
					existing.size(), names);
			return false;
		}
	}

	const String dir_absolute = resolved.absolute.get_base_dir();
	if (!DirAccess::dir_exists_absolute(dir_absolute)) {
		const Error make_error = DirAccess::make_dir_recursive_absolute(dir_absolute);
		if (make_error != OK) {
			r_error = vformat("the memory folder '%s' could not be created", dir_absolute);
			return false;
		}
	}

	Note note;
	note.name = slug;
	note.subject = subject;
	note.body = body;
	note.updated = Time::get_singleton()->get_datetime_string_from_system(true, false) + "Z";

	Error open_error = OK;
	Ref<FileAccess> file = FileAccess::open(resolved.absolute, FileAccess::WRITE, &open_error);
	if (file.is_null() || open_error != OK) {
		r_error = vformat("the note '%s' could not be written", slug);
		return false;
	}
	file->store_string(serialize(note));
	file->close();

	r_note = note;
	return true;
}

bool MCPProjectMemory::erase(const String &p_name, String &r_error) {
	String res_path;
	if (!note_path(p_name, res_path, r_error)) {
		return false;
	}
	MCPPaths::Resolved resolved;
	if (!MCPPaths::resolve(res_path, resolved, r_error)) {
		return false;
	}
	if (!resolved.exists) {
		r_error = vformat("nothing is remembered under '%s'", slugify(p_name));
		return false;
	}
	if (DirAccess::remove_absolute(resolved.absolute) != OK) {
		r_error = vformat("the note '%s' could not be removed", slugify(p_name));
		return false;
	}
	return true;
}
