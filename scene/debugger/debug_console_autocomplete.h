/**************************************************************************/
/*  debug_console_autocomplete.h                                          */
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

#pragma once

#ifdef DEBUG_ENABLED

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/callable.h"
#include "scene/resources/font.h"

class DebugConsoleAutocomplete {
public:
	struct CompletionEntry {
		String name; // "teleport", "query.health", "god_mode"
		String description; // "Teleport player. x:float y:float"
		String type_label; // "[cmd]", "[cvar]", "[action]", "[query]"
		Color type_color;

		bool operator<(const CompletionEntry &p_other) const {
			return name.naturalcasecmp_to(p_other.name) < 0;
		}
	};

private:
	Vector<CompletionEntry> entries;
	bool sorted = false;

	void _rebuild();

public:
	// Rebuild the completion list if needed.
	void rebuild_if_dirty();

	// Get completions for a prefix.
	Vector<const CompletionEntry *> get_completions(const String &p_prefix, int p_max_results = 8) const;

	// Get argument completions for a specific command.
	PackedStringArray get_argument_completions(const String &p_command, const String &p_partial) const;

	int get_entry_count() const { return entries.size(); }
};

#endif // DEBUG_ENABLED
