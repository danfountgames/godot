/**************************************************************************/
/*  mcp_tool.cpp                                                          */
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

#include "mcp_tool.h"

#include "mcp_activity.h"

#include "core/io/json.h"

Array MCPTool::get_activity_subjects(const Dictionary &p_arguments) const {
	return MCPActivity::extract_subjects(p_arguments);
}

String MCPTool::describe_invocation(const Dictionary &p_arguments) const {
	if (p_arguments.is_empty()) {
		return get_tool_name() + "()";
	}
	// Compact and bounded: approval prompts and audit lines must stay readable.
	String summary;
	const Array keys = p_arguments.keys();
	for (int i = 0; i < keys.size(); i++) {
		if (i > 0) {
			summary += ", ";
		}
		const String key = keys[i];
		String value;
		if (mcp_is_sensitive_key(key)) {
			// Redacted at the source, so no downstream consumer has to remember to.
			value = "\"<redacted>\"";
		} else {
			value = JSON::stringify(p_arguments[key]);
			if (value.length() > 80) {
				value = value.substr(0, 77) + "...";
			}
		}
		summary += key + "=" + value;
	}
	return get_tool_name() + "(" + summary + ")";
}

Dictionary MCPCallableTool::run(const Dictionary &p_arguments, MCPToolError &r_error) {
	if (!handler.is_valid()) {
		r_error.set(MCPToolError::INVALID_STATE,
				vformat("tool '%s' no longer has a valid handler; its plugin was probably unloaded", name));
		return Dictionary();
	}

	const Variant argument = p_arguments;
	const Variant *argument_pointer = &argument;
	Variant result;
	Callable::CallError call_error;
	handler.callp(&argument_pointer, 1, result, call_error);

	if (call_error.error != Callable::CallError::CALL_OK) {
		r_error.set(MCPToolError::FAILED,
				vformat("tool '%s' handler failed: %s", name,
						Variant::get_callable_error_text(handler, &argument_pointer, 1, call_error)));
		return Dictionary();
	}

	// A handler may report a failure by returning a dictionary with `error`.
	if (result.get_type() == Variant::DICTIONARY) {
		const Dictionary result_dictionary = result;
		if (result_dictionary.has("error")) {
			r_error.set(MCPToolError::FAILED, String(result_dictionary["error"]));
			return Dictionary();
		}
		return result_dictionary;
	}

	if (result.get_type() == Variant::NIL) {
		return Dictionary();
	}

	// Non-dictionary results are wrapped so the protocol layer always has an object.
	Dictionary wrapped;
	wrapped["value"] = result;
	return wrapped;
}

Ref<MCPCallableTool> MCPCallableTool::from_descriptor(const Dictionary &p_descriptor, String &r_error) {
	Ref<MCPCallableTool> tool;

	if (!p_descriptor.has("name") || String(p_descriptor["name"]).is_empty()) {
		r_error = "tool descriptor requires a non-empty 'name'";
		return tool;
	}
	if (!p_descriptor.has("handler")) {
		r_error = "tool descriptor requires a 'handler' Callable";
		return tool;
	}
	const Variant handler_value = p_descriptor["handler"];
	if (handler_value.get_type() != Variant::CALLABLE) {
		r_error = "tool descriptor 'handler' must be a Callable";
		return tool;
	}
	const Callable handler_callable = handler_value;
	if (!handler_callable.is_valid()) {
		r_error = "tool descriptor 'handler' is not a valid Callable";
		return tool;
	}

	MCPCapability capability = MCP_CAP_READ_PROJECT;
	if (p_descriptor.has("capability")) {
		if (!mcp_capability_from_string(p_descriptor["capability"], capability)) {
			r_error = vformat("unknown capability '%s'", String(p_descriptor["capability"]));
			return tool;
		}
	}
	// Plugins must not be able to grant themselves unrestricted reach.
	if (capability == MCP_CAP_DANGEROUS_EXEC) {
		r_error = "the 'dangerous_exec' capability cannot be claimed by a registered tool";
		return tool;
	}

	const Variant input_schema_value = p_descriptor.has("input_schema") ? p_descriptor["input_schema"] : Variant();
	if (input_schema_value.get_type() != Variant::NIL && input_schema_value.get_type() != Variant::DICTIONARY) {
		r_error = "tool descriptor 'input_schema' must be a Dictionary";
		return tool;
	}

	tool.instantiate();
	tool->name = p_descriptor["name"];
	tool->description = p_descriptor.has("description") ? String(p_descriptor["description"]) : String();
	tool->capability = capability;
	tool->handler = handler_callable;

	if (input_schema_value.get_type() == Variant::DICTIONARY) {
		tool->input_schema = input_schema_value;
	} else {
		// An unspecified schema means "no arguments", not "anything goes".
		tool->input_schema = MCPSchema::object_schema(Dictionary());
	}
	if (p_descriptor.has("output_schema") && Variant(p_descriptor["output_schema"]).get_type() == Variant::DICTIONARY) {
		tool->output_schema = p_descriptor["output_schema"];
	}
	return tool;
}
