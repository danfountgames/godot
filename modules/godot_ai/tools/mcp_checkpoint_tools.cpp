/**************************************************************************/
/*  mcp_checkpoint_tools.cpp                                              */
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

#include "../mcp_checkpoints.h"
#include "../mcp_tool_registry.h"

namespace {

class ListCheckpointsTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_ListCheckpoints"; }
	virtual String get_description() const override {
		return "List the file snapshots taken before tools changed the project, newest first. "
			   "Checkpoints cover files written by tools; use Godot_UndoLastAction for scene "
			   "edits that have not been saved yet.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }
	virtual Dictionary get_input_schema() const override { return MCPSchema::object_schema(Dictionary()); }
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["checkpoints"] = MCPSchema::array_property("Checkpoints, newest first.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		Dictionary result;
		result["checkpoints"] = MCPCheckpoints::list();
		return result;
	}
};

class RestoreCheckpointTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_RestoreCheckpoint"; }
	virtual String get_description() const override {
		return "Restore the files captured by a checkpoint: files that existed are put back "
			   "as they were, and files the tool created are removed again.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_FILES; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["id"] = MCPSchema::string_property("Checkpoint id, as reported by Godot_ListCheckpoints.");
		Vector<String> required;
		required.push_back("id");
		return MCPSchema::object_schema(properties, required);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["id"] = MCPSchema::string_property("Checkpoint that was restored.");
		properties["files_restored"] = MCPSchema::integer_property("Files put back to their previous contents.");
		properties["files_removed"] = MCPSchema::integer_property("Files removed because the tool had created them.");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		const String id = p_arguments["id"];
		int restored = 0;
		int removed = 0;
		String error;
		if (!MCPCheckpoints::restore(id, restored, removed, error)) {
			r_error.set(MCPToolError::NOT_FOUND, error);
			return Dictionary();
		}

		Dictionary result;
		result["id"] = id;
		result["files_restored"] = restored;
		result["files_removed"] = removed;
		return result;
	}
};

} // namespace

void mcp_register_checkpoint_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);

	registry->register_tool(Ref<MCPTool>(memnew(ListCheckpointsTool)));
	registry->register_tool(Ref<MCPTool>(memnew(RestoreCheckpointTool)));
}
