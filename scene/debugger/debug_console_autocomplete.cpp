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

#include "debug_semantic_registry.h"

void DebugConsoleAutocomplete::_rebuild_from_registry() {
	DebugSemanticRegistry *reg = DebugSemanticRegistry::get_singleton();
	if (!reg) {
		return;
	}

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
	Color color_cmd = Color(0.4, 0.8, 1.0);
	Color color_cvar = Color(0.4, 1.0, 0.6);
	Color color_action = Color(1.0, 0.8, 0.4);
	Color color_query = Color(0.7, 0.7, 0.9);

	for (int i = 0; builtins[i]; i++) {
		CompletionEntry e;
		e.name = builtins[i];
		e.description = builtin_descs[i];
		e.type_label = "[built-in]";
		e.type_color = color_builtin;
		entries.push_back(e);
	}

	// Commands from registry.
	for (const KeyValue<String, DebugSemanticRegistry::CommandEntry> &kv : reg->get_commands()) {
		CompletionEntry e;
		e.name = kv.key;
		e.description = kv.value.description;
		e.type_label = "[cmd]";
		e.type_color = color_cmd;
		entries.push_back(e);
	}

	// CVars from registry.
	for (const KeyValue<String, DebugSemanticRegistry::CVarEntry> &kv : reg->get_cvars()) {
		if (kv.value.flags & DebugSemanticRegistry::CVAR_HIDDEN) {
			continue;
		}
		CompletionEntry e;
		e.name = kv.key;
		e.description = kv.value.description;
		e.type_label = "[cvar]";
		e.type_color = color_cvar;
		entries.push_back(e);
	}

	// Actions from registry (prefixed with "action.").
	for (const KeyValue<String, DebugSemanticRegistry::ActionEntry> &kv : reg->get_actions()) {
		CompletionEntry e;
		e.name = "action." + kv.key;
		e.description = kv.value.description;
		e.type_label = "[action]";
		e.type_color = color_action;
		entries.push_back(e);
	}

	// Queries from registry (prefixed with "query.").
	for (const KeyValue<String, DebugSemanticRegistry::QueryEntry> &kv : reg->get_queries()) {
		CompletionEntry e;
		e.name = "query." + kv.key;
		e.description = kv.value.description;
		e.type_label = "[query]";
		e.type_color = color_query;
		entries.push_back(e);
	}

	entries.sort();
	sorted = true;
}

void DebugConsoleAutocomplete::rebuild_if_dirty() {
	DebugSemanticRegistry *reg = DebugSemanticRegistry::get_singleton();
	if (reg && reg->is_completions_dirty()) {
		_rebuild_from_registry();
		reg->mark_completions_clean();
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
	DebugSemanticRegistry *reg = DebugSemanticRegistry::get_singleton();
	if (!reg) {
		return PackedStringArray();
	}

	// Check if the command has a custom completion function.
	const DebugSemanticRegistry::CommandEntry *cmd = reg->get_commands().getptr(p_command);
	if (cmd && cmd->completion_func.is_valid()) {
		Variant result;
		Callable::CallError ce;
		const Variant partial_var = p_partial;
		const Variant *args[1] = { &partial_var };
		cmd->completion_func.callp(args, 1, result, ce);
		if (ce.error == Callable::CallError::CALL_OK && result.get_type() == Variant::PACKED_STRING_ARRAY) {
			return result;
		}
	}

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

	// Argument completions for "watch" / "unwatch".
	if (p_command == "watch" || p_command == "unwatch") {
		PackedStringArray queries;
		for (const KeyValue<String, DebugSemanticRegistry::QueryEntry> &kv : reg->get_queries()) {
			String name = "query." + kv.key;
			if (p_partial.is_empty() || name.begins_with(p_partial)) {
				queries.push_back(name);
			}
		}
		return queries;
	}

	return PackedStringArray();
}

#endif // DEBUG_ENABLED
