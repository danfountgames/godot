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
#include "../mcp_agent_state.h"
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
		result["goal"] = MCPAgentIntent::get_goal();
		result["activity"] = MCPAgentIntent::get_activity();
		result["control"] = MCPAgentControl::to_dictionary();
		return result;
	}
};

// Declaring what the work is for.
//
// The dock's first two questions - what is the agent trying to achieve, and what is it
// doing right now - cannot be answered from tool names. `Godot_ManageNode(...)` does not
// say "measuring the current jump height". So the agent says it, and every call it makes
// afterwards carries what it said.
//
// This is a claim, not evidence. The audit log stays the record of what actually
// happened, and the dock shows the two side by side precisely so they can be compared.
class SetIntentTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_SetIntent"; }
	virtual String get_description() const override {
		return "Say what you are working towards and what you are doing right now, so the user "
			   "watching the Agent Activity dock can follow along. Set `goal` once when you start "
			   "a piece of work; update `activity` whenever you move to a different step. Both "
			   "are stamped onto every tool call you make afterwards. Write them the way you "
			   "would tell a person - 'Measuring the current jump height', not "
			   "'calling Godot_GetRuntimeProperty'. Do not narrate your reasoning here; this is "
			   "for what you are doing, and the audit log already records what you did.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["goal"] = MCPSchema::string_property(
				"The overall objective, in the user's terms. Omit to leave it unchanged.");
		properties["activity"] = MCPSchema::string_property(
				"What you are doing right now, in one line. Omit to leave it unchanged.");
		properties["clear"] = MCPSchema::bool_property(
				"Clear both, when the work is finished. A stale goal over unrelated work is "
				"worse than none.",
				false);
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["goal"] = MCPSchema::string_property("The goal now in effect.");
		properties["activity"] = MCPSchema::string_property("The activity now in effect.");
		properties["control_state"] = MCPSchema::string_property(
				"'running', 'paused' or 'stopped' - whether the user is letting you act.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		// get() rather than operator[]: a missing key read through a const Dictionary
		// inserts a null, which schema validation then rejects as wrongly typed.
		if ((bool)p_arguments.get("clear", false)) {
			MCPAgentIntent::clear();
		}
		if (p_arguments.has("goal")) {
			MCPAgentIntent::set_goal(p_arguments.get("goal", String()));
		}
		if (p_arguments.has("activity")) {
			MCPAgentIntent::set_activity(p_arguments.get("activity", String()));
		}

		Dictionary result = MCPAgentIntent::to_dictionary();
		result["control_state"] = MCPAgentControl::state_to_string(MCPAgentControl::get_state());
		return result;
	}
};

// Letting the agent hold itself - but never release itself.
//
// There is deliberately no 'resume' action. A user who presses Stop in the dock has to
// be able to rely on it, and a control the held party can lift is advisory rather than a
// control. The tool is also not on the always-allowed list, so once held, this too is
// refused: only the dock's Resume button starts the agent again.
//
// Pausing itself is still useful - an agent that has finished, or that wants a decision
// before doing something wide-ranging, can stop and say so.
class SetAgentControlTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_SetAgentControl"; }
	virtual String get_description() const override {
		return "Hold yourself: 'pause' if you want the user to look before you go on, 'stop' when "
			   "your work is finished or you have hit something you should not decide alone. "
			   "Give a reason - it is shown in the Agent Activity dock and returned in the "
			   "refusal of every call you make afterwards. There is no resume: only the user can "
			   "start you again, from the dock. Once held you cannot call this tool either, so do "
			   "not use it to pause for a moment.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		Vector<String> actions;
		actions.push_back("pause");
		actions.push_back("stop");
		properties["action"] = MCPSchema::enum_property(
				"'pause' to wait for the user, 'stop' when you are done.", actions);
		properties["reason"] = MCPSchema::string_property(
				"Why, in the user's terms. Shown in the dock.");
		Vector<String> required;
		required.push_back("action");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["state"] = MCPSchema::string_property("'paused' or 'stopped'.");
		properties["reason"] = MCPSchema::string_property("What was recorded.");
		properties["note"] = MCPSchema::string_property("How to get going again.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		const String action = p_arguments.get("action", String());
		const String reason = p_arguments.get("reason", String());
		if (action == "pause") {
			MCPAgentControl::pause(reason.is_empty() ? String("the agent paused itself") : reason);
		} else if (action == "stop") {
			MCPAgentControl::stop(reason.is_empty() ? String("the agent stopped itself") : reason);
		} else {
			r_error.set(MCPToolError::INVALID_ARGUMENTS,
					vformat("unknown action '%s'; expected 'pause' or 'stop'. There is no "
							"'resume': only the user can start you again.",
							action));
			return Dictionary();
		}

		Dictionary result = MCPAgentControl::to_dictionary();
		result["note"] = "You are held. Every call except reads that explain what you already did "
						 "will be refused until the user presses Resume in the Agent Activity "
						 "dock. You cannot lift this yourself.";
		return result;
	}
};

} // namespace

void mcp_register_activity_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->register_tool(Ref<MCPTool>(memnew(GetActivityTool)));
	registry->register_tool(Ref<MCPTool>(memnew(SetIntentTool)));
	registry->register_tool(Ref<MCPTool>(memnew(SetAgentControlTool)));
}
