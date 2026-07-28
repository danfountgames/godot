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

#ifndef MCP_TOOL_REGISTRY_H
#define MCP_TOOL_REGISTRY_H

#include "mcp_schema.h"
#include "mcp_tool.h"

#include "core/object/object.h"
#include "core/templates/hash_map.h"

// The single authority on which tools exist, what they accept, and what they may
// touch. Discovery (`tools/list`) and execution (`tools/call`) are both served from
// here, so an advertised schema is always the schema that gets enforced.
//
// Exposed as an Engine singleton named "MCPToolRegistry" so editor plugins written
// in GDScript or C# can register tools without engine changes.
class MCPToolRegistry : public Object {
	GDCLASS(MCPToolRegistry, Object);

	static MCPToolRegistry *singleton;

	HashMap<String, Ref<MCPTool>> tools;
	// Sorted so `tools/list` is deterministic across sessions, which matters for
	// diffing transcripts and for cacheable client behaviour.
	Vector<String> sorted_names;

	void _rebuild_sorted_names();

protected:
	static void _bind_methods();

public:
	static MCPToolRegistry *get_singleton() { return singleton; }

	MCPToolRegistry();
	~MCPToolRegistry();

	// Returns ERR_ALREADY_EXISTS for a duplicate name, ERR_INVALID_PARAMETER for a
	// malformed tool.
	Error register_tool(const Ref<MCPTool> &p_tool);
	bool unregister_tool(const String &p_name);
	void clear();

	bool has_tool(const String &p_name) const;
	Ref<MCPTool> get_tool(const String &p_name) const;
	Vector<String> get_tool_names() const { return sorted_names; }
	int get_tool_count() const { return tools.size(); }

	// MCP `tools/list` payload: `name`, `description`, `inputSchema`, optional
	// `outputSchema`, plus Godot capability annotations under `_meta`.
	Array get_tool_descriptors() const;
	Dictionary get_tool_descriptor(const String &p_name) const;

	// Script-facing registration (Engine singleton).
	Error register_tool_from_descriptor(const Dictionary &p_descriptor);

	// Validates arguments against the tool's own schema before invoking it.
	Dictionary call_tool(const String &p_name, const Dictionary &p_arguments, MCPToolError &r_error);
};

#endif // MCP_TOOL_REGISTRY_H
