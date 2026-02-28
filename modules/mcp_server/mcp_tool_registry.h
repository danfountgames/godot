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

struct ProgressContext;

class MCPToolRegistry {
public:
	// Function pointer type for progress-aware tool handlers.
	// ProgressContext cannot be passed through Godot's Callable system,
	// so we use a raw function pointer for the progress-aware path.
	typedef Dictionary (*ProgressHandler)(const Dictionary &, ProgressContext *);

	// Permission check callback. Called before every tool execution and during
	// list_tools() to filter tools the current session may not access.
	// Returns true if the tool is allowed; false to deny (sets r_deny_reason).
	typedef bool (*PermissionCheckFn)(const String &p_tool_name, void *p_userdata, String &r_deny_reason);

	struct ToolDef {
		String name;
		String title;
		String description;
		Dictionary input_schema; // JSON Schema object.
		Dictionary annotations; // MCP annotations: readOnlyHint, destructiveHint, etc.
		Callable handler; // fn(Dictionary args) -> Dictionary result.
		ProgressHandler progress_handler = nullptr; // Optional progress-aware handler.
	};

private:
	HashMap<String, ToolDef> tools;
	HashMap<String, String> aliases; // alias name → canonical tool name.

	// Permission check (set by MCPProtocol to enforce per-session tool access).
	PermissionCheckFn permission_check = nullptr;
	void *permission_check_userdata = nullptr;

	// Find the closest matching tool name for "did you mean?" suggestions.
	String _find_closest_tool(const String &p_name) const;

	// Resolve a tool name through the alias table. Returns the canonical
	// name if an alias exists, or the input unchanged if not.
	String _resolve_alias(const String &p_name) const;

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

	// Overload with optional progress-aware handler for long-running tools.
	void register_tool(
			const String &p_name,
			const String &p_title,
			const String &p_description,
			const Dictionary &p_input_schema,
			const Dictionary &p_annotations,
			const Callable &p_handler,
			ProgressHandler p_progress_handler);

	// Register an alias that resolves to a canonical tool name.
	// When tools/call receives the alias, it transparently dispatches
	// to the canonical tool. Aliases do NOT appear in tools/list.
	void register_alias(const String &p_alias, const String &p_canonical);

	// JSON-RPC handler for "tools/list". Returns the tool list per MCP spec.
	// Accepts params with optional "cursor" for pagination (we return all at once).
	Dictionary list_tools(const Dictionary &p_params);

	// JSON-RPC handler for "tools/call". Dispatches to the named tool's handler.
	// Resolves aliases before lookup. On miss, suggests closest match.
	// Accepts params: { "name": "tool/name", "arguments": { ... } }
	// Returns the tool result dictionary, or a JSON-RPC error if tool not found.
	Dictionary call_tool(const Dictionary &p_params);

	// Dispatch with progress context. Uses the registered progress_handler
	// if available. Falls back to call_tool() for tools without one.
	Dictionary call_tool_with_progress(const String &p_name,
			const Dictionary &p_arguments, ProgressContext *p_ctx);

	// Returns true for tools that benefit from SSE streaming even without
	// an explicit progressToken from the client (>500ms expected duration).
	bool is_long_running_tool(const String &p_name) const;

	bool has_tool(const String &p_name) const;
	int get_tool_count() const;

	// Install a permission check that gates every tool call and filters
	// list_tools(). The check is called with the resolved tool name.
	// Caller is responsible for setting/clearing any per-request context
	// that the callback reads (e.g. session ID).
	void set_permission_check(PermissionCheckFn p_fn, void *p_userdata);
};
