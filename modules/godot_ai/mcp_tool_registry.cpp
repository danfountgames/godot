/**************************************************************************/
/*  mcp_tool_registry.cpp                                                 */
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

#include "mcp_tool_registry.h"

#include "core/error/error_macros.h"
#include "core/object/class_db.h"

MCPToolRegistry *MCPToolRegistry::singleton = nullptr;

MCPToolRegistry::MCPToolRegistry() {
	ERR_FAIL_COND_MSG(singleton != nullptr, "MCPToolRegistry is a singleton and was instantiated twice.");
	singleton = this;
}

MCPToolRegistry::~MCPToolRegistry() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

void MCPToolRegistry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("register_tool", "descriptor"), &MCPToolRegistry::register_tool_from_descriptor);
	ClassDB::bind_method(D_METHOD("unregister_tool", "name"), &MCPToolRegistry::unregister_tool);
	ClassDB::bind_method(D_METHOD("has_tool", "name"), &MCPToolRegistry::has_tool);
	ClassDB::bind_method(D_METHOD("get_tool_count"), &MCPToolRegistry::get_tool_count);
	ClassDB::bind_method(D_METHOD("get_tool_descriptors"), &MCPToolRegistry::get_tool_descriptors);
	ClassDB::bind_method(D_METHOD("get_tool_descriptor", "name"), &MCPToolRegistry::get_tool_descriptor);

	// Emitted whenever the tool set changes, so the service can send
	// notifications/tools/list_changed to every connected client.
	ADD_SIGNAL(MethodInfo("tools_changed"));
}

void MCPToolRegistry::_rebuild_sorted_names() {
	sorted_names.clear();
	for (const KeyValue<String, Ref<MCPTool>> &entry : tools) {
		sorted_names.push_back(entry.key);
	}
	sorted_names.sort();
}

Error MCPToolRegistry::register_tool(const Ref<MCPTool> &p_tool) {
	if (p_tool.is_null()) {
		ERR_PRINT("Cannot register a null MCP tool.");
		return ERR_INVALID_PARAMETER;
	}

	const String name = p_tool->get_tool_name();
	if (name.is_empty()) {
		ERR_PRINT("Cannot register an MCP tool with an empty name.");
		return ERR_INVALID_PARAMETER;
	}
	if (tools.has(name)) {
		// Silently replacing would let a late plugin hijack a built-in tool name.
		ERR_PRINT(vformat("An MCP tool named '%s' is already registered.", name));
		return ERR_ALREADY_EXISTS;
	}
	const Dictionary schema = p_tool->get_input_schema();
	if (schema.is_empty() || !schema.has("type")) {
		ERR_PRINT(vformat("MCP tool '%s' must declare an input schema with a 'type'.", name));
		return ERR_INVALID_PARAMETER;
	}

	tools[name] = p_tool;
	_rebuild_sorted_names();
	emit_signal(SNAME("tools_changed"));
	return OK;
}

bool MCPToolRegistry::unregister_tool(const String &p_name) {
	if (!tools.has(p_name)) {
		return false;
	}
	tools.erase(p_name);
	_rebuild_sorted_names();
	emit_signal(SNAME("tools_changed"));
	return true;
}

void MCPToolRegistry::clear() {
	if (tools.is_empty()) {
		return;
	}
	tools.clear();
	sorted_names.clear();
	emit_signal(SNAME("tools_changed"));
}

bool MCPToolRegistry::has_tool(const String &p_name) const {
	return tools.has(p_name);
}

Ref<MCPTool> MCPToolRegistry::get_tool(const String &p_name) const {
	const Ref<MCPTool> *found = tools.getptr(p_name);
	return found ? *found : Ref<MCPTool>();
}

Dictionary MCPToolRegistry::get_tool_descriptor(const String &p_name) const {
	Dictionary descriptor;
	const Ref<MCPTool> tool = get_tool(p_name);
	if (tool.is_null()) {
		return descriptor;
	}
	descriptor["name"] = tool->get_tool_name();
	descriptor["description"] = tool->get_description();
	descriptor["inputSchema"] = tool->get_input_schema();
	const Dictionary output_schema = tool->get_output_schema();
	if (!output_schema.is_empty()) {
		descriptor["outputSchema"] = output_schema;
	}
	// Godot-specific annotations travel in `_meta`, which MCP reserves for exactly
	// this, so a standard client can ignore them safely.
	Dictionary meta;
	meta["capability"] = mcp_capability_to_string(tool->get_capability());
	meta["mutating"] = tool->is_mutating();
	descriptor["_meta"] = meta;
	return descriptor;
}

Array MCPToolRegistry::get_tool_descriptors() const {
	Array descriptors;
	for (const String &name : sorted_names) {
		descriptors.push_back(get_tool_descriptor(name));
	}
	return descriptors;
}

Error MCPToolRegistry::register_tool_from_descriptor(const Dictionary &p_descriptor) {
	String error;
	Ref<MCPCallableTool> tool = MCPCallableTool::from_descriptor(p_descriptor, error);
	if (tool.is_null()) {
		ERR_PRINT(vformat("Could not register MCP tool: %s", error));
		return ERR_INVALID_PARAMETER;
	}
	return register_tool(tool);
}

Dictionary MCPToolRegistry::call_tool(const String &p_name, const Dictionary &p_arguments, MCPToolError &r_error) {
	r_error.clear();

	const Ref<MCPTool> tool = get_tool(p_name);
	if (tool.is_null()) {
		r_error.set(MCPToolError::NOT_FOUND, vformat("unknown tool '%s'", p_name));
		return Dictionary();
	}

	Variant validated;
	String validation_error;
	if (!MCPSchema::validate(p_arguments, tool->get_input_schema(), validated, validation_error)) {
		r_error.set(MCPToolError::INVALID_ARGUMENTS, validation_error);
		return Dictionary();
	}

	return tool->run(validated, r_error);
}
