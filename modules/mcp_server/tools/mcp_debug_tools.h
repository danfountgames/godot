/**************************************************************************/
/*  mcp_debug_tools.h                                                     */
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
#include "core/templates/pair.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

class MCPToolRegistry;

class MCPDebugTools {
public:
	// Register all debug/inspection/session tools into the registry.
	static void register_tools(MCPToolRegistry *p_registry);

	// --- Category E: Game Lifecycle ---
	static Dictionary handle_run_project(const Dictionary &p_args);
	static Dictionary handle_run_scene(const Dictionary &p_args);
	static Dictionary handle_stop(const Dictionary &p_args);

	// --- Category F: Live Inspection ---
	static Dictionary handle_get_status(const Dictionary &p_args);
	static Dictionary handle_get_output(const Dictionary &p_args);
	static Dictionary handle_get_errors(const Dictionary &p_args);
	static Dictionary handle_get_scene_tree(const Dictionary &p_args);
	static Dictionary handle_get_node_properties(const Dictionary &p_args);

	// --- Inspection/Search ---
	static Dictionary handle_search_scene_tree(const Dictionary &p_args);
	static Dictionary handle_get_performance(const Dictionary &p_args);

	// --- Scene Tree Browser ---
	static Dictionary handle_browse_scene_tree(const Dictionary &p_args);

	// --- Category H: Session Summary ---
	static Dictionary handle_session_summary(const Dictionary &p_args);

private:
	// Helper: get the MCPDebuggerBridge pointer, or return nullptr.
	static class MCPDebuggerBridge *_get_bridge();

	// Helper: check game is running, return error dict if not.
	// Returns empty Dictionary if game IS running (meaning: proceed).
	static Dictionary _require_game_running();

	// Helper: truncate scene tree Dictionary to max_depth.
	static Dictionary _truncate_tree(const Dictionary &p_tree, int p_max_depth, int p_current_depth = 0);

	// Helper: count nodes in a tree Dictionary.
	static int _count_tree_nodes(const Dictionary &p_tree);

	// Helper: render tree Dictionary as indented text with IDs.
	static String _tree_to_text_with_ids(const Dictionary &p_tree, int p_indent = 0, int p_max_depth = -1, int p_current_depth = 0);

	// Helper: search nodes in a cached tree Dictionary.
	static Array _search_tree(const Dictionary &p_tree, const String &p_name_pattern,
			const String &p_type_filter, const String &p_current_path = String());

	// --- Browse Scene Tree Helpers ---

	// Find a subtree by path in the hierarchical tree Dictionary.
	// Returns empty Dictionary if not found.
	static Dictionary _find_subtree(const Dictionary &p_tree,
			const String &p_target_path, const String &p_current_path = String());

	// Count total descendants of a tree node (not counting the node itself).
	static int _count_descendants(const Dictionary &p_tree);

	// Build a browse-format node from a full tree node.
	// Recurses up to p_remaining_depth levels. Applies pagination via offset/limit
	// to the direct children of the root level only (depth 0).
	static Dictionary _build_browse_node(const Dictionary &p_tree_node,
			const String &p_current_path, int p_remaining_depth,
			bool p_include_indicators, int p_child_offset = 0,
			int p_child_limit = 50, bool p_is_root = true);

	// Build type distribution map from a subtree.
	static Dictionary _build_type_distribution(const Dictionary &p_tree);

	// Accumulate type counts into a HashMap (recursive helper).
	static void _accumulate_types(const Dictionary &p_tree,
			HashMap<String, int> &r_counts);

	// Build group membership counts from a subtree (requires extended tree data).
	static Dictionary _build_group_stats(const Dictionary &p_tree);

	// Accumulate group counts (recursive helper).
	static void _accumulate_groups(const Dictionary &p_tree,
			HashMap<String, int> &r_counts);

	// Apply type/name filters to a tree, returning a pruned copy that
	// retains ancestor paths for structural context.
	// Returns empty Dictionary if no nodes match.
	static Dictionary _filter_tree(const Dictionary &p_tree,
			const String &p_type_filter, const String &p_name_pattern);

	// Check if a subtree contains any nodes matching the filters.
	static bool _subtree_has_match(const Dictionary &p_tree,
			const String &p_type_filter, const String &p_name_pattern);

	// Render a browse-format node as compact indented text.
	static String _browse_node_to_text(const Dictionary &p_node,
			int p_indent = 0, bool p_include_indicators = true);
};
