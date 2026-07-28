/**************************************************************************/
/*  mcp_skills.cpp                                                        */
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

#include "mcp_skills.h"

#include "mcp_paths.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/variant/array.h"
#include "core/version.h"

#ifdef TOOLS_ENABLED
#include "editor/editor_paths.h"
#include "editor/editor_settings.h"
#endif

static Vector<String> s_roots_override;
static bool s_allow_override_set = false;
static Vector<String> s_allow_override;

// A skill file is instructions, not data; a very large one is a mistake rather than
// something to load into a model's context.
static const int MAX_SKILL_BYTES = 1024 * 1024;

void MCPSkills::set_roots_override(const Vector<String> &p_roots) {
	s_roots_override = p_roots;
}

void MCPSkills::clear_roots_override() {
	s_roots_override.clear();
}

void MCPSkills::set_allow_override(const Vector<String> &p_allowed) {
	s_allow_override_set = true;
	s_allow_override = p_allowed;
}

void MCPSkills::clear_allow_override() {
	s_allow_override_set = false;
	s_allow_override.clear();
}

Dictionary MCPSkill::to_dictionary() const {
	Dictionary out;
	out["name"] = name;
	out["description"] = description;
	out["enabled"] = enabled;
	out["allowed"] = allowed;
	out["usable"] = is_usable();
	out["source"] = root_kind;
	out["path"] = res_path.is_empty() ? path : res_path;
	if (!required_editor_version.is_empty()) {
		out["required_editor_version"] = required_editor_version;
		out["version_supported"] = version_supported;
	}
	if (!tools.is_empty()) {
		Array tool_names;
		for (const String &tool : tools) {
			tool_names.push_back(tool);
		}
		out["tools"] = tool_names;
	}
	if (!problem.is_empty()) {
		out["problem"] = problem;
	}
	return out;
}

Vector<String> MCPSkills::get_roots() {
	if (!s_roots_override.is_empty()) {
		return s_roots_override;
	}

	Vector<String> roots;
	const String project_root = MCPPaths::get_project_root();
	if (!project_root.is_empty()) {
		// Project skills win over everything: they travel with the repository and are
		// the ones a team reviews together.
		roots.push_back(project_root.path_join("ai_skills"));

		// Plugin-provided skills, one directory per addon.
		Ref<DirAccess> addons = DirAccess::open(project_root.path_join("addons"));
		if (addons.is_valid()) {
			addons->list_dir_begin();
			String entry = addons->get_next();
			while (!entry.is_empty()) {
				if (addons->current_is_dir() && entry != "." && entry != "..") {
					roots.push_back(project_root.path_join("addons").path_join(entry).path_join("ai_skills"));
				}
				entry = addons->get_next();
			}
			addons->list_dir_end();
		}
	}

#ifdef TOOLS_ENABLED
	if (EditorPaths::get_singleton()) {
		roots.push_back(EditorPaths::get_singleton()->get_data_dir().path_join("godot_ai").path_join("skills"));
	}
#endif
	return roots;
}

static String root_kind_for(const String &p_root, const String &p_project_root) {
	if (p_project_root.is_empty()) {
		return "user";
	}
	if (p_root == p_project_root.path_join("ai_skills")) {
		return "project";
	}
	if (p_root.begins_with(p_project_root.path_join("addons"))) {
		return "plugin";
	}
	return "user";
}

bool MCPSkills::_parse_frontmatter(const String &p_text, Dictionary &r_fields, String &r_body, String &r_error) {
	const PackedStringArray lines = p_text.replace("\r\n", "\n").split("\n");
	if (lines.is_empty() || lines[0].strip_edges() != "---") {
		r_error = "the file does not start with a '---' frontmatter block";
		return false;
	}

	int index = 1;
	String pending_list_key;
	bool closed = false;
	for (; index < lines.size(); index++) {
		const String raw = lines[index];
		const String line = raw.strip_edges();
		if (line == "---") {
			closed = true;
			index++;
			break;
		}
		if (line.is_empty() || line.begins_with("#")) {
			continue;
		}

		// List item belonging to the previous key.
		if (line.begins_with("- ") || line == "-") {
			if (pending_list_key.is_empty()) {
				r_error = vformat("list item on line %d does not belong to any key", index + 1);
				return false;
			}
			Array items = r_fields[pending_list_key];
			const String item = line.trim_prefix("-").strip_edges().trim_prefix("\"").trim_suffix("\"");
			if (!item.is_empty()) {
				items.push_back(item);
			}
			r_fields[pending_list_key] = items;
			continue;
		}

		const int colon = line.find(":");
		if (colon <= 0) {
			r_error = vformat("line %d is neither 'key: value' nor a list item", index + 1);
			return false;
		}
		const String key = line.substr(0, colon).strip_edges();
		String value = line.substr(colon + 1).strip_edges();
		// Strip matching quotes; keep inner ones untouched.
		if (value.length() >= 2 && ((value.begins_with("\"") && value.ends_with("\"")) || (value.begins_with("'") && value.ends_with("'")))) {
			value = value.substr(1, value.length() - 2);
		}

		if (value.is_empty()) {
			// A key with no value opens a list.
			pending_list_key = key;
			r_fields[key] = Array();
		} else {
			pending_list_key = String();
			r_fields[key] = value;
		}
	}

	if (!closed) {
		r_error = "the frontmatter block is never closed with '---'";
		return false;
	}

	String body;
	for (; index < lines.size(); index++) {
		body += lines[index];
		if (index + 1 < lines.size()) {
			body += "\n";
		}
	}
	r_body = body.strip_edges();
	return true;
}

bool MCPSkills::version_satisfied(const String &p_constraint, int p_major, int p_minor) {
	String constraint = p_constraint.strip_edges();
	if (constraint.is_empty()) {
		return true;
	}

	String op = ">=";
	if (constraint.begins_with(">=") || constraint.begins_with("<=") || constraint.begins_with("==")) {
		op = constraint.substr(0, 2);
		constraint = constraint.substr(2).strip_edges();
	} else if (constraint.begins_with(">") || constraint.begins_with("<") || constraint.begins_with("=")) {
		op = constraint.substr(0, 1);
		constraint = constraint.substr(1).strip_edges();
	}

	const PackedStringArray parts = constraint.split(".", false);
	if (parts.is_empty() || !parts[0].is_valid_int()) {
		// An unreadable constraint must not silently pass.
		return false;
	}
	const int wanted_major = parts[0].to_int();
	const int wanted_minor = parts.size() > 1 && parts[1].is_valid_int() ? parts[1].to_int() : 0;

	const int actual = p_major * 1000 + p_minor;
	const int wanted = wanted_major * 1000 + wanted_minor;

	if (op == ">=") {
		return actual >= wanted;
	}
	if (op == ">") {
		return actual > wanted;
	}
	if (op == "<=") {
		return actual <= wanted;
	}
	if (op == "<") {
		return actual < wanted;
	}
	return actual == wanted;
}

bool MCPSkills::parse(const String &p_text, MCPSkill &r_skill, String &r_body, String &r_error) {
	if (p_text.length() > MAX_SKILL_BYTES) {
		r_error = "the skill file is too large";
		return false;
	}

	Dictionary fields;
	if (!_parse_frontmatter(p_text, fields, r_body, r_error)) {
		return false;
	}

	if (!fields.has("name") || String(fields["name"]).strip_edges().is_empty()) {
		r_error = "frontmatter is missing a 'name'";
		return false;
	}
	r_skill.name = String(fields["name"]).strip_edges();
	r_skill.description = fields.has("description") ? String(fields["description"]).strip_edges() : String();

	if (fields.has("enabled")) {
		const String enabled = String(fields["enabled"]).to_lower();
		r_skill.enabled = enabled != "false" && enabled != "no" && enabled != "0";
	}

	if (fields.has("required_editor_version")) {
		r_skill.required_editor_version = String(fields["required_editor_version"]).strip_edges();
		r_skill.version_supported = version_satisfied(r_skill.required_editor_version, VERSION_MAJOR, VERSION_MINOR);
	}

	r_skill.tools.clear();
	if (fields.has("tools")) {
		const Variant tools_value = fields["tools"];
		if (tools_value.get_type() == Variant::ARRAY) {
			const Array items = tools_value;
			for (int i = 0; i < items.size(); i++) {
				r_skill.tools.push_back(String(items[i]).strip_edges());
			}
		} else {
			// Inline comma-separated form.
			for (const String &item : String(tools_value).split(",", false)) {
				r_skill.tools.push_back(item.strip_edges());
			}
		}
	}

	return true;
}

Vector<MCPSkill> MCPSkills::discover() {
	Vector<MCPSkill> skills;
	Vector<String> seen_names;
	const String project_root = MCPPaths::get_project_root();

	for (const String &root : get_roots()) {
		Ref<DirAccess> dir = DirAccess::open(root);
		if (dir.is_null()) {
			continue;
		}
		dir->list_dir_begin();
		String entry = dir->get_next();
		while (!entry.is_empty()) {
			if (!dir->current_is_dir() || entry == "." || entry == "..") {
				entry = dir->get_next();
				continue;
			}
			const String skill_path = root.path_join(entry).path_join("SKILL.md");
			if (!FileAccess::exists(skill_path)) {
				entry = dir->get_next();
				continue;
			}

			MCPSkill skill;
			skill.path = skill_path;
			skill.root_kind = root_kind_for(root, project_root);
			if (!project_root.is_empty() && skill_path.begins_with(project_root + "/")) {
				skill.res_path = "res://" + skill_path.trim_prefix(project_root + "/");
			}

			const String text = FileAccess::get_file_as_string(skill_path);
			String body;
			String error;
			if (!parse(text, skill, body, error)) {
				// Reported rather than hidden: a broken skill the user wrote should be
				// visible, not silently missing.
				skill.name = entry;
				skill.problem = error;
				skills.push_back(skill);
				entry = dir->get_next();
				continue;
			}

			if (seen_names.has(skill.name)) {
				skill.problem = vformat("another skill named '%s' was already found in a higher-precedence location", skill.name);
				skills.push_back(skill);
				entry = dir->get_next();
				continue;
			}

			skill.allowed = is_allowed(skill.name);
			seen_names.push_back(skill.name);
			skills.push_back(skill);
			entry = dir->get_next();
		}
		dir->list_dir_end();
	}

	return skills;
}

static String allowed_skills_setting() {
	return "network/godot_ai/allowed_skills";
}

bool MCPSkills::is_allowed(const String &p_name) {
	if (s_allow_override_set) {
		return s_allow_override.has(p_name);
	}
	// The same automation opt-in that bypasses first-connection client approval: it
	// means "no human is present to decide", and it must be set deliberately.
	if (OS::get_singleton()->get_environment("GODOT_AI_AUTO_APPROVE") == "1") {
		return true;
	}
#ifdef TOOLS_ENABLED
	if (EditorSettings::get_singleton() && EditorSettings::get_singleton()->has_setting(allowed_skills_setting())) {
		const PackedStringArray allowed = EditorSettings::get_singleton()->get_setting(allowed_skills_setting());
		return allowed.has(p_name);
	}
#endif
	// Denied by default.
	return false;
}

void MCPSkills::set_allowed(const String &p_name, bool p_allowed) {
#ifdef TOOLS_ENABLED
	if (!EditorSettings::get_singleton()) {
		return;
	}
	PackedStringArray allowed;
	if (EditorSettings::get_singleton()->has_setting(allowed_skills_setting())) {
		allowed = EditorSettings::get_singleton()->get_setting(allowed_skills_setting());
	}
	const int index = allowed.find(p_name);
	if (p_allowed && index < 0) {
		allowed.push_back(p_name);
	} else if (!p_allowed && index >= 0) {
		allowed.remove_at(index);
	} else {
		return;
	}
	EditorSettings::get_singleton()->set_setting(allowed_skills_setting(), allowed);
#endif
}

bool MCPSkills::read_instructions(const MCPSkill &p_skill, String &r_body, String &r_error) {
	if (!p_skill.problem.is_empty()) {
		r_error = p_skill.problem;
		return false;
	}
	if (!p_skill.allowed) {
		r_error = vformat("skill '%s' has not been allowed; allow it in Editor Settings > Network > Godot AI", p_skill.name);
		return false;
	}
	if (!p_skill.enabled) {
		r_error = vformat("skill '%s' is disabled by its own frontmatter", p_skill.name);
		return false;
	}
	if (!p_skill.version_supported) {
		r_error = vformat("skill '%s' requires editor version %s", p_skill.name, p_skill.required_editor_version);
		return false;
	}

	MCPSkill parsed;
	const String text = FileAccess::get_file_as_string(p_skill.path);
	String error;
	if (!parse(text, parsed, r_body, error)) {
		r_error = error;
		return false;
	}
	return true;
}

bool MCPSkills::read_resource(const MCPSkill &p_skill, const String &p_relative, String &r_contents, String &r_error) {
	if (!p_skill.allowed) {
		r_error = vformat("skill '%s' has not been allowed", p_skill.name);
		return false;
	}
	if (p_relative.strip_edges().is_empty()) {
		r_error = "a resource path is required";
		return false;
	}

	// Supporting files live beside the skill; the same confinement rule that applies
	// to the project applies to a skill's own directory.
	const String skill_dir = p_skill.path.get_base_dir();
	const String candidate = skill_dir.path_join(p_relative).simplify_path();
	if (candidate != skill_dir && !candidate.begins_with(skill_dir + "/")) {
		r_error = vformat("'%s' is outside the skill's directory", p_relative);
		return false;
	}
	if (!FileAccess::exists(candidate)) {
		r_error = vformat("skill '%s' has no resource '%s'", p_skill.name, p_relative);
		return false;
	}

	r_contents = FileAccess::get_file_as_string(candidate);
	return true;
}
