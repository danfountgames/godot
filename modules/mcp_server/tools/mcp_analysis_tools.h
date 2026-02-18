/**************************************************************************/
/*  mcp_analysis_tools.h                                                  */
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
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

class MCPToolRegistry;

class MCPAnalysisTools {
public:
	// -----------------------------------------------------------------------
	// Project Index data structures
	// -----------------------------------------------------------------------

	struct ScriptSymbol {
		String name;
		String type; // "function", "variable", "signal"
		String file; // res:// path
		int line = 0;
		bool is_exported = false;
		bool is_static = false;
		String return_type;
		Vector<String> parameters;
	};

	struct ScriptFile {
		String path;
		String class_name;
		String extends;
		bool is_tool = false;
		bool is_autoload = false;
		Vector<ScriptSymbol> functions;
		Vector<ScriptSymbol> variables;
		Vector<ScriptSymbol> signals;
		Vector<String> signal_connections; // connect("sig", ...) calls
		Vector<String> signal_emissions; // sig.emit() / emit_signal("sig") calls
		Vector<String> references; // Identifiers used in code
		Vector<String> dynamic_call_refs; // call("method"), call_deferred("method"), etc.
		int total_lines = 0;
		int typed_function_count = 0; // Functions with -> return type annotation.
		HashMap<String, int> function_complexities;
		HashMap<String, bool> function_has_doc_comment; // func_name -> has ## comment above.
	};

	// Signal connection from .tscn [connection] blocks.
	struct TscnConnection {
		String signal_name;
		String method_name;
		String scene_path;
	};

	struct ProjectIndex {
		HashMap<String, ScriptFile> files; // path -> ScriptFile
		HashMap<String, Vector<String>> symbol_definitions; // name -> [defining files]
		HashMap<String, Vector<String>> symbol_references; // name -> [referencing files]
		Vector<TscnConnection> tscn_connections; // Signal connections from .tscn scenes.
		bool is_built = false;
		uint64_t build_time_msec = 0;
	};

	// -----------------------------------------------------------------------
	// Registration
	// -----------------------------------------------------------------------

	static void register_tools(MCPToolRegistry *p_registry);

	// -----------------------------------------------------------------------
	// Tool handlers
	// -----------------------------------------------------------------------

	static Dictionary handle_dead_code(const Dictionary &p_args);
	static Dictionary handle_signal_flow(const Dictionary &p_args);
	static Dictionary handle_complexity(const Dictionary &p_args);
	static Dictionary handle_dependencies(const Dictionary &p_args);
	static Dictionary handle_project_health(const Dictionary &p_args);
	static Dictionary handle_duplication(const Dictionary &p_args);
	static Dictionary handle_validate_scenes(const Dictionary &p_args);
	static Dictionary handle_asset_analysis(const Dictionary &p_args);
	static Dictionary handle_input_mappings(const Dictionary &p_args);
	static Dictionary handle_unused_files(const Dictionary &p_args);

private:
	// -----------------------------------------------------------------------
	// Index building
	// -----------------------------------------------------------------------

	static ProjectIndex s_index;

	static void _build_index(const String &p_root);
	static void _find_gd_files(const String &p_dir, Vector<String> &r_files);
	static void _find_tscn_files(const String &p_dir, Vector<String> &r_files);
	static void _parse_script_file(const String &p_path, ScriptFile &r_file);
	static void _parse_tscn_connections(const String &p_path, Vector<TscnConnection> &r_connections);
	static int _compute_complexity(const Vector<String> &p_lines, int p_start, int p_end);
	static String _get_complexity_grade(int p_complexity);
	static bool _is_virtual_override(const String &p_name);
	static bool _is_public_api_pattern(const String &p_name);
};
