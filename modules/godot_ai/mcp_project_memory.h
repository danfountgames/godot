/**************************************************************************/
/*  mcp_project_memory.h                                                  */
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

#ifndef MCP_PROJECT_MEMORY_H
#define MCP_PROJECT_MEMORY_H

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

// What the agent knows about *this game*, kept between sessions.
//
// The tooling project has `.agent/` and it works; the user's game had nothing, so
// every session relearned the codebase, the conventions and the thing that broke last
// time. This is that store, for the project being edited.
//
// Three properties are deliberate, and each is a defence against a way memory
// normally goes wrong:
//
//   * **It is files, not a database.** `res://.godot_ai/memory/<slug>.md`, markdown
//     with the same frontmatter shape as a skill. It diffs, it reviews, it merges, and
//     a user who disagrees with something the agent believes can delete the line.
//   * **Recall is an index by default.** Returning every note on every session is how
//     a memory store turns into context poisoning. The index carries subject, age and
//     a one-line summary; bodies are fetched by name, on purpose.
//   * **It is bounded, and says so.** A cap that silently drops the oldest note is a
//     store that lies. Writing past the cap is refused, and the refusal names what to
//     forget.
class MCPProjectMemory {
public:
	// Room for a real project's worth of standing facts, and no room to accumulate a
	// transcript. A store at the cap is a prompt to consolidate, which is why writing
	// past it is refused rather than absorbed.
	static constexpr int MAX_NOTES = 64;
	static constexpr int MAX_NOTE_CHARS = 4000;
	static constexpr int SUMMARY_CHARS = 160;

	// Project-relative home of the store. Inside the project on purpose: this is
	// knowledge about the game, so it belongs to the repository and to the team, not
	// to one machine's user directory.
	static const char *MEMORY_DIR;

	struct Note {
		String name; // Slug; also the filename stem.
		String subject; // Human-readable title.
		String updated; // ISO-8601 UTC, or empty when the file predates the field.
		String body;

		// First non-empty line of the body, clipped. What the index shows so a caller
		// can tell whether the full note is worth reading.
		String summary() const;

		Dictionary to_dictionary(bool p_with_body) const;
	};

	// Caller-supplied names become filenames, so they are reduced to `[a-z0-9-]`
	// before they ever touch the filesystem. `..`, separators and drive letters
	// cannot survive this, which is the point: the store's confinement does not
	// depend on remembering to check.
	static String slugify(const String &p_text);

	// Serialisation, free of the filesystem so it can be tested directly.
	static String serialize(const Note &p_note);
	static bool parse(const String &p_text, Note &r_note, String &r_error);

	// Everything in the store, sorted most-recently-updated first.
	static Vector<Note> list(String &r_error);

	// One note by name. False with `r_error` set when it is not there.
	static bool read(const String &p_name, Note &r_note, String &r_error);

	// Writes or replaces one note. Refuses an over-long body, and refuses a *new*
	// note once the store is full - replacing an existing one always works, because
	// consolidating is the way out of a full store.
	static bool write(const String &p_name, const String &p_subject, const String &p_body, Note &r_note, String &r_error);

	static bool erase(const String &p_name, String &r_error);

	// Absolute path a write would touch. The protocol layer snapshots this before the
	// tool runs, so a memory write is as revertible as any other edit.
	static bool note_path(const String &p_name, String &r_res_path, String &r_error);
};

#endif // MCP_PROJECT_MEMORY_H
