/**************************************************************************/
/*  mcp_memory_tools.h                                                    */
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

#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"

class MCPToolRegistry;

class MCPMemoryTools {
public:
	// Register all 7 memory profiling tools into the registry.
	static void register_tools(MCPToolRegistry *p_registry);

	// Tool handlers -- each takes a Dictionary of arguments and returns
	// an MCP tool result Dictionary.
	static Dictionary handle_get_stats(const Dictionary &p_args);
	static Dictionary handle_get_orphans(const Dictionary &p_args);
	static Dictionary handle_take_snapshot(const Dictionary &p_args);
	static Dictionary handle_diff(const Dictionary &p_args);
	static Dictionary handle_track_trend(const Dictionary &p_args);
	static Dictionary handle_detect_leaks(const Dictionary &p_args);
	static Dictionary handle_class_breakdown(const Dictionary &p_args);

private:
	// Helper: get the MCPDebuggerBridge pointer, or return nullptr.
	static class MCPDebuggerBridge *_get_bridge();

	// Helper: check game is running, return error dict if not.
	// Returns empty Dictionary if game IS running (meaning: proceed).
	static Dictionary _require_game_running();

	// Snapshot storage: in-memory snapshots keyed by label.
	static HashMap<String, Dictionary> snapshot_store;

	// Helper: collect a performance snapshot (reusable across tools).
	// Returns a Dictionary with all performance counters, or an error dict.
	static Dictionary _collect_snapshot();

	// Helper: compute diff between two snapshot Dictionaries.
	// Returns a structured diff with deltas and warnings.
	static Dictionary _compute_diff(const Dictionary &p_snapshot_a, const Dictionary &p_snapshot_b);

	// Helper: generate automated warnings from a diff.
	static Array _generate_warnings(const Dictionary &p_diff);
};
