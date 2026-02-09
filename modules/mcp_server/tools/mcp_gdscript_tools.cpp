/**************************************************************************/
/*  mcp_gdscript_tools.cpp                                                */
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

#include "mcp_gdscript_tools.h"

#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/script_language.h"
#include "modules/gdscript/gdscript.h"

// ============================================================================
// Tool Registration
// ============================================================================

void MCPGDScriptTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ---- gdscript/check_errors ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Path to the GDScript file in res:// format (e.g., res://scripts/player.gd)");
		Array required;
		required.push_back("path");
		p_registry->register_tool(
				"gdscript/check_errors",
				"Check GDScript Errors",
				"Validate a single GDScript file for compile errors and warnings. Returns all "
				"errors and warnings with file path, line number, column, and message. Use this "
				"after modifying a .gd file to check for mistakes before running. Reads the file "
				"fresh from disk each time (not from editor cache). Path must use res:// format.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPGDScriptTools::handle_check_errors));
	}

	// ---- gdscript/check_all ----
	{
		Dictionary props;
		props["include_warnings"] = make_prop("boolean",
				"Whether to include warnings in the output (default: false, only errors)");
		Array required;
		p_registry->register_tool(
				"gdscript/check_all",
				"Check All GDScript Files",
				"Validate every .gd file in the project for compile errors. Returns a per-file "
				"summary showing which files have errors, which have warnings, and which are "
				"clean. Useful for a project-wide health check after large changes. Can be slow "
				"on projects with many scripts (>100 files). Optionally includes warnings.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPGDScriptTools::handle_check_all));
	}
}

// ============================================================================
// Tool D1: gdscript/check_errors
// ============================================================================

Dictionary MCPGDScriptTools::handle_check_errors(const Dictionary &p_args) {
	String path = p_args.get("path", "");

	if (path.is_empty()) {
		return make_tool_error("Missing required parameter: path");
	}
	if (!validate_path(path)) {
		return make_tool_error(vformat(
				"Invalid path: %s\n\n"
				"Paths must start with res:// and cannot contain '..' traversal sequences.",
				path));
	}

	// Must be a .gd file.
	if (path.get_extension().to_lower() != "gd") {
		return make_tool_error(vformat(
				"Not a GDScript file: %s\n\n"
				"gdscript/check_errors only validates .gd files.",
				path));
	}

	// Check file exists.
	if (!FileAccess::exists(path)) {
		return make_tool_error(vformat("File not found: %s", path));
	}

	Dictionary file_result = _validate_single_file(path);

	// Build text output.
	bool valid = file_result["valid"];
	Array errors = file_result["errors"];
	Array warnings = file_result["warnings"];

	String text;
	if (valid && errors.size() == 0 && warnings.size() == 0) {
		text = vformat("%s: No errors or warnings.", path);
	} else {
		text = vformat("Found %d errors and %d warnings in %s:\n",
				errors.size(), warnings.size(), path);

		if (errors.size() > 0) {
			text += "\nErrors:";
			for (int i = 0; i < errors.size(); i++) {
				Dictionary e = errors[i];
				text += vformat("\n  Line %d, Col %d: %s",
						(int)e["line"], (int)e["column"], (String)e["message"]);
			}
		}

		if (warnings.size() > 0) {
			text += "\n\nWarnings:";
			for (int i = 0; i < warnings.size(); i++) {
				Dictionary w = warnings[i];
				text += vformat("\n  Line %d: %s [%s]",
						(int)w["line"], (String)w["message"], (String)w["code_name"]);
			}
		}
	}

	return make_tool_result(text, file_result);
}

// ============================================================================
// Tool D2: gdscript/check_all
// ============================================================================

Dictionary MCPGDScriptTools::handle_check_all(const Dictionary &p_args) {
	bool include_warnings = p_args.get("include_warnings", false);

	// Find all .gd files in the project.
	Vector<String> gd_files;
	_find_gd_files_recursive("res://", gd_files);

	if (gd_files.size() == 0) {
		Dictionary structured;
		structured["total_files"] = 0;
		structured["files_with_errors"] = 0;
		structured["files_with_warnings"] = 0;
		structured["files_clean"] = 0;
		structured["files"] = Array();
		return make_tool_result("No .gd files found in project.", structured);
	}

	// Sort for deterministic output.
	gd_files.sort();

	// Validate each file.
	Array file_results;
	int files_with_errors = 0;
	int files_with_warnings = 0;
	int files_clean = 0;
	String text;

	for (int i = 0; i < gd_files.size(); i++) {
		Dictionary result = _validate_single_file(gd_files[i]);
		Array errors = result["errors"];
		Array warnings = result["warnings"];

		if (errors.size() > 0) {
			files_with_errors++;
			text += vformat("=== %s (%d errors) ===\n", gd_files[i], errors.size());
			for (int j = 0; j < errors.size(); j++) {
				Dictionary e = errors[j];
				text += vformat("  Line %d, Col %d: %s\n",
						(int)e["line"], (int)e["column"], (String)e["message"]);
			}
		} else if (warnings.size() > 0 && include_warnings) {
			files_with_warnings++;
			text += vformat("=== %s (%d warnings) ===\n", gd_files[i], warnings.size());
			for (int j = 0; j < warnings.size(); j++) {
				Dictionary w = warnings[j];
				text += vformat("  Line %d: %s [%s]\n",
						(int)w["line"], (String)w["message"], (String)w["code_name"]);
			}
		} else if (warnings.size() > 0) {
			files_with_warnings++;
			// Don't print details when include_warnings is false.
		} else {
			files_clean++;
		}

		file_results.push_back(result);
	}

	text += vformat("\nSummary: %d files checked, %d with errors, %d with warnings, %d clean",
			gd_files.size(), files_with_errors, files_with_warnings, files_clean);

	Dictionary structured;
	structured["total_files"] = gd_files.size();
	structured["files_with_errors"] = files_with_errors;
	structured["files_with_warnings"] = files_with_warnings;
	structured["files_clean"] = files_clean;
	structured["files"] = file_results;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool D2b: gdscript/check_all (progress-aware variant)
// ============================================================================

#include "../mcp_progress.h"

Dictionary MCPGDScriptTools::handle_check_all_with_progress(
		const Dictionary &p_args, ProgressContext *p_ctx) {
	bool include_warnings = p_args.get("include_warnings", false);

	// Find all .gd files in the project.
	Vector<String> gd_files;
	_find_gd_files_recursive("res://", gd_files);

	if (gd_files.size() == 0) {
		Dictionary structured;
		structured["total_files"] = 0;
		structured["files_with_errors"] = 0;
		structured["files_with_warnings"] = 0;
		structured["files_clean"] = 0;
		structured["files"] = Array();
		return make_tool_result("No .gd files found in project.", structured);
	}

	// Sort for deterministic output.
	gd_files.sort();

	int total = gd_files.size();
	Array file_results;
	int files_with_errors = 0;
	int files_with_warnings = 0;
	int files_clean = 0;
	String text;

	for (int i = 0; i < total; i++) {
		// Cooperative cancellation check.
		if (p_ctx && p_ctx->is_cancelled()) {
			break;
		}

		// Report progress.
		if (p_ctx) {
			p_ctx->report_progress(i, total, "Checking " + gd_files[i]);
		}

		Dictionary result = _validate_single_file(gd_files[i]);
		Array errors = result["errors"];
		Array warnings = result["warnings"];

		if (errors.size() > 0) {
			files_with_errors++;
			text += vformat("=== %s (%d errors) ===\n", gd_files[i], errors.size());
			for (int j = 0; j < errors.size(); j++) {
				Dictionary e = errors[j];
				text += vformat("  Line %d, Col %d: %s\n",
						(int)e["line"], (int)e["column"], (String)e["message"]);
			}
		} else if (warnings.size() > 0 && include_warnings) {
			files_with_warnings++;
			text += vformat("=== %s (%d warnings) ===\n", gd_files[i], warnings.size());
			for (int j = 0; j < warnings.size(); j++) {
				Dictionary w = warnings[j];
				text += vformat("  Line %d: %s [%s]\n",
						(int)w["line"], (String)w["message"], (String)w["code_name"]);
			}
		} else if (warnings.size() > 0) {
			files_with_warnings++;
		} else {
			files_clean++;
		}

		file_results.push_back(result);
	}

	// Final progress.
	if (p_ctx) {
		p_ctx->report_progress(total, total, "Done");
	}

	bool was_cancelled = (p_ctx && p_ctx->is_cancelled());
	int files_checked = files_with_errors + files_with_warnings + files_clean;

	text += vformat("\nSummary: %d of %d files checked, %d with errors, %d with warnings, %d clean",
			files_checked, total, files_with_errors, files_with_warnings, files_clean);
	if (was_cancelled) {
		text += " (Cancelled by client)";
	}

	Dictionary structured;
	structured["total_files"] = total;
	structured["files_checked"] = files_checked;
	structured["files_with_errors"] = files_with_errors;
	structured["files_with_warnings"] = files_with_warnings;
	structured["files_clean"] = files_clean;
	structured["cancelled"] = was_cancelled;
	structured["files"] = file_results;

	return make_tool_result(text, structured);
}

// ============================================================================
// Internal: Validate a single .gd file
// ============================================================================

Dictionary MCPGDScriptTools::_validate_single_file(const String &p_path) {
	Dictionary result;
	result["path"] = p_path;
	result["valid"] = true;
	result["errors"] = Array();
	result["warnings"] = Array();
	result["error_count"] = 0;
	result["warning_count"] = 0;

	// Read the file from disk (fresh read, not cached).
	String source = FileAccess::get_file_as_string(p_path);
	if (FileAccess::get_open_error() != OK) {
		result["valid"] = false;
		Array errors;
		Dictionary err;
		err["line"] = 0;
		err["column"] = 0;
		err["message"] = vformat("Failed to read file: %s", p_path);
		errors.push_back(err);
		result["errors"] = errors;
		result["error_count"] = 1;
		return result;
	}

	// Use GDScriptLanguage::validate() -- this is thread-safe as it creates
	// local parser/analyzer instances.
	GDScriptLanguage *gdscript = GDScriptLanguage::get_singleton();
	if (!gdscript) {
		result["valid"] = false;
		Array errors;
		Dictionary err;
		err["line"] = 0;
		err["column"] = 0;
		err["message"] = "GDScriptLanguage singleton not available.";
		errors.push_back(err);
		result["errors"] = errors;
		result["error_count"] = 1;
		return result;
	}

	List<String> functions;
	List<ScriptLanguage::ScriptError> script_errors;
	List<ScriptLanguage::Warning> script_warnings;
	HashSet<int> safe_lines;

	bool is_valid = gdscript->validate(
			source,
			p_path,
			&functions,
			&script_errors,
			&script_warnings,
			&safe_lines);

	result["valid"] = is_valid;

	// Convert errors to Dictionary array.
	Array errors;
	for (const ScriptLanguage::ScriptError &se : script_errors) {
		Dictionary e;
		e["line"] = se.line;
		e["column"] = se.column;
		e["message"] = se.message;
		errors.push_back(e);
	}
	result["errors"] = errors;
	result["error_count"] = errors.size();

	// Convert warnings to Dictionary array.
	// ScriptLanguage::Warning has: start_line, end_line, code, string_code, message.
	Array warnings;
	for (const ScriptLanguage::Warning &sw : script_warnings) {
		Dictionary w;
		w["line"] = sw.start_line;
		w["end_line"] = sw.end_line;
		w["message"] = sw.message;
		w["code"] = sw.code;
		w["code_name"] = sw.string_code;
		warnings.push_back(w);
	}
	result["warnings"] = warnings;
	result["warning_count"] = warnings.size();

	return result;
}

// ============================================================================
// Internal: Find all .gd files recursively
// ============================================================================

void MCPGDScriptTools::_find_gd_files_recursive(const String &p_dir,
		Vector<String> &r_files) {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return;
	}

	da->list_dir_begin();
	String item = da->get_next();
	while (!item.is_empty()) {
		if (da->current_is_dir()) {
			if (!is_skip_directory(item) && !item.begins_with(".")) {
				_find_gd_files_recursive(p_dir + item + "/", r_files);
			}
		} else {
			if (item.get_extension().to_lower() == "gd") {
				r_files.push_back(p_dir + item);
			}
		}
		item = da->get_next();
	}
	da->list_dir_end();
}
