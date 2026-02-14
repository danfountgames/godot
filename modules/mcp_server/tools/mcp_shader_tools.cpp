/**************************************************************************/
/*  mcp_shader_tools.cpp                                                  */
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

#include "mcp_shader_tools.h"

#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// ============================================================================
// Tool Registration
// ============================================================================

void MCPShaderTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ---- shader/find ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Directory to scan for shader files (default: res://). "
				"Uses res:// format.");
		Array required;
		p_registry->register_tool(
				"shader/find",
				"Find Shaders",
				"Scan the project for all .gdshader and .gdshaderinc files. Returns each "
				"shader's path, shader_type (spatial, canvas_item, particles, sky, fog), "
				"line count, uniform count, varying count, and render modes used. Results "
				"are grouped by shader type for easy overview.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPShaderTools::handle_find_shaders));
	}

	// ---- shader/get_builtins ----
	{
		Dictionary props;
		props["shader_type"] = make_prop("string",
				"The shader type to query: 'spatial', 'canvas_item', 'particles', "
				"'sky', or 'fog'. Required.");
		props["stage"] = make_prop("string",
				"Optional: filter to a specific stage (e.g., 'vertex', 'fragment', "
				"'light', 'start', 'process', 'sky', 'fog'). If omitted, returns "
				"all stages for the shader type.");
		Array required;
		required.push_back("shader_type");
		p_registry->register_tool(
				"shader/get_builtins",
				"Get Shader Built-ins",
				"Query the engine's shading language database for built-in variables, "
				"render modes, and stage-specific functions available for a given shader "
				"type. Returns the real data that powers the shader editor's code "
				"completion: variable names, types, whether each is read-only or writable, "
				"and all valid render modes. For spatial shaders this includes surface "
				"outputs (ALBEDO, METALLIC, ROUGHNESS, EMISSION, etc.), vertex inputs "
				"(VERTEX, NORMAL, UV, etc.), and lighting variables. Note: Godot uses its "
				"own shading language (not raw GLSL) — use this tool to get the correct "
				"built-in names instead of guessing.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPShaderTools::handle_get_builtins));
	}

	// ---- shader/get_functions ----
	{
		Dictionary props;
		props["filter"] = make_prop("string",
				"Optional substring filter for function names (e.g., 'texture', 'mix'). "
				"Case-insensitive. If omitted, returns all built-in functions.");
		Array required;
		p_registry->register_tool(
				"shader/get_functions",
				"Get Shader Built-in Functions",
				"List all built-in functions available in Godot's shading language. While "
				"based on GLSL ES 3.0, Godot's shading language has its own set of "
				"supported functions. This returns the complete set from the engine's "
				"shader compiler, categorized by type (texture sampling, trigonometric, "
				"geometric, etc.). Use the optional filter to search for specific "
				"functions (e.g., filter='texture' for texture sampling functions).",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPShaderTools::handle_get_functions));
	}

	// ---- shader/get_language_ref ----
	{
		Dictionary props;
		props["topic"] = make_prop("string",
				"The reference topic to query. Valid values: 'types' (all data types), "
				"'uniform_hints' (all uniform hint qualifiers), 'texture_filters' "
				"(sampler filter modes), 'texture_repeat' (sampler repeat modes), "
				"'keywords' (all shader language keywords), 'all' (everything). "
				"Default: 'all'.");
		Array required;
		p_registry->register_tool(
				"shader/get_language_ref",
				"Get Shading Language Reference",
				"Return Godot shading language reference data queried directly from the "
				"engine's shader compiler. Covers data types (float, vec2, vec3, vec4, "
				"mat4, sampler2D, etc.), uniform hint qualifiers (source_color, "
				"hint_range, hint_screen_texture, hint_normal, etc.), texture filter "
				"and repeat modes, and all language keywords. This is the authoritative "
				"reference — it matches exactly what the shader compiler accepts.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPShaderTools::handle_get_language_ref));
	}
}

// ============================================================================
// Helper: Recursive .gdshader file finder
// ============================================================================

void MCPShaderTools::_find_shader_files(const String &p_dir, Vector<String> &r_files) {
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
			String ext = item.get_extension().to_lower();
			if (ext == "gdshader" || ext == "shader" || ext == "gdshaderinc") {
				r_files.push_back(full_path);
			}
		}
		item = da->get_next();
	}
	da->list_dir_end();

	for (const String &subdir : subdirs) {
		_find_shader_files(subdir, r_files);
	}
}

// ============================================================================
// Helper: Map shader type string to RS::ShaderMode
// ============================================================================

static bool _shader_type_to_mode(const String &p_type, RS::ShaderMode &r_mode) {
	if (p_type == "spatial") {
		r_mode = RS::SHADER_SPATIAL;
	} else if (p_type == "canvas_item") {
		r_mode = RS::SHADER_CANVAS_ITEM;
	} else if (p_type == "particles") {
		r_mode = RS::SHADER_PARTICLES;
	} else if (p_type == "sky") {
		r_mode = RS::SHADER_SKY;
	} else if (p_type == "fog") {
		r_mode = RS::SHADER_FOG;
	} else {
		return false;
	}
	return true;
}

// ============================================================================
// Tool 1: shader/find
// ============================================================================

Dictionary MCPShaderTools::handle_find_shaders(const Dictionary &p_args) {
	String root = p_args.get("path", "res://");
	if (root.is_empty()) {
		root = "res://";
	}

	if (!validate_path(root)) {
		return make_tool_error("Invalid path. Must start with res://.");
	}

	Vector<String> files;
	_find_shader_files(root, files);

	if (files.is_empty()) {
		Dictionary structured;
		structured["shaders"] = Array();
		structured["count"] = 0;
		return make_tool_result("No shader files found.", structured);
	}

	// Parse each shader file for basic info.
	Array shaders_array;
	HashMap<String, Array> by_type; // shader_type -> array of shader dicts
	int total_lines = 0;

	for (const String &path : files) {
		Ref<FileAccess> fa = FileAccess::open(path, FileAccess::READ);
		if (fa.is_null()) {
			continue;
		}

		String content = fa->get_as_utf8_string();
		fa->close();

		// Determine file kind.
		String ext = path.get_extension().to_lower();
		bool is_include = (ext == "gdshaderinc");

		// Extract shader_type using the engine's own parser.
		String shader_type;
		if (is_include) {
			shader_type = "include";
		} else {
			shader_type = ShaderLanguage::get_shader_type(content);
			if (shader_type.is_empty()) {
				shader_type = "unknown";
			}
		}

		// Count lines.
		Vector<String> lines = content.split("\n");
		int line_count = lines.size();
		total_lines += line_count;

		// Count uniforms, varyings, and collect render modes.
		int uniform_count = 0;
		int varying_count = 0;
		String render_modes_str;

		for (const String &line : lines) {
			String trimmed = line.strip_edges();
			if (trimmed.begins_with("uniform ")) {
				uniform_count++;
			} else if (trimmed.begins_with("varying ") ||
					trimmed.begins_with("flat ") ||
					trimmed.begins_with("smooth ")) {
				varying_count++;
			} else if (trimmed.begins_with("render_mode ")) {
				// Extract render mode list.
				int semi = trimmed.find(";");
				if (semi > 0) {
					render_modes_str = trimmed.substr(12, semi - 12).strip_edges();
				}
			}
		}

		Dictionary entry;
		entry["path"] = path;
		entry["shader_type"] = shader_type;
		entry["lines"] = line_count;
		entry["uniforms"] = uniform_count;
		if (varying_count > 0) {
			entry["varyings"] = varying_count;
		}
		if (!render_modes_str.is_empty()) {
			entry["render_modes"] = render_modes_str;
		}

		shaders_array.push_back(entry);

		if (!by_type.has(shader_type)) {
			by_type[shader_type] = Array();
		}
		by_type[shader_type].push_back(entry);
	}

	// Build text output.
	String text = vformat("Found %d shader file(s) (%d total lines):\n\n", files.size(), total_lines);

	// Group by type.
	for (const KeyValue<String, Array> &kv : by_type) {
		text += vformat("== %s (%d) ==\n", kv.key, kv.value.size());
		for (int i = 0; i < kv.value.size(); i++) {
			Dictionary s = kv.value[i];
			text += vformat("  %s  [%d lines, %d uniforms",
					String(s["path"]), (int)s["lines"], (int)s["uniforms"]);
			if (s.has("varyings")) {
				text += vformat(", %d varyings", (int)s["varyings"]);
			}
			if (s.has("render_modes")) {
				text += vformat(", render_mode %s", String(s["render_modes"]));
			}
			text += "]\n";
		}
		text += "\n";
	}

	// Build structured output.
	Dictionary by_type_dict;
	for (const KeyValue<String, Array> &kv : by_type) {
		by_type_dict[kv.key] = kv.value;
	}

	Dictionary structured;
	structured["shaders"] = shaders_array;
	structured["by_type"] = by_type_dict;
	structured["count"] = files.size();
	structured["total_lines"] = total_lines;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 2: shader/get_builtins
// ============================================================================

Dictionary MCPShaderTools::handle_get_builtins(const Dictionary &p_args) {
	String shader_type = p_args.get("shader_type", "");
	String stage_filter = p_args.get("stage", "");

	if (shader_type.is_empty()) {
		return make_tool_error(
				"Missing required parameter: shader_type\n\n"
				"Valid values: spatial, canvas_item, particles, sky, fog");
	}

	RS::ShaderMode mode;
	if (!_shader_type_to_mode(shader_type, mode)) {
		return make_tool_error(vformat(
				"Invalid shader_type: '%s'\n\n"
				"Valid values: spatial, canvas_item, particles, sky, fog",
				shader_type));
	}

	ShaderTypes *st = ShaderTypes::get_singleton();
	ERR_FAIL_NULL_V(st, make_tool_error("ShaderTypes singleton not available."));

	const HashMap<StringName, ShaderLanguage::FunctionInfo> &functions = st->get_functions(mode);
	const Vector<ShaderLanguage::ModeInfo> &modes = st->get_modes(mode);
	const Vector<ShaderLanguage::ModeInfo> &stencil_modes = st->get_stencil_modes(mode);

	String text;
	Dictionary structured;

	// ---- Built-in variables by stage ----

	text = vformat("Godot shading language built-ins for shader_type %s:\n", shader_type);
	text += "(Godot uses its own shading language, not raw GLSL. These are the "
			"exact built-in names the shader compiler accepts.)\n";

	Dictionary stages_dict;

	for (const KeyValue<StringName, ShaderLanguage::FunctionInfo> &kv : functions) {
		String stage_name = String(kv.key);

		// Apply stage filter if provided.
		if (!stage_filter.is_empty() && stage_name != stage_filter) {
			continue;
		}

		const ShaderLanguage::FunctionInfo &fi = kv.value;

		// Built-in variables.
		Array vars_array;
		Vector<String> var_names;

		for (const KeyValue<StringName, ShaderLanguage::BuiltInInfo> &bkv : fi.built_ins) {
			var_names.push_back(String(bkv.key));
		}
		var_names.sort();

		text += vformat("\n--- %s ---", stage_name);
		if (fi.main_function) {
			text += vformat(" (void %s() entry point)", stage_name);
		}
		text += "\n";

		if (fi.can_discard) {
			text += "  (supports 'discard' statement)\n";
		}

		for (const String &var_name : var_names) {
			const ShaderLanguage::BuiltInInfo &bi = fi.built_ins[StringName(var_name)];
			String type_name = ShaderLanguage::get_datatype_name(bi.type);
			bool is_const = bi.constant;

			Dictionary var_dict;
			var_dict["name"] = var_name;
			var_dict["type"] = type_name;
			var_dict["constant"] = is_const;
			vars_array.push_back(var_dict);

			text += vformat("  %s %s%s\n", type_name, var_name,
					is_const ? " (read-only)" : "");
		}

		// Stage functions (special built-in functions for this stage).
		Array stage_funcs_array;
		for (const KeyValue<StringName, ShaderLanguage::StageFunctionInfo> &sfkv : fi.stage_functions) {
			String func_name = String(sfkv.key);
			const ShaderLanguage::StageFunctionInfo &sfi = sfkv.value;
			String ret_type = ShaderLanguage::get_datatype_name(sfi.return_type);

			String params;
			Array params_array;
			for (int i = 0; i < sfi.arguments.size(); i++) {
				String arg_type = ShaderLanguage::get_datatype_name(sfi.arguments[i].type);
				String arg_name = String(sfi.arguments[i].name);
				if (i > 0) {
					params += ", ";
				}
				params += arg_type + " " + arg_name;

				Dictionary arg_dict;
				arg_dict["name"] = arg_name;
				arg_dict["type"] = arg_type;
				params_array.push_back(arg_dict);
			}

			Dictionary func_dict;
			func_dict["name"] = func_name;
			func_dict["return_type"] = ret_type;
			func_dict["parameters"] = params_array;
			stage_funcs_array.push_back(func_dict);

			text += vformat("  %s %s(%s)  [stage function]\n", ret_type, func_name, params);
		}

		Dictionary stage_dict;
		stage_dict["built_ins"] = vars_array;
		stage_dict["can_discard"] = fi.can_discard;
		stage_dict["main_function"] = fi.main_function;
		if (!stage_funcs_array.is_empty()) {
			stage_dict["stage_functions"] = stage_funcs_array;
		}
		stages_dict[stage_name] = stage_dict;
	}

	structured["shader_type"] = shader_type;
	structured["stages"] = stages_dict;

	// ---- Render modes ----

	if (stage_filter.is_empty()) {
		text += "\n--- render modes ---\n";
		text += "(Use as: render_mode mode1, mode2, ...;)\n";

		Array modes_array;
		for (const ShaderLanguage::ModeInfo &mi : modes) {
			Dictionary mode_dict;
			mode_dict["name"] = String(mi.name);

			if (mi.options.is_empty()) {
				// Boolean flag mode.
				mode_dict["type"] = "flag";
				text += vformat("  %s\n", String(mi.name));
			} else {
				// Multi-choice mode.
				Array opts;
				String opts_text;
				for (int i = 0; i < mi.options.size(); i++) {
					opts.push_back(String(mi.options[i]));
					if (i > 0) {
						opts_text += ", ";
					}
					opts_text += String(mi.name) + "_" + String(mi.options[i]);
				}
				mode_dict["type"] = "enum";
				mode_dict["options"] = opts;
				text += vformat("  %s: %s\n", String(mi.name), opts_text);
			}
			modes_array.push_back(mode_dict);
		}

		structured["render_modes"] = modes_array;

		// Stencil modes (Godot 4.6+).
		if (!stencil_modes.is_empty()) {
			text += "\n--- stencil modes (Godot 4.6+) ---\n";

			Array stencil_array;
			for (const ShaderLanguage::ModeInfo &mi : stencil_modes) {
				Dictionary mode_dict;
				mode_dict["name"] = String(mi.name);

				if (mi.options.is_empty()) {
					mode_dict["type"] = "flag";
					text += vformat("  stencil_%s\n", String(mi.name));
				} else {
					Array opts;
					String opts_text;
					for (int i = 0; i < mi.options.size(); i++) {
						opts.push_back(String(mi.options[i]));
						if (i > 0) {
							opts_text += ", ";
						}
						opts_text += "stencil_" + String(mi.name) + "_" + String(mi.options[i]);
					}
					mode_dict["type"] = "enum";
					mode_dict["options"] = opts;
					text += vformat("  stencil_%s: %s\n", String(mi.name), opts_text);
				}
				stencil_array.push_back(mode_dict);
			}

			structured["stencil_modes"] = stencil_array;
		}
	}

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 3: shader/get_functions
// ============================================================================

Dictionary MCPShaderTools::handle_get_functions(const Dictionary &p_args) {
	String filter = p_args.get("filter", "");

	List<String> all_funcs;
	ShaderLanguage::get_builtin_funcs(&all_funcs);

	// Sort alphabetically.
	Vector<String> sorted;
	for (const String &f : all_funcs) {
		if (!filter.is_empty()) {
			if (f.findn(filter) == -1) {
				continue;
			}
		}
		sorted.push_back(f);
	}
	sorted.sort();

	// Categorize functions for better readability.
	HashMap<String, Vector<String>> categories;

	for (const String &f : sorted) {
		String cat;
		if (f.begins_with("texture")) {
			cat = "Texture Sampling";
		} else if (f == "sin" || f == "cos" || f == "tan" || f == "asin" ||
				f == "acos" || f == "atan" || f == "sinh" || f == "cosh" ||
				f == "tanh" || f == "asinh" || f == "acosh" || f == "atanh") {
			cat = "Trigonometric";
		} else if (f == "pow" || f == "exp" || f == "exp2" || f == "log" ||
				f == "log2" || f == "sqrt" || f == "inversesqrt") {
			cat = "Exponential";
		} else if (f == "abs" || f == "sign" || f == "floor" || f == "ceil" ||
				f == "fract" || f == "mod" || f == "min" || f == "max" ||
				f == "clamp" || f == "mix" || f == "step" || f == "smoothstep" ||
				f == "round" || f == "roundEven" || f == "trunc" || f == "fma" ||
				f == "frexp" || f == "ldexp" || f == "isnan" || f == "isinf" ||
				f == "modf") {
			cat = "Common Math";
		} else if (f == "length" || f == "distance" || f == "dot" || f == "cross" ||
				f == "normalize" || f == "reflect" || f == "refract" ||
				f == "faceforward") {
			cat = "Geometric";
		} else if (f == "matrixCompMult" || f == "outerProduct" ||
				f == "transpose" || f == "determinant" || f == "inverse") {
			cat = "Matrix";
		} else if (f == "lessThan" || f == "lessThanEqual" || f == "greaterThan" ||
				f == "greaterThanEqual" || f == "equal" || f == "notEqual" ||
				f == "any" || f == "all" || f == "not") {
			cat = "Vector Relational";
		} else if (f.begins_with("pack") || f.begins_with("unpack")) {
			cat = "Packing";
		} else if (f.begins_with("bitfield") || f == "bitCount" ||
				f == "findLSB" || f == "findMSB" ||
				f == "uaddCarry" || f == "usubBorrow" || f == "umulExtended" ||
				f == "imulExtended") {
			cat = "Bitwise / Integer";
		} else if (f == "dFdx" || f == "dFdy" || f == "fwidth" ||
				f.begins_with("dFdx") || f.begins_with("dFdy") ||
				f.begins_with("fwidth")) {
			cat = "Fragment Derivatives";
		} else if (f == "floatBitsToInt" || f == "floatBitsToUint" ||
				f == "intBitsToFloat" || f == "uintBitsToFloat") {
			cat = "Type Conversion";
		} else {
			cat = "Other";
		}

		if (!categories.has(cat)) {
			categories[cat] = Vector<String>();
		}
		categories[cat].push_back(f);
	}

	// Build text output.
	String text;
	if (!filter.is_empty()) {
		text = vformat("Godot shading language built-in functions matching '%s' (%d):\n\n", filter, sorted.size());
	} else {
		text = vformat("All Godot shading language built-in functions (%d):\n", sorted.size());
		text += "(Based on GLSL ES 3.0 with Godot-specific additions.)\n\n";
	}

	// Sort category names for consistent output.
	Vector<String> cat_names;
	for (const KeyValue<String, Vector<String>> &kv : categories) {
		cat_names.push_back(kv.key);
	}
	cat_names.sort();

	Dictionary categories_dict;

	for (const String &cat : cat_names) {
		const Vector<String> &funcs = categories[cat];
		text += vformat("== %s (%d) ==\n", cat, funcs.size());

		Array funcs_array;
		for (const String &f : funcs) {
			text += vformat("  %s\n", f);
			funcs_array.push_back(f);
		}
		text += "\n";

		categories_dict[cat] = funcs_array;
	}

	// Build structured output.
	Array all_names;
	for (const String &f : sorted) {
		all_names.push_back(f);
	}

	Dictionary structured;
	structured["functions"] = all_names;
	structured["categories"] = categories_dict;
	structured["count"] = sorted.size();
	if (!filter.is_empty()) {
		structured["filter"] = filter;
	}

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool 4: shader/get_language_ref
// ============================================================================

Dictionary MCPShaderTools::handle_get_language_ref(const Dictionary &p_args) {
	String topic = p_args.get("topic", "all");
	if (topic.is_empty()) {
		topic = "all";
	}

	bool want_types = (topic == "types" || topic == "all");
	bool want_hints = (topic == "uniform_hints" || topic == "all");
	bool want_filters = (topic == "texture_filters" || topic == "all");
	bool want_repeat = (topic == "texture_repeat" || topic == "all");
	bool want_keywords = (topic == "keywords" || topic == "all");

	if (!want_types && !want_hints && !want_filters && !want_repeat && !want_keywords) {
		return make_tool_error(
				"Invalid topic: '" + topic + "'\n\n"
				"Valid values: types, uniform_hints, texture_filters, "
				"texture_repeat, keywords, all");
	}

	String text = "Godot Shading Language Reference\n";
	text += "(Godot uses its own shading language based on GLSL ES 3.0, "
			"with significant differences. This is the authoritative reference "
			"from the shader compiler.)\n";
	Dictionary structured;

	// ---- Data types ----
	if (want_types) {
		text += "\n== Data Types ==\n";
		text += "(Use these in uniform, varying, and variable declarations.)\n";

		Array types_array;

		// Iterate all data types from the enum.
		for (int i = 0; i < ShaderLanguage::TYPE_MAX; i++) {
			ShaderLanguage::DataType dt = (ShaderLanguage::DataType)i;
			if (dt == ShaderLanguage::TYPE_VOID || dt == ShaderLanguage::TYPE_STRUCT) {
				continue; // Skip void and struct (struct is user-defined).
			}

			String name = ShaderLanguage::get_datatype_name(dt);

			Dictionary type_dict;
			type_dict["name"] = name;
			type_dict["is_sampler"] = ShaderLanguage::is_sampler_type(dt);
			type_dict["is_scalar"] = ShaderLanguage::is_scalar_type(dt);
			type_dict["is_float"] = ShaderLanguage::is_float_type(dt);

			types_array.push_back(type_dict);

			String flags;
			if (ShaderLanguage::is_sampler_type(dt)) {
				flags += " [sampler]";
			}
			if (ShaderLanguage::is_scalar_type(dt)) {
				flags += " [scalar]";
			}

			text += vformat("  %s%s\n", name, flags);
		}

		structured["types"] = types_array;
	}

	// ---- Uniform hints ----
	if (want_hints) {
		text += "\n== Uniform Hints ==\n";
		text += "(Use after the type in a uniform declaration, e.g.: "
				"uniform float speed : hint_range(0, 100);)\n";

		Array hints_array;

		// Iterate all hint values from the enum.
		for (int i = 0; i < ShaderLanguage::ShaderNode::Uniform::HINT_MAX; i++) {
			ShaderLanguage::ShaderNode::Uniform::Hint hint =
					(ShaderLanguage::ShaderNode::Uniform::Hint)i;
			if (hint == ShaderLanguage::ShaderNode::Uniform::HINT_NONE) {
				continue;
			}

			String hint_name = ShaderLanguage::get_uniform_hint_name(hint);
			if (hint_name.is_empty()) {
				continue;
			}

			Dictionary hint_dict;
			hint_dict["name"] = hint_name;

			// Add usage notes for hints that take parameters.
			String usage;
			switch (hint) {
				case ShaderLanguage::ShaderNode::Uniform::HINT_RANGE:
					usage = "hint_range(min, max[, step])";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_ENUM:
					usage = "hint_enum(\"Label1\", \"Label2\", ...)";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_SOURCE_COLOR:
					usage = "source_color — marks a vec4/vec3 as sRGB color";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE:
					usage = "hint_screen_texture — access the screen backbuffer";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE:
					usage = "hint_normal_roughness_texture — access the normal/roughness buffer";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_DEPTH_TEXTURE:
					usage = "hint_depth_texture — access the depth buffer";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_NORMAL:
					usage = "hint_normal — marks texture as normal map";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_DEFAULT_BLACK:
					usage = "hint_default_black — sampler defaults to black";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_DEFAULT_WHITE:
					usage = "hint_default_white — sampler defaults to white";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_DEFAULT_TRANSPARENT:
					usage = "hint_default_transparent — sampler defaults to transparent";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_ANISOTROPY:
					usage = "hint_anisotropy — anisotropy flowmap texture";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_ROUGHNESS_NORMAL:
					usage = "hint_roughness_normal — roughness derived from normal map";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_ROUGHNESS_R:
					usage = "hint_roughness_r — roughness from red channel";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_ROUGHNESS_G:
					usage = "hint_roughness_g — roughness from green channel";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_ROUGHNESS_B:
					usage = "hint_roughness_b — roughness from blue channel";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_ROUGHNESS_A:
					usage = "hint_roughness_a — roughness from alpha channel";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_ROUGHNESS_GRAY:
					usage = "hint_roughness_gray — roughness from grayscale";
					break;
				case ShaderLanguage::ShaderNode::Uniform::HINT_COLOR_CONVERSION_DISABLED:
					usage = "color_conversion_disabled — disable sRGB conversion";
					break;
				default:
					break;
			}

			if (!usage.is_empty()) {
				hint_dict["usage"] = usage;
				text += vformat("  %s\n", usage);
			} else {
				text += vformat("  %s\n", hint_name);
			}

			hints_array.push_back(hint_dict);
		}

		structured["uniform_hints"] = hints_array;
	}

	// ---- Texture filter modes ----
	if (want_filters) {
		text += "\n== Texture Filter Modes ==\n";
		text += "(Use on sampler uniforms, e.g.: "
				"uniform sampler2D tex : filter_nearest;)\n";

		Array filters_array;

		// The valid texture filter values (skip FILTER_DEFAULT which is
		// the internal default, not a user-facing keyword).
		const ShaderLanguage::TextureFilter filter_values[] = {
			ShaderLanguage::FILTER_NEAREST,
			ShaderLanguage::FILTER_LINEAR,
			ShaderLanguage::FILTER_NEAREST_MIPMAP,
			ShaderLanguage::FILTER_LINEAR_MIPMAP,
			ShaderLanguage::FILTER_NEAREST_MIPMAP_ANISOTROPIC,
			ShaderLanguage::FILTER_LINEAR_MIPMAP_ANISOTROPIC,
		};

		for (ShaderLanguage::TextureFilter tf : filter_values) {
			String name = ShaderLanguage::get_texture_filter_name(tf);
			if (!name.is_empty()) {
				Dictionary fd;
				fd["name"] = name;
				filters_array.push_back(fd);
				text += vformat("  %s\n", name);
			}
		}

		structured["texture_filters"] = filters_array;
	}

	// ---- Texture repeat modes ----
	if (want_repeat) {
		text += "\n== Texture Repeat Modes ==\n";
		text += "(Use on sampler uniforms, e.g.: "
				"uniform sampler2D tex : repeat_enable;)\n";

		Array repeat_array;

		const ShaderLanguage::TextureRepeat repeat_values[] = {
			ShaderLanguage::REPEAT_DISABLE,
			ShaderLanguage::REPEAT_ENABLE,
		};

		for (ShaderLanguage::TextureRepeat tr : repeat_values) {
			String name = ShaderLanguage::get_texture_repeat_name(tr);
			if (!name.is_empty()) {
				Dictionary rd;
				rd["name"] = name;
				repeat_array.push_back(rd);
				text += vformat("  %s\n", name);
			}
		}

		structured["texture_repeat"] = repeat_array;
	}

	// ---- Keywords ----
	if (want_keywords) {
		text += "\n== Shader Language Keywords ==\n";

		List<String> kw_list;
		ShaderLanguage::get_keyword_list(&kw_list);

		Vector<String> kw_sorted;
		for (const String &kw : kw_list) {
			kw_sorted.push_back(kw);
		}
		kw_sorted.sort();

		Array kw_array;
		for (const String &kw : kw_sorted) {
			kw_array.push_back(kw);
			text += vformat("  %s\n", kw);
		}

		structured["keywords"] = kw_array;
	}

	return make_tool_result(text, structured);
}
