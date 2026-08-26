/**************************************************************************/
/*  mcp_workspace_tools.cpp                                               */
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

// Driving the GodotAI workspace: several games at once, each addressable on its own.
//
// Everything here addresses **one registered instance**. That is the difference from
// Godot_PlayMainScene and Godot_StopPlaying, which drive the editor's single current
// run - the one a human pressing play also gets. An agent comparing three variants must
// be able to pause one of them without pausing the user's game, and these tools are how.

#include "mcp_builtin_tools.h"

#include "../mcp_runtime_instances.h"
#include "../mcp_tool_registry.h"
#include "../mcp_workspace.h"

#include "core/variant/array.h"

namespace {

MCPWorkspace *workspace_or_null() {
	MCPWorkspacePlugin *plugin = mcp_workspace_get_plugin();
	return plugin ? plugin->get_workspace() : nullptr;
}

Dictionary instance_reply(const String &p_instance_id) {
	MCPRuntimeInstances::Instance instance;
	if (!MCPRuntimeInstances::get(p_instance_id, instance)) {
		return Dictionary();
	}
	return MCPRuntimeInstances::to_dictionary(instance);
}

class LaunchInstanceTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_LaunchInstance"; }
	virtual String get_description() const override {
		return "Start a game in the GodotAI workspace, as its own tile beside any others. Use "
			   "this instead of Godot_PlayMainScene when you need more than one game at once - "
			   "three tunings to compare, a server and its clients, or a test that must not "
			   "disturb the game the user is playing. Each instance gets an id; every other "
			   "workspace tool addresses one id, so pausing or stopping one leaves the rest and "
			   "the user's own run alone. Needs a display that can embed windows.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_RUN_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["label"] = MCPSchema::string_property(
				"What to call this instance in the workspace, for example 'Jump Variant B'.");
		properties["role"] = MCPSchema::string_property(
				"What it is for: 'candidate', 'client', 'server', 'baseline'.", "candidate");
		properties["task"] = MCPSchema::string_property("The work this instance belongs to.");
		properties["scene"] = MCPSchema::string_property(
				"Scene to run, as a res:// path. Empty runs the project's main scene.");
		Vector<String> retentions;
		retentions.push_back("ephemeral");
		retentions.push_back("interactive");
		retentions.push_back("persistent");
		retentions.push_back("background");
		properties["retention"] = MCPSchema::enum_property(
				"How long it should live. 'ephemeral' is a quick test, 'interactive' is one the "
				"user is expected to look at, 'persistent' is a long playtest that only an "
				"explicit stop ends.",
				retentions, "ephemeral");
		Vector<String> required;
		required.push_back("label");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["instance_id"] = MCPSchema::string_property("Address this instance by this id.");
		properties["label"] = MCPSchema::string_property("What it is called.");
		properties["lifecycle"] = MCPSchema::string_property("Where it got to.");
		properties["pid"] = MCPSchema::integer_property("Its process id.");
		properties["live_count"] = MCPSchema::integer_property("Agent instances now running.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPWorkspace *workspace = workspace_or_null();
		if (!workspace) {
			r_error.set(MCPToolError::UNSUPPORTED,
					"'Godot_LaunchInstance' needs a running Godot editor with the GodotAI "
					"workspace");
			return Dictionary();
		}
		// get() rather than operator[]: a missing key read through a const Dictionary
		// inserts a null, which schema validation then rejects as wrongly typed.
		const String retention_name = p_arguments.get("retention", "ephemeral");
		MCPRuntimeInstances::Retention retention = MCPRuntimeInstances::RETENTION_EPHEMERAL;
		if (retention_name == "interactive") {
			retention = MCPRuntimeInstances::RETENTION_INTERACTIVE;
		} else if (retention_name == "persistent") {
			retention = MCPRuntimeInstances::RETENTION_PERSISTENT;
		} else if (retention_name == "background") {
			retention = MCPRuntimeInstances::RETENTION_BACKGROUND;
		}

		String error;
		const String instance_id = MCPWorkspaceLauncher::launch(workspace,
				p_arguments.get("label", String()), p_arguments.get("role", "candidate"),
				p_arguments.get("task", String()), p_arguments.get("scene", String()),
				retention, error);
		if (instance_id.is_empty()) {
			r_error.set(MCPToolError::FAILED, error);
			return Dictionary();
		}

		Dictionary result = instance_reply(instance_id);
		if (!error.is_empty()) {
			// Registered but did not reach a running state. The id is still returned so
			// the caller can read its lifecycle rather than being told only "failed".
			result["error"] = error;
		}
		result["live_count"] = MCPRuntimeInstances::live().size();
		return result;
	}
};

class ListInstancesTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_ListInstances"; }
	virtual String get_description() const override {
		return "List the game instances this agent owns, running and finished, with what each is "
			   "for, whether the agent or the user is controlling it, and its process id. Does "
			   "not list the game the user started themselves - that one is deliberately not the "
			   "agent's to touch.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["live_only"] = MCPSchema::bool_property(
				"Only instances with a running process.", false);
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["instances"] = MCPSchema::array_property("One entry per instance.",
				MCPSchema::object_schema(Dictionary(), Vector<String>(), true));
		properties["count"] = MCPSchema::integer_property("How many were returned.");
		properties["live_count"] = MCPSchema::integer_property("How many are running.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		const bool live_only = p_arguments.get("live_only", false);
		Array instances;
		for (const MCPRuntimeInstances::Instance &instance :
				(live_only ? MCPRuntimeInstances::live() : MCPRuntimeInstances::list())) {
			instances.push_back(MCPRuntimeInstances::to_dictionary(instance));
		}
		Dictionary result;
		result["instances"] = instances;
		result["count"] = instances.size();
		result["live_count"] = MCPRuntimeInstances::live().size();
		return result;
	}
};

class ControlInstanceTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_ControlInstance"; }
	virtual String get_description() const override {
		return "Pause, resume, step, change the speed of, mute or stop **one** instance. The "
			   "editor's own pause and speed controls reach every running game at once; these "
			   "reach exactly the instance you name, which is what makes comparing several of "
			   "them possible. An instance the user has taken control of still responds to these, "
			   "but you should leave it alone until they hand it back.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_RUN_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["instance_id"] = MCPSchema::string_property("Which instance, from Godot_ListInstances.");
		Vector<String> actions;
		actions.push_back("pause");
		actions.push_back("resume");
		actions.push_back("next_frame");
		actions.push_back("time_scale");
		actions.push_back("mute");
		actions.push_back("unmute");
		actions.push_back("stop");
		properties["action"] = MCPSchema::enum_property("What to do to it.", actions);
		properties["scale"] = MCPSchema::number_property(
				"For 'time_scale': how fast to run, where 1.0 is normal.", 1.0);
		Vector<String> required;
		required.push_back("instance_id");
		required.push_back("action");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["instance_id"] = MCPSchema::string_property("Instance that was addressed.");
		properties["action"] = MCPSchema::string_property("What was done.");
		properties["applied"] = MCPSchema::bool_property("True when the instance received it.");
		properties["lifecycle"] = MCPSchema::string_property("Its state afterwards.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		const String instance_id = p_arguments.get("instance_id", String());
		const String action = p_arguments.get("action", String());
		if (!MCPRuntimeInstances::exists(instance_id)) {
			r_error.set(MCPToolError::NOT_FOUND,
					vformat("no instance '%s'; Godot_ListInstances shows what there is", instance_id));
			return Dictionary();
		}

		bool applied = false;
		if (action == "pause") {
			applied = MCPRuntimeInstances::set_suspended(instance_id, true);
		} else if (action == "resume") {
			applied = MCPRuntimeInstances::set_suspended(instance_id, false);
		} else if (action == "next_frame") {
			applied = MCPRuntimeInstances::next_frame(instance_id);
		} else if (action == "time_scale") {
			applied = MCPRuntimeInstances::set_time_scale(instance_id, p_arguments.get("scale", 1.0));
		} else if (action == "mute") {
			applied = MCPRuntimeInstances::set_muted(instance_id, true);
		} else if (action == "unmute") {
			applied = MCPRuntimeInstances::set_muted(instance_id, false);
		} else if (action == "stop") {
			applied = MCPWorkspaceLauncher::stop(instance_id);
		} else {
			r_error.set(MCPToolError::INVALID_ARGUMENTS, vformat("unknown action '%s'", action));
			return Dictionary();
		}

		Dictionary result = instance_reply(instance_id);
		result["action"] = action;
		result["applied"] = applied;
		if (!applied) {
			// Never silently a no-op: an unreachable instance must not look controlled.
			result["note"] = "The instance did not receive this. Its debugger session may not be "
							 "connected yet, or the process has already gone - read `lifecycle`.";
		}
		return result;
	}
};

class StopAllInstancesTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_StopAllInstances"; }
	virtual String get_description() const override {
		return "Stop every game this agent launched. The game the user started themselves is not "
			   "touched, because it was never registered as the agent's.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_RUN_PROJECT; }

	virtual Dictionary get_input_schema() const override {
		return MCPSchema::object_schema(Dictionary());
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["stopped"] = MCPSchema::integer_property("How many were stopped.");
		properties["live_count"] = MCPSchema::integer_property("How many are still running.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		Dictionary result;
		result["stopped"] = MCPWorkspaceLauncher::stop_all();
		result["live_count"] = MCPRuntimeInstances::live().size();
		return result;
	}
};

} // namespace

void mcp_register_workspace_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->register_tool(Ref<MCPTool>(memnew(LaunchInstanceTool)));
	registry->register_tool(Ref<MCPTool>(memnew(ListInstancesTool)));
	registry->register_tool(Ref<MCPTool>(memnew(ControlInstanceTool)));
	registry->register_tool(Ref<MCPTool>(memnew(StopAllInstancesTool)));
}
