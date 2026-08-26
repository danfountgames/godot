/**************************************************************************/
/*  mcp_tool.h                                                            */
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

#ifndef MCP_TOOL_H
#define MCP_TOOL_H

#include "mcp_schema.h"
#include "mcp_types.h"

#include "core/object/ref_counted.h"
#include "core/variant/callable.h"

// Base class for everything callable over `tools/call`.
//
// A tool owns its own schema so discovery and execution can never disagree: the
// registry validates arguments with exactly the schema it advertises.
class MCPTool : public RefCounted {
	GDCLASS(MCPTool, RefCounted);

public:
	virtual String get_tool_name() const = 0;
	virtual String get_description() const = 0;
	virtual Dictionary get_input_schema() const = 0;
	virtual Dictionary get_output_schema() const { return Dictionary(); }
	virtual MCPCapability get_capability() const = 0;

	// A one-line summary for approval prompts and the audit log, built from the
	// actual arguments so a user approves a concrete action, not a category.
	virtual String describe_invocation(const Dictionary &p_arguments) const;

	// Project files this invocation may write, as res:// paths. The protocol layer
	// snapshots them before running the tool, so a mutating tool that returns nothing
	// here is declaring that it changes no files - which is true for scene edits that
	// live in the undo history until the scene is saved.
	virtual Vector<String> get_checkpoint_paths(const Dictionary &p_arguments) const { return Vector<String>(); }

	// The concrete things this invocation touches, as an Array of
	// { kind: "node" | "file", path: String }. The Activity dock reveals them in the
	// Scene and FileSystem docks, which is what turns "ran Godot_ManageNode" into
	// "renamed Main/Player/Sprite".
	//
	// The default reads the arguments heuristically (MCPActivity::extract_subjects).
	// Override wherever the tool knows better - a tool that resolves a node by search,
	// for instance, knows the path it settled on and the arguments do not carry it.
	virtual Array get_activity_subjects(const Dictionary &p_arguments) const;

	// Runs the tool. Arguments have already been schema-validated and defaulted.
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) = 0;

	// True when the tool changes persistent project state.
	bool is_mutating() const { return mcp_capability_is_mutating(get_capability()); }
};

// Tool implemented by a script (GDScript or C# editor plugin) through a Callable.
// This is the extension surface the specification requires, without giving plugins
// access to privileged internals.
class MCPCallableTool : public MCPTool {
	GDCLASS(MCPCallableTool, MCPTool);

	String name;
	String description;
	Dictionary input_schema;
	Dictionary output_schema;
	MCPCapability capability = MCP_CAP_READ_PROJECT;
	Callable handler;

public:
	virtual String get_tool_name() const override { return name; }
	virtual String get_description() const override { return description; }
	virtual Dictionary get_input_schema() const override { return input_schema; }
	virtual Dictionary get_output_schema() const override { return output_schema; }
	virtual MCPCapability get_capability() const override { return capability; }
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override;

	// Builds a tool from a script-supplied descriptor. Returns null and fills
	// r_error when the descriptor is not usable.
	static Ref<MCPCallableTool> from_descriptor(const Dictionary &p_descriptor, String &r_error);
};

#endif // MCP_TOOL_H
