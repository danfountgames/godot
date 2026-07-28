/**************************************************************************/
/*  mcp_output_tools.cpp                                                  */
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

#include "../mcp_tool_registry.h"

#include "core/variant/array.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"

namespace {

// The editor log aggregates the editor's own output and everything the running game
// prints back through the debugger, which is exactly what an agent needs to see
// after starting a scene.
static String message_type_name(EditorLog::MessageType p_type) {
	switch (p_type) {
		case EditorLog::MSG_TYPE_ERROR:
			return "error";
		case EditorLog::MSG_TYPE_WARNING:
			return "warning";
		case EditorLog::MSG_TYPE_EDITOR:
			return "editor";
		case EditorLog::MSG_TYPE_STD_RICH:
		case EditorLog::MSG_TYPE_STD:
		default:
			return "output";
	}
}

class ReadOutputLogTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_ReadOutputLog"; }
	virtual String get_description() const override {
		return "Read the editor's Output panel, which also carries what the running game "
			   "printed. Returns the most recent lines first-to-last, optionally filtered "
			   "to errors and warnings.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["limit"] = MCPSchema::integer_property(
				"How many of the most recent messages to return.", 100);
		properties["problems_only"] = MCPSchema::bool_property(
				"Return only errors and warnings.", false);
		properties["contains"] = MCPSchema::string_property(
				"Only return messages containing this text.", "");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary message_properties;
		message_properties["text"] = MCPSchema::string_property("Message text.");
		message_properties["type"] = MCPSchema::string_property("output, editor, warning or error.");
		message_properties["repeats"] = MCPSchema::integer_property("How many times it repeated consecutively.");

		Dictionary properties;
		properties["messages"] = MCPSchema::array_property("Messages, oldest first.",
				MCPSchema::object_schema(message_properties));
		properties["total"] = MCPSchema::integer_property("Total messages currently held by the log.");
		properties["truncated"] = MCPSchema::bool_property("True when older messages were left out.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!EditorNode::get_singleton() || !EditorNode::get_log()) {
			r_error.set(MCPToolError::UNSUPPORTED,
					"the editor output log is not available in this process");
			return Dictionary();
		}
		EditorLog *log = EditorNode::get_log();

		const int limit = MAX(1, (int)p_arguments["limit"]);
		const bool problems_only = p_arguments["problems_only"];
		const String contains = String(p_arguments["contains"]).strip_edges();

		// Collect matches first, then keep the newest `limit`: filtering after
		// truncating would return fewer matches than asked for.
		Array matched;
		const int total = log->get_message_count();
		for (int i = 0; i < total; i++) {
			const EditorLog::MessageType type = log->get_message_type(i);
			if (problems_only && type != EditorLog::MSG_TYPE_ERROR && type != EditorLog::MSG_TYPE_WARNING) {
				continue;
			}
			const String text = log->get_message_text(i);
			if (!contains.is_empty() && !text.contains(contains)) {
				continue;
			}

			Dictionary message;
			message["text"] = text;
			message["type"] = message_type_name(type);
			message["repeats"] = log->get_message_repeat_count(i);
			matched.push_back(message);
		}

		const bool truncated = matched.size() > limit;
		if (truncated) {
			matched = matched.slice(matched.size() - limit, matched.size());
		}

		Dictionary result;
		result["messages"] = matched;
		result["total"] = total;
		result["truncated"] = truncated;
		return result;
	}
};

} // namespace

void mcp_register_output_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);

	registry->register_tool(Ref<MCPTool>(memnew(ReadOutputLogTool)));
}
