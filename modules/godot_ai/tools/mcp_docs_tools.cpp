/**************************************************************************/
/*  mcp_docs_tools.cpp                                                    */
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

#include "../mcp_docs.h"
#include "../mcp_schema.h"
#include "../mcp_tool_registry.h"

#include "core/variant/array.h"

namespace {

// The editor's own class reference, which the agent could not previously read.
//
// It matters more here than it would in a general coding assistant. This is a
// 4.8-dev fork; a model working from training data is recalling a different
// engine version, and it cannot recall the user's own script classes at all -
// those are in the same reference, generated from their code, which turns this
// from "look up Godot" into "look up this project".
//
// The whole tool is shaped around not returning too much. A class like Node3D has
// hundreds of members and dumping them is the same mistake as returning every
// memory note on every recall, so the default answer is a summary with member
// *names*, and full text is asked for.
class LookupClassTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_LookupClass"; }
	virtual String get_description() const override {
		return "Read the editor's class reference: this engine build's real API, plus the "
			   "documentation generated from the project's own scripts. Use it instead of "
			   "recalling an API from memory - this is a 4.8-dev fork and the project's own "
			   "classes are not something you can have seen before. Pass 'search' alone to find "
			   "classes by name or description, 'class_name' alone for a class summary with its "
			   "member names, both together to find members within a class, and 'member' for one "
			   "member's full signature and description.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["class_name"] = MCPSchema::string_property(
				"Class to look up, e.g. 'CharacterBody2D' or one of the project's own script classes.", "");
		properties["search"] = MCPSchema::string_property(
				"Substring to search for. Without class_name it searches class names and briefs; "
				"with one it searches that class's members.", "");
		properties["member"] = MCPSchema::string_property(
				"One method, property, signal or constant to return in full.", "");
		properties["include_inherited"] = MCPSchema::bool_property(
				"Include members inherited from base classes. Off by default: a leaf class's own "
				"members are usually what was meant, and the full chain is large.", false);
		properties["limit"] = MCPSchema::integer_property(
				"Maximum entries to return. Defaults to 40.", 40);
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["classes"] = MCPSchema::array_property("Matching classes, when searching.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["class_name"] = MCPSchema::string_property("The class that was looked up.");
		properties["inherits"] = MCPSchema::string_property("Its immediate base class.");
		properties["inheritance_chain"] = MCPSchema::array_property("Base classes, nearest first.",
				MCPSchema::string_property("Class name."));
		properties["api_type"] = MCPSchema::string_property(
				"Where the class comes from: core, editor, extension, or the project's own scripts.");
		properties["brief"] = MCPSchema::string_property("One-line description.");
		properties["description"] = MCPSchema::string_property("Full description, when a class was named.");
		properties["members"] = MCPSchema::array_property("Members, or the one requested in full.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["truncated"] = MCPSchema::bool_property("True when the limit clipped the answer.");
		properties["total"] = MCPSchema::integer_property("How many entries matched before the limit.");
		properties["note"] = MCPSchema::string_property("Guidance when the answer needs it.");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPDocs::Query query;
		query.class_name = String(p_arguments.get("class_name", String())).strip_edges();
		query.search = String(p_arguments.get("search", String())).strip_edges();
		query.member = String(p_arguments.get("member", String())).strip_edges();
		query.include_inherited = (bool)p_arguments.get("include_inherited", false);
		query.limit = MAX(1, (int)p_arguments.get("limit", MCPDocs::DEFAULT_LIMIT));

		Dictionary result;
		String error;
		switch (MCPDocs::lookup(query, result, error)) {
			case MCPDocs::RESULT_OK:
				return result;
			case MCPDocs::RESULT_NOT_FOUND:
				r_error.set(MCPToolError::NOT_FOUND, error);
				return Dictionary();
			case MCPDocs::RESULT_UNAVAILABLE:
				r_error.set(MCPToolError::UNSUPPORTED, error);
				return Dictionary();
			case MCPDocs::RESULT_INVALID:
			default:
				r_error.set(MCPToolError::INVALID_ARGUMENTS, error);
				return Dictionary();
		}
	}
};

} // namespace

void mcp_register_docs_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);

	registry->register_tool(Ref<MCPTool>(memnew(LookupClassTool)));
}
