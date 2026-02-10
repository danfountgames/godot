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

#include "mcp_progress.h"

#include "core/error/error_macros.h"
#include "core/string/print_string.h"
#include "modules/jsonrpc/jsonrpc.h"

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void MCPToolRegistry::register_tool(
		const String &p_name,
		const String &p_title,
		const String &p_description,
		const Dictionary &p_input_schema,
		const Dictionary &p_annotations,
		const Callable &p_handler) {
	ERR_FAIL_COND_MSG(tools.has(p_name),
			vformat("MCP tool already registered: %s", p_name));
	ERR_FAIL_COND_MSG(!p_handler.is_valid(),
			vformat("MCP tool handler is invalid for: %s", p_name));

	ToolDef def;
	def.name = p_name;
	def.title = p_title;
	def.description = p_description;
	def.input_schema = p_input_schema;
	def.annotations = p_annotations;
	def.handler = p_handler;
	tools.insert(p_name, def);

	print_verbose("[MCP] Tool registered: " + p_name);
}

void MCPToolRegistry::register_tool(
		const String &p_name,
		const String &p_title,
		const String &p_description,
		const Dictionary &p_input_schema,
		const Dictionary &p_annotations,
		const Callable &p_handler,
		ProgressHandler p_progress_handler) {
	// Delegate to the base overload, then set the progress handler.
	register_tool(p_name, p_title, p_description, p_input_schema,
			p_annotations, p_handler);
	if (tools.has(p_name)) {
		tools[p_name].progress_handler = p_progress_handler;
	}
}

// ---------------------------------------------------------------------------
// tools/list
// ---------------------------------------------------------------------------

Dictionary MCPToolRegistry::list_tools(const Dictionary &p_params) {
	// Build the MCP tools/list response.
	// MCP spec: { tools: [ { name, title, description, inputSchema, annotations }, ... ] }
	// Pagination: we return all tools at once with no nextCursor.

	Array tool_array;
	for (const KeyValue<String, ToolDef> &kv : tools) {
		const ToolDef &def = kv.value;

		Dictionary tool_dict;
		tool_dict["name"] = def.name;
		tool_dict["title"] = def.title;
		tool_dict["description"] = def.description;
		tool_dict["inputSchema"] = def.input_schema;
		tool_dict["annotations"] = def.annotations;
		tool_array.push_back(tool_dict);
	}

	Dictionary result;
	result["tools"] = tool_array;
	// No pagination -- omit nextCursor (null means "no more pages").
	return result;
}

// ---------------------------------------------------------------------------
// tools/call
// ---------------------------------------------------------------------------

Dictionary MCPToolRegistry::call_tool(const Dictionary &p_params) {
	// Extract tool name and arguments from the request params.
	// MCP spec: params = { "name": "tool/name", "arguments": { ... } }

	if (!p_params.has("name")) {
		// Return a JSON-RPC error (not a tool error -- this is a protocol violation).
		Dictionary err;
		err["code"] = JSONRPC::INVALID_PARAMS;
		err["message"] = "Missing required parameter: name";
		Dictionary response;
		response["error"] = err;
		return response;
	}

	String tool_name = p_params["name"];

	if (!tools.has(tool_name)) {
		// JSON-RPC Invalid Params -- tools/call is a valid method, but the tool name is not.
		Dictionary err;
		err["code"] = JSONRPC::INVALID_PARAMS;
		err["message"] = vformat("Unknown tool: %s", tool_name);
		Dictionary response;
		response["error"] = err;
		return response;
	}

	// Extract arguments (default to empty Dictionary if not provided).
	Dictionary arguments;
	if (p_params.has("arguments")) {
		arguments = p_params["arguments"];
	}

	// Call the tool handler.
	const ToolDef &def = tools[tool_name];
	Variant result;
	Callable::CallError ce;
	const Variant *argptrs[1];
	Variant args_variant = arguments;
	argptrs[0] = &args_variant;
	def.handler.callp(argptrs, 1, result, ce);

	if (ce.error != Callable::CallError::CALL_OK) {
		// Internal error -- the handler itself failed to be called.
		// Return as a tool error (isError:true), not a JSON-RPC error,
		// so the client knows the tool was found but execution failed.
		Dictionary content_item;
		content_item["type"] = "text";
		content_item["text"] = vformat("Internal error calling tool %s: call error %d",
				tool_name, (int)ce.error);
		Array content;
		content.push_back(content_item);

		Dictionary tool_result;
		tool_result["content"] = content;
		tool_result["isError"] = true;
		return tool_result;
	}

	// The handler returns a Dictionary in MCP tool result format.
	// Pass it through directly.
	if (result.get_type() == Variant::DICTIONARY) {
		return result;
	}

	// Unexpected return type -- wrap in an error.
	Dictionary content_item;
	content_item["type"] = "text";
	content_item["text"] = vformat("Tool %s returned unexpected type: %s",
			tool_name, Variant::get_type_name(result.get_type()));
	Array content;
	content.push_back(content_item);

	Dictionary tool_result;
	tool_result["content"] = content;
	tool_result["isError"] = true;
	return tool_result;
}

// ---------------------------------------------------------------------------
// Progress-aware dispatch
// ---------------------------------------------------------------------------

bool MCPToolRegistry::is_long_running_tool(const String &p_name) const {
	// Tools that typically take >500ms and benefit from SSE streaming.
	// Checked when the client sends Accept: text/event-stream but does NOT
	// provide a progressToken.
	static const char *long_running_tools[] = {
		"gdscript/check_all",
		"debug/get_scene_tree",
		"debug/browse_scene_tree",
		"debug/evaluate",
		"debug/get_session_summary",
		"debug/get_node_properties",
		"debug/get_performance",
		"debug/ui_get_control_info",
		"debug/ui_set_text",
		"debug/ui_get_text",
		"debug/ui_set_range_value",
		"debug/ui_get_range_value",
		"debug/ui_select_option",
		"debug/ui_get_options",
		"debug/ui_set_tab",
		"debug/ui_get_tabs",
		"debug/ui_set_checked",
		"debug/ui_focus",
		"debug/send_key",
		"debug/send_joypad",
		"debug/type_text",
		"debug/send_input_sequence",
		"debug/get_held_inputs",
		"debug/get_break_state",
		"debug/step",
		"debug/get_breakpoints",
		"debug/get_node_signals",
		"debug/emit_signal",
		"debug/set_node_property",
		nullptr // Sentinel.
	};

	for (int i = 0; long_running_tools[i]; i++) {
		if (p_name == long_running_tools[i]) {
			return true;
		}
	}
	return false;
}

Dictionary MCPToolRegistry::call_tool_with_progress(
		const String &p_name, const Dictionary &p_arguments,
		ProgressContext *p_ctx) {
	// Use the registered progress handler if available.
	if (tools.has(p_name) && tools[p_name].progress_handler) {
		return tools[p_name].progress_handler(p_arguments, p_ctx);
	}

	// Fallback: standard dispatch for tools without progress support.
	Dictionary params;
	params["name"] = p_name;
	params["arguments"] = p_arguments;
	return call_tool(params);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool MCPToolRegistry::has_tool(const String &p_name) const {
	return tools.has(p_name);
}

int MCPToolRegistry::get_tool_count() const {
	return tools.size();
}
