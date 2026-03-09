/**************************************************************************/
/*  material_storage_avp.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                 */
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

#include "material_storage_avp.h"

#include "avp_bridge.h"
#include "core/config/project_settings.h"
#include "core/string/print_string.h"
#include "texture_storage_avp.h"

MaterialStorageAVP *MaterialStorageAVP::singleton = nullptr;

MaterialStorageAVP::MaterialStorageAVP() {
	singleton = this;
	ShaderCompiler::DefaultIdentifierActions actions;
	avp_compiler.initialize(actions);
}

MaterialStorageAVP::~MaterialStorageAVP() {
	singleton = nullptr;
	global_shader_variables.clear();
}

String MaterialStorageAVP::_parse_rk_shader_annotation(const String &p_code) const {
	// Look for @rk_shader("ShaderName") annotation in comments.
	// Format: // @rk_shader("ToonMaterial")
	int idx = p_code.find("@rk_shader");
	if (idx == -1) {
		return String();
	}

	int paren_start = p_code.find("(", idx);
	if (paren_start == -1) {
		return String();
	}

	int quote_start = p_code.find("\"", paren_start);
	if (quote_start == -1) {
		return String();
	}

	int quote_end = p_code.find("\"", quote_start + 1);
	if (quote_end == -1) {
		return String();
	}

	return p_code.substr(quote_start + 1, quote_end - quote_start - 1);
}

String MaterialStorageAVP::_serialize_params_to_json(const HashMap<StringName, Variant> &p_params) const {
	// Build a simple JSON object from the material parameters.
	// Supports: float, int, bool, Color (as array), Vector2/3/4 (as array), String.
	String json = "{";
	bool first = true;

	for (const KeyValue<StringName, Variant> &E : p_params) {
		if (!first) {
			json += ",";
		}
		first = false;

		// Key in snake_case.
		json += "\"" + String(E.key) + "\":";

		// Value based on type.
		const Variant &v = E.value;
		switch (v.get_type()) {
			case Variant::FLOAT:
				json += String::num(float(v), 6);
				break;
			case Variant::INT:
				json += itos(int(v));
				break;
			case Variant::BOOL:
				json += bool(v) ? "true" : "false";
				break;
			case Variant::STRING:
				json += "\"" + String(v) + "\"";
				break;
			case Variant::COLOR: {
				Color c = v;
				json += vformat("[%f,%f,%f,%f]", c.r, c.g, c.b, c.a);
			} break;
			case Variant::VECTOR2: {
				Vector2 vec = v;
				json += vformat("[%f,%f]", vec.x, vec.y);
			} break;
			case Variant::VECTOR3: {
				Vector3 vec = v;
				json += vformat("[%f,%f,%f]", vec.x, vec.y, vec.z);
			} break;
			case Variant::VECTOR4: {
				Vector4 vec = v;
				json += vformat("[%f,%f,%f,%f]", vec.x, vec.y, vec.z, vec.w);
			} break;
			default:
				// Unsupported type — emit as null.
				json += "null";
				break;
		}
	}

	json += "}";
	return json;
}

void MaterialStorageAVP::_upload_material_to_bridge(RID p_material, AVPMaterial *p_avp_material) {
	if (p_avp_material->uploaded_to_bridge) {
		return;
	}

	uint64_t material_id = p_material.get_id();

	// Check if the shader has an @rk_shader annotation.
	AVPShader *shader = shader_owner.get_or_null(p_avp_material->shader);

	if (shader && !shader->code.is_empty()) {
		if (!shader->rk_shader_name.is_empty()) {
			// Custom RealityKit shader material with @rk_shader annotation.
			// Serialize material parameters to JSON for the Swift shader factory.
			CharString params_json;
			if (!p_avp_material->params.is_empty()) {
				String json = _serialize_params_to_json(p_avp_material->params);
				if (!json.is_empty()) {
					params_json = json.utf8();
				}
			}

			godot_avp_material_create_custom(
					material_id,
					shader->rk_shader_name.utf8().get_data(),
					params_json.length() > 0 ? params_json.get_data() : nullptr);
			p_avp_material->uploaded_to_bridge = true;
			return;
		} else {
			// Custom shader WITHOUT @rk_shader annotation.
			// Phase 6.4: Log a warning and fall back to default PBR.
			WARN_PRINT(vformat(
					"AVP: Material %d uses a custom shader without @rk_shader annotation. "
					"Falling back to default PBR material. To use a custom RealityKit shader, "
					"add '// @rk_shader(\"ShaderName\")' to the shader source.",
					material_id));
			// Fall through to PBR creation below.
		}
	}

	// Default: create a PBR material with defaults.
	// Extract parameters from the material's param map.
	Color albedo_color = Color(1.0, 1.0, 1.0, 1.0);
	float roughness = 1.0f;
	float metallic = 0.0f;
	float normal_scale = 1.0f;
	Color emission_color = Color(0.0, 0.0, 0.0);
	float emission_energy = 0.0f;
	float clearcoat = 0.0f;
	float clearcoat_roughness = 0.0f;
	int alpha_mode = 0; // opaque
	float alpha_scissor_threshold = 0.5f;
	int cull_mode = 0; // back

	// Texture RIDs (0 = no texture).
	uint64_t albedo_tex = 0;
	uint64_t roughness_tex = 0;
	uint64_t metallic_tex = 0;
	uint64_t normal_tex = 0;
	uint64_t emission_tex = 0;
	uint64_t ao_tex = 0;

	// Extract scalar parameters.
	if (p_avp_material->params.has("albedo_color")) {
		albedo_color = p_avp_material->params["albedo_color"];
	}
	if (p_avp_material->params.has("roughness")) {
		roughness = p_avp_material->params["roughness"];
	}
	if (p_avp_material->params.has("metallic")) {
		metallic = p_avp_material->params["metallic"];
	}
	if (p_avp_material->params.has("normal_scale")) {
		normal_scale = p_avp_material->params["normal_scale"];
	}
	if (p_avp_material->params.has("emission")) {
		emission_color = p_avp_material->params["emission"];
	}
	if (p_avp_material->params.has("emission_energy")) {
		emission_energy = p_avp_material->params["emission_energy"];
	}
	if (p_avp_material->params.has("clearcoat")) {
		clearcoat = p_avp_material->params["clearcoat"];
	}
	if (p_avp_material->params.has("clearcoat_roughness")) {
		clearcoat_roughness = p_avp_material->params["clearcoat_roughness"];
	}

	// Extract alpha/cull mode.
	if (p_avp_material->params.has("transparency")) {
		int transparency = p_avp_material->params["transparency"];
		if (transparency == 1) { // TRANSPARENCY_ALPHA
			alpha_mode = 1;
		} else if (transparency == 2) { // TRANSPARENCY_ALPHA_SCISSOR
			alpha_mode = 2;
		}
	}
	if (p_avp_material->params.has("alpha_scissor_threshold")) {
		alpha_scissor_threshold = p_avp_material->params["alpha_scissor_threshold"];
	}
	if (p_avp_material->params.has("cull_mode")) {
		cull_mode = p_avp_material->params["cull_mode"];
	}

	// Extract texture RIDs and ensure they are uploaded to the bridge.
	TextureStorageAVP *tex_storage = TextureStorageAVP::get_singleton();
	auto extract_texture = [&](const StringName &p_param) -> uint64_t {
		if (!p_avp_material->params.has(p_param)) {
			return 0;
		}
		RID tex_rid = p_avp_material->params[p_param];
		if (!tex_rid.is_valid()) {
			return 0;
		}
		if (tex_storage) {
			tex_storage->ensure_texture_uploaded(tex_rid);
		}
		return tex_rid.get_id();
	};

	albedo_tex = extract_texture("texture_albedo");
	roughness_tex = extract_texture("texture_roughness");
	metallic_tex = extract_texture("texture_metallic");
	normal_tex = extract_texture("texture_normal");
	emission_tex = extract_texture("texture_emission");
	ao_tex = extract_texture("texture_ambient_occlusion");

	godot_avp_material_create_pbr(
			material_id,
			albedo_tex,
			albedo_color.r, albedo_color.g, albedo_color.b, albedo_color.a,
			roughness, roughness_tex,
			metallic, metallic_tex,
			normal_tex, normal_scale,
			emission_tex,
			emission_color.r, emission_color.g, emission_color.b,
			emission_energy,
			ao_tex,
			clearcoat, clearcoat_roughness,
			alpha_mode, alpha_scissor_threshold,
			cull_mode);

	p_avp_material->uploaded_to_bridge = true;
}

void MaterialStorageAVP::ensure_material_uploaded(RID p_material) {
	AVPMaterial *m = material_owner.get_or_null(p_material);
	if (m && !m->uploaded_to_bridge) {
		_upload_material_to_bridge(p_material, m);
	}
}

/* GLOBAL SHADER UNIFORM API */

void MaterialStorageAVP::global_shader_parameter_add(const StringName &p_name, RS::GlobalShaderParameterType p_type, const Variant &p_value) {
	ERR_FAIL_COND(global_shader_variables.has(p_name));
	global_shader_variables[p_name] = p_type;
}

void MaterialStorageAVP::global_shader_parameter_remove(const StringName &p_name) {
	if (!global_shader_variables.has(p_name)) {
		return;
	}
	global_shader_variables.erase(p_name);
}

Vector<StringName> MaterialStorageAVP::global_shader_parameter_get_list() const {
	Vector<StringName> names;
	for (const KeyValue<StringName, RS::GlobalShaderParameterType> &E : global_shader_variables) {
		names.push_back(E.key);
	}
	names.sort_custom<StringName::AlphCompare>();
	return names;
}

RS::GlobalShaderParameterType MaterialStorageAVP::global_shader_parameter_get_type(const StringName &p_name) const {
	if (!global_shader_variables.has(p_name)) {
		return RS::GLOBAL_VAR_TYPE_MAX;
	}
	return global_shader_variables[p_name];
}

void MaterialStorageAVP::global_shader_parameters_load_settings(bool p_load_textures) {
	List<PropertyInfo> settings;
	ProjectSettings::get_singleton()->get_property_list(&settings);

	for (const PropertyInfo &E : settings) {
		if (E.name.begins_with("shader_globals/")) {
			StringName name = E.name.get_slicec('/', 1);
			Dictionary d = GLOBAL_GET(E.name);

			ERR_CONTINUE(!d.has("type"));
			ERR_CONTINUE(!d.has("value"));

			String type = d["type"];

			static const char *global_var_type_names[RS::GLOBAL_VAR_TYPE_MAX] = {
				"bool",
				"bvec2",
				"bvec3",
				"bvec4",
				"int",
				"ivec2",
				"ivec3",
				"ivec4",
				"rect2i",
				"uint",
				"uvec2",
				"uvec3",
				"uvec4",
				"float",
				"vec2",
				"vec3",
				"vec4",
				"color",
				"rect2",
				"mat2",
				"mat3",
				"mat4",
				"transform_2d",
				"transform",
				"sampler2D",
				"sampler2DArray",
				"sampler3D",
				"samplerCube",
				"samplerExternalOES"
			};

			RS::GlobalShaderParameterType gvtype = RS::GLOBAL_VAR_TYPE_MAX;

			for (int i = 0; i < RS::GLOBAL_VAR_TYPE_MAX; i++) {
				if (global_var_type_names[i] == type) {
					gvtype = RS::GlobalShaderParameterType(i);
					break;
				}
			}

			ERR_CONTINUE(gvtype == RS::GLOBAL_VAR_TYPE_MAX);

			if (!global_shader_variables.has(name)) {
				global_shader_parameter_add(name, gvtype, Variant());
			}
		}
	}
}

/* SHADER API */

RID MaterialStorageAVP::shader_allocate() {
	return shader_owner.allocate_rid();
}

void MaterialStorageAVP::shader_initialize(RID p_rid, bool p_embedded) {
	shader_owner.initialize_rid(p_rid, AVPShader());
}

void MaterialStorageAVP::shader_free(RID p_rid) {
	AVPShader *shader = shader_owner.get_or_null(p_rid);
	ERR_FAIL_NULL(shader);
	shader_owner.free(p_rid);
}

void MaterialStorageAVP::shader_set_code(RID p_shader, const String &p_code) {
	AVPShader *shader = shader_owner.get_or_null(p_shader);
	ERR_FAIL_NULL(shader);

	shader->code = p_code;

	if (p_code.is_empty()) {
		return;
	}

	// Parse @rk_shader annotation.
	shader->rk_shader_name = _parse_rk_shader_annotation(p_code);

	// Parse the shader for uniform extraction.
	String mode_string = ShaderLanguage::get_shader_type(p_code);

	RS::ShaderMode new_mode;
	if (mode_string == "canvas_item") {
		new_mode = RS::SHADER_CANVAS_ITEM;
	} else if (mode_string == "particles") {
		new_mode = RS::SHADER_PARTICLES;
	} else if (mode_string == "spatial") {
		new_mode = RS::SHADER_SPATIAL;
	} else if (mode_string == "sky") {
		new_mode = RS::SHADER_SKY;
	} else if (mode_string == "fog") {
		new_mode = RS::SHADER_FOG;
	} else {
		new_mode = RS::SHADER_MAX;
		ERR_FAIL_MSG("Shader type " + mode_string + " not supported in AVP renderer.");
	}

	ShaderCompiler::IdentifierActions actions;
	actions.uniforms = &shader->uniforms;
	ShaderCompiler::GeneratedCode gen_code;

	Error err = avp_compiler.compile(new_mode, p_code, &actions, "", gen_code);
	ERR_FAIL_COND_MSG(err != OK, "Shader compilation failed.");
}

String MaterialStorageAVP::shader_get_code(RID p_shader) const {
	AVPShader *shader = shader_owner.get_or_null(p_shader);
	ERR_FAIL_NULL_V(shader, "");
	return shader->code;
}

void MaterialStorageAVP::get_shader_parameter_list(RID p_shader, List<PropertyInfo> *p_param_list) const {
	AVPShader *shader = shader_owner.get_or_null(p_shader);
	ERR_FAIL_NULL(shader);

	SortArray<Pair<StringName, int>, ShaderLanguage::UniformOrderComparator> sorter;
	LocalVector<Pair<StringName, int>> filtered_uniforms;

	for (const KeyValue<StringName, ShaderLanguage::ShaderNode::Uniform> &E : shader->uniforms) {
		if (E.value.scope != ShaderLanguage::ShaderNode::Uniform::SCOPE_LOCAL) {
			continue;
		}
		filtered_uniforms.push_back(Pair<StringName, int>(E.key, E.value.prop_order));
	}
	int uniform_count = filtered_uniforms.size();
	sorter.sort(filtered_uniforms.ptr(), uniform_count);

	String last_group;
	for (int i = 0; i < uniform_count; i++) {
		const StringName &uniform_name = filtered_uniforms[i].first;
		const ShaderLanguage::ShaderNode::Uniform &uniform = shader->uniforms[uniform_name];

		String group = uniform.group;
		if (!uniform.subgroup.is_empty()) {
			group += "::" + uniform.subgroup;
		}

		if (group != last_group) {
			PropertyInfo pi;
			pi.usage = PROPERTY_USAGE_GROUP;
			pi.name = group;
			p_param_list->push_back(pi);
			last_group = group;
		}

		PropertyInfo pi = ShaderLanguage::uniform_to_property_info(uniform);
		pi.name = uniform_name;
		p_param_list->push_back(pi);
	}
}

/* MATERIAL API */

RID MaterialStorageAVP::material_allocate() {
	return material_owner.allocate_rid();
}

void MaterialStorageAVP::material_initialize(RID p_rid) {
	material_owner.initialize_rid(p_rid, AVPMaterial());
}

void MaterialStorageAVP::material_free(RID p_rid) {
	AVPMaterial *material = material_owner.get_or_null(p_rid);
	ERR_FAIL_NULL(material);

	if (material->uploaded_to_bridge) {
		godot_avp_material_destroy(p_rid.get_id());
	}

	material_owner.free(p_rid);
}

void MaterialStorageAVP::material_set_shader(RID p_material, RID p_shader) {
	AVPMaterial *material = material_owner.get_or_null(p_material);
	ERR_FAIL_NULL(material);
	material->shader = p_shader;
	material->uploaded_to_bridge = false; // Needs re-upload with new shader.
}

void MaterialStorageAVP::material_set_param(RID p_material, const StringName &p_param, const Variant &p_value) {
	AVPMaterial *material = material_owner.get_or_null(p_material);
	ERR_FAIL_NULL(material);
	material->params[p_param] = p_value;
	material->uploaded_to_bridge = false; // Needs re-upload with new params.
}

Variant MaterialStorageAVP::material_get_param(RID p_material, const StringName &p_param) const {
	AVPMaterial *material = material_owner.get_or_null(p_material);
	ERR_FAIL_NULL_V(material, Variant());

	if (material->params.has(p_param)) {
		return material->params[p_param];
	}
	return Variant();
}

void MaterialStorageAVP::material_set_next_pass(RID p_material, RID p_next_material) {
	AVPMaterial *material = material_owner.get_or_null(p_material);
	ERR_FAIL_NULL(material);
	material->next_pass = p_next_material;
}

void MaterialStorageAVP::material_get_instance_shader_parameters(RID p_material, List<InstanceShaderParam> *r_parameters) {
	AVPMaterial *material = material_owner.get_or_null(p_material);
	ERR_FAIL_NULL(material);
	AVPShader *shader = shader_owner.get_or_null(material->shader);

	if (shader) {
		for (const KeyValue<StringName, ShaderLanguage::ShaderNode::Uniform> &E : shader->uniforms) {
			if (E.value.scope != ShaderLanguage::ShaderNode::Uniform::SCOPE_INSTANCE) {
				continue;
			}

			RendererMaterialStorage::InstanceShaderParam p;
			p.info = ShaderLanguage::uniform_to_property_info(E.value);
			p.info.name = E.key;
			p.index = E.value.instance_index;
			p.default_value = ShaderLanguage::constant_value_to_variant(E.value.default_value, E.value.type, E.value.array_size, E.value.hint);
			r_parameters->push_back(p);
		}
	}
	if (material->next_pass.is_valid()) {
		material_get_instance_shader_parameters(material->next_pass, r_parameters);
	}
}
