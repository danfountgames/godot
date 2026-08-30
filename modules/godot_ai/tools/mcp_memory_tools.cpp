/**************************************************************************/
/*  mcp_memory_tools.cpp                                                  */
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

#include "../mcp_project_memory.h"
#include "../mcp_tool_registry.h"

#include "core/variant/array.h"

namespace {

class RecallProjectMemoryTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_RecallProjectMemory"; }
	virtual String get_description() const override {
		return "Read what previous sessions recorded about this project: its conventions, which "
			   "node owns what, decisions already taken, and problems already hit. Call this "
			   "before investigating a project you have not seen this session - it is cheaper "
			   "than rediscovering the same facts. With no name it returns an index of subjects "
			   "and one-line summaries; pass a name to read that note in full.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["name"] = MCPSchema::string_property(
				"Note to read in full, as reported in the index. Omit for the index.", "");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["notes"] = MCPSchema::array_property("The index, or the single requested note.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["count"] = MCPSchema::integer_property("How many notes the store holds.");
		properties["capacity"] = MCPSchema::integer_property("How many it can hold.");
		properties["note"] = MCPSchema::string_property("Guidance when the store is empty or full.");
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		const String name = String(p_arguments.get("name", String())).strip_edges();

		Dictionary result;
		result["capacity"] = MCPProjectMemory::MAX_NOTES;

		if (!name.is_empty()) {
			MCPProjectMemory::Note note;
			String error;
			if (!MCPProjectMemory::read(name, note, error)) {
				r_error.set(MCPToolError::NOT_FOUND, error);
				return Dictionary();
			}
			Array notes;
			notes.push_back(note.to_dictionary(true));
			result["notes"] = notes;
			result["count"] = 1;
			return result;
		}

		String error;
		const Vector<MCPProjectMemory::Note> stored = MCPProjectMemory::list(error);
		if (!error.is_empty()) {
			r_error.set(MCPToolError::INVALID_STATE, error);
			return Dictionary();
		}

		Array notes;
		for (const MCPProjectMemory::Note &note : stored) {
			notes.push_back(note.to_dictionary(false));
		}
		result["notes"] = notes;
		result["count"] = stored.size();
		if (stored.is_empty()) {
			// An empty store is the normal state of a new project, and saying so beats
			// letting a caller read the empty list as a failure.
			result["note"] = "Nothing has been recorded about this project yet. When you learn "
							 "something that will still be true next month, record it with "
							 "Godot_UpdateProjectMemory.";
		} else if (stored.size() >= MCPProjectMemory::MAX_NOTES) {
			result["note"] = "The store is full. Merge or forget something before recording more.";
		}
		return result;
	}
};

class UpdateProjectMemoryTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_UpdateProjectMemory"; }
	virtual String get_description() const override {
		return "Record something about this project for later sessions, or forget something that "
			   "is no longer true. Memory is for standing facts - conventions, ownership, "
			   "decisions, traps - not for a transcript of what you just did; the activity log "
			   "already holds that. Writing to a name that exists replaces it, which is how a "
			   "fact that changed gets corrected.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_EDIT_FILES; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		Vector<String> actions;
		actions.push_back("remember");
		actions.push_back("forget");
		properties["action"] = MCPSchema::enum_property("Whether to record or remove a note.", actions, "remember");
		properties["name"] = MCPSchema::string_property(
				"Short identifier for the note, e.g. 'player-movement'. Reused to replace or forget it.");
		properties["subject"] = MCPSchema::string_property(
				"One line saying what the note is about. Shown in the index. Required to remember.", "");
		properties["body"] = MCPSchema::string_property(
				"What to remember. Required to remember.", "");
		Vector<String> required;
		required.push_back("name");
		return MCPSchema::object_schema(properties, required);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["action"] = MCPSchema::string_property("What was done.");
		properties["name"] = MCPSchema::string_property("The note's identifier, after normalisation.");
		properties["subject"] = MCPSchema::string_property("The note's subject.");
		properties["updated"] = MCPSchema::string_property("When it was written, UTC.");
		properties["path"] = MCPSchema::string_property("Where it lives in the project.");
		properties["count"] = MCPSchema::integer_property("How many notes the store now holds.");
		return MCPSchema::object_schema(properties);
	}
	virtual Vector<String> get_checkpoint_paths(const Dictionary &p_arguments) const override {
		// Declared so the protocol layer snapshots the note before it is replaced: a
		// memory write is an edit to the project like any other, and a correction that
		// turns out to be wrong has to be revertible.
		Vector<String> paths;
		String res_path;
		String error;
		if (MCPProjectMemory::note_path(String(p_arguments.get("name", String())), res_path, error)) {
			paths.push_back(res_path);
		}
		return paths;
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		const String action = String(p_arguments.get("action", "remember"));
		const String name = String(p_arguments["name"]);

		Dictionary result;
		result["action"] = action;

		if (action == "forget") {
			String error;
			if (!MCPProjectMemory::erase(name, error)) {
				r_error.set(MCPToolError::NOT_FOUND, error);
				return Dictionary();
			}
			result["name"] = MCPProjectMemory::slugify(name);
		} else {
			MCPProjectMemory::Note note;
			String error;
			if (!MCPProjectMemory::write(name,
						String(p_arguments.get("subject", String())),
						String(p_arguments.get("body", String())), note, error)) {
				r_error.set(MCPToolError::INVALID_ARGUMENTS, error);
				return Dictionary();
			}
			String path_error;
			String res_path;
			MCPProjectMemory::note_path(name, res_path, path_error);
			result["name"] = note.name;
			result["subject"] = note.subject;
			result["updated"] = note.updated;
			result["path"] = res_path;
		}

		String list_error;
		result["count"] = MCPProjectMemory::list(list_error).size();
		return result;
	}
};

} // namespace

void mcp_register_memory_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);

	registry->register_tool(Ref<MCPTool>(memnew(RecallProjectMemoryTool)));
	registry->register_tool(Ref<MCPTool>(memnew(UpdateProjectMemoryTool)));
}
