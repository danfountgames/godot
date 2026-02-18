/**************************************************************************/
/*  mcp_analysis_tools.cpp                                                */
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

#include "mcp_analysis_tools.h"

#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/string/ustring.h"

// ============================================================================
// Static members
// ============================================================================

MCPAnalysisTools::ProjectIndex MCPAnalysisTools::s_index;

// ============================================================================
// Tool Registration
// ============================================================================

void MCPAnalysisTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ---- analysis/dead_code ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Root path to analyze (default: \"res://\")");
		props["include_private"] = make_prop("boolean",
				"Include _-prefixed names in results (default: true)");
		props["ignore_virtual"] = make_prop("boolean",
				"Skip virtual overrides like _ready, _process (default: true)");
		Array required;
		p_registry->register_tool(
				"analysis/dead_code",
				"Find Dead Code",
				"Find unused functions, signals, and variables in the project. Reports code that "
				"is defined but never referenced, connected, or called from anywhere in the project. "
				"Virtual overrides (_ready, _process, etc.) and @export variables are excluded by "
				"default. Dead code is safe to remove and reduces maintenance burden.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPAnalysisTools::handle_dead_code));
	}

	// ---- analysis/signal_flow ----
	{
		Dictionary props;
		props["signal_name"] = make_prop("string",
				"Trace a specific signal by name (optional, omit for all signals)");
		props["file"] = make_prop("string",
				"Scope to a specific file path in res:// format (optional)");
		props["direction"] = make_prop("string",
				"Filter direction: \"emitters\", \"receivers\", or \"both\" (default: \"both\")");
		Array required;
		p_registry->register_tool(
				"analysis/signal_flow",
				"Trace Signal Flow",
				"Trace signal flow across the project. Shows where each signal is defined, "
				"emitted, and connected. Identifies orphan signals (defined but never emitted) "
				"and orphan connections (connected to signal not defined in project). Use to "
				"understand event-driven communication patterns or debug signal wiring issues.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPAnalysisTools::handle_signal_flow));
	}

	// ---- analysis/complexity ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Root path to analyze (default: \"res://\")");
		props["threshold"] = make_prop("integer",
				"Only report functions above this complexity (default: 10)");
		props["sort_by"] = make_prop("string",
				"Sort by \"complexity\" or \"file\" (default: \"complexity\")");
		Array required;
		p_registry->register_tool(
				"analysis/complexity",
				"Analyze Complexity",
				"Calculate cyclomatic complexity for every function in the project. Complex "
				"functions (many branches/conditions) are harder to test and maintain. Default "
				"threshold of 10 -- functions above this are reported. Grades: A (1-5 simple), "
				"B (6-10 moderate), C (11-15 complex), D (16-25 very complex), F (26+ untestable).",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPAnalysisTools::handle_complexity));
	}

	// ---- analysis/dependencies ----
	{
		Dictionary props;
		props["include_preloads"] = make_prop("boolean",
				"Include preload() references in dependency graph (default: true)");
		Array required;
		p_registry->register_tool(
				"analysis/dependencies",
				"Map Dependencies",
				"Map autoload dependencies and detect circular references. Circular dependencies "
				"between autoloads cause initialization order bugs and are hard to debug. Also "
				"shows the full dependency graph for understanding project architecture.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPAnalysisTools::handle_dependencies));
	}

	// ---- analysis/project_health ----
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"analysis/project_health",
				"Project Health Dashboard",
				"Comprehensive project health dashboard. Scores the project on dead code, "
				"complexity, dependencies, signal hygiene, and syntax errors. Returns an "
				"overall grade (A-F) and the top issues to fix. Use this for a high-level "
				"project quality review.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPAnalysisTools::handle_project_health));
	}

	// ---- analysis/duplication ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Root path to analyze (default: \"res://\")");
		props["min_lines"] = make_prop("integer",
				"Minimum function body lines to consider for duplication (default: 5)");
		Array required;
		p_registry->register_tool(
				"analysis/duplication",
				"Find Code Duplication",
				"Find duplicated function bodies across the project. Normalizes code "
				"(strips comments, normalizes strings and numbers) then hashes function bodies "
				"to find exact and near-exact duplicates. Reports duplicate locations and "
				"sample code. Use to identify refactoring opportunities.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPAnalysisTools::handle_duplication));
	}

	// ---- analysis/validate_scenes ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Root path for scene files (default: \"res://\")");
		Array required;
		p_registry->register_tool(
				"analysis/validate_scenes",
				"Validate Scene Files",
				"Validate all .tscn scene files for broken references, missing scripts, "
				"suspicious NodePaths, duplicate node names, and unconventional signal handler "
				"names. Catches bugs early before they cause runtime crashes. Reports errors, "
				"warnings, and informational issues per scene.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPAnalysisTools::handle_validate_scenes));
	}

	// ---- analysis/assets ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Root path to analyze (default: \"res://\")");
		props["size_threshold_mb"] = make_prop("number",
				"Report files larger than this size in MB (default: 2.0)");
		Array required;
		p_registry->register_tool(
				"analysis/assets",
				"Analyze Project Assets",
				"Analyze project assets for optimization opportunities. Finds unused images, "
				"audio files, and fonts by cross-referencing res:// paths across all scenes, "
				"scripts, and resources. Also identifies large files that may benefit from "
				"compression. Use to reduce project size and clean up unused assets.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPAnalysisTools::handle_asset_analysis));
	}

	// ---- analysis/input_mappings ----
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"analysis/input_mappings",
				"Validate Input Mappings",
				"Cross-reference input action definitions in project.godot with their usage in "
				"scripts. Finds actions used in code but not defined (will crash at runtime) and "
				"actions defined but never used (dead config). Checks Input.is_action_pressed(), "
				"Input.is_action_just_pressed(), Input.get_action_strength(), Input.get_vector(), "
				"and similar Input methods.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPAnalysisTools::handle_input_mappings));
	}

	// ---- analysis/unused_files ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Root path to analyze (default: \"res://\")");
		Array required;
		p_registry->register_tool(
				"analysis/unused_files",
				"Find Unused Files",
				"Find scripts, scenes, and resources that are never referenced anywhere in the "
				"project. Cross-references all res:// paths including preload() and load() calls. "
				"Autoloads, test files, and addon files are excluded from false positives. Use to "
				"clean up abandoned files and reduce project clutter.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPAnalysisTools::handle_unused_files));
	}
}

// ============================================================================
// Index Building
// ============================================================================

void MCPAnalysisTools::_find_gd_files(const String &p_dir, Vector<String> &r_files) {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return;
	}

	da->list_dir_begin();
	String item = da->get_next();
	Vector<String> subdirs;

	while (!item.is_empty()) {
		if (item == "." || item == "..") {
			item = da->get_next();
			continue;
		}

		String full_path = p_dir.path_join(item);

		if (da->current_is_dir()) {
			if (!is_skip_directory(item) && !item.begins_with(".")) {
				subdirs.push_back(full_path);
			}
		} else {
			if (item.get_extension().to_lower() == "gd") {
				r_files.push_back(full_path);
			}
		}
		item = da->get_next();
	}
	da->list_dir_end();

	subdirs.sort();
	for (const String &subdir : subdirs) {
		_find_gd_files(subdir, r_files);
	}
}

void MCPAnalysisTools::_find_tscn_files(const String &p_dir, Vector<String> &r_files) {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return;
	}

	da->list_dir_begin();
	String item = da->get_next();
	Vector<String> subdirs;

	while (!item.is_empty()) {
		if (item == "." || item == "..") {
			item = da->get_next();
			continue;
		}

		String full_path = p_dir.path_join(item);

		if (da->current_is_dir()) {
			if (!is_skip_directory(item) && !item.begins_with(".")) {
				subdirs.push_back(full_path);
			}
		} else {
			if (item.get_extension().to_lower() == "tscn") {
				r_files.push_back(full_path);
			}
		}
		item = da->get_next();
	}
	da->list_dir_end();

	subdirs.sort();
	for (const String &subdir : subdirs) {
		_find_tscn_files(subdir, r_files);
	}
}

void MCPAnalysisTools::_parse_tscn_connections(const String &p_path, Vector<TscnConnection> &r_connections) {
	String source = FileAccess::get_file_as_string(p_path);
	if (source.is_empty()) {
		return;
	}

	Vector<String> lines = source.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();

		// [connection signal="pressed" from="Button" to="." method="_on_button_pressed"]
		if (line.begins_with("[connection ")) {
			String signal_name;
			String method_name;

			int sig_pos = line.find("signal=\"");
			if (sig_pos >= 0) {
				String after = line.substr(sig_pos + 8);
				int quote_end = after.find("\"");
				if (quote_end > 0) {
					signal_name = after.substr(0, quote_end);
				}
			}

			int method_pos = line.find("method=\"");
			if (method_pos >= 0) {
				String after = line.substr(method_pos + 8);
				int quote_end = after.find("\"");
				if (quote_end > 0) {
					method_name = after.substr(0, quote_end);
				}
			}

			if (!signal_name.is_empty() && !method_name.is_empty()) {
				TscnConnection conn;
				conn.signal_name = signal_name;
				conn.method_name = method_name;
				conn.scene_path = p_path;
				r_connections.push_back(conn);
			}
		}
	}
}

bool MCPAnalysisTools::_is_virtual_override(const String &p_name) {
	static const char *virtuals[] = {
		// Lifecycle.
		"_init", "_ready", "_enter_tree", "_exit_tree", "_notification",
		// Process.
		"_process", "_physics_process",
		"_input", "_unhandled_input", "_unhandled_key_input",
		"_shortcut_input", "_gui_input",
		// Drawing.
		"_draw", "_get_minimum_size",
		// Properties.
		"_get", "_set", "_get_property_list",
		"_property_can_revert", "_property_get_revert", "_validate_property",
		// Strings.
		"_to_string",
		// Editor.
		"_get_configuration_warnings", "_get_tool_buttons", "_run",
		// Resources.
		"_setup_local_to_scene",
		// State machine patterns (common in game dev).
		"_state_enter", "_state_exit", "_state_process", "_state_physics_process",
		"enter", "exit", "update", "physics_update",
		nullptr
	};
	for (int i = 0; virtuals[i] != nullptr; i++) {
		if (p_name == virtuals[i]) {
			return true;
		}
	}
	return false;
}

bool MCPAnalysisTools::_is_public_api_pattern(const String &p_name) {
	// Common public API patterns that are meant for external use.
	// Matches: get_*, set_*, is_*, has_*, can_*, add_*, remove_*, clear_*,
	// start_*, stop_*, pause_*, resume_*, enable_*, disable_*, toggle_*,
	// load_*, save_*, reset_*, show_*, hide_*, update_*, play_*, queue_*,
	// emit_*, broadcast_*, register_*, unregister_*, on_*, handle_*.
	static const char *prefixes[] = {
		"get_", "set_", "is_", "has_", "can_",
		"add_", "remove_", "clear_",
		"start_", "stop_", "pause_", "resume_",
		"enable_", "disable_", "toggle_",
		"load_", "save_", "reset_",
		"show_", "hide_", "update_",
		"play_", "queue_",
		"emit_", "broadcast_",
		"register_", "unregister_",
		"on_", "handle_",
		nullptr
	};
	for (int i = 0; prefixes[i] != nullptr; i++) {
		if (p_name.begins_with(prefixes[i])) {
			return true;
		}
	}
	return false;
}

String MCPAnalysisTools::_get_complexity_grade(int p_complexity) {
	if (p_complexity <= 5) {
		return "A";
	} else if (p_complexity <= 10) {
		return "B";
	} else if (p_complexity <= 15) {
		return "C";
	} else if (p_complexity <= 25) {
		return "D";
	}
	return "F";
}

// Compute cyclomatic complexity for lines [p_start, p_end) of p_lines.
int MCPAnalysisTools::_compute_complexity(const Vector<String> &p_lines, int p_start, int p_end) {
	int complexity = 1; // Base complexity.

	for (int i = p_start; i < p_end && i < p_lines.size(); i++) {
		String line = p_lines[i].strip_edges();

		// Skip empty lines and comments.
		if (line.is_empty() || line.begins_with("#")) {
			continue;
		}

		// Strip inline comments for analysis.
		int comment_pos = line.find("#");
		if (comment_pos > 0) {
			// Make sure it's not inside a string (simple heuristic: count quotes).
			int single_quotes = 0;
			int double_quotes = 0;
			for (int c = 0; c < comment_pos; c++) {
				if (line[c] == '\'' && (c == 0 || line[c - 1] != '\\')) {
					single_quotes++;
				}
				if (line[c] == '"' && (c == 0 || line[c - 1] != '\\')) {
					double_quotes++;
				}
			}
			if (single_quotes % 2 == 0 && double_quotes % 2 == 0) {
				line = line.substr(0, comment_pos).strip_edges();
			}
		}

		// Decision points: if, elif.
		if (line.begins_with("if ") || line.begins_with("if(") || line == "if:") {
			complexity++;
		} else if (line.begins_with("elif ") || line.begins_with("elif(") || line == "elif:") {
			complexity++;
		}

		// Loops: for, while.
		if (line.begins_with("for ") || line.begins_with("for(")) {
			complexity++;
		}
		if (line.begins_with("while ") || line.begins_with("while(") || line == "while:") {
			complexity++;
		}

		// Match statement (the match keyword itself adds a decision point).
		if (line.begins_with("match ") || line.begins_with("match(")) {
			complexity++;
		}

		// Boolean operators in conditions.
		// Count occurrences of " and " and " or " as decision points.
		int search_pos = 0;
		while (true) {
			int and_pos = line.find(" and ", search_pos);
			if (and_pos == -1) {
				break;
			}
			complexity++;
			search_pos = and_pos + 5;
		}
		search_pos = 0;
		while (true) {
			int or_pos = line.find(" or ", search_pos);
			if (or_pos == -1) {
				break;
			}
			complexity++;
			search_pos = or_pos + 4;
		}

		// Ternary: "x if condition else y" (inline if).
		// Only count if "if" appears mid-line (not at start) and "else" also appears.
		if (!line.begins_with("if ") && !line.begins_with("elif ")) {
			int if_pos = line.find(" if ");
			if (if_pos > 0 && line.find(" else ", if_pos) > if_pos) {
				complexity++;
			}
		}
	}

	return complexity;
}

void MCPAnalysisTools::_parse_script_file(const String &p_path, ScriptFile &r_file) {
	r_file.path = p_path;

	String source = FileAccess::get_file_as_string(p_path);
	if (source.is_empty()) {
		return;
	}

	Vector<String> lines = source.split("\n");
	r_file.total_lines = lines.size();

	// State tracking for function boundaries.
	String current_func_name;
	int current_func_start = -1;
	int current_func_indent = -1;
	bool next_var_exported = false;
	bool next_is_static = false;
	bool prev_line_is_doc_comment = false; // Track ## doc comments.
	for (int i = 0; i < lines.size(); i++) {
		String raw_line = lines[i];
		String line = raw_line.strip_edges();

		// Track doc comments (## prefix) for the next function definition.
		if (line.begins_with("##")) {
			prev_line_is_doc_comment = true;
			continue;
		}

		// Skip empty lines and full-line comments.
		if (line.is_empty() || line.begins_with("#")) {
			// Regular comments don't reset doc comment tracking.
			continue;
		}

		// Calculate indentation level (number of leading tabs).
		int indent = 0;
		for (int c = 0; c < raw_line.length(); c++) {
			if (raw_line[c] == '\t') {
				indent++;
			} else if (raw_line[c] == ' ') {
				// Count 4 spaces as one tab for mixed-indent projects.
				// This is approximate but handles common cases.
			} else {
				break;
			}
		}

		// Detect end of current function body.
		// A function ends when we encounter a line at the same or lesser indent
		// as the function definition, UNLESS that line is empty or a comment.
		if (current_func_start >= 0 && indent <= current_func_indent && !line.is_empty() && !line.begins_with("#")) {
			// End the current function.
			int func_end = i;
			int complexity = _compute_complexity(lines, current_func_start + 1, func_end);
			r_file.function_complexities[current_func_name] = complexity;
			current_func_name = "";
			current_func_start = -1;
			current_func_indent = -1;
		}

		// @tool annotation.
		if (line == "@tool") {
			r_file.is_tool = true;
			continue;
		}

		// @export annotation (marks next variable).
		if (line.begins_with("@export")) {
			next_var_exported = true;
			continue;
		}

		// @static_unload or static func.
		if (line.begins_with("static ")) {
			next_is_static = true;
			// Don't continue -- fall through so "static func" gets parsed.
		}

		// class_name declaration.
		if (line.begins_with("class_name ")) {
			String class_name = line.substr(11).strip_edges();
			// Remove anything after a space or comment.
			int space_pos = class_name.find(" ");
			if (space_pos > 0) {
				class_name = class_name.substr(0, space_pos);
			}
			r_file.class_name = class_name;
			continue;
		}

		// extends declaration.
		if (line.begins_with("extends ")) {
			String extends_class = line.substr(8).strip_edges();
			int space_pos = extends_class.find(" ");
			if (space_pos > 0) {
				extends_class = extends_class.substr(0, space_pos);
			}
			r_file.extends = extends_class;
			continue;
		}

		// Signal definition: signal my_signal or signal my_signal(args).
		if (line.begins_with("signal ")) {
			String sig_str = line.substr(7).strip_edges();
			String sig_name;
			int paren_pos = sig_str.find("(");
			if (paren_pos > 0) {
				sig_name = sig_str.substr(0, paren_pos).strip_edges();
			} else {
				sig_name = sig_str.strip_edges();
				// Remove trailing comments.
				int hash_pos = sig_name.find("#");
				if (hash_pos > 0) {
					sig_name = sig_name.substr(0, hash_pos).strip_edges();
				}
			}

			if (!sig_name.is_empty()) {
				ScriptSymbol sym;
				sym.name = sig_name;
				sym.type = "signal";
				sym.file = p_path;
				sym.line = i + 1; // 1-based line numbers.
				r_file.signals.push_back(sym);
			}
			continue;
		}

		// Function definition: func my_func( or static func my_func(.
		String func_search = line;
		if (func_search.begins_with("static ")) {
			func_search = func_search.substr(7).strip_edges();
		}
		if (func_search.begins_with("func ")) {
			String func_str = func_search.substr(5).strip_edges();
			String func_name;
			int paren_pos = func_str.find("(");
			if (paren_pos > 0) {
				func_name = func_str.substr(0, paren_pos).strip_edges();
			} else {
				func_name = func_str.strip_edges();
			}

			if (!func_name.is_empty()) {
				ScriptSymbol sym;
				sym.name = func_name;
				sym.type = "function";
				sym.file = p_path;
				sym.line = i + 1;
				sym.is_static = next_is_static || line.begins_with("static ");

				// Extract return type if present: func name() -> Type:
				int arrow_pos = line.find("->");
				if (arrow_pos > 0) {
					String ret = line.substr(arrow_pos + 2).strip_edges();
					int colon_pos = ret.find(":");
					if (colon_pos > 0) {
						ret = ret.substr(0, colon_pos).strip_edges();
					}
					sym.return_type = ret;
					r_file.typed_function_count++;
				}

				// Extract parameters.
				if (paren_pos > 0) {
					int close_paren = func_str.find(")");
					if (close_paren > paren_pos) {
						String params_str = func_str.substr(paren_pos + 1, close_paren - paren_pos - 1).strip_edges();
						if (!params_str.is_empty()) {
							Vector<String> params = params_str.split(",");
							for (int p = 0; p < params.size(); p++) {
								String param = params[p].strip_edges();
								// Extract just the parameter name (before : or =).
								int colon_pos2 = param.find(":");
								int eq_pos = param.find("=");
								if (colon_pos2 > 0) {
									param = param.substr(0, colon_pos2).strip_edges();
								} else if (eq_pos > 0) {
									param = param.substr(0, eq_pos).strip_edges();
								}
								if (!param.is_empty()) {
									sym.parameters.push_back(param);
								}
							}
						}
					}
				}

				r_file.functions.push_back(sym);

				// Record if this function has a doc comment above it.
				r_file.function_has_doc_comment[func_name] = prev_line_is_doc_comment;

				// Start tracking function body.
				current_func_name = func_name;
				current_func_start = i;
				current_func_indent = indent;
			}
			prev_line_is_doc_comment = false;
			next_is_static = false;
			continue;
		}
		prev_line_is_doc_comment = false;
		next_is_static = false;

		// Variable definition (class-level only): var my_var or @export var my_var.
		// Only count variables at indent level 0 (class-level).
		if (indent == 0 && (line.begins_with("var ") || line.begins_with("@onready var "))) {
			String var_str = line;
			if (var_str.begins_with("@onready ")) {
				var_str = var_str.substr(9).strip_edges();
			}
			if (var_str.begins_with("var ")) {
				var_str = var_str.substr(4).strip_edges();
			}

			// Extract variable name (before :, =, or space).
			String var_name;
			for (int c = 0; c < var_str.length(); c++) {
				char32_t ch = var_str[c];
				if (ch == ':' || ch == '=' || ch == ' ' || ch == '\t') {
					break;
				}
				var_name += String::chr(ch);
			}

			if (!var_name.is_empty()) {
				ScriptSymbol sym;
				sym.name = var_name;
				sym.type = "variable";
				sym.file = p_path;
				sym.line = i + 1;
				sym.is_exported = next_var_exported;
				r_file.variables.push_back(sym);
			}
			next_var_exported = false;
			continue;
		}
		next_var_exported = false;

		// --- Reference extraction (identifiers used in code) ---

		// Signal connections: .connect("signal_name", ...) or .signal_name.connect(.
		{
			// Pattern: .connect("signal_name"
			int connect_pos = line.find(".connect(\"");
			if (connect_pos >= 0) {
				String after = line.substr(connect_pos + 10);
				int quote_end = after.find("\"");
				if (quote_end > 0) {
					String sig_name = after.substr(0, quote_end);
					r_file.signal_connections.push_back(sig_name);
				}
			}

			// Pattern: .signal_name.connect(
			// Look for: identifier.identifier.connect(
			int dot_connect = line.find(".connect(");
			if (dot_connect > 0) {
				// Walk backwards from the dot before "connect" to find the signal name.
				// e.g., "something.my_signal.connect(handler)"
				// We want "my_signal".
				String before = line.substr(0, dot_connect);
				int prev_dot = before.rfind(".");
				if (prev_dot >= 0) {
					String sig_name = before.substr(prev_dot + 1).strip_edges();
					// Validate it looks like an identifier.
					bool valid = !sig_name.is_empty();
					for (int c = 0; c < sig_name.length() && valid; c++) {
						char32_t ch = sig_name[c];
						valid = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
								(ch >= '0' && ch <= '9') || ch == '_';
					}
					if (valid && sig_name != "connect") {
						r_file.signal_connections.push_back(sig_name);
					}
				}
			}
		}

		// Signal emissions: .emit() or emit_signal("signal_name").
		{
			// Pattern: signal_name.emit()
			int emit_pos = line.find(".emit(");
			if (emit_pos > 0) {
				// Walk backwards to find the signal name.
				String before = line.substr(0, emit_pos);
				// Find the last identifier before .emit(.
				int start = emit_pos - 1;
				while (start >= 0) {
					char32_t ch = before[start];
					if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
								(ch >= '0' && ch <= '9') || ch == '_')) {
						break;
					}
					start--;
				}
				String sig_name = before.substr(start + 1);
				if (!sig_name.is_empty()) {
					r_file.signal_emissions.push_back(sig_name);
				}
			}

			// Pattern: emit_signal("signal_name")
			int emit_signal_pos = line.find("emit_signal(\"");
			if (emit_signal_pos >= 0) {
				String after = line.substr(emit_signal_pos + 13);
				int quote_end = after.find("\"");
				if (quote_end > 0) {
					String sig_name = after.substr(0, quote_end);
					r_file.signal_emissions.push_back(sig_name);
				}
			}
		}

		// Dynamic call detection: call("method"), call_deferred("method"),
		// callv("method"), call_thread_safe("method"), Callable(self, "method"),
		// has_method("method").
		{
			static const char *call_patterns[] = {
				"call(\"", "call_deferred(\"", "callv(\"", "call_thread_safe(\"",
				"has_method(\"", nullptr
			};
			for (int cp = 0; call_patterns[cp] != nullptr; cp++) {
				String pattern = call_patterns[cp];
				int pos = line.find(pattern);
				if (pos >= 0) {
					String after = line.substr(pos + pattern.length());
					int quote_end = after.find("\"");
					if (quote_end > 0) {
						String method_name = after.substr(0, quote_end);
						r_file.dynamic_call_refs.push_back(method_name);
						r_file.references.push_back(method_name);
					}
				}
			}

			// Callable(self, "method") or Callable(node, "method").
			int callable_pos = line.find("Callable(");
			if (callable_pos >= 0) {
				String after = line.substr(callable_pos + 9);
				// Skip past the first argument (before the comma).
				int comma_pos = after.find(",");
				if (comma_pos >= 0) {
					String rest = after.substr(comma_pos + 1).strip_edges();
					if (rest.begins_with("\"") || rest.begins_with("'")) {
						String str_after = rest.substr(1);
						int quote_end = str_after.find("\"");
						if (quote_end <= 0) {
							quote_end = str_after.find("'");
						}
						if (quote_end > 0) {
							String method_name = str_after.substr(0, quote_end);
							r_file.dynamic_call_refs.push_back(method_name);
							r_file.references.push_back(method_name);
						}
					}
				}
			}
		}

		// Preload references: preload("path") or load("path").
		{
			int preload_pos = line.find("preload(\"");
			if (preload_pos >= 0) {
				String after = line.substr(preload_pos + 9);
				int quote_end = after.find("\"");
				if (quote_end > 0) {
					String ref_path = after.substr(0, quote_end);
					r_file.references.push_back(ref_path);
				}
			}
			int load_pos = line.find("load(\"");
			if (load_pos >= 0) {
				String after = line.substr(load_pos + 6);
				int quote_end = after.find("\"");
				if (quote_end > 0) {
					String ref_path = after.substr(0, quote_end);
					r_file.references.push_back(ref_path);
				}
			}
		}

		// Extract all identifiers for cross-reference (simple word extraction).
		// Look for words that could be function calls or variable references.
		{
			String word;
			for (int c = 0; c < line.length(); c++) {
				char32_t ch = line[c];
				if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
						(ch >= '0' && ch <= '9') || ch == '_') {
					word += String::chr(ch);
				} else {
					if (!word.is_empty() && word.length() >= 2) {
						// Skip GDScript keywords.
						if (word != "if" && word != "else" && word != "elif" &&
								word != "for" && word != "while" && word != "match" &&
								word != "func" && word != "var" && word != "const" &&
								word != "class" && word != "extends" && word != "signal" &&
								word != "return" && word != "pass" && word != "break" &&
								word != "continue" && word != "true" && word != "false" &&
								word != "null" && word != "self" && word != "super" &&
								word != "and" && word != "or" && word != "not" &&
								word != "in" && word != "is" && word != "as" &&
								word != "await" && word != "yield" && word != "void" &&
								word != "int" && word != "float" && word != "bool" &&
								word != "String" && word != "Vector2" && word != "Vector3") {
							r_file.references.push_back(word);
						}
					}
					word = "";
				}
			}
			// Final word at end of line.
			if (!word.is_empty() && word.length() >= 2) {
				if (word != "if" && word != "else" && word != "elif" &&
						word != "for" && word != "while" && word != "match" &&
						word != "func" && word != "var" && word != "const" &&
						word != "class" && word != "extends" && word != "signal" &&
						word != "return" && word != "pass" && word != "break" &&
						word != "continue" && word != "true" && word != "false" &&
						word != "null" && word != "self" && word != "super" &&
						word != "and" && word != "or" && word != "not" &&
						word != "in" && word != "is" && word != "as" &&
						word != "await" && word != "yield" && word != "void" &&
						word != "int" && word != "float" && word != "bool" &&
						word != "String" && word != "Vector2" && word != "Vector3") {
					r_file.references.push_back(word);
				}
			}
		}
	}

	// Close any unclosed function at end of file.
	if (current_func_start >= 0) {
		int complexity = _compute_complexity(lines, current_func_start + 1, lines.size());
		r_file.function_complexities[current_func_name] = complexity;
	}
}

void MCPAnalysisTools::_build_index(const String &p_root) {
	uint64_t start_msec = OS::get_singleton()->get_ticks_msec();

	s_index.files.clear();
	s_index.symbol_definitions.clear();
	s_index.symbol_references.clear();
	s_index.tscn_connections.clear();

	// Step 1: Find all .gd files and .tscn files.
	Vector<String> gd_files;
	_find_gd_files(p_root, gd_files);

	Vector<String> tscn_files;
	_find_tscn_files(p_root, tscn_files);

	// Step 2: Identify autoloads from project settings.
	HashMap<String, String> autoload_paths; // path -> autoload name
	{
		List<PropertyInfo> settings;
		ProjectSettings::get_singleton()->get_property_list(&settings);
		for (const PropertyInfo &pi : settings) {
			if (pi.name.begins_with("autoload/")) {
				String name = pi.name.get_slice("/", 1);
				String path = ProjectSettings::get_singleton()->get_setting(pi.name);
				if (path.begins_with("*")) {
					path = path.substr(1);
				}
				autoload_paths[path] = name;
			}
		}
	}

	// Step 3: Parse each file.
	for (int i = 0; i < gd_files.size(); i++) {
		ScriptFile sf;
		_parse_script_file(gd_files[i], sf);

		// Mark as autoload if applicable.
		if (autoload_paths.has(sf.path)) {
			sf.is_autoload = true;
		}

		s_index.files[sf.path] = sf;
	}

	// Step 3b: Parse .tscn files for signal connections.
	for (int i = 0; i < tscn_files.size(); i++) {
		_parse_tscn_connections(tscn_files[i], s_index.tscn_connections);
	}

	// Step 4: Build cross-reference maps.
	for (const KeyValue<String, ScriptFile> &E : s_index.files) {
		const ScriptFile &sf = E.value;

		// Symbol definitions.
		for (int i = 0; i < sf.functions.size(); i++) {
			s_index.symbol_definitions[sf.functions[i].name].push_back(sf.path);
		}
		for (int i = 0; i < sf.variables.size(); i++) {
			s_index.symbol_definitions[sf.variables[i].name].push_back(sf.path);
		}
		for (int i = 0; i < sf.signals.size(); i++) {
			s_index.symbol_definitions[sf.signals[i].name].push_back(sf.path);
		}

		// Symbol references.
		for (int i = 0; i < sf.references.size(); i++) {
			const String &ref = sf.references[i];
			if (!s_index.symbol_references.has(ref)) {
				s_index.symbol_references[ref] = Vector<String>();
			}
			// Add uniquely.
			Vector<String> &ref_files = s_index.symbol_references[ref];
			bool found = false;
			for (int j = 0; j < ref_files.size(); j++) {
				if (ref_files[j] == sf.path) {
					found = true;
					break;
				}
			}
			if (!found) {
				ref_files.push_back(sf.path);
			}
		}
	}

	s_index.is_built = true;
	s_index.build_time_msec = OS::get_singleton()->get_ticks_msec() - start_msec;
}

// ============================================================================
// Tool 1: analysis/dead_code
// ============================================================================

Dictionary MCPAnalysisTools::handle_dead_code(const Dictionary &p_args) {
	String root = p_args.get("path", "res://");
	bool include_private = true;
	if (p_args.has("include_private")) {
		include_private = (bool)p_args["include_private"];
	}
	bool ignore_virtual = true;
	if (p_args.has("ignore_virtual")) {
		ignore_virtual = (bool)p_args["ignore_virtual"];
	}

	if (!root.is_empty() && !validate_path(root)) {
		return make_tool_error("Invalid path: " + root);
	}

	_build_index(root.is_empty() ? "res://" : root);

	if (!s_index.is_built || s_index.files.is_empty()) {
		return make_tool_error("No GDScript files found to analyze.");
	}

	Array dead_functions;
	Array dead_signals;
	Array dead_variables;
	int total_functions = 0;
	int total_signals = 0;
	int total_variables = 0;

	for (const KeyValue<String, ScriptFile> &E : s_index.files) {
		const ScriptFile &sf = E.value;

		// Check functions.
		for (int i = 0; i < sf.functions.size(); i++) {
			const ScriptSymbol &func = sf.functions[i];
			total_functions++;

			// Skip virtual overrides if configured.
			if (ignore_virtual && _is_virtual_override(func.name)) {
				continue;
			}

			// Skip signal handlers (_on_*) -- these are connected in the editor.
			if (func.name.begins_with("_on_")) {
				continue;
			}

			// Skip private (_-prefixed) if configured.
			if (!include_private && func.name.begins_with("_")) {
				continue;
			}

			// Skip documented public API functions (## doc comment above).
			// If a function is public and has a doc comment, it's meant for external use.
			if (!func.name.begins_with("_")) {
				if (sf.function_has_doc_comment.has(func.name) && sf.function_has_doc_comment[func.name]) {
					continue;
				}
			}

			// Skip common public API patterns (getters, setters, lifecycle methods).
			if (!func.name.begins_with("_") && _is_public_api_pattern(func.name)) {
				continue;
			}

			// Check if this function is referenced anywhere in any OTHER file.
			bool is_referenced = false;

			if (s_index.symbol_references.has(func.name)) {
				const Vector<String> &ref_files = s_index.symbol_references[func.name];
				for (int j = 0; j < ref_files.size(); j++) {
					if (ref_files[j] != sf.path) {
						is_referenced = true;
						break;
					}
				}
				// Also count self-references from other functions in same file.
				// A function is "alive" if used within its own file too.
				if (!is_referenced && ref_files.size() > 0) {
					// Check if it's referenced in its own file but outside its own definition.
					// For simplicity, if the name appears as a reference in the file,
					// count it as alive.
					for (int j = 0; j < ref_files.size(); j++) {
						if (ref_files[j] == sf.path) {
							// Count occurrences in file references.
							int ref_count = 0;
							for (int r = 0; r < sf.references.size(); r++) {
								if (sf.references[r] == func.name) {
									ref_count++;
								}
							}
							// More than 1 reference means it's used somewhere besides the def.
							// (The def itself generates a reference from the line parser.)
							if (ref_count > 1) {
								is_referenced = true;
							}
							break;
						}
					}
				}
			}

			// Check if it's a signal connection target (in-code connect calls).
			if (!is_referenced) {
				for (const KeyValue<String, ScriptFile> &F : s_index.files) {
					for (int c = 0; c < F.value.signal_connections.size(); c++) {
						if (F.value.signal_connections[c] == func.name) {
							is_referenced = true;
							break;
						}
					}
					if (is_referenced) {
						break;
					}
				}
			}

			// Check if it's a signal handler connected in .tscn scenes.
			if (!is_referenced) {
				for (int t = 0; t < s_index.tscn_connections.size(); t++) {
					if (s_index.tscn_connections[t].method_name == func.name) {
						is_referenced = true;
						break;
					}
				}
			}

			// Check if it's referenced via dynamic calls (call("method"), etc.).
			if (!is_referenced) {
				for (const KeyValue<String, ScriptFile> &F : s_index.files) {
					for (int d = 0; d < F.value.dynamic_call_refs.size(); d++) {
						if (F.value.dynamic_call_refs[d] == func.name) {
							is_referenced = true;
							break;
						}
					}
					if (is_referenced) {
						break;
					}
				}
			}

			if (!is_referenced) {
				Dictionary dead;
				dead["name"] = func.name;
				dead["file"] = sf.path;
				dead["line"] = func.line;
				dead_functions.push_back(dead);
			}
		}

		// Check signals.
		for (int i = 0; i < sf.signals.size(); i++) {
			const ScriptSymbol &sig = sf.signals[i];
			total_signals++;

			bool is_emitted = false;
			bool is_connected = false;

			// Check if emitted anywhere.
			for (const KeyValue<String, ScriptFile> &F : s_index.files) {
				for (int e = 0; e < F.value.signal_emissions.size(); e++) {
					if (F.value.signal_emissions[e] == sig.name) {
						is_emitted = true;
						break;
					}
				}
				if (is_emitted) {
					break;
				}
			}

			// Check if connected anywhere (in-code connects).
			for (const KeyValue<String, ScriptFile> &F : s_index.files) {
				for (int c = 0; c < F.value.signal_connections.size(); c++) {
					if (F.value.signal_connections[c] == sig.name) {
						is_connected = true;
						break;
					}
				}
				if (is_connected) {
					break;
				}
			}

			// Check if connected in .tscn scene files.
			if (!is_connected) {
				for (int t = 0; t < s_index.tscn_connections.size(); t++) {
					if (s_index.tscn_connections[t].signal_name == sig.name) {
						is_connected = true;
						break;
					}
				}
			}

			// Also check if referenced by name in code.
			bool is_referenced_by_name = false;
			if (s_index.symbol_references.has(sig.name)) {
				const Vector<String> &ref_files = s_index.symbol_references[sig.name];
				for (int j = 0; j < ref_files.size(); j++) {
					if (ref_files[j] != sf.path) {
						is_referenced_by_name = true;
						break;
					}
				}
			}

			if (!is_emitted && !is_connected && !is_referenced_by_name) {
				Dictionary dead;
				dead["name"] = sig.name;
				dead["file"] = sf.path;
				dead["line"] = sig.line;
				dead["reason"] = "defined but never emitted or connected";
				dead_signals.push_back(dead);
			} else if (!is_emitted && !is_referenced_by_name) {
				Dictionary dead;
				dead["name"] = sig.name;
				dead["file"] = sf.path;
				dead["line"] = sig.line;
				dead["reason"] = "connected but never emitted";
				dead_signals.push_back(dead);
			}
		}

		// Check variables.
		for (int i = 0; i < sf.variables.size(); i++) {
			const ScriptSymbol &var_sym = sf.variables[i];
			total_variables++;

			// Skip exported variables (set from editor).
			if (var_sym.is_exported) {
				continue;
			}

			// Skip private if configured.
			if (!include_private && var_sym.name.begins_with("_")) {
				continue;
			}

			// Check if referenced anywhere.
			bool is_referenced = false;
			if (s_index.symbol_references.has(var_sym.name)) {
				const Vector<String> &ref_files = s_index.symbol_references[var_sym.name];
				for (int j = 0; j < ref_files.size(); j++) {
					// Count as referenced if it appears in any file.
					// For same-file, check if referenced more than just the definition.
					if (ref_files[j] != sf.path) {
						is_referenced = true;
						break;
					} else {
						int ref_count = 0;
						for (int r = 0; r < sf.references.size(); r++) {
							if (sf.references[r] == var_sym.name) {
								ref_count++;
							}
						}
						// More than 1 means used beyond definition.
						if (ref_count > 1) {
							is_referenced = true;
							break;
						}
					}
				}
			}

			if (!is_referenced) {
				Dictionary dead;
				dead["name"] = var_sym.name;
				dead["file"] = sf.path;
				dead["line"] = var_sym.line;
				dead_variables.push_back(dead);
			}
		}
	}

	// Build summary.
	int total_dead = dead_functions.size() + dead_signals.size() + dead_variables.size();
	int total_symbols = total_functions + total_signals + total_variables;
	float dead_pct = total_symbols > 0 ? (float)total_dead / (float)total_symbols * 100.0f : 0.0f;

	Dictionary summary;
	summary["dead_functions"] = dead_functions.size();
	summary["dead_signals"] = dead_signals.size();
	summary["dead_variables"] = dead_variables.size();
	summary["total_functions"] = total_functions;
	summary["total_signals"] = total_signals;
	summary["total_variables"] = total_variables;
	summary["percentage_dead"] = vformat("%.1f%%", dead_pct);
	summary["index_build_time_msec"] = (int64_t)s_index.build_time_msec;

	// Build text.
	String text = "=== DEAD CODE ANALYSIS ===\n\n";
	text += vformat("Scanned %d files (%d functions, %d signals, %d variables)\n",
			s_index.files.size(), total_functions, total_signals, total_variables);
	text += vformat("Dead code: %.1f%% (%d items)\n\n", dead_pct, total_dead);

	if (dead_functions.size() > 0) {
		text += vformat("Unused Functions (%d):\n", dead_functions.size());
		for (int i = 0; i < dead_functions.size(); i++) {
			Dictionary d = dead_functions[i];
			text += vformat("  %s  %s:%d\n", (String)d["name"], (String)d["file"], (int)d["line"]);
		}
		text += "\n";
	}

	if (dead_signals.size() > 0) {
		text += vformat("Unused Signals (%d):\n", dead_signals.size());
		for (int i = 0; i < dead_signals.size(); i++) {
			Dictionary d = dead_signals[i];
			text += vformat("  %s  %s:%d  (%s)\n", (String)d["name"], (String)d["file"],
					(int)d["line"], (String)d["reason"]);
		}
		text += "\n";
	}

	if (dead_variables.size() > 0) {
		text += vformat("Unused Variables (%d):\n", dead_variables.size());
		for (int i = 0; i < dead_variables.size(); i++) {
			Dictionary d = dead_variables[i];
			text += vformat("  %s  %s:%d\n", (String)d["name"], (String)d["file"], (int)d["line"]);
		}
		text += "\n";
	}

	if (total_dead == 0) {
		text += "No dead code found. Project is clean!\n";
	}

	text += "=== END DEAD CODE ANALYSIS ===";

	Dictionary structured;
	structured["dead_functions"] = dead_functions;
	structured["dead_signals"] = dead_signals;
	structured["dead_variables"] = dead_variables;
	structured["summary"] = summary;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 2: analysis/signal_flow
// ============================================================================

Dictionary MCPAnalysisTools::handle_signal_flow(const Dictionary &p_args) {
	String signal_name = p_args.get("signal_name", "");
	String file_filter = p_args.get("file", "");
	String direction = p_args.get("direction", "both");

	if (!file_filter.is_empty() && !validate_path(file_filter)) {
		return make_tool_error("Invalid file path: " + file_filter);
	}

	_build_index("res://");

	if (!s_index.is_built || s_index.files.is_empty()) {
		return make_tool_error("No GDScript files found to analyze.");
	}

	// Collect all unique signal names defined in the project.
	HashMap<String, Vector<const ScriptSymbol *>> signal_defs; // sig_name -> definitions
	for (const KeyValue<String, ScriptFile> &E : s_index.files) {
		const ScriptFile &sf = E.value;
		for (int i = 0; i < sf.signals.size(); i++) {
			if (!signal_name.is_empty() && sf.signals[i].name != signal_name) {
				continue;
			}
			if (!file_filter.is_empty() && sf.path != file_filter) {
				continue;
			}
			signal_defs[sf.signals[i].name].push_back(&sf.signals[i]);
		}
	}

	// If a signal_name filter was given but no definitions found, still check
	// for connections/emissions of that name.
	if (!signal_name.is_empty() && !signal_defs.has(signal_name)) {
		signal_defs[signal_name] = Vector<const ScriptSymbol *>();
	}

	Array signals_result;
	Array orphan_signals;
	Array orphan_connections;

	for (const KeyValue<String, Vector<const ScriptSymbol *>> &E : signal_defs) {
		const String &sig_name = E.key;

		Dictionary sig_info;
		sig_info["name"] = sig_name;

		// Where is it defined?
		Array defined_in;
		for (int i = 0; i < E.value.size(); i++) {
			defined_in.push_back(vformat("%s:%d", E.value[i]->file, E.value[i]->line));
		}
		sig_info["defined_in"] = defined_in;

		// Find emitters.
		Array emitters;
		if (direction == "both" || direction == "emitters") {
			for (const KeyValue<String, ScriptFile> &F : s_index.files) {
				const ScriptFile &sf = F.value;
				for (int e = 0; e < sf.signal_emissions.size(); e++) {
					if (sf.signal_emissions[e] == sig_name) {
						Dictionary emitter;
						emitter["file"] = sf.path;
						emitters.push_back(emitter);
						break; // One entry per file.
					}
				}
			}
		}
		sig_info["emitters"] = emitters;

		// Find receivers (connections) -- from in-code connect calls.
		Array receivers;
		if (direction == "both" || direction == "receivers") {
			for (const KeyValue<String, ScriptFile> &F : s_index.files) {
				const ScriptFile &sf = F.value;
				for (int c = 0; c < sf.signal_connections.size(); c++) {
					if (sf.signal_connections[c] == sig_name) {
						Dictionary receiver;
						receiver["file"] = sf.path;
						receiver["source"] = "code";
						receivers.push_back(receiver);
						break; // One entry per file.
					}
				}
			}

			// Also check .tscn scene connections.
			for (int t = 0; t < s_index.tscn_connections.size(); t++) {
				const TscnConnection &conn = s_index.tscn_connections[t];
				if (conn.signal_name == sig_name) {
					Dictionary receiver;
					receiver["file"] = conn.scene_path;
					receiver["method"] = conn.method_name;
					receiver["source"] = "scene";
					receivers.push_back(receiver);
				}
			}
		}
		sig_info["receivers"] = receivers;

		signals_result.push_back(sig_info);

		// Orphan detection.
		if (E.value.size() > 0) {
			bool has_emitters = emitters.size() > 0;
			bool has_receivers = receivers.size() > 0;

			if (!has_emitters && !has_receivers) {
				Dictionary orphan;
				orphan["name"] = sig_name;
				orphan["file"] = E.value[0]->file;
				orphan["line"] = E.value[0]->line;
				orphan["issue"] = "defined but never emitted or connected";
				orphan_signals.push_back(orphan);
			} else if (!has_emitters) {
				Dictionary orphan;
				orphan["name"] = sig_name;
				orphan["file"] = E.value[0]->file;
				orphan["line"] = E.value[0]->line;
				orphan["issue"] = "defined and connected but never emitted";
				orphan_signals.push_back(orphan);
			}
		}
	}

	// Find orphan connections: connections to signals not defined in the project.
	for (const KeyValue<String, ScriptFile> &E : s_index.files) {
		const ScriptFile &sf = E.value;
		for (int c = 0; c < sf.signal_connections.size(); c++) {
			const String &conn_sig = sf.signal_connections[c];
			if (!signal_defs.has(conn_sig)) {
				Dictionary orphan;
				orphan["signal"] = conn_sig;
				orphan["receiver_file"] = sf.path;
				orphan["source"] = "code";
				orphan["issue"] = "connected to signal not defined in project scripts (may be a built-in signal)";
				orphan_connections.push_back(orphan);
			}
		}
	}

	// Also check .tscn scene connections for orphans.
	for (int t = 0; t < s_index.tscn_connections.size(); t++) {
		const TscnConnection &conn = s_index.tscn_connections[t];
		if (!signal_defs.has(conn.signal_name)) {
			Dictionary orphan;
			orphan["signal"] = conn.signal_name;
			orphan["receiver_file"] = conn.scene_path;
			orphan["method"] = conn.method_name;
			orphan["source"] = "scene";
			orphan["issue"] = "scene connection to signal not defined in project scripts (may be a built-in signal)";
			orphan_connections.push_back(orphan);
		}
	}

	// Build text output.
	String text = "=== SIGNAL FLOW ANALYSIS ===\n\n";
	text += vformat("Analyzed %d files, found %d signal definitions\n\n",
			s_index.files.size(), signals_result.size());

	for (int i = 0; i < signals_result.size(); i++) {
		Dictionary sig = signals_result[i];
		text += vformat("signal %s\n", (String)sig["name"]);
		Array def = sig["defined_in"];
		for (int d = 0; d < def.size(); d++) {
			text += vformat("  Defined: %s\n", (String)def[d]);
		}
		Array em = sig["emitters"];
		for (int e = 0; e < em.size(); e++) {
			Dictionary emitter = em[e];
			text += vformat("  Emitter: %s\n", (String)emitter["file"]);
		}
		Array rc = sig["receivers"];
		for (int r = 0; r < rc.size(); r++) {
			Dictionary receiver = rc[r];
			text += vformat("  Receiver: %s\n", (String)receiver["file"]);
		}
		text += "\n";
	}

	if (orphan_signals.size() > 0) {
		text += vformat("Orphan Signals (%d):\n", orphan_signals.size());
		for (int i = 0; i < orphan_signals.size(); i++) {
			Dictionary o = orphan_signals[i];
			text += vformat("  %s in %s:%d - %s\n",
					(String)o["name"], (String)o["file"], (int)o["line"], (String)o["issue"]);
		}
		text += "\n";
	}

	if (orphan_connections.size() > 0) {
		text += vformat("Orphan Connections (%d):\n", orphan_connections.size());
		for (int i = 0; i < orphan_connections.size(); i++) {
			Dictionary o = orphan_connections[i];
			text += vformat("  %s in %s - %s\n",
					(String)o["signal"], (String)o["receiver_file"], (String)o["issue"]);
		}
		text += "\n";
	}

	text += "=== END SIGNAL FLOW ANALYSIS ===";

	Dictionary structured;
	structured["signals"] = signals_result;
	structured["orphan_signals"] = orphan_signals;
	structured["orphan_connections"] = orphan_connections;
	structured["index_build_time_msec"] = (int64_t)s_index.build_time_msec;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 3: analysis/complexity
// ============================================================================

Dictionary MCPAnalysisTools::handle_complexity(const Dictionary &p_args) {
	String root = p_args.get("path", "res://");
	int threshold = 10;
	if (p_args.has("threshold")) {
		threshold = (int)p_args["threshold"];
		if (threshold < 1) {
			threshold = 1;
		}
	}
	String sort_by = p_args.get("sort_by", "complexity");

	if (!root.is_empty() && !validate_path(root)) {
		return make_tool_error("Invalid path: " + root);
	}

	_build_index(root.is_empty() ? "res://" : root);

	if (!s_index.is_built || s_index.files.is_empty()) {
		return make_tool_error("No GDScript files found to analyze.");
	}

	// Collect all functions with their complexities.
	struct FuncComplexity {
		String name;
		String file;
		int line;
		int complexity;
		String grade;
	};

	Vector<FuncComplexity> all_funcs;
	Vector<FuncComplexity> above_threshold;

	int grade_counts[5] = { 0, 0, 0, 0, 0 }; // A, B, C, D, F
	int total_complexity = 0;

	for (const KeyValue<String, ScriptFile> &E : s_index.files) {
		const ScriptFile &sf = E.value;

		for (int i = 0; i < sf.functions.size(); i++) {
			const ScriptSymbol &func = sf.functions[i];
			int complexity = 1; // Default.
			if (sf.function_complexities.has(func.name)) {
				complexity = sf.function_complexities[func.name];
			}

			FuncComplexity fc;
			fc.name = func.name;
			fc.file = sf.path;
			fc.line = func.line;
			fc.complexity = complexity;
			fc.grade = _get_complexity_grade(complexity);

			// Count grades.
			if (fc.grade == "A") {
				grade_counts[0]++;
			} else if (fc.grade == "B") {
				grade_counts[1]++;
			} else if (fc.grade == "C") {
				grade_counts[2]++;
			} else if (fc.grade == "D") {
				grade_counts[3]++;
			} else {
				grade_counts[4]++;
			}

			total_complexity += complexity;
			all_funcs.push_back(fc);

			if (complexity >= threshold) {
				above_threshold.push_back(fc);
			}
		}
	}

	// Sort above_threshold.
	if (sort_by == "complexity") {
		// Sort by complexity descending.
		for (int i = 0; i < above_threshold.size(); i++) {
			for (int j = i + 1; j < above_threshold.size(); j++) {
				if (above_threshold[j].complexity > above_threshold[i].complexity) {
					SWAP(above_threshold.write[i], above_threshold.write[j]);
				}
			}
		}
	} else {
		// Sort by file then line.
		for (int i = 0; i < above_threshold.size(); i++) {
			for (int j = i + 1; j < above_threshold.size(); j++) {
				bool swap = false;
				if (above_threshold[j].file < above_threshold[i].file) {
					swap = true;
				} else if (above_threshold[j].file == above_threshold[i].file &&
						above_threshold[j].line < above_threshold[i].line) {
					swap = true;
				}
				if (swap) {
					SWAP(above_threshold.write[i], above_threshold.write[j]);
				}
			}
		}
	}

	float avg_complexity = all_funcs.size() > 0
			? (float)total_complexity / (float)all_funcs.size()
			: 0.0f;

	// Build structured result.
	Array functions_arr;
	for (int i = 0; i < above_threshold.size(); i++) {
		const FuncComplexity &fc = above_threshold[i];
		Dictionary func_dict;
		func_dict["name"] = fc.name;
		func_dict["file"] = fc.file;
		func_dict["line"] = fc.line;
		func_dict["complexity"] = fc.complexity;
		func_dict["grade"] = fc.grade;

		// Provide suggestion based on grade.
		String suggestion;
		if (fc.grade == "C") {
			suggestion = "Consider breaking into smaller functions";
		} else if (fc.grade == "D") {
			suggestion = "Refactor into smaller functions or use a state machine pattern";
		} else if (fc.grade == "F") {
			suggestion = "Critical: this function is very hard to test and maintain. Refactor immediately.";
		}
		if (!suggestion.is_empty()) {
			func_dict["suggestion"] = suggestion;
		}

		functions_arr.push_back(func_dict);
	}

	Dictionary grade_dist;
	grade_dist["A"] = grade_counts[0];
	grade_dist["B"] = grade_counts[1];
	grade_dist["C"] = grade_counts[2];
	grade_dist["D"] = grade_counts[3];
	grade_dist["F"] = grade_counts[4];

	Dictionary summary;
	summary["total_functions"] = all_funcs.size();
	summary["average_complexity"] = vformat("%.1f", avg_complexity);
	summary["above_threshold"] = above_threshold.size();
	summary["grade_distribution"] = grade_dist;
	summary["index_build_time_msec"] = (int64_t)s_index.build_time_msec;

	// Build text.
	String text = "=== COMPLEXITY ANALYSIS ===\n\n";
	text += vformat("Analyzed %d functions across %d files\n",
			all_funcs.size(), s_index.files.size());
	text += vformat("Average complexity: %.1f\n", avg_complexity);
	text += vformat("Threshold: %d (showing %d functions above threshold)\n\n",
			threshold, above_threshold.size());

	text += vformat("Grade Distribution: A=%d  B=%d  C=%d  D=%d  F=%d\n",
			grade_counts[0], grade_counts[1], grade_counts[2],
			grade_counts[3], grade_counts[4]);
	text += "Grading: A (1-5), B (6-10), C (11-15), D (16-25), F (26+)\n\n";

	if (above_threshold.size() > 0) {
		text += "Functions Above Threshold:\n";
		for (int i = 0; i < above_threshold.size(); i++) {
			const FuncComplexity &fc = above_threshold[i];
			text += vformat("  [%s] %s  complexity=%d  %s:%d\n",
					fc.grade, fc.name, fc.complexity, fc.file, fc.line);
		}
	} else {
		text += "All functions are within acceptable complexity limits.\n";
	}

	text += "\n=== END COMPLEXITY ANALYSIS ===";

	Dictionary structured;
	structured["functions"] = functions_arr;
	structured["summary"] = summary;
	structured["grading"] = "A: 1-5, B: 6-10, C: 11-15, D: 16-25, F: 26+";

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 4: analysis/dependencies
// ============================================================================

Dictionary MCPAnalysisTools::handle_dependencies(const Dictionary &p_args) {
	bool include_preloads = true;
	if (p_args.has("include_preloads")) {
		include_preloads = (bool)p_args["include_preloads"];
	}

	_build_index("res://");

	if (!s_index.is_built) {
		return make_tool_error("Failed to build project index.");
	}

	// Step 1: Read autoloads from project settings.
	struct AutoloadEntry {
		String name;
		String path;
		Vector<String> depends_on; // Names of other autoloads this depends on.
	};

	Vector<AutoloadEntry> autoloads;
	HashMap<String, int> autoload_name_to_idx;

	{
		List<PropertyInfo> settings;
		ProjectSettings::get_singleton()->get_property_list(&settings);
		for (const PropertyInfo &pi : settings) {
			if (pi.name.begins_with("autoload/")) {
				AutoloadEntry entry;
				entry.name = pi.name.get_slice("/", 1);
				entry.path = ProjectSettings::get_singleton()->get_setting(pi.name);
				if (entry.path.begins_with("*")) {
					entry.path = entry.path.substr(1);
				}
				autoload_name_to_idx[entry.name] = autoloads.size();
				autoloads.push_back(entry);
			}
		}
	}

	if (autoloads.is_empty()) {
		String text = "=== DEPENDENCY ANALYSIS ===\n\nNo autoloads defined in this project.\n"
					  "\n=== END DEPENDENCY ANALYSIS ===";
		Dictionary structured;
		structured["autoloads"] = Array();
		structured["cycles"] = Array();
		structured["warnings"] = Array();
		return make_tool_result(text, structured);
	}

	// Step 2: For each autoload, check which other autoloads it references.
	for (int i = 0; i < autoloads.size(); i++) {
		AutoloadEntry &entry = autoloads.write[i];

		if (!s_index.files.has(entry.path)) {
			continue;
		}

		const ScriptFile &sf = s_index.files[entry.path];

		// Check references for other autoload names.
		for (int r = 0; r < sf.references.size(); r++) {
			const String &ref = sf.references[r];
			if (autoload_name_to_idx.has(ref) && ref != entry.name) {
				// This autoload references another autoload.
				bool already_added = false;
				for (int d = 0; d < entry.depends_on.size(); d++) {
					if (entry.depends_on[d] == ref) {
						already_added = true;
						break;
					}
				}
				if (!already_added) {
					entry.depends_on.push_back(ref);
				}
			}
		}

		// Check preload/load references.
		if (include_preloads) {
			for (int r = 0; r < sf.references.size(); r++) {
				const String &ref = sf.references[r];
				// Check if this is a path to an autoload script.
				for (int j = 0; j < autoloads.size(); j++) {
					if (j != i && autoloads[j].path == ref) {
						bool already_added = false;
						for (int d = 0; d < entry.depends_on.size(); d++) {
							if (entry.depends_on[d] == autoloads[j].name) {
								already_added = true;
								break;
							}
						}
						if (!already_added) {
							entry.depends_on.push_back(autoloads[j].name);
						}
					}
				}
			}
		}
	}

	// Step 3: Cycle detection using DFS with coloring.
	// WHITE = 0 (unvisited), GRAY = 1 (in progress), BLACK = 2 (done).
	enum DFSColor { WHITE = 0,
		GRAY = 1,
		BLACK = 2 };

	Vector<int> color;
	color.resize(autoloads.size());
	for (int i = 0; i < autoloads.size(); i++) {
		color.write[i] = WHITE;
	}

	Array cycles;
	Vector<String> path_stack;

	// DFS lambda (implemented as iterative using explicit stack).
	struct DFSFrame {
		int node;
		int dep_idx;
	};

	for (int start = 0; start < autoloads.size(); start++) {
		if (color[start] != WHITE) {
			continue;
		}

		Vector<DFSFrame> stack;
		DFSFrame first;
		first.node = start;
		first.dep_idx = 0;
		stack.push_back(first);
		color.write[start] = GRAY;
		path_stack.push_back(autoloads[start].name);

		while (!stack.is_empty()) {
			DFSFrame &frame = stack.write[stack.size() - 1];
			const AutoloadEntry &entry = autoloads[frame.node];

			if (frame.dep_idx >= entry.depends_on.size()) {
				// All dependencies explored. Mark as done.
				color.write[frame.node] = BLACK;
				path_stack.resize(path_stack.size() - 1);
				stack.resize(stack.size() - 1);
				continue;
			}

			const String &dep_name = entry.depends_on[frame.dep_idx];
			frame.dep_idx++;

			if (!autoload_name_to_idx.has(dep_name)) {
				continue;
			}

			int dep_idx = autoload_name_to_idx[dep_name];

			if (color[dep_idx] == GRAY) {
				// Cycle found! Build the cycle path.
				Array cycle;
				bool recording = false;
				for (int p = 0; p < path_stack.size(); p++) {
					if (path_stack[p] == dep_name) {
						recording = true;
					}
					if (recording) {
						cycle.push_back(path_stack[p]);
					}
				}
				cycle.push_back(dep_name); // Close the cycle.
				cycles.push_back(cycle);
			} else if (color[dep_idx] == WHITE) {
				color.write[dep_idx] = GRAY;
				path_stack.push_back(dep_name);
				DFSFrame next;
				next.node = dep_idx;
				next.dep_idx = 0;
				stack.push_back(next);
			}
		}
	}

	// Step 4: Build warnings from cycles.
	Array warnings;
	for (int i = 0; i < cycles.size(); i++) {
		Array cycle = cycles[i];
		String cycle_str;
		for (int j = 0; j < cycle.size(); j++) {
			if (j > 0) {
				cycle_str += " -> ";
			}
			cycle_str += (String)cycle[j];
		}

		Dictionary warning;
		warning["severity"] = "HIGH";
		warning["message"] = "Circular dependency: " + cycle_str;
		warning["fix"] = "Extract shared state into a third autoload or use signals instead of direct references.";
		warnings.push_back(warning);
	}

	// Step 5: Topological sort for suggested load order.
	// Uses Kahn's algorithm (BFS-based) -- handles cycles gracefully.
	Array suggested_load_order;
	{
		// Compute in-degrees.
		HashMap<String, int> in_degree;
		for (int i = 0; i < autoloads.size(); i++) {
			if (!in_degree.has(autoloads[i].name)) {
				in_degree[autoloads[i].name] = 0;
			}
		}
		for (int i = 0; i < autoloads.size(); i++) {
			for (int d = 0; d < autoloads[i].depends_on.size(); d++) {
				if (in_degree.has(autoloads[i].depends_on[d])) {
					// autoloads[i] depends on depends_on[d],
					// so autoloads[i] should come AFTER depends_on[d].
					// Edge: depends_on[d] -> autoloads[i].name.
					in_degree[autoloads[i].name]++;
				}
			}
		}

		// BFS queue with nodes that have in_degree 0.
		Vector<String> queue;
		for (int i = 0; i < autoloads.size(); i++) {
			if (in_degree[autoloads[i].name] == 0) {
				queue.push_back(autoloads[i].name);
			}
		}

		while (!queue.is_empty()) {
			String current = queue[0];
			queue.remove_at(0);
			suggested_load_order.push_back(current);

			// Find which autoloads depend on 'current' and reduce their in-degree.
			for (int i = 0; i < autoloads.size(); i++) {
				for (int d = 0; d < autoloads[i].depends_on.size(); d++) {
					if (autoloads[i].depends_on[d] == current) {
						in_degree[autoloads[i].name]--;
						if (in_degree[autoloads[i].name] == 0) {
							queue.push_back(autoloads[i].name);
						}
					}
				}
			}
		}

		// Any remaining nodes with in_degree > 0 are in cycles.
		// Add them at the end with a note.
		for (int i = 0; i < autoloads.size(); i++) {
			bool already_added = false;
			for (int j = 0; j < suggested_load_order.size(); j++) {
				if ((String)suggested_load_order[j] == autoloads[i].name) {
					already_added = true;
					break;
				}
			}
			if (!already_added) {
				suggested_load_order.push_back(autoloads[i].name + " (in cycle)");
			}
		}
	}

	// Build text.
	String text = "=== DEPENDENCY ANALYSIS ===\n\n";
	text += vformat("Autoloads: %d\n\n", autoloads.size());

	for (int i = 0; i < autoloads.size(); i++) {
		const AutoloadEntry &entry = autoloads[i];
		text += vformat("%s (%s)\n", entry.name, entry.path);
		if (entry.depends_on.size() > 0) {
			text += "  Depends on: ";
			for (int d = 0; d < entry.depends_on.size(); d++) {
				if (d > 0) {
					text += ", ";
				}
				text += entry.depends_on[d];
			}
			text += "\n";
		} else {
			text += "  No dependencies\n";
		}
	}

	if (cycles.size() > 0) {
		text += vformat("\nCircular Dependencies (%d):\n", cycles.size());
		for (int i = 0; i < warnings.size(); i++) {
			Dictionary w = warnings[i];
			text += vformat("  [%s] %s\n", (String)w["severity"], (String)w["message"]);
			text += vformat("  Fix: %s\n\n", (String)w["fix"]);
		}
	} else {
		text += "\nNo circular dependencies detected.\n";
	}

	text += "\nSuggested Load Order:\n";
	for (int i = 0; i < suggested_load_order.size(); i++) {
		text += vformat("  %d. %s\n", i + 1, (String)suggested_load_order[i]);
	}

	text += "\n=== END DEPENDENCY ANALYSIS ===";

	// Build structured.
	Array autoloads_arr;
	for (int i = 0; i < autoloads.size(); i++) {
		Dictionary al;
		al["name"] = autoloads[i].name;
		al["path"] = autoloads[i].path;
		Array deps;
		for (int d = 0; d < autoloads[i].depends_on.size(); d++) {
			deps.push_back(autoloads[i].depends_on[d]);
		}
		al["depends_on"] = deps;
		autoloads_arr.push_back(al);
	}

	Dictionary structured;
	structured["autoloads"] = autoloads_arr;
	structured["cycles"] = cycles;
	structured["warnings"] = warnings;
	structured["suggested_load_order"] = suggested_load_order;
	structured["index_build_time_msec"] = (int64_t)s_index.build_time_msec;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 5: analysis/project_health
// ============================================================================

Dictionary MCPAnalysisTools::handle_project_health(const Dictionary &p_args) {
	_build_index("res://");

	if (!s_index.is_built || s_index.files.is_empty()) {
		return make_tool_error("No GDScript files found to analyze.");
	}

	// Gather stats.
	int total_scripts = s_index.files.size();
	int total_lines = 0;
	int total_functions = 0;
	int total_signals = 0;
	int total_variables = 0;
	int total_typed_functions = 0;

	for (const KeyValue<String, ScriptFile> &E : s_index.files) {
		const ScriptFile &sf = E.value;
		total_lines += sf.total_lines;
		total_functions += sf.functions.size();
		total_signals += sf.signals.size();
		total_variables += sf.variables.size();
		total_typed_functions += sf.typed_function_count;
	}

	// --- Dead code score ---
	Dictionary dead_args;
	dead_args["path"] = "res://";
	dead_args["include_private"] = true;
	dead_args["ignore_virtual"] = true;
	Dictionary dead_result = handle_dead_code(dead_args);

	int dead_count = 0;
	float dead_pct = 0.0f;
	int dead_score = 10;
	String dead_detail;
	if (dead_result.has("structuredContent")) {
		Dictionary dc = dead_result["structuredContent"];
		if (dc.has("summary")) {
			Dictionary summary = dc["summary"];
			int dead_funcs = summary.get("dead_functions", 0);
			int dead_sigs = summary.get("dead_signals", 0);
			int dead_vars = summary.get("dead_variables", 0);
			dead_count = dead_funcs + dead_sigs + dead_vars;
			String pct_str = summary.get("percentage_dead", "0.0%");
			dead_pct = pct_str.to_float();
		}
	}

	if (dead_pct < 2.0f) {
		dead_score = 10;
		dead_detail = vformat("%.1f%% dead code (excellent)", dead_pct);
	} else if (dead_pct < 5.0f) {
		dead_score = 8;
		dead_detail = vformat("%.1f%% dead code (good)", dead_pct);
	} else if (dead_pct < 10.0f) {
		dead_score = 6;
		dead_detail = vformat("%.1f%% dead code (moderate)", dead_pct);
	} else if (dead_pct < 20.0f) {
		dead_score = 4;
		dead_detail = vformat("%.1f%% dead code (high)", dead_pct);
	} else {
		dead_score = 2;
		dead_detail = vformat("%.1f%% dead code (very high)", dead_pct);
	}

	// --- Complexity score ---
	int complexity_score = 10;
	String complexity_detail;
	int funcs_above_10 = 0;
	int worst_complexity = 0;
	String worst_func_name;
	float avg_complexity = 0.0f;

	{
		int total_complexity = 0;
		int func_count = 0;

		for (const KeyValue<String, ScriptFile> &E : s_index.files) {
			const ScriptFile &sf = E.value;
			for (const KeyValue<String, int> &FC : sf.function_complexities) {
				total_complexity += FC.value;
				func_count++;
				if (FC.value > 10) {
					funcs_above_10++;
				}
				if (FC.value > worst_complexity) {
					worst_complexity = FC.value;
					worst_func_name = FC.key;
				}
			}
		}

		avg_complexity = func_count > 0 ? (float)total_complexity / (float)func_count : 0.0f;
	}

	if (avg_complexity <= 4.0f) {
		complexity_score = 10;
		complexity_detail = vformat("Average %.1f (excellent)", avg_complexity);
	} else if (avg_complexity <= 6.0f) {
		complexity_score = 8;
		complexity_detail = vformat("Average %.1f (good)", avg_complexity);
	} else if (avg_complexity <= 8.0f) {
		complexity_score = 6;
		complexity_detail = vformat("Average %.1f (moderate)", avg_complexity);
	} else if (avg_complexity <= 12.0f) {
		complexity_score = 4;
		complexity_detail = vformat("Average %.1f (high)", avg_complexity);
	} else {
		complexity_score = 2;
		complexity_detail = vformat("Average %.1f (very high)", avg_complexity);
	}

	// --- Dependency score ---
	Dictionary dep_args;
	dep_args["include_preloads"] = true;
	Dictionary dep_result = handle_dependencies(dep_args);

	int cycle_count = 0;
	int dependency_score = 10;
	String dependency_detail;

	if (dep_result.has("structuredContent")) {
		Dictionary dc = dep_result["structuredContent"];
		if (dc.has("cycles")) {
			Array cycles_arr = dc["cycles"];
			cycle_count = cycles_arr.size();
		}
	}

	if (cycle_count == 0) {
		dependency_score = 10;
		dependency_detail = "No circular dependencies";
	} else if (cycle_count <= 2) {
		dependency_score = 5;
		dependency_detail = vformat("%d circular dependency(ies)", cycle_count);
	} else {
		dependency_score = 2;
		dependency_detail = vformat("%d circular dependencies", cycle_count);
	}

	// --- Signal hygiene score ---
	Dictionary sig_args;
	Dictionary sig_result = handle_signal_flow(sig_args);

	int orphan_signal_count = 0;
	int signal_score = 10;
	String signal_detail;

	if (sig_result.has("structuredContent")) {
		Dictionary sc = sig_result["structuredContent"];
		if (sc.has("orphan_signals")) {
			Array orphans = sc["orphan_signals"];
			orphan_signal_count = orphans.size();
		}
	}

	if (orphan_signal_count == 0) {
		signal_score = 10;
		signal_detail = "No orphan signals";
	} else if (orphan_signal_count <= 3) {
		signal_score = 8;
		signal_detail = vformat("%d orphan signal(s)", orphan_signal_count);
	} else if (orphan_signal_count <= 8) {
		signal_score = 6;
		signal_detail = vformat("%d orphan signals", orphan_signal_count);
	} else {
		signal_score = 3;
		signal_detail = vformat("%d orphan signals", orphan_signal_count);
	}

	// --- Type annotation score ---
	float type_annotation_pct = total_functions > 0
			? (float)total_typed_functions / (float)total_functions * 100.0f
			: 100.0f;
	int type_score = 10;
	String type_detail;

	if (type_annotation_pct >= 80.0f) {
		type_score = 10;
		type_detail = vformat("%.0f%% typed (excellent)", type_annotation_pct);
	} else if (type_annotation_pct >= 60.0f) {
		type_score = 8;
		type_detail = vformat("%.0f%% typed (good)", type_annotation_pct);
	} else if (type_annotation_pct >= 40.0f) {
		type_score = 6;
		type_detail = vformat("%.0f%% typed (moderate)", type_annotation_pct);
	} else if (type_annotation_pct >= 20.0f) {
		type_score = 4;
		type_detail = vformat("%.0f%% typed (low)", type_annotation_pct);
	} else {
		type_score = 2;
		type_detail = vformat("%.0f%% typed (very low)", type_annotation_pct);
	}

	// --- Autoload count ---
	int total_autoloads = 0;
	{
		List<PropertyInfo> settings;
		ProjectSettings::get_singleton()->get_property_list(&settings);
		for (const PropertyInfo &pi : settings) {
			if (pi.name.begins_with("autoload/")) {
				total_autoloads++;
			}
		}
	}

	// --- Overall grade ---
	// Weighted: type_safety=20%, dead_code=15%, complexity=25%, dependencies=15%, signals=15%, organization=10%.
	float weighted_score = (float)type_score * 0.20f +
			(float)dead_score * 0.15f +
			(float)complexity_score * 0.25f +
			(float)dependency_score * 0.15f +
			(float)signal_score * 0.15f +
			// File organization bonus (if scripts > 0 and no parse errors, give 8/10).
			8.0f * 0.10f;

	String overall_grade;
	if (weighted_score >= 9.0f) {
		overall_grade = "A";
	} else if (weighted_score >= 8.0f) {
		overall_grade = "A-";
	} else if (weighted_score >= 7.0f) {
		overall_grade = "B+";
	} else if (weighted_score >= 6.0f) {
		overall_grade = "B";
	} else if (weighted_score >= 5.0f) {
		overall_grade = "B-";
	} else if (weighted_score >= 4.0f) {
		overall_grade = "C";
	} else if (weighted_score >= 3.0f) {
		overall_grade = "D";
	} else {
		overall_grade = "F";
	}

	// --- Top issues ---
	Array top_issues;

	if (type_annotation_pct < 50.0f && total_functions > 5) {
		top_issues.push_back(vformat("Only %.0f%% of functions have return type annotations -- add -> Type to function signatures", type_annotation_pct));
	}

	if (worst_complexity > 15 && !worst_func_name.is_empty()) {
		top_issues.push_back(vformat("%s() has complexity %d -- refactor into smaller functions",
				worst_func_name, worst_complexity));
	}

	if (funcs_above_10 > 0) {
		top_issues.push_back(vformat("%d function(s) exceed complexity threshold of 10", funcs_above_10));
	}

	if (dead_count > 0) {
		top_issues.push_back(vformat("%d dead code items found -- consider removing unused code", dead_count));
	}

	if (cycle_count > 0) {
		top_issues.push_back(vformat("%d circular autoload dependency(ies) detected -- refactor to break cycles", cycle_count));
	}

	if (orphan_signal_count > 0) {
		top_issues.push_back(vformat("%d orphan signal(s) -- remove or connect them", orphan_signal_count));
	}

	if (top_issues.is_empty()) {
		top_issues.push_back("No major issues found. Project is in good shape!");
	}

	// Build text.
	String text = "=== PROJECT HEALTH DASHBOARD ===\n\n";
	text += vformat("Overall Grade: %s  (weighted score: %.1f/10)\n\n", overall_grade, weighted_score);

	text += "Scores:\n";
	text += vformat("  Type Safety:    %d/10  %s\n", type_score, type_detail);
	text += vformat("  Dead Code:      %d/10  %s\n", dead_score, dead_detail);
	text += vformat("  Complexity:     %d/10  %s\n", complexity_score, complexity_detail);
	text += vformat("  Dependencies:   %d/10  %s\n", dependency_score, dependency_detail);
	text += vformat("  Signal Hygiene: %d/10  %s\n\n", signal_score, signal_detail);

	text += "Stats:\n";
	text += vformat("  Scripts: %d | Lines: %d | Functions: %d | Signals: %d | Autoloads: %d\n\n",
			total_scripts, total_lines, total_functions, total_signals, total_autoloads);

	text += "Top Issues:\n";
	for (int i = 0; i < top_issues.size(); i++) {
		text += vformat("  %d. %s\n", i + 1, (String)top_issues[i]);
	}

	text += "\n=== END PROJECT HEALTH DASHBOARD ===";

	// Build structured.
	Dictionary scores;

	Dictionary type_score_d;
	type_score_d["score"] = type_score;
	type_score_d["detail"] = type_detail;
	type_score_d["annotation_rate"] = vformat("%.0f%%", type_annotation_pct);
	scores["type_safety"] = type_score_d;

	Dictionary dead_score_d;
	dead_score_d["score"] = dead_score;
	dead_score_d["detail"] = dead_detail;
	scores["dead_code"] = dead_score_d;

	Dictionary comp_score_d;
	comp_score_d["score"] = complexity_score;
	comp_score_d["detail"] = complexity_detail;
	scores["complexity"] = comp_score_d;

	Dictionary dep_score_d;
	dep_score_d["score"] = dependency_score;
	dep_score_d["detail"] = dependency_detail;
	scores["dependencies"] = dep_score_d;

	Dictionary sig_score_d;
	sig_score_d["score"] = signal_score;
	sig_score_d["detail"] = signal_detail;
	scores["signal_hygiene"] = sig_score_d;

	Dictionary stats;
	stats["total_scripts"] = total_scripts;
	stats["total_lines"] = total_lines;
	stats["total_functions"] = total_functions;
	stats["typed_functions"] = total_typed_functions;
	stats["total_signals"] = total_signals;
	stats["total_autoloads"] = total_autoloads;

	Dictionary structured;
	structured["grade"] = overall_grade;
	structured["weighted_score"] = vformat("%.1f", weighted_score);
	structured["scores"] = scores;
	structured["top_issues"] = top_issues;
	structured["stats"] = stats;
	structured["index_build_time_msec"] = (int64_t)s_index.build_time_msec;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 6: analysis/duplication
// ============================================================================

Dictionary MCPAnalysisTools::handle_duplication(const Dictionary &p_args) {
	String root = p_args.get("path", "res://");
	int min_lines = 5;
	if (p_args.has("min_lines")) {
		min_lines = (int)p_args["min_lines"];
		if (min_lines < 3) {
			min_lines = 3;
		}
	}

	if (!root.is_empty() && !validate_path(root)) {
		return make_tool_error("Invalid path: " + root);
	}

	_build_index(root.is_empty() ? "res://" : root);

	if (!s_index.is_built || s_index.files.is_empty()) {
		return make_tool_error("No GDScript files found to analyze.");
	}

	// For each file, extract function bodies, normalize, and hash them.
	struct FuncBody {
		String file;
		String name;
		int line;
		String normalized;
		String sample; // First 200 chars of original code.
	};

	// Hash -> list of function bodies with that hash.
	HashMap<uint32_t, Vector<FuncBody>> hash_buckets;

	for (const KeyValue<String, ScriptFile> &E : s_index.files) {
		const ScriptFile &sf = E.value;

		String source = FileAccess::get_file_as_string(sf.path);
		if (source.is_empty()) {
			continue;
		}

		Vector<String> lines = source.split("\n");

		// Walk through functions.
		for (int fi = 0; fi < sf.functions.size(); fi++) {
			const ScriptSymbol &func = sf.functions[fi];
			int func_start = func.line - 1; // Convert to 0-based.
			if (func_start < 0 || func_start >= lines.size()) {
				continue;
			}

			// Determine the function's indentation.
			int func_indent = 0;
			{
				String raw_line = lines[func_start];
				for (int c = 0; c < raw_line.length(); c++) {
					if (raw_line[c] == '\t') {
						func_indent++;
					} else {
						break;
					}
				}
			}

			// Collect the function body (lines after the func definition).
			Vector<String> body_lines;
			for (int li = func_start + 1; li < lines.size(); li++) {
				String raw = lines[li];
				String stripped = raw.strip_edges();

				if (stripped.is_empty()) {
					continue;
				}

				int body_indent = 0;
				for (int c = 0; c < raw.length(); c++) {
					if (raw[c] == '\t') {
						body_indent++;
					} else {
						break;
					}
				}

				if (body_indent <= func_indent && !stripped.is_empty() && !stripped.begins_with("#")) {
					break;
				}

				body_lines.push_back(stripped);
			}

			if (body_lines.size() < min_lines) {
				continue;
			}

			// Normalize: strip comments, normalize strings and numbers, collapse whitespace.
			String normalized;
			for (int li = 0; li < body_lines.size(); li++) {
				String l = body_lines[li];

				if (l.begins_with("#")) {
					continue;
				}

				int hash_pos = l.find("#");
				if (hash_pos > 0) {
					l = l.substr(0, hash_pos).strip_edges();
				}

				// Normalize quoted strings to "".
				String norm_line;
				bool in_str = false;
				char32_t str_char = 0;
				for (int c = 0; c < l.length(); c++) {
					char32_t ch = l[c];
					if (in_str) {
						if (ch == str_char && (c == 0 || l[c - 1] != '\\')) {
							in_str = false;
							norm_line += "\"\"";
						}
					} else {
						if (ch == '"' || ch == '\'') {
							in_str = true;
							str_char = ch;
						} else {
							norm_line += String::chr(ch);
						}
					}
				}
				if (in_str) {
					norm_line += "\"\"";
				}

				// Normalize numbers to N.
				String final_line;
				bool in_number = false;
				for (int c = 0; c < norm_line.length(); c++) {
					char32_t ch = norm_line[c];
					if ((ch >= '0' && ch <= '9') || (in_number && ch == '.')) {
						if (!in_number) {
							final_line += "N";
							in_number = true;
						}
					} else {
						in_number = false;
						final_line += String::chr(ch);
					}
				}

				// Collapse whitespace.
				String collapsed;
				bool prev_space = false;
				for (int c = 0; c < final_line.length(); c++) {
					char32_t ch = final_line[c];
					if (ch == ' ' || ch == '\t') {
						if (!prev_space) {
							collapsed += " ";
						}
						prev_space = true;
					} else {
						collapsed += String::chr(ch);
						prev_space = false;
					}
				}

				if (!collapsed.strip_edges().is_empty()) {
					normalized += collapsed.strip_edges() + "\n";
				}
			}

			if (normalized.is_empty()) {
				continue;
			}

			uint32_t hash = normalized.hash();

			FuncBody fb;
			fb.file = sf.path;
			fb.name = func.name;
			fb.line = func.line;
			fb.normalized = normalized;

			String sample;
			for (int li = 0; li < body_lines.size() && sample.length() < 200; li++) {
				if (li > 0) {
					sample += "\n";
				}
				sample += body_lines[li];
			}
			if (sample.length() > 200) {
				sample = sample.substr(0, 200) + "...";
			}
			fb.sample = sample;

			hash_buckets[hash].push_back(fb);
		}
	}

	// Find duplicates.
	Array duplicates;
	for (const KeyValue<uint32_t, Vector<FuncBody>> &B : hash_buckets) {
		if (B.value.size() < 2) {
			continue;
		}

		// Group by exact normalized match.
		Vector<Vector<int>> groups;
		Vector<bool> assigned;
		assigned.resize(B.value.size());
		for (int i = 0; i < assigned.size(); i++) {
			assigned.write[i] = false;
		}

		for (int i = 0; i < B.value.size(); i++) {
			if (assigned[i]) {
				continue;
			}
			Vector<int> group;
			group.push_back(i);
			assigned.write[i] = true;
			for (int j = i + 1; j < B.value.size(); j++) {
				if (!assigned[j] && B.value[i].normalized == B.value[j].normalized) {
					group.push_back(j);
					assigned.write[j] = true;
				}
			}
			if (group.size() >= 2) {
				groups.push_back(group);
			}
		}

		for (int g = 0; g < groups.size(); g++) {
			Dictionary dup;
			dup["occurrences"] = groups[g].size();

			Array locations;
			for (int k = 0; k < groups[g].size(); k++) {
				const FuncBody &fb = B.value[groups[g][k]];
				Dictionary loc;
				loc["file"] = fb.file;
				loc["function"] = fb.name;
				loc["line"] = fb.line;
				locations.push_back(loc);
			}
			dup["locations"] = locations;
			dup["sample"] = B.value[groups[g][0]].sample;
			duplicates.push_back(dup);
		}
	}

	// Sort by occurrence count descending.
	for (int i = 0; i < duplicates.size(); i++) {
		for (int j = i + 1; j < duplicates.size(); j++) {
			Dictionary di = duplicates[i];
			Dictionary dj = duplicates[j];
			if ((int)dj["occurrences"] > (int)di["occurrences"]) {
				duplicates[i] = dj;
				duplicates[j] = di;
			}
		}
	}

	// Build text.
	String text = "=== CODE DUPLICATION ANALYSIS ===\n\n";
	text += vformat("Analyzed %d files, min function body length: %d lines\n", s_index.files.size(), min_lines);
	text += vformat("Duplicate groups found: %d\n\n", duplicates.size());

	for (int i = 0; i < duplicates.size() && i < 20; i++) {
		Dictionary dup = duplicates[i];
		text += vformat("Duplicate #%d (%d occurrences):\n", i + 1, (int)dup["occurrences"]);
		Array locs = dup["locations"];
		for (int l = 0; l < locs.size(); l++) {
			Dictionary loc = locs[l];
			text += vformat("  %s::%s  line %d\n", (String)loc["file"], (String)loc["function"], (int)loc["line"]);
		}
		text += vformat("  Sample: %s\n\n", (String)dup["sample"]);
	}

	if (duplicates.is_empty()) {
		text += "No significant code duplication found.\n";
	} else {
		text += "Recommendation: Consider extracting duplicated code into shared utility functions.\n";
	}

	text += "\n=== END CODE DUPLICATION ANALYSIS ===";

	Dictionary summary;
	summary["total_duplicates"] = duplicates.size();
	summary["files_analyzed"] = (int)s_index.files.size();
	summary["min_lines_threshold"] = min_lines;

	Dictionary structured;
	structured["summary"] = summary;
	structured["duplicates"] = duplicates;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 7: analysis/validate_scenes
// ============================================================================

Dictionary MCPAnalysisTools::handle_validate_scenes(const Dictionary &p_args) {
	String root = p_args.get("path", "res://");

	if (!root.is_empty() && !validate_path(root)) {
		return make_tool_error("Invalid path: " + root);
	}

	String scan_root = root.is_empty() ? "res://" : root;

	Vector<String> tscn_files;
	_find_tscn_files(scan_root, tscn_files);

	if (tscn_files.is_empty()) {
		return make_tool_error("No .tscn scene files found to validate.");
	}

	Array scene_issues;
	int total_errors = 0;
	int total_warnings = 0;
	int total_info = 0;
	int valid_scenes = 0;

	for (int si = 0; si < tscn_files.size(); si++) {
		String source = FileAccess::get_file_as_string(tscn_files[si]);
		if (source.is_empty()) {
			continue;
		}

		Vector<String> lines = source.split("\n");
		Array issues;

		// 1. Check ext_resource paths exist on disk.
		for (int i = 0; i < lines.size(); i++) {
			String line = lines[i].strip_edges();

			if (line.begins_with("[ext_resource ")) {
				int path_pos = line.find("path=\"");
				if (path_pos >= 0) {
					String after = line.substr(path_pos + 6);
					int quote_end = after.find("\"");
					if (quote_end > 0) {
						String res_path = after.substr(0, quote_end);
						if (res_path.begins_with("res://")) {
							if (!FileAccess::exists(res_path)) {
								Dictionary issue;
								issue["type"] = "error";
								issue["category"] = "missing_resource";
								issue["line"] = i + 1;
								issue["message"] = "Missing external resource: " + res_path;
								issues.push_back(issue);
								total_errors++;
							}
						}
					}
				}
			}
		}

		// 2. Check for suspicious NodePath references.
		for (int i = 0; i < lines.size(); i++) {
			String line = lines[i];
			int np_pos = line.find("NodePath(\"");
			if (np_pos >= 0) {
				String after = line.substr(np_pos + 10);
				int quote_end = after.find("\"");
				if (quote_end > 0) {
					String node_path = after.substr(0, quote_end);
					int levels = 0;
					int search = 0;
					while (true) {
						int found = node_path.find("../", search);
						if (found == -1) {
							break;
						}
						levels++;
						search = found + 3;
					}
					if (levels > 5) {
						Dictionary issue;
						issue["type"] = "warning";
						issue["category"] = "suspicious_path";
						issue["line"] = i + 1;
						issue["message"] = vformat("Suspicious deep NodePath (%d levels up): %s", levels, node_path);
						issues.push_back(issue);
						total_warnings++;
					}
				}
			}
		}

		// 3. Check for duplicate node names at the same parent level.
		HashMap<String, Vector<String>> nodes_by_parent;
		for (int i = 0; i < lines.size(); i++) {
			String line = lines[i].strip_edges();
			if (line.begins_with("[node ")) {
				String node_name;
				String parent_path;

				int name_pos = line.find("name=\"");
				if (name_pos >= 0) {
					String after = line.substr(name_pos + 6);
					int quote_end = after.find("\"");
					if (quote_end > 0) {
						node_name = after.substr(0, quote_end);
					}
				}

				int parent_pos = line.find("parent=\"");
				if (parent_pos >= 0) {
					String after = line.substr(parent_pos + 8);
					int quote_end = after.find("\"");
					if (quote_end > 0) {
						parent_path = after.substr(0, quote_end);
					}
				} else {
					parent_path = "__root__";
				}

				if (!node_name.is_empty()) {
					if (!nodes_by_parent.has(parent_path)) {
						nodes_by_parent[parent_path] = Vector<String>();
					}
					Vector<String> &names = nodes_by_parent[parent_path];
					for (int n = 0; n < names.size(); n++) {
						if (names[n] == node_name) {
							Dictionary issue;
							issue["type"] = "warning";
							issue["category"] = "duplicate_name";
							issue["line"] = i + 1;
							issue["message"] = vformat("Duplicate node name \"%s\" under parent \"%s\"",
									node_name, parent_path == "__root__" ? "root" : parent_path);
							issues.push_back(issue);
							total_warnings++;
							break;
						}
					}
					names.push_back(node_name);
				}
			}
		}

		// 4. Check for unconventional signal handler names.
		for (int i = 0; i < lines.size(); i++) {
			String line = lines[i].strip_edges();
			if (line.begins_with("[connection ")) {
				int method_pos = line.find("method=\"");
				if (method_pos >= 0) {
					String after = line.substr(method_pos + 8);
					int quote_end = after.find("\"");
					if (quote_end > 0) {
						String method = after.substr(0, quote_end);
						if (!method.begins_with("_on_") && !method.begins_with("_")) {
							Dictionary issue;
							issue["type"] = "info";
							issue["category"] = "unconventional_handler";
							issue["line"] = i + 1;
							issue["message"] = vformat("Signal handler doesn't follow _on_* convention: %s", method);
							issues.push_back(issue);
							total_info++;
						}
					}
				}
			}
		}

		if (issues.size() > 0) {
			Dictionary scene_entry;
			scene_entry["scene"] = tscn_files[si];
			scene_entry["issues"] = issues;
			scene_issues.push_back(scene_entry);
		} else {
			valid_scenes++;
		}
	}

	// Build text.
	String text = "=== SCENE VALIDATION ===\n\n";
	text += vformat("Scanned %d scene files\n", tscn_files.size());
	text += vformat("Valid: %d | Errors: %d | Warnings: %d | Info: %d\n\n",
			valid_scenes, total_errors, total_warnings, total_info);

	for (int i = 0; i < scene_issues.size(); i++) {
		Dictionary entry = scene_issues[i];
		text += vformat("Scene: %s\n", (String)entry["scene"]);
		Array issues = entry["issues"];
		for (int j = 0; j < issues.size(); j++) {
			Dictionary issue = issues[j];
			text += vformat("  [%s] L%d: %s\n",
					(String)issue["type"], (int)issue["line"], (String)issue["message"]);
		}
		text += "\n";
	}

	if (total_errors == 0 && total_warnings == 0) {
		text += "All scenes are valid!\n";
	} else if (total_errors > 0) {
		text += "Fix missing resources and scripts before running the game.\n";
	}

	text += "\n=== END SCENE VALIDATION ===";

	Dictionary summary;
	summary["total_scenes"] = tscn_files.size();
	summary["valid"] = valid_scenes;
	summary["errors"] = total_errors;
	summary["warnings"] = total_warnings;
	summary["info"] = total_info;

	Dictionary structured;
	structured["summary"] = summary;
	structured["health"] = total_errors == 0 ? "HEALTHY" : "NEEDS ATTENTION";
	structured["issues"] = scene_issues;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 8: analysis/assets
// ============================================================================

Dictionary MCPAnalysisTools::handle_asset_analysis(const Dictionary &p_args) {
	String root = p_args.get("path", "res://");
	float size_threshold_mb = 2.0f;
	if (p_args.has("size_threshold_mb")) {
		size_threshold_mb = (float)(double)p_args["size_threshold_mb"];
		if (size_threshold_mb < 0.1f) {
			size_threshold_mb = 0.1f;
		}
	}

	if (!root.is_empty() && !validate_path(root)) {
		return make_tool_error("Invalid path: " + root);
	}

	String scan_root = root.is_empty() ? "res://" : root;

	// Collect all files by type using recursive directory scanning.
	Vector<String> asset_files;
	Vector<String> scene_files;
	Vector<String> script_files;
	Vector<String> resource_files;

	struct AssetFileCollector {
		static void collect(const String &p_dir,
				Vector<String> &r_assets, Vector<String> &r_scenes,
				Vector<String> &r_scripts, Vector<String> &r_resources) {
			Ref<DirAccess> da = DirAccess::open(p_dir);
			if (da.is_null()) {
				return;
			}
			da->list_dir_begin();
			String item = da->get_next();
			Vector<String> subdirs;
			while (!item.is_empty()) {
				if (item != "." && item != ".." && !item.begins_with(".")) {
					String full_path = p_dir.path_join(item);
					if (da->current_is_dir()) {
						if (!is_skip_directory(item)) {
							subdirs.push_back(full_path);
						}
					} else {
						String ext = item.get_extension().to_lower();
						if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp" ||
								ext == "svg" || ext == "wav" || ext == "ogg" || ext == "mp3" ||
								ext == "ttf" || ext == "otf" || ext == "woff" || ext == "woff2") {
							r_assets.push_back(full_path);
						} else if (ext == "tscn") {
							r_scenes.push_back(full_path);
						} else if (ext == "gd") {
							r_scripts.push_back(full_path);
						} else if (ext == "tres") {
							r_resources.push_back(full_path);
						}
					}
				}
				item = da->get_next();
			}
			da->list_dir_end();
			subdirs.sort();
			for (const String &subdir : subdirs) {
				collect(subdir, r_assets, r_scenes, r_scripts, r_resources);
			}
		}
	};

	AssetFileCollector::collect(scan_root, asset_files, scene_files, script_files, resource_files);

	// Collect all res:// references from scenes, scripts, and resources.
	HashSet<String> referenced_paths;
	Vector<String> all_source_files;
	all_source_files.append_array(scene_files);
	all_source_files.append_array(script_files);
	all_source_files.append_array(resource_files);

	for (int i = 0; i < all_source_files.size(); i++) {
		String content = FileAccess::get_file_as_string(all_source_files[i]);
		if (content.is_empty()) {
			continue;
		}

		int search_pos = 0;
		while (true) {
			int res_pos = content.find("res://", search_pos);
			if (res_pos == -1) {
				break;
			}
			int end = res_pos + 6;
			while (end < content.length()) {
				char32_t ch = content[end];
				if (ch == '"' || ch == '\'' || ch == ' ' || ch == ')' || ch == '\n' || ch == '\r') {
					break;
				}
				end++;
			}
			String ref_path = content.substr(res_pos, end - res_pos);
			referenced_paths.insert(ref_path);
			search_pos = end;
		}
	}

	// Categorize assets and find unused.
	Array unused_images;
	Array unused_audio;
	Array large_files;
	int total_images = 0;
	int total_audio = 0;
	int total_fonts = 0;

	for (int i = 0; i < asset_files.size(); i++) {
		String ext = asset_files[i].get_extension().to_lower();
		bool is_image = (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp" || ext == "svg");
		bool is_audio = (ext == "wav" || ext == "ogg" || ext == "mp3");

		if (is_image) {
			total_images++;
		} else if (is_audio) {
			total_audio++;
		} else {
			total_fonts++;
		}

		bool is_referenced = referenced_paths.has(asset_files[i]);

		Ref<FileAccess> fa = FileAccess::open(asset_files[i], FileAccess::READ);
		uint64_t file_size = fa.is_valid() ? fa->get_length() : 0;
		float size_mb = (float)file_size / (1024.0f * 1024.0f);

		if (!is_referenced) {
			Dictionary entry;
			entry["path"] = asset_files[i];
			entry["size"] = vformat("%.2f MB", size_mb);
			if (is_image) {
				unused_images.push_back(entry);
			} else if (is_audio) {
				unused_audio.push_back(entry);
			}
		}

		if (size_mb > size_threshold_mb) {
			Dictionary entry;
			entry["path"] = asset_files[i];
			entry["size"] = vformat("%.2f MB", size_mb);
			entry["type"] = is_image ? "image" : (is_audio ? "audio" : "font");
			if (is_image && size_mb > 5.0f) {
				entry["recommendation"] = "Consider compressing or using WebP format";
			} else if (is_audio && size_mb > 5.0f) {
				entry["recommendation"] = "Consider using OGG format for compression";
			} else {
				entry["recommendation"] = "Large file - verify if needed";
			}
			large_files.push_back(entry);
		}
	}

	int total_assets = total_images + total_audio + total_fonts;
	int unused_count = unused_images.size() + unused_audio.size();
	float waste_pct = total_assets > 0 ? (float)unused_count / (float)total_assets * 100.0f : 0.0f;

	String text = "=== ASSET ANALYSIS ===\n\n";
	text += vformat("Total assets: %d (images: %d, audio: %d, fonts: %d)\n",
			total_assets, total_images, total_audio, total_fonts);
	text += vformat("Unused: %d (%.1f%%)\n", unused_count, waste_pct);
	text += vformat("Large files (>%.1f MB): %d\n\n", size_threshold_mb, large_files.size());

	if (unused_images.size() > 0) {
		text += vformat("Unused Images (%d):\n", unused_images.size());
		for (int i = 0; i < unused_images.size() && i < 20; i++) {
			Dictionary e = unused_images[i];
			text += vformat("  %s (%s)\n", (String)e["path"], (String)e["size"]);
		}
		text += "\n";
	}

	if (unused_audio.size() > 0) {
		text += vformat("Unused Audio (%d):\n", unused_audio.size());
		for (int i = 0; i < unused_audio.size() && i < 10; i++) {
			Dictionary e = unused_audio[i];
			text += vformat("  %s (%s)\n", (String)e["path"], (String)e["size"]);
		}
		text += "\n";
	}

	if (large_files.size() > 0) {
		text += vformat("Large Files (%d):\n", large_files.size());
		for (int i = 0; i < large_files.size() && i < 10; i++) {
			Dictionary e = large_files[i];
			text += vformat("  %s (%s) - %s\n", (String)e["path"], (String)e["size"], (String)e["recommendation"]);
		}
		text += "\n";
	}

	if (unused_count == 0 && large_files.is_empty()) {
		text += "All assets are referenced and within size limits.\n";
	}

	text += "\n=== END ASSET ANALYSIS ===";

	Dictionary summary;
	summary["total_images"] = total_images;
	summary["total_audio"] = total_audio;
	summary["total_fonts"] = total_fonts;
	summary["unused_images"] = unused_images.size();
	summary["unused_audio"] = unused_audio.size();
	summary["large_files"] = large_files.size();
	summary["waste_percentage"] = vformat("%.1f%%", waste_pct);

	Dictionary structured;
	structured["summary"] = summary;
	structured["health"] = waste_pct < 5.0f ? "EXCELLENT" : (waste_pct < 15.0f ? "GOOD" : "NEEDS CLEANUP");
	structured["unused_images"] = unused_images;
	structured["unused_audio"] = unused_audio;
	structured["large_files"] = large_files;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 9: analysis/input_mappings
// ============================================================================

Dictionary MCPAnalysisTools::handle_input_mappings(const Dictionary &p_args) {
	String project_content = FileAccess::get_file_as_string("res://project.godot");
	if (project_content.is_empty()) {
		return make_tool_error("Could not read project.godot");
	}

	// Extract [input] section actions.
	HashSet<String> defined_actions;
	Vector<String> project_lines = project_content.split("\n");
	bool in_input_section = false;
	for (int i = 0; i < project_lines.size(); i++) {
		String line = project_lines[i].strip_edges();
		if (line == "[input]") {
			in_input_section = true;
			continue;
		}
		if (line.begins_with("[") && line != "[input]") {
			in_input_section = false;
			continue;
		}
		if (in_input_section && !line.is_empty()) {
			int eq_pos = line.find("=");
			if (eq_pos > 0) {
				String action_name = line.substr(0, eq_pos).strip_edges();
				if (!action_name.is_empty()) {
					defined_actions.insert(action_name);
				}
			}
		}
	}

	_build_index("res://");

	HashSet<String> used_actions;
	Array undefined_usages;

	static const char *input_methods[] = {
		"is_action_pressed(\"", "is_action_just_pressed(\"",
		"is_action_just_released(\"", "get_action_strength(\"",
		"get_action_raw_strength(\"", "is_action(\"",
		nullptr
	};

	for (const KeyValue<String, ScriptFile> &E : s_index.files) {
		String source = FileAccess::get_file_as_string(E.value.path);
		if (source.is_empty()) {
			continue;
		}

		for (int m = 0; input_methods[m] != nullptr; m++) {
			String pattern = String("Input.") + input_methods[m];
			int search_pos = 0;
			while (true) {
				int pos = source.find(pattern, search_pos);
				if (pos == -1) {
					break;
				}
				String after = source.substr(pos + pattern.length());
				int quote_end = after.find("\"");
				if (quote_end > 0) {
					String action = after.substr(0, quote_end);
					used_actions.insert(action);
					if (!defined_actions.has(action)) {
						Dictionary entry;
						entry["file"] = E.value.path;
						entry["action"] = action;
						entry["issue"] = "Action used but not defined in project.godot [input] section";
						undefined_usages.push_back(entry);
					}
				}
				search_pos = pos + pattern.length();
			}
		}

		// Check Input.get_vector() calls (4 action arguments).
		{
			String pattern = "Input.get_vector(\"";
			int search_pos = 0;
			while (true) {
				int pos = source.find(pattern, search_pos);
				if (pos == -1) {
					break;
				}
				String after = source.substr(pos + pattern.length());
				int arg_start = 0;
				for (int a = 0; a < 4; a++) {
					int quote_end = after.find("\"", arg_start);
					if (quote_end < 0) {
						break;
					}
					String action = after.substr(arg_start, quote_end - arg_start);
					used_actions.insert(action);
					if (!defined_actions.has(action)) {
						Dictionary entry;
						entry["file"] = E.value.path;
						entry["action"] = action;
						entry["issue"] = "Action used in get_vector() but not defined in project.godot";
						undefined_usages.push_back(entry);
					}
					int next_quote = after.find("\"", quote_end + 1);
					if (next_quote < 0) {
						break;
					}
					arg_start = next_quote + 1;
				}
				search_pos = pos + pattern.length();
			}
		}
	}

	// Find unused defined actions.
	Array unused_actions;
	for (const String &action : defined_actions) {
		if (!used_actions.has(action)) {
			unused_actions.push_back(action);
		}
	}

	String text = "=== INPUT MAPPING VALIDATION ===\n\n";
	text += vformat("Defined actions: %d\n", defined_actions.size());
	text += vformat("Used actions: %d\n", used_actions.size());
	text += vformat("Unused actions: %d\n", unused_actions.size());
	text += vformat("Undefined usages: %d\n\n", undefined_usages.size());

	if (undefined_usages.size() > 0) {
		text += "Actions used but NOT defined (will crash at runtime):\n";
		for (int i = 0; i < undefined_usages.size() && i < 20; i++) {
			Dictionary e = undefined_usages[i];
			text += vformat("  %s in %s\n", (String)e["action"], (String)e["file"]);
		}
		text += "\n";
	}

	if (unused_actions.size() > 0) {
		text += "Actions defined but never used (dead config):\n";
		for (int i = 0; i < unused_actions.size(); i++) {
			text += vformat("  %s\n", (String)unused_actions[i]);
		}
		text += "\n";
	}

	if (undefined_usages.is_empty() && unused_actions.is_empty()) {
		text += "All input mappings are properly configured!\n";
	}

	text += "\n=== END INPUT MAPPING VALIDATION ===";

	Dictionary summary;
	summary["defined_actions"] = defined_actions.size();
	summary["used_actions"] = used_actions.size();
	summary["unused_actions"] = unused_actions.size();
	summary["undefined_usages"] = undefined_usages.size();

	String health = "HEALTHY";
	if (undefined_usages.size() > 0) {
		health = "ERRORS FOUND";
	} else if (unused_actions.size() > 3) {
		health = "NEEDS CLEANUP";
	}

	Dictionary structured;
	structured["summary"] = summary;
	structured["health"] = health;
	structured["unused_actions"] = unused_actions;
	structured["undefined_usages"] = undefined_usages;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 10: analysis/unused_files
// ============================================================================

Dictionary MCPAnalysisTools::handle_unused_files(const Dictionary &p_args) {
	String root = p_args.get("path", "res://");

	if (!root.is_empty() && !validate_path(root)) {
		return make_tool_error("Invalid path: " + root);
	}

	String scan_root = root.is_empty() ? "res://" : root;

	// Find all scripts, scenes, and resources.
	Vector<String> all_scripts;
	Vector<String> all_scenes;
	Vector<String> all_resources;

	_find_gd_files(scan_root, all_scripts);
	_find_tscn_files(scan_root, all_scenes);

	// Find .tres files.
	struct TresCollector {
		static void collect(const String &p_dir, Vector<String> &r_files) {
			Ref<DirAccess> da = DirAccess::open(p_dir);
			if (da.is_null()) {
				return;
			}
			da->list_dir_begin();
			String item = da->get_next();
			Vector<String> subdirs;
			while (!item.is_empty()) {
				if (item != "." && item != ".." && !item.begins_with(".")) {
					String full_path = p_dir.path_join(item);
					if (da->current_is_dir()) {
						if (!is_skip_directory(item)) {
							subdirs.push_back(full_path);
						}
					} else {
						if (item.get_extension().to_lower() == "tres") {
							r_files.push_back(full_path);
						}
					}
				}
				item = da->get_next();
			}
			da->list_dir_end();
			subdirs.sort();
			for (const String &subdir : subdirs) {
				collect(subdir, r_files);
			}
		}
	};

	TresCollector::collect(scan_root, all_resources);

	// Collect all res:// references from project.godot, scenes, scripts, and resources.
	HashSet<String> references;

	{
		String project_content = FileAccess::get_file_as_string("res://project.godot");
		if (!project_content.is_empty()) {
			int search_pos = 0;
			while (true) {
				int pos = project_content.find("res://", search_pos);
				if (pos == -1) {
					break;
				}
				int end = pos + 6;
				while (end < project_content.length()) {
					char32_t ch = project_content[end];
					if (ch == '"' || ch == '\'' || ch == ' ' || ch == ')' || ch == '\n' || ch == '\r') {
						break;
					}
					end++;
				}
				String ref = project_content.substr(pos, end - pos);
				references.insert(ref);
				search_pos = end;
			}
		}
	}

	Vector<String> all_check_files;
	all_check_files.append_array(all_scripts);
	all_check_files.append_array(all_scenes);
	all_check_files.append_array(all_resources);

	for (int i = 0; i < all_check_files.size(); i++) {
		String content = FileAccess::get_file_as_string(all_check_files[i]);
		if (content.is_empty()) {
			continue;
		}

		int search_pos = 0;
		while (true) {
			int pos = content.find("res://", search_pos);
			if (pos == -1) {
				break;
			}
			int end = pos + 6;
			while (end < content.length()) {
				char32_t ch = content[end];
				if (ch == '"' || ch == '\'' || ch == ' ' || ch == ')' || ch == '\n' || ch == '\r') {
					break;
				}
				end++;
			}
			String ref = content.substr(pos, end - pos);
			references.insert(ref);
			search_pos = end;
		}
	}

	// Find unreferenced files.
	Array unreferenced_scripts;
	Array unreferenced_scenes;
	Array unreferenced_resources;

	HashSet<String> autoload_paths;
	{
		List<PropertyInfo> settings;
		ProjectSettings::get_singleton()->get_property_list(&settings);
		for (const PropertyInfo &pi : settings) {
			if (pi.name.begins_with("autoload/")) {
				String path = ProjectSettings::get_singleton()->get_setting(pi.name);
				if (path.begins_with("*")) {
					path = path.substr(1);
				}
				autoload_paths.insert(path);
			}
		}
	}

	for (int i = 0; i < all_scripts.size(); i++) {
		String path = all_scripts[i];
		if (autoload_paths.has(path)) {
			continue;
		}
		if (path.find("test_") >= 0 || path.find("/tests/") >= 0) {
			continue;
		}
		if (path.find("/addons/") >= 0) {
			continue;
		}
		if (!references.has(path)) {
			unreferenced_scripts.push_back(path);
		}
	}

	for (int i = 0; i < all_scenes.size(); i++) {
		String path = all_scenes[i];
		if (path.find("/addons/") >= 0) {
			continue;
		}
		if (!references.has(path)) {
			unreferenced_scenes.push_back(path);
		}
	}

	for (int i = 0; i < all_resources.size(); i++) {
		String path = all_resources[i];
		if (path.find("/addons/") >= 0) {
			continue;
		}
		if (!references.has(path)) {
			unreferenced_resources.push_back(path);
		}
	}

	int total_unused = unreferenced_scripts.size() + unreferenced_scenes.size() + unreferenced_resources.size();
	int total_files = all_scripts.size() + all_scenes.size() + all_resources.size();
	float waste_pct = total_files > 0 ? (float)total_unused / (float)total_files * 100.0f : 0.0f;

	String text = "=== UNUSED FILE DETECTION ===\n\n";
	text += vformat("Total files: %d (scripts: %d, scenes: %d, resources: %d)\n",
			total_files, all_scripts.size(), all_scenes.size(), all_resources.size());
	text += vformat("Unreferenced: %d (%.1f%%)\n\n", total_unused, waste_pct);

	if (unreferenced_scripts.size() > 0) {
		text += vformat("Unreferenced Scripts (%d):\n", unreferenced_scripts.size());
		for (int i = 0; i < unreferenced_scripts.size() && i < 20; i++) {
			text += vformat("  %s\n", (String)unreferenced_scripts[i]);
		}
		text += "\n";
	}

	if (unreferenced_scenes.size() > 0) {
		text += vformat("Unreferenced Scenes (%d):\n", unreferenced_scenes.size());
		for (int i = 0; i < unreferenced_scenes.size() && i < 10; i++) {
			text += vformat("  %s\n", (String)unreferenced_scenes[i]);
		}
		text += "\n";
	}

	if (unreferenced_resources.size() > 0) {
		text += vformat("Unreferenced Resources (%d):\n", unreferenced_resources.size());
		for (int i = 0; i < unreferenced_resources.size() && i < 10; i++) {
			text += vformat("  %s\n", (String)unreferenced_resources[i]);
		}
		text += "\n";
	}

	if (total_unused == 0) {
		text += "No unused files detected! Project is clean.\n";
	} else {
		text += "Note: Autoloads, test files, and addons are excluded. Verify before deleting.\n";
	}

	text += "\n=== END UNUSED FILE DETECTION ===";

	Dictionary summary;
	summary["total_files"] = total_files;
	summary["unreferenced_scripts"] = unreferenced_scripts.size();
	summary["unreferenced_scenes"] = unreferenced_scenes.size();
	summary["unreferenced_resources"] = unreferenced_resources.size();
	summary["waste_percentage"] = vformat("%.1f%%", waste_pct);

	String health = "CLEAN";
	if (total_unused >= 15) {
		health = "SIGNIFICANT CLEANUP NEEDED";
	} else if (total_unused >= 5) {
		health = "SOME CLEANUP NEEDED";
	}

	Dictionary structured;
	structured["summary"] = summary;
	structured["health"] = health;
	structured["unreferenced_scripts"] = unreferenced_scripts;
	structured["unreferenced_scenes"] = unreferenced_scenes;
	structured["unreferenced_resources"] = unreferenced_resources;
	structured["note"] = "Autoloads, test files, and addons are excluded. Verify before deleting.";

	return make_tool_result(text, structured);
}
