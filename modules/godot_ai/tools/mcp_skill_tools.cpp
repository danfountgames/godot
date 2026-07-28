/**************************************************************************/
/*  mcp_skill_tools.cpp                                                   */
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

#include "mcp_builtin_tools.h"

#include "../mcp_skills.h"
#include "../mcp_tool_registry.h"

#include "core/variant/array.h"

namespace {

static bool find_skill(const String &p_name, MCPSkill &r_skill, MCPToolError &r_error) {
	for (const MCPSkill &skill : MCPSkills::discover()) {
		if (skill.name == p_name) {
			r_skill = skill;
			return true;
		}
	}
	r_error.set(MCPToolError::NOT_FOUND, vformat("no skill named '%s' was found", p_name));
	return false;
}

class ListSkillsTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_ListSkills"; }
	virtual String get_description() const override {
		return "List the workflow skills discovered in this project, the installed addons and "
			   "the user's skill folder. Skills are not usable until the user allows them.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["usable_only"] = MCPSchema::bool_property(
				"Return only skills that are allowed, enabled and match this editor version.", false);
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["skills"] = MCPSchema::array_property("Discovered skills.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		const bool usable_only = p_arguments["usable_only"];
		Array entries;
		for (const MCPSkill &skill : MCPSkills::discover()) {
			if (usable_only && !skill.is_usable()) {
				continue;
			}
			entries.push_back(skill.to_dictionary());
		}
		Dictionary result;
		result["skills"] = entries;
		return result;
	}
};

class ReadSkillTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_ReadSkill"; }
	virtual String get_description() const override {
		return "Read a skill's instructions, or one of its supporting files. Supporting files "
			   "are loaded on demand so a skill's instructions can reference them without "
			   "everything being loaded up front.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["name"] = MCPSchema::string_property("Skill name, as reported by Godot_ListSkills.");
		properties["resource"] = MCPSchema::string_property(
				"Optional supporting file relative to the skill folder, e.g. 'references/naming.md'.", "");
		Vector<String> required;
		required.push_back("name");
		return MCPSchema::object_schema(properties, required);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["name"] = MCPSchema::string_property("Skill name.");
		properties["text"] = MCPSchema::string_property("Instructions, or the supporting file's contents.");
		properties["resource"] = MCPSchema::string_property("Supporting file that was read, when one was requested.");
		properties["tools"] = MCPSchema::array_property("Tools the skill declares it needs.",
				MCPSchema::string_property("Tool name."));
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPSkill skill;
		if (!find_skill(p_arguments["name"], skill, r_error)) {
			return Dictionary();
		}

		const String resource = String(p_arguments["resource"]).strip_edges();
		String text;
		String error;
		if (resource.is_empty()) {
			if (!MCPSkills::read_instructions(skill, text, error)) {
				// A skill the user has not allowed is a permission decision, not a
				// missing file, and the message says how to change it.
				r_error.set(skill.allowed ? MCPToolError::INVALID_STATE : MCPToolError::PERMISSION_DENIED, error);
				return Dictionary();
			}
		} else if (!MCPSkills::read_resource(skill, resource, text, error)) {
			r_error.set(skill.allowed ? MCPToolError::NOT_FOUND : MCPToolError::PERMISSION_DENIED, error);
			return Dictionary();
		}

		Array tools;
		for (const String &tool : skill.tools) {
			tools.push_back(tool);
		}

		Dictionary result;
		result["name"] = skill.name;
		result["text"] = text;
		result["tools"] = tools;
		if (!resource.is_empty()) {
			result["resource"] = resource;
		}
		return result;
	}
};

} // namespace

void mcp_register_skill_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);

	registry->register_tool(Ref<MCPTool>(memnew(ListSkillsTool)));
	registry->register_tool(Ref<MCPTool>(memnew(ReadSkillTool)));
}
