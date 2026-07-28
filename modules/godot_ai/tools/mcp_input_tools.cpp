/**************************************************************************/
/*  mcp_input_tools.cpp                                                   */
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

#include "../mcp_deferred.h"
#include "../mcp_runtime_bridge.h"
#include "../mcp_tool_registry.h"

#include "editor/editor_interface.h"
#include "editor/editor_node.h"

namespace {

class SendPointerInputTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_SendPointerInput"; }
	virtual String get_description() const override {
		return "Deliver a real pointer event to the running game. The event goes through the same "
			   "input pipeline as physical hardware, so what it proves is what a player would "
			   "experience - unlike calling a control's handler, which proves only that the "
			   "handler works. Requires a running game.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_SIMULATE_INPUT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		Vector<String> actions;
		actions.push_back("move");
		actions.push_back("press");
		actions.push_back("release");
		actions.push_back("click");
		properties["action"] = MCPSchema::enum_property(
				"A click is a press and a release at the same point; sending a press alone "
				"leaves the control latched.",
				actions, "click");
		properties["x"] = MCPSchema::integer_property("Horizontal position in game window pixels.");
		properties["y"] = MCPSchema::integer_property("Vertical position in game window pixels.");
		properties["button"] = MCPSchema::integer_property(
				"Mouse button index: 1 left, 2 right, 3 middle.", 1);
		Vector<String> required;
		required.push_back("x");
		required.push_back("y");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["action"] = MCPSchema::string_property("The action performed.");
		properties["x"] = MCPSchema::integer_property("Where it was delivered.");
		properties["y"] = MCPSchema::integer_property("Where it was delivered.");
		properties["events"] = MCPSchema::integer_property("How many input events were delivered.");
		properties["window_width"] = MCPSchema::integer_property("Game window width at delivery.");
		properties["window_height"] = MCPSchema::integer_property("Game window height at delivery.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		if (!EditorNode::get_singleton() || !EditorInterface::get_singleton()) {
			r_error.set(MCPToolError::UNSUPPORTED, "this process has no running Godot editor");
			return Dictionary();
		}
		MCPRuntimeBridge *bridge = MCPRuntimeBridge::get_singleton();
		if (!bridge || !bridge->is_game_reachable()) {
			r_error.set(MCPToolError::INVALID_STATE,
					"no game is running, so there is nothing to send input to; start one with "
					"Godot_PlayCurrentScene or Godot_PlayMainScene first");
			return Dictionary();
		}

		const MCPDeferred::Token token = bridge->send("send_pointer", p_arguments);
		if (token == MCPDeferred::INVALID_TOKEN) {
			r_error.set(MCPToolError::INVALID_STATE, "the running game could not be reached");
			return Dictionary();
		}
		return MCPDeferred::make_deferred_result(token);
	}
};

} // namespace

void mcp_register_input_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->register_tool(Ref<MCPTool>(memnew(SendPointerInputTool)));
}
