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
		return "List the file snapshots taken before tools changed the project, newest first, "
			   "and the tasks they group into. Checkpoints cover files written by tools; use "
			   "Godot_UndoLastAction for scene edits that have not been saved yet.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }
	virtual Dictionary get_input_schema() const override { return MCPSchema::object_schema(Dictionary()); }
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["checkpoints"] = MCPSchema::array_property("Checkpoints, newest first.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["tasks"] = MCPSchema::array_property(
				"The tasks those checkpoints belong to, most recently active first, with how "
				"many each holds. Godot_RestoreCheckpoint undoes one of these whole.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		Dictionary result;
		result["checkpoints"] = MCPCheckpoints::list();
		result["tasks"] = MCPCheckpoints::list_tasks();
		return result;
	}
};

class RestoreCheckpointTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_RestoreCheckpoint"; }
	virtual String get_description() const override {
		return "Undo what a tool wrote: files that existed are put back as they were, and "
			   "files the tool created are removed again. Pass 'id' for one call, or 'task' "
			   "to undo a whole task at once - every checkpoint taken while that goal was "
			   "current, back to the state before it began. Undoing a twelve-call session "
			   "one checkpoint at a time is what 'task' exists to avoid.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_FILES; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["id"] = MCPSchema::string_property(
				"Checkpoint id, as reported by Godot_ListCheckpoints.", "");
		properties["task"] = MCPSchema::string_property(
				"Task to undo entirely, as reported by Godot_ListCheckpoints. Mutually "
				"exclusive with 'id'.", "");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["id"] = MCPSchema::string_property("Checkpoint that was restored, when one was named.");
		properties["task"] = MCPSchema::string_property("Task that was undone, when one was named.");
		properties["checkpoints_restored"] = MCPSchema::integer_property(
				"How many checkpoints were rolled back. One, unless a task was named.");
		properties["files_restored"] = MCPSchema::integer_property("Files put back to their previous contents.");
		properties["files_removed"] = MCPSchema::integer_property("Files removed because the tool had created them.");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		// get(), not subscript: reading a missing key through a const Dictionary
		// inserts a null, and the call is then rejected as wrongly typed.
		const String id = String(p_arguments.get("id", String())).strip_edges();
		const String task = String(p_arguments.get("task", String())).strip_edges();

		if (id.is_empty() == task.is_empty()) {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					id.is_empty() ? "name a checkpoint 'id' or a 'task' to undo"
								  : "name either a checkpoint 'id' or a 'task', not both: they "
									"undo different amounts and the difference is the point");
			return Dictionary();
		}

		int checkpoints = 1;
		int restored = 0;
		int removed = 0;
		String error;
		if (task.is_empty()) {
			if (!MCPCheckpoints::restore(id, restored, removed, error)) {
				r_error.set(MCPToolError::NOT_FOUND, error);
				return Dictionary();
			}
		} else if (!MCPCheckpoints::restore_task(task, checkpoints, restored, removed, error)) {
			r_error.set(MCPToolError::NOT_FOUND, error);
			return Dictionary();
		}

		Dictionary result;
		if (task.is_empty()) {
			result["id"] = id;
		} else {
			result["task"] = task;
		}
		result["checkpoints_restored"] = checkpoints;
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
