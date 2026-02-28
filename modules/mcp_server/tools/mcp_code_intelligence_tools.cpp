/**************************************************************************/
/*  mcp_code_intelligence_tools.cpp                                       */
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

#include "mcp_code_intelligence_tools.h"

#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/script_language.h"

// GDScript language server: ExtendGDScriptParser for parsing and API generation.
#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/language_server/gdscript_extend_parser.h"

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void MCPCodeIntelligenceTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ---- analysis/describe_script ----
	{
		Dictionary props;
		props["path"] = make_prop("string", "res:// path to a .gd file");
		props["include_docs"] = make_prop("boolean", "Include ## doc comments (default: false)");
		Array required;
		required.push_back("path");
		p_registry->register_tool(
				"analysis/describe_script",
				"Describe Script API",
				"Get the typed API of a GDScript file: class_name, extends, all properties "
				"with types, signals with parameters, methods with full signatures. "
				"This is the user-script equivalent of doc/get_class. Use BEFORE modifying "
				"a script to understand its current interface, or when navigating unfamiliar code.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPCodeIntelligenceTools::handle_describe_script));
	}

	// ---- analysis/goto_definition ----
	{
		Dictionary props;
		props["path"] = make_prop("string", "res:// path to the file containing the symbol");
		props["line"] = make_prop("integer", "1-based line number");
		props["column"] = make_prop("integer", "1-based column number");
		props["symbol"] = make_prop("string", "Symbol text (optional, auto-detected from position)");
		Array required;
		required.push_back("path");
		required.push_back("line");
		required.push_back("column");
		p_registry->register_tool(
				"analysis/goto_definition",
				"Go To Definition",
				"Resolve a symbol at a specific position in a GDScript file to its definition. "
				"Returns the file path, line number, and type of the definition (class, method, "
				"property, signal, constant, or script location). Use when reading code to "
				"understand where a called function, referenced variable, or extended class is defined.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPCodeIntelligenceTools::handle_goto_definition));
	}

	// ---- analysis/find_references ----
	{
		Dictionary props;
		props["path"] = make_prop("string", "res:// path to the file containing the symbol definition");
		props["symbol"] = make_prop("string", "Name of the symbol to search for");
		props["line"] = make_prop("integer", "1-based line number of the definition (helps verify the correct symbol)");
		props["scope"] = make_prop("string", "Search scope: 'project' (default) or 'file' (current file only)");
		Array required;
		required.push_back("symbol");
		p_registry->register_tool(
				"analysis/find_references",
				"Find All References",
				"Find every reference to a symbol (function, variable, signal, class_name) "
				"across the project. Uses whole-word matching with comment/string filtering. "
				"Use before renaming or deleting a symbol to understand its usage footprint.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPCodeIntelligenceTools::handle_find_references));
	}

	// ---- analysis/rename_symbol ----
	{
		Dictionary props;
		props["path"] = make_prop("string", "res:// path to the file containing the symbol");
		props["symbol"] = make_prop("string", "Current name of the symbol");
		props["new_name"] = make_prop("string", "New name for the symbol");
		props["apply"] = make_prop("boolean", "Execute the rename (default: false = dry-run preview)");
		Array required;
		required.push_back("symbol");
		required.push_back("new_name");
		p_registry->register_tool(
				"analysis/rename_symbol",
				"Rename Symbol",
				"Rename a symbol (function, variable, signal) across the entire project. "
				"By default shows a dry-run preview of all changes. Pass apply: true to execute. "
				"Uses whole-word matching with comment/string filtering. Only modifies .gd files. "
				"Run testing/check_all_scripts after applying to verify zero errors.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPCodeIntelligenceTools::handle_rename_symbol));
	}
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool MCPCodeIntelligenceTools::_is_ident_char(char32_t c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_';
}

void MCPCodeIntelligenceTools::_find_gd_files(const String &p_dir, Vector<String> &r_files) {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return;
	}
	da->list_dir_begin();
	String item = da->get_next();
	Vector<String> subdirs;
	while (!item.is_empty()) {
		if (item != "." && item != "..") {
			String full_path = p_dir.path_join(item);
			if (da->current_is_dir()) {
				if (!is_skip_directory(item) && !item.begins_with(".")) {
					subdirs.push_back(full_path);
				}
			} else if (item.get_extension().to_lower() == "gd") {
				r_files.push_back(full_path);
			}
		}
		item = da->get_next();
	}
	da->list_dir_end();
	for (const String &subdir : subdirs) {
		_find_gd_files(subdir, r_files);
	}
}

Vector<MCPCodeIntelligenceTools::Reference> MCPCodeIntelligenceTools::_find_text_references(
		const String &p_symbol, const String &p_scope_file, bool p_local_only) {
	Vector<Reference> refs;
	Vector<String> files;

	if (p_local_only && !p_scope_file.is_empty()) {
		files.push_back(p_scope_file);
	} else {
		_find_gd_files("res://", files);
	}

	for (const String &file_path : files) {
		String source = FileAccess::get_file_as_string(file_path);
		if (source.is_empty()) {
			continue;
		}

		Vector<String> lines = source.split("\n");
		for (int i = 0; i < lines.size(); i++) {
			const String &line = lines[i];
			int col = line.find(p_symbol);
			while (col >= 0) {
				// Whole-word boundary check.
				bool left_ok = (col == 0) || !_is_ident_char(line[col - 1]);
				int end_col = col + p_symbol.length();
				bool right_ok = (end_col >= line.length()) || !_is_ident_char(line[end_col]);

				if (left_ok && right_ok) {
					// Simple comment/string filter: scan up to the match position.
					bool skip = false;
					bool in_string = false;
					char32_t str_char = 0;
					for (int c = 0; c < col; c++) {
						char32_t ch = line[c];
						if (ch == '#' && !in_string) {
							skip = true; // Rest of line is a comment.
							break;
						}
						if (!in_string && (ch == '"' || ch == '\'')) {
							in_string = true;
							str_char = ch;
						} else if (in_string && ch == str_char && (c == 0 || line[c - 1] != '\\')) {
							in_string = false;
						}
					}
					if (in_string) {
						skip = true; // Match is inside a string.
					}

					if (!skip) {
						Reference ref;
						ref.file_path = file_path;
						ref.line = i;
						ref.column = col;
						ref.context_line = line.strip_edges();
						refs.push_back(ref);
					}
				}
				col = line.find(p_symbol, col + 1);
			}
		}
	}
	return refs;
}

String MCPCodeIntelligenceTools::_format_api(const Dictionary &p_api, bool p_include_docs) {
	String out;

	// Header: class_name + extends.
	String name = p_api.get("name", "");
	Array extends_arr = p_api.get("extends_class", Array());
	String extends_file = p_api.get("extends_file", "");

	if (!name.is_empty()) {
		out += "class_name " + name;
	}
	if (extends_arr.size() > 0) {
		if (!name.is_empty()) {
			out += " ";
		}
		out += "extends " + String(extends_arr[0]);
	} else if (!extends_file.is_empty()) {
		if (!name.is_empty()) {
			out += " ";
		}
		out += "extends \"" + extends_file + "\"";
	}
	if (!out.is_empty()) {
		out += "\n";
	}

	if (p_include_docs) {
		String desc = p_api.get("description", "");
		if (!desc.is_empty()) {
			out += "## " + desc.strip_edges() + "\n";
		}
	}

	// Properties.
	Array members = p_api.get("members", Array());
	if (members.size() > 0) {
		out += "\n";
		for (int i = 0; i < members.size(); i++) {
			Dictionary m = members[i];
			bool exported = m.get("export", false);
			String sig = m.get("signature", "");
			if (exported) {
				out += "@export ";
			}
			if (!sig.is_empty()) {
				out += sig;
			} else {
				out += "var " + String(m["name"]) + ": " + String(m.get("data_type", "Variant"));
			}
			out += "\n";
			if (p_include_docs) {
				String desc = m.get("description", "");
				if (!desc.is_empty()) {
					out += "  ## " + desc.strip_edges() + "\n";
				}
			}
		}
	}

	// Signals.
	Array sigs = p_api.get("signals", Array());
	if (sigs.size() > 0) {
		out += "\n";
		for (int i = 0; i < sigs.size(); i++) {
			Dictionary s = sigs[i];
			String sig = s.get("signature", "");
			if (!sig.is_empty()) {
				out += sig;
			} else {
				out += "signal " + String(s["name"]);
				Array args = s.get("arguments", Array());
				if (args.size() > 0) {
					out += "(";
					for (int j = 0; j < args.size(); j++) {
						if (j > 0) {
							out += ", ";
						}
						out += String(args[j]);
					}
					out += ")";
				}
			}
			out += "\n";
			if (p_include_docs) {
				String desc = s.get("description", "");
				if (!desc.is_empty()) {
					out += "  ## " + desc.strip_edges() + "\n";
				}
			}
		}
	}

	// Constants and enums.
	Array constants = p_api.get("constants", Array());
	if (constants.size() > 0) {
		out += "\n";
		for (int i = 0; i < constants.size(); i++) {
			Dictionary c = constants[i];
			String sig = c.get("signature", "");
			if (!sig.is_empty()) {
				out += sig;
			} else {
				out += "const " + String(c["name"]);
				String dt = c.get("data_type", "");
				if (!dt.is_empty()) {
					out += ": " + dt;
				}
				out += " = " + String(c.get("value", ""));
			}
			out += "\n";
		}
	}

	// Methods.
	Array methods = p_api.get("methods", Array());
	if (methods.size() > 0) {
		out += "\n";
		for (int i = 0; i < methods.size(); i++) {
			Dictionary m = methods[i];
			String sig = m.get("signature", "");
			if (!sig.is_empty()) {
				out += sig;
			} else {
				out += "func " + String(m["name"]) + "(";
				Array args = m.get("arguments", Array());
				for (int j = 0; j < args.size(); j++) {
					Dictionary arg = args[j];
					if (j > 0) {
						out += ", ";
					}
					out += String(arg["name"]);
					String t = arg.get("type", "");
					if (!t.is_empty() && t != "Variant") {
						out += ": " + t;
					}
					if (arg.has("default_value")) {
						out += " = " + String(arg["default_value"]);
					}
				}
				out += ")";
				String ret = m.get("return_type", "");
				if (!ret.is_empty() && ret != "void" && ret != "Variant") {
					out += " -> " + ret;
				}
			}
			out += "\n";
			if (p_include_docs) {
				String desc = m.get("description", "");
				if (!desc.is_empty()) {
					out += "  ## " + desc.strip_edges() + "\n";
				}
			}
		}
	}

	// Static functions.
	Array statics = p_api.get("static_functions", Array());
	if (statics.size() > 0) {
		out += "\n";
		for (int i = 0; i < statics.size(); i++) {
			Dictionary m = statics[i];
			String sig = m.get("signature", "");
			if (!sig.is_empty()) {
				out += "static " + sig;
			} else {
				out += "static func " + String(m["name"]) + "(...)";
			}
			out += "\n";
		}
	}

	// Inner classes (recursive).
	Array sub_classes = p_api.get("sub_classes", Array());
	for (int i = 0; i < sub_classes.size(); i++) {
		out += "\n# Inner class:\n";
		out += _format_api(sub_classes[i], p_include_docs);
	}

	return out;
}

// ---------------------------------------------------------------------------
// Tool Handlers
// ---------------------------------------------------------------------------

Dictionary MCPCodeIntelligenceTools::handle_describe_script(const Dictionary &p_args) {
	String path = p_args.get("path", "");
	if (path.is_empty()) {
		return make_tool_error("Missing required parameter: path");
	}
	if (!path.ends_with(".gd")) {
		return make_tool_error("Path must be a .gd file: " + path);
	}

	String source = FileAccess::get_file_as_string(path);
	if (source.is_empty() && !FileAccess::exists(path)) {
		return make_tool_error("File not found: " + path);
	}

	bool include_docs = p_args.get("include_docs", false);

	// Parse with ExtendGDScriptParser to get the full symbol tree + API.
	ExtendGDScriptParser parser;
	parser.parse(source, path);

	Dictionary api = parser.generate_api();
	if (api.is_empty()) {
		// Parsing failed badly. Return what we can.
		String out = "# " + path + "\n";
		if (parser.parse_result != OK) {
			out += "Parse error: could not fully analyze this script.\n";
			const Vector<LSP::Diagnostic> &diags = parser.get_diagnostics();
			for (int i = 0; i < diags.size() && i < 5; i++) {
				out += vformat("  Line %d: %s\n",
						diags[i].range.start.line + 1, diags[i].message);
			}
		}
		return make_tool_result(out);
	}

	String out = "# " + path + "\n";
	out += _format_api(api, include_docs);

	return make_tool_result(out, api);
}

Dictionary MCPCodeIntelligenceTools::handle_goto_definition(const Dictionary &p_args) {
	String path = p_args.get("path", "");
	int line = p_args.get("line", 0);
	int column = p_args.get("column", 0);
	String symbol_hint = p_args.get("symbol", "");

	if (path.is_empty()) {
		return make_tool_error("Missing required parameter: path");
	}
	if (line <= 0 || column <= 0) {
		return make_tool_error("line and column must be >= 1 (1-based)");
	}

	String source = FileAccess::get_file_as_string(path);
	if (source.is_empty() && !FileAccess::exists(path)) {
		return make_tool_error("File not found: " + path);
	}

	// Parse the file to use its helper methods.
	ExtendGDScriptParser parser;
	parser.parse(source, path);

	// Convert 1-based user coords to 0-based LSP coords.
	LSP::Position lsp_pos;
	lsp_pos.line = line - 1;
	lsp_pos.character = column - 1;

	// Extract the identifier under the cursor if not provided.
	String symbol_name = symbol_hint;
	if (symbol_name.is_empty()) {
		LSP::Range ident_range;
		symbol_name = parser.get_identifier_under_position(lsp_pos, ident_range);
		if (!symbol_name.is_empty()) {
			// Place cursor at end of identifier for lookup.
			lsp_pos.character = ident_range.end.character;
		}
	}

	if (symbol_name.is_empty()) {
		return make_tool_error(vformat("No identifier found at %s:%d:%d", path, line, column));
	}

	// Build cursor-marked code and call lookup_code.
	String marked_code = parser.get_text_for_lookup_symbol(lsp_pos, symbol_name);

	GDScriptLanguage *lang = GDScriptLanguage::get_singleton();
	if (!lang) {
		return make_tool_error("GDScript language not available");
	}

	ScriptLanguage::LookupResult result;
	Error err = lang->lookup_code(marked_code, symbol_name, path, nullptr, result);
	if (err != OK) {
		// Fallback: check if it's a global class.
		if (ScriptServer::is_global_class(symbol_name)) {
			String class_path = ScriptServer::get_global_class_path(symbol_name);
			String out = vformat("Symbol: %s\nType: Global class\nLocation: %s\n", symbol_name, class_path);
			Dictionary structured;
			structured["symbol"] = symbol_name;
			structured["type"] = "global_class";
			structured["script_path"] = class_path;
			return make_tool_result(out, structured);
		}
		return make_tool_error(vformat("Could not resolve symbol '%s' at %s:%d:%d", symbol_name, path, line, column));
	}

	// Format the LookupResult.
	Dictionary structured;
	structured["symbol"] = symbol_name;
	String out;

	static const char *type_names[] = {
		"Script location", "Class", "Class constant", "Class property",
		"Class method", "Class signal", "Class enum", "Global scope (deprecated)",
		"Class annotation", "Local constant", "Local variable"
	};

	int type_idx = (int)result.type;
	String type_str = (type_idx >= 0 && type_idx <= 10) ? type_names[type_idx] : "Unknown";
	structured["type"] = type_str;

	out += "Symbol: " + symbol_name + "\n";
	out += "Type: " + type_str + "\n";

	if (!result.class_name.is_empty()) {
		out += "Class: " + result.class_name + "\n";
		structured["class_name"] = result.class_name;
	}
	if (!result.class_member.is_empty()) {
		out += "Member: " + result.class_member + "\n";
		structured["class_member"] = result.class_member;
	}

	String target_path = path;
	if (result.script.is_valid()) {
		target_path = result.script->get_path();
	} else if (!result.script_path.is_empty()) {
		target_path = result.script_path;
	}

	if (result.location >= 0) {
		out += vformat("Location: %s:%d\n", target_path, result.location);
		structured["script_path"] = target_path;
		structured["location"] = result.location;
	} else if (!result.class_name.is_empty()) {
		out += "Defined in: native engine class " + result.class_name + "\n";
		out += "Use doc/get_class class_name=\"" + result.class_name + "\" for full documentation.\n";
	}

	return make_tool_result(out, structured);
}

Dictionary MCPCodeIntelligenceTools::handle_find_references(const Dictionary &p_args) {
	String symbol = p_args.get("symbol", "");
	if (symbol.is_empty()) {
		return make_tool_error("Missing required parameter: symbol");
	}

	String path = p_args.get("path", "");
	int def_line = p_args.get("line", 0);
	String scope = p_args.get("scope", "project");
	bool local_only = (scope == "file");

	// If path is given and line is given, verify the symbol exists at that line.
	if (!path.is_empty() && def_line > 0) {
		String source = FileAccess::get_file_as_string(path);
		if (!source.is_empty()) {
			Vector<String> lines = source.split("\n");
			int idx = def_line - 1;
			if (idx >= 0 && idx < lines.size()) {
				if (lines[idx].find(symbol) < 0) {
					return make_tool_error(vformat(
							"Symbol '%s' not found on line %d of %s", symbol, def_line, path));
				}
			}
		}
	}

	Vector<Reference> refs = _find_text_references(symbol, path, local_only);

	// Cap output for very common symbols.
	const int MAX_REFS = 200;
	int total = refs.size();

	String out;
	if (total == 0) {
		out = vformat("No references to '%s' found.\n", symbol);
	} else {
		out = vformat("Found %d reference%s to '%s':\n\n",
				total, total == 1 ? "" : "s", symbol);
		int show = MIN(total, MAX_REFS);
		for (int i = 0; i < show; i++) {
			const Reference &ref = refs[i];
			out += vformat("%s:%d  %s\n", ref.file_path, ref.line + 1, ref.context_line);
		}
		if (total > MAX_REFS) {
			out += vformat("\n... and %d more (showing first %d)\n", total - MAX_REFS, MAX_REFS);
		}
	}

	// Structured output.
	Dictionary structured;
	structured["symbol"] = symbol;
	structured["total"] = total;
	Array ref_arr;
	int show = MIN(total, MAX_REFS);
	for (int i = 0; i < show; i++) {
		Dictionary r;
		r["file"] = refs[i].file_path;
		r["line"] = refs[i].line + 1;
		r["column"] = refs[i].column + 1;
		r["context"] = refs[i].context_line;
		ref_arr.push_back(r);
	}
	structured["references"] = ref_arr;

	return make_tool_result(out, structured);
}

// Sort references in reverse order (highest line first, then highest column)
// so that bottom-up replacement preserves positions of earlier references.
struct _ReferenceReverseSorter {
	_FORCE_INLINE_ bool operator()(const MCPCodeIntelligenceTools::Reference &a,
			const MCPCodeIntelligenceTools::Reference &b) const {
		if (a.line != b.line) {
			return a.line > b.line;
		}
		return a.column > b.column;
	}
};

Dictionary MCPCodeIntelligenceTools::handle_rename_symbol(const Dictionary &p_args) {
	String symbol = p_args.get("symbol", "");
	String new_name = p_args.get("new_name", "");
	bool apply = p_args.get("apply", false);
	String path = p_args.get("path", "");

	if (symbol.is_empty()) {
		return make_tool_error("Missing required parameter: symbol");
	}
	if (new_name.is_empty()) {
		return make_tool_error("Missing required parameter: new_name");
	}
	if (symbol == new_name) {
		return make_tool_error("new_name must be different from symbol");
	}

	// Validate the new name is a legal identifier.
	if (new_name.length() == 0 || (!_is_ident_char(new_name[0]) || (new_name[0] >= '0' && new_name[0] <= '9'))) {
		return make_tool_error("Invalid identifier: " + new_name);
	}
	for (int i = 1; i < new_name.length(); i++) {
		if (!_is_ident_char(new_name[i])) {
			return make_tool_error("Invalid identifier: " + new_name);
		}
	}

	// Find all references.
	Vector<Reference> refs = _find_text_references(symbol, path, false);
	if (refs.is_empty()) {
		return make_tool_error(vformat("No references to '%s' found.", symbol));
	}

	// Group by file.
	HashMap<String, Vector<Reference>> by_file;
	for (const Reference &ref : refs) {
		by_file[ref.file_path].push_back(ref);
	}

	if (!apply) {
		// Dry-run: show preview.
		String out = vformat("Rename '%s' -> '%s' (DRY RUN)\n", symbol, new_name);
		out += vformat("%d occurrence%s in %d file%s:\n\n",
				refs.size(), refs.size() == 1 ? "" : "s",
				(int)by_file.size(), by_file.size() == 1 ? "" : "s");

		for (const KeyValue<String, Vector<Reference>> &kv : by_file) {
			out += kv.key + ":\n";
			for (const Reference &ref : kv.value) {
				String before = ref.context_line;
				String after = ref.context_line.replace(symbol, new_name);
				out += vformat("  L%d: %s\n", ref.line + 1, before);
				out += vformat("    -> %s\n", after);
			}
			out += "\n";
		}

		out += "Pass apply: true to execute this rename.\n";

		Dictionary structured;
		structured["dry_run"] = true;
		structured["symbol"] = symbol;
		structured["new_name"] = new_name;
		structured["total_occurrences"] = refs.size();
		structured["files_affected"] = (int)by_file.size();
		return make_tool_result(out, structured);
	}

	// Apply mode: modify files.
	int files_changed = 0;
	int occurrences_changed = 0;
	Vector<String> errors;

	for (const KeyValue<String, Vector<Reference>> &kv : by_file) {
		String source = FileAccess::get_file_as_string(kv.key);
		if (source.is_empty()) {
			errors.push_back("Could not read: " + kv.key);
			continue;
		}

		Vector<String> lines = source.split("\n");

		// Sort references by line (desc) then column (desc) to apply bottom-up.
		Vector<Reference> sorted = kv.value;
		sorted.sort_custom<_ReferenceReverseSorter>();

		for (const Reference &ref : sorted) {
			if (ref.line >= 0 && ref.line < lines.size()) {
				String &line = lines.write[ref.line];
				// Verify the symbol is still at the expected position.
				if (ref.column + symbol.length() <= line.length() &&
						line.substr(ref.column, symbol.length()) == symbol) {
					line = line.substr(0, ref.column) + new_name +
							line.substr(ref.column + symbol.length());
					occurrences_changed++;
				}
			}
		}

		// Write back.
		String new_source = String("\n").join(lines);
		Ref<FileAccess> fa = FileAccess::open(kv.key, FileAccess::WRITE);
		if (fa.is_valid()) {
			fa->store_string(new_source);
			fa->close();
			files_changed++;
		} else {
			errors.push_back("Could not write: " + kv.key);
		}
	}

	String out = vformat("Renamed '%s' -> '%s'\n", symbol, new_name);
	out += vformat("%d occurrence%s changed in %d file%s.\n",
			occurrences_changed, occurrences_changed == 1 ? "" : "s",
			files_changed, files_changed == 1 ? "" : "s");

	if (!errors.is_empty()) {
		out += "\nErrors:\n";
		for (const String &e : errors) {
			out += "  " + e + "\n";
		}
	}

	out += "\nRun testing/check_all_scripts to verify the rename introduced no errors.\n";

	Dictionary structured;
	structured["dry_run"] = false;
	structured["symbol"] = symbol;
	structured["new_name"] = new_name;
	structured["occurrences_changed"] = occurrences_changed;
	structured["files_changed"] = files_changed;
	return make_tool_result(out, structured);
}

