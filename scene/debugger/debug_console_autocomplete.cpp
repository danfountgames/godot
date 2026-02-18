/**************************************************************************/
/*  debug_console_autocomplete.cpp                                        */
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

#ifdef DEBUG_ENABLED

#include "debug_console_autocomplete.h"

void DebugConsoleAutocomplete::_rebuild() {
	entries.clear();

	// Built-in commands (always available).
	const char *builtins[] = {
		"help", "clear", "list", "watch", "unwatch",
		"pause", "resume", "step", "step!", "timescale",
		"screenshot", "echo", "exec",
		"node", "cd", "ls", "pwd", "ui", nullptr
	};
	const char *builtin_descs[] = {
		"Show help for a command or list all commands",
		"Clear the console output",
		"List registered actions/queries/events/cvars/commands",
		"Pin a query to the watch overlay",
		"Remove a pinned query from the watch overlay",
		"Suspend the game",
		"Resume the game",
		"Advance N frames then re-suspend",
		"Advance N frames instantly (no rendering)",
		"Set Engine time scale",
		"Capture screenshot to user://",
		"Print text to console",
		"Execute commands from a file",
		"Inspect/modify scene tree nodes",
		"Change working directory in scene tree",
		"List children of current (or specified) node",
		"Print current working directory in scene tree",
		"Interact with UI controls (buttons, sliders, toggles, menus)",
		nullptr
	};

	Color color_builtin = Color(0.6, 0.6, 0.7);

	for (int i = 0; builtins[i]; i++) {
		CompletionEntry e;
		e.name = builtins[i];
		e.description = builtin_descs[i];
		e.type_label = "[built-in]";
		e.type_color = color_builtin;
		entries.push_back(e);
	}

	entries.sort();
	sorted = true;
}

void DebugConsoleAutocomplete::rebuild_if_dirty() {
	if (!sorted) {
		_rebuild();
	}
}

Vector<const DebugConsoleAutocomplete::CompletionEntry *> DebugConsoleAutocomplete::get_completions(const String &p_prefix, int p_max_results) const {
	Vector<const CompletionEntry *> results;
	if (p_prefix.is_empty() || entries.is_empty()) {
		return results;
	}

	String prefix_lower = p_prefix.to_lower();

	// Phase 1: Prefix match (binary search for first match, then linear scan).
	// Since entries are sorted, we can binary search for the start.
	int lo = 0;
	int hi = entries.size();
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		if (entries[mid].name.to_lower() < prefix_lower) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}

	for (int i = lo; i < entries.size() && results.size() < p_max_results; i++) {
		if (entries[i].name.to_lower().begins_with(prefix_lower)) {
			results.push_back(&entries[i]);
		} else {
			break; // Sorted, so no more prefix matches.
		}
	}

	// Phase 2: If no prefix matches, try substring match.
	if (results.is_empty()) {
		for (int i = 0; i < entries.size() && results.size() < p_max_results; i++) {
			if (entries[i].name.to_lower().contains(prefix_lower)) {
				results.push_back(&entries[i]);
			}
		}
	}

	return results;
}

PackedStringArray DebugConsoleAutocomplete::get_argument_completions(const String &p_command, const String &p_partial) const {
	// Built-in argument completions for "list".
	if (p_command == "list") {
		PackedStringArray options;
		options.push_back("actions");
		options.push_back("queries");
		options.push_back("events");
		options.push_back("cvars");
		options.push_back("commands");
		options.push_back("interactables");
		options.push_back("pages");

		if (p_partial.is_empty()) {
			return options;
		}

		PackedStringArray filtered;
		for (const String &opt : options) {
			if (opt.begins_with(p_partial)) {
				filtered.push_back(opt);
			}
		}
		return filtered;
	}

	return PackedStringArray();
}

#endif // DEBUG_ENABLED
