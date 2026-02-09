/**************************************************************************/
/*  mcp_tool_registry.h                                                   */
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
#include "core/variant/array.h"
#include "core/variant/callable.h"
#include "core/variant/dictionary.h"

class MCPToolRegistry {
public:
	struct ToolDef {
		String name;
		String title;
		String description;
		Dictionary input_schema; // JSON Schema object.
		Dictionary annotations; // MCP annotations: readOnlyHint, destructiveHint, etc.
		Callable handler; // fn(Dictionary args) -> Dictionary result.
	};

private:
	HashMap<String, ToolDef> tools;

public:
	// Register a tool. The handler callable must accept a single Dictionary
	// argument (the tool's arguments from the client) and return a Dictionary
	// in MCP tool result format: {content:[], isError:bool, structuredContent:{}}.
	void register_tool(
			const String &p_name,
			const String &p_title,
			const String &p_description,
			const Dictionary &p_input_schema,
			const Dictionary &p_annotations,
			const Callable &p_handler);

	// JSON-RPC handler for "tools/list". Returns the tool list per MCP spec.
	// Accepts params with optional "cursor" for pagination (we return all at once).
	Dictionary list_tools(const Dictionary &p_params);

	// JSON-RPC handler for "tools/call". Dispatches to the named tool's handler.
	// Accepts params: { "name": "tool/name", "arguments": { ... } }
	// Returns the tool result dictionary, or a JSON-RPC error if tool not found.
	Dictionary call_tool(const Dictionary &p_params);

	bool has_tool(const String &p_name) const;
	int get_tool_count() const;
};
