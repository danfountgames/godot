/**************************************************************************/
/*  mcp_activity_tools.cpp                                                */
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

// Reading back what the agent has been doing.
//
// This exists as a tool and not only as a dock for one reason recorded in the
// experience spec (E7): anything reachable only through a UI cannot be regression
// tested in this repository. The end-to-end script asserts on this; the dock renders
// the same records.

#include "mcp_builtin_tools.h"

#include "../mcp_activity.h"
#include "../mcp_tool_registry.h"

namespace {

class GetActivityTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_GetActivity"; }
	virtual String get_description() const override {
		return "Return what this editor's AI service has been doing: one record per tool call, "
			   "with the capability it used, a one-line summary built from the real arguments, "
			   "the nodes and files it touched, how long it took, how it ended, and the "
			   "checkpoint id if one was taken. This is the live stream the Activity dock "
			   "renders - bounded and in memory, lost when the editor exits. The durable "
			   "record is the audit log on disk. Poll with `after_sequence` set to the highest "
			   "sequence you have already seen to get only what is new. Records that end "
			   "'deferred' were handed off to a tool that answers asynchronously; this stream "
			   "is not told when those finish, so their duration is the time up to the handoff "
			   "and not the time the work took.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["after_sequence"] = MCPSchema::integer_property(
				"Return only records newer than this sequence number. 0 reads from the start "
				"of what is still buffered.",
				0);
		properties["limit"] = MCPSchema::integer_property(
				"Most records to return, oldest first.", 100);
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["records"] = MCPSchema::array_property(
				"One entry per tool call, oldest first.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["count"] = MCPSchema::integer_property("How many records were returned.");
		properties["latest_sequence"] = MCPSchema::integer_property(
				"Sequence number of the newest record the service holds, whether or not it was "
				"returned here. Pass it back as `after_sequence` to poll.");
		properties["running"] = MCPSchema::bool_property(
				"True while at least one call is still in flight.");
		properties["capacity"] = MCPSchema::integer_property(
				"How many records the buffer keeps before dropping the oldest.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		// get() rather than operator[]: reading a missing key through a const Dictionary
		// inserts a null, which schema validation then rejects as wrongly typed.
		const int64_t after = p_arguments.get("after_sequence", 0);
		const int limit = MAX(1, (int)p_arguments.get("limit", 100));

		const Array records = MCPActivity::snapshot(after, limit);

		Dictionary result;
		result["records"] = records;
		result["count"] = records.size();
		result["latest_sequence"] = MCPActivity::get_latest_sequence();
		result["running"] = MCPActivity::has_running();
		result["capacity"] = MCPActivity::get_capacity();
		return result;
	}
};

} // namespace

void mcp_register_activity_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->register_tool(Ref<MCPTool>(memnew(GetActivityTool)));
}
