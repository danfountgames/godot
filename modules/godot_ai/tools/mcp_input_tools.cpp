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

#include "core/crypto/crypto_core.h"
#include "core/object/callable_method_pointer.h"
#include "core/io/file_access.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"

// Matches Godot_CaptureViewport: an image bigger than this is referenced by path only,
// because a client's context is not the place for a multi-megabyte frame.
static const int MAX_INLINE_IMAGE_BYTES = 3 * 1024 * 1024;

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

// Every runtime tool has the same two refusals in front of it, and they are worth
// wording well: an agent that gets "no game is running" and is told which tool starts
// one does not need to ask anybody anything.
static bool require_running_game(MCPToolError &r_error, MCPRuntimeBridge **r_bridge) {
	if (!EditorNode::get_singleton() || !EditorInterface::get_singleton()) {
		r_error.set(MCPToolError::UNSUPPORTED, "this process has no running Godot editor");
		return false;
	}
	MCPRuntimeBridge *bridge = MCPRuntimeBridge::get_singleton();
	if (!bridge || !bridge->is_game_reachable()) {
		r_error.set(MCPToolError::INVALID_STATE,
				"no game is running; start one with Godot_PlayCurrentScene or "
				"Godot_PlayMainScene first");
		return false;
	}
	*r_bridge = bridge;
	return true;
}

class SendKeyInputTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_SendKeyInput"; }
	virtual String get_description() const override {
		return "Deliver real keyboard events to the running game: type text, or press, release "
			   "or tap a named key. Text is typed one character at a time carrying its unicode "
			   "value, which is what a text field actually reads - setting a field's text "
			   "directly would prove nothing about whether a player could type into it.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_SIMULATE_INPUT; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		Vector<String> actions;
		actions.push_back("type");
		actions.push_back("press");
		actions.push_back("release");
		actions.push_back("tap");
		properties["action"] = MCPSchema::enum_property(
				"type sends text; tap is a press and a release of one key.", actions, "type");
		properties["text"] = MCPSchema::string_property("Text to type, for action=type.", "");
		properties["key"] = MCPSchema::string_property(
				"Key name for press, release or tap: Enter, Escape, Tab, A, F1, and so on.", "");
		properties["shift"] = MCPSchema::bool_property("Hold shift.", false);
		properties["ctrl"] = MCPSchema::bool_property("Hold control.", false);
		properties["alt"] = MCPSchema::bool_property("Hold alt.", false);
		properties["meta"] = MCPSchema::bool_property("Hold meta/command.", false);
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["action"] = MCPSchema::string_property("The action performed.");
		properties["events"] = MCPSchema::integer_property("How many key events were delivered.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPRuntimeBridge *bridge = nullptr;
		if (!require_running_game(r_error, &bridge)) {
			return Dictionary();
		}
		return MCPDeferred::make_deferred_result(bridge->send("send_key", p_arguments));
	}
};

class CaptureGameTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_CaptureGame"; }
	virtual String get_description() const override {
		return "Capture what the *running game* is showing, as a PNG returned inline when small "
			   "enough. This is the game's own window - Godot_CaptureViewport photographs the "
			   "editor instead, which is a different question and a common way to review the "
			   "wrong thing.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["inline_image"] = MCPSchema::bool_property(
				"Also return the image in the response, when it is small enough.", true);
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Absolute path of the saved PNG.");
		properties["width"] = MCPSchema::integer_property("Image width in pixels.");
		properties["height"] = MCPSchema::integer_property("Image height in pixels.");
		properties["frame"] = MCPSchema::integer_property("Which frame of the game this is.");
		properties["inlined"] = MCPSchema::bool_property("True when the image is also in the response.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPRuntimeBridge *bridge = nullptr;
		if (!require_running_game(r_error, &bridge)) {
			return Dictionary();
		}
		// The game writes the file and reports where; the editor turns that into an
		// image block. Sending pixels over the debugger bus would be the wrong use of a
		// channel meant for small messages.
		return MCPDeferred::make_deferred_result(
				bridge->send("capture", p_arguments, 20.0,
						callable_mp_static(&CaptureGameTool::_inline_image)));
	}

	// Runs when the game's reply arrives, before the client sees it.
	static Dictionary _inline_image(const Dictionary &p_result) {
		Dictionary result = p_result;
		result["inlined"] = false;
		const String path = result.get("path", String());
		if (path.is_empty() || !FileAccess::exists(path)) {
			return result;
		}
		const Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(path);
		if (bytes.is_empty() || bytes.size() > MAX_INLINE_IMAGE_BYTES) {
			return result;
		}
		Array content;
		Dictionary image_block;
		image_block["type"] = "image";
		image_block["data"] = CryptoCore::b64_encode_str(bytes.ptr(), bytes.size());
		image_block["mimeType"] = "image/png";
		content.push_back(image_block);
		result["_content"] = content;
		result["inlined"] = true;
		return result;
	}
};

class GetRuntimePropertyTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_GetRuntimeProperty"; }
	virtual String get_description() const override {
		return "Read a property from a node in the running game. This is the other half of "
			   "Godot_SetRuntimeProperty: without it, a runtime change can be made but never "
			   "confirmed, which makes every runtime assertion circular.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property(
				"Node path in the running game, such as /root/Main/Player.");
		properties["property"] = MCPSchema::string_property("Property name, such as position.");
		Vector<String> required;
		required.push_back("path");
		required.push_back("property");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Node that was read.");
		properties["property"] = MCPSchema::string_property("Property that was read.");
		properties["type"] = MCPSchema::string_property("Godot type name of the value.");
		properties["value"] = MCPSchema::any_property("The value, for types JSON can carry.");
		properties["text"] = MCPSchema::string_property(
				"The value in Godot's own text form, which round-trips for every type.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPRuntimeBridge *bridge = nullptr;
		if (!require_running_game(r_error, &bridge)) {
			return Dictionary();
		}
		return MCPDeferred::make_deferred_result(bridge->send("get_property", p_arguments));
	}
};

class WaitForRuntimeConditionTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_WaitForRuntimeCondition"; }
	virtual String get_description() const override {
		return "Wait until a property of a node in the running game equals a value, or fail after "
			   "a timeout. Use this instead of waiting a fixed time: 'wait two seconds and hope' "
			   "is the usual reason a test passes on a fast machine and fails on a loaded one, "
			   "and it hides the difference between slow and broken.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }

	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Node path, such as /root/Main/Player.");
		properties["property"] = MCPSchema::string_property("Property to watch.");
		properties["equals"] = MCPSchema::any_property(
				"The value to wait for. Converted to the property's real type, so [64, 32] "
				"matches a Vector2.");
		properties["timeout_seconds"] = MCPSchema::integer_property(
				"How long to wait before failing.", 10);
		Vector<String> required;
		required.push_back("path");
		required.push_back("property");
		required.push_back("equals");
		return MCPSchema::object_schema(properties, required);
	}

	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Node that was watched.");
		properties["property"] = MCPSchema::string_property("Property that was watched.");
		properties["satisfied"] = MCPSchema::bool_property("Always true when this succeeds.");
		properties["text"] = MCPSchema::string_property("The value when the wait was satisfied.");
		return MCPSchema::object_schema(properties);
	}

	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPRuntimeBridge *bridge = nullptr;
		if (!require_running_game(r_error, &bridge)) {
			return Dictionary();
		}
		// The bridge's own deadline sits past the wait's, so the game gets to answer -
		// including with its own, more useful, "it is still X" failure.
		const double timeout = p_arguments.has("timeout_seconds")
				? (double)p_arguments["timeout_seconds"]
				: 10.0;
		return MCPDeferred::make_deferred_result(
				bridge->send("wait_for", p_arguments, timeout + 5.0));
	}
};

class GetRuntimeNodeInfoTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_GetRuntimeNodeInfo"; }
	virtual String get_description() const override {
		return "Describe a node in the running game: class, script, groups, children, visibility, "
			   "and - for a Control - where it actually is on screen, which is what pointer input "
			   "needs in order to aim at something by name rather than by guessed coordinates.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }
	virtual Dictionary get_input_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Node path, such as /root/Main/Hud/Play.");
		Vector<String> required;
		required.push_back("path");
		return MCPSchema::object_schema(properties, required);
	}
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["path"] = MCPSchema::string_property("Node that was described.");
		properties["name"] = MCPSchema::string_property("Node name.");
		properties["type"] = MCPSchema::string_property("Class name.");
		properties["script"] = MCPSchema::string_property("Attached script, empty when none.");
		properties["groups"] = MCPSchema::array_property("Groups the node is in.",
				MCPSchema::string_property("Group name."));
		properties["children"] = MCPSchema::array_property("Child node names.",
				MCPSchema::string_property("Child name."));
		properties["visible"] = MCPSchema::bool_property("Whether it is visible.");
		properties["rect"] = MCPSchema::object_schema(Dictionary(), Vector<String>(), true);
		return MCPSchema::object_schema(properties);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPRuntimeBridge *bridge = nullptr;
		if (!require_running_game(r_error, &bridge)) {
			return Dictionary();
		}
		return MCPDeferred::make_deferred_result(bridge->send("node_info", p_arguments));
	}
};

class GetPerformanceMetricsTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_GetPerformanceMetrics"; }
	virtual String get_description() const override {
		return "Sample the running game's performance monitors: frame rate, frame and physics "
			   "time, memory, object and node counts, draw calls. One call is one sample - "
			   "measure a window of frames before treating it as a verdict on a budget.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }
	virtual Dictionary get_input_schema() const override { return MCPSchema::object_schema(Dictionary()); }
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["fps"] = MCPSchema::integer_property("Frames per second.");
		properties["frame_time_ms"] = MCPSchema::integer_property("Process time this frame.");
		properties["physics_time_ms"] = MCPSchema::integer_property("Physics time this frame.");
		properties["static_memory_bytes"] = MCPSchema::integer_property("Static memory in use.");
		properties["object_count"] = MCPSchema::integer_property("Live objects.");
		properties["node_count"] = MCPSchema::integer_property("Live nodes.");
		properties["draw_calls"] = MCPSchema::integer_property("Draw calls this frame.");
		properties["note"] = MCPSchema::string_property("What this sample does and does not show.");
		return MCPSchema::object_schema(properties, Vector<String>(), true);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPRuntimeBridge *bridge = nullptr;
		if (!require_running_game(r_error, &bridge)) {
			return Dictionary();
		}
		return MCPDeferred::make_deferred_result(bridge->send("performance", p_arguments));
	}
};

class GetGameWindowInfoTool : public MCPTool {
public:
	virtual String get_tool_name() const override { return "Godot_GetGameWindowInfo"; }
	virtual String get_description() const override {
		return "Report the running game's window and viewport size, aspect ratio and content "
			   "scale - what a screenshot means, and what coordinate space pointer input is in.";
	}
	virtual MCPCapability get_capability() const override { return MCP_CAP_READ_RUNTIME; }
	virtual Dictionary get_input_schema() const override { return MCPSchema::object_schema(Dictionary()); }
	virtual Dictionary get_output_schema() const override {
		Dictionary properties;
		properties["width"] = MCPSchema::integer_property("Window width in pixels.");
		properties["height"] = MCPSchema::integer_property("Window height in pixels.");
		properties["viewport_width"] = MCPSchema::integer_property("Visible viewport width.");
		properties["viewport_height"] = MCPSchema::integer_property("Visible viewport height.");
		return MCPSchema::object_schema(properties, Vector<String>(), true);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPRuntimeBridge *bridge = nullptr;
		if (!require_running_game(r_error, &bridge)) {
			return Dictionary();
		}
		return MCPDeferred::make_deferred_result(bridge->send("window_info", p_arguments));
	}
};

// A family of small tools that all forward one command to the running game. They are
// separate tools rather than one with a mode argument because a client chooses from a
// list of names, and "send touch" is a different intention from "send gamepad".
class RuntimeCommandTool : public MCPTool {
	String tool_name;
	String command;
	String description;
	MCPCapability capability;
	Dictionary schema;
	double timeout;

public:
	RuntimeCommandTool(const String &p_name, const String &p_command, const String &p_description,
			MCPCapability p_capability, const Dictionary &p_schema, double p_timeout = 10.0) :
			tool_name(p_name), command(p_command), description(p_description),
			capability(p_capability), schema(p_schema), timeout(p_timeout) {}

	virtual String get_tool_name() const override { return tool_name; }
	virtual String get_description() const override { return description; }
	virtual MCPCapability get_capability() const override { return capability; }
	virtual Dictionary get_input_schema() const override { return schema; }
	virtual Dictionary get_output_schema() const override {
		return MCPSchema::object_schema(Dictionary(), Vector<String>(), true);
	}
	virtual Dictionary run(const Dictionary &p_arguments, MCPToolError &r_error) override {
		MCPRuntimeBridge *bridge = nullptr;
		if (!require_running_game(r_error, &bridge)) {
			return Dictionary();
		}
		return MCPDeferred::make_deferred_result(bridge->send(command, p_arguments, timeout));
	}
};

Dictionary touch_schema() {
	Dictionary properties;
	Vector<String> actions;
	actions.push_back("down");
	actions.push_back("up");
	actions.push_back("tap");
	actions.push_back("drag");
	properties["action"] = MCPSchema::enum_property("Touch phase.", actions, "tap");
	properties["x"] = MCPSchema::integer_property("Horizontal position in game window pixels.");
	properties["y"] = MCPSchema::integer_property("Vertical position in game window pixels.");
	properties["index"] = MCPSchema::integer_property(
			"Finger index, so multi-touch can be exercised.", 0);
	properties["relative_x"] = MCPSchema::integer_property("Horizontal movement, for drag.", 0);
	properties["relative_y"] = MCPSchema::integer_property("Vertical movement, for drag.", 0);
	Vector<String> required;
	required.push_back("x");
	required.push_back("y");
	return MCPSchema::object_schema(properties, required);
}

Dictionary gamepad_schema() {
	Dictionary properties;
	Vector<String> actions;
	actions.push_back("press");
	actions.push_back("release");
	actions.push_back("tap");
	actions.push_back("axis");
	actions.push_back("connect");
	actions.push_back("disconnect");
	properties["action"] = MCPSchema::enum_property(
			"Button action, an axis movement, or a device appearing or going away.",
			actions, "tap");
	properties["device"] = MCPSchema::integer_property("Device index.", 0);
	properties["button"] = MCPSchema::integer_property("JoyButton index, for button actions.", 0);
	properties["axis"] = MCPSchema::integer_property("JoyAxis index, for action=axis.", 0);
	properties["value"] = MCPSchema::integer_property("Axis value from -1 to 1.", 0);
	return MCPSchema::object_schema(properties);
}

Dictionary clearable_schema(const String &p_what) {
	Dictionary properties;
	properties["clear"] = MCPSchema::bool_property(
			vformat("Empty the %s after returning it, so the next call starts fresh.", p_what), false);
	return MCPSchema::object_schema(properties);
}

Dictionary resize_schema() {
	Dictionary properties;
	properties["width"] = MCPSchema::integer_property("New window width in pixels.");
	properties["height"] = MCPSchema::integer_property("New window height in pixels.");
	Vector<String> required;
	required.push_back("width");
	required.push_back("height");
	return MCPSchema::object_schema(properties, required);
}

} // namespace

void mcp_register_input_tools() {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	ERR_FAIL_NULL(registry);
	registry->register_tool(Ref<MCPTool>(memnew(SendPointerInputTool)));
	registry->register_tool(Ref<MCPTool>(memnew(SendKeyInputTool)));
	registry->register_tool(Ref<MCPTool>(memnew(CaptureGameTool)));
	registry->register_tool(Ref<MCPTool>(memnew(GetRuntimePropertyTool)));
	registry->register_tool(Ref<MCPTool>(memnew(WaitForRuntimeConditionTool)));
	registry->register_tool(Ref<MCPTool>(memnew(GetRuntimeNodeInfoTool)));
	registry->register_tool(Ref<MCPTool>(memnew(GetPerformanceMetricsTool)));
	registry->register_tool(Ref<MCPTool>(memnew(GetGameWindowInfoTool)));

	registry->register_tool(Ref<MCPTool>(memnew(RuntimeCommandTool(
			"Godot_SendTouchInput", "send_touch",
			"Deliver real touch events to the running game: tap, press, release or drag, with a "
			"finger index so multi-touch and gesture thresholds can be exercised. A game that "
			"only ever receives mouse events has not been tested on a touch device.",
			MCP_CAP_SIMULATE_INPUT, touch_schema()))));

	registry->register_tool(Ref<MCPTool>(memnew(RuntimeCommandTool(
			"Godot_SendGamepadInput", "send_gamepad",
			"Deliver real gamepad events: buttons, axes, and a controller connecting or going "
			"away. The last one matters more than it sounds - a controller unplugged mid-game "
			"must not strand the player in a menu they can no longer move through.",
			MCP_CAP_SIMULATE_INPUT, gamepad_schema()))));

	registry->register_tool(Ref<MCPTool>(memnew(RuntimeCommandTool(
			"Godot_GetInputTrace", "input_trace",
			"Return the input events this editor has delivered to the running game, with the "
			"frame and time each was sent. This is what makes a claimed interaction checkable "
			"after the fact rather than something to be argued about.",
			MCP_CAP_READ_RUNTIME, clearable_schema("trace")))));

	registry->register_tool(Ref<MCPTool>(memnew(RuntimeCommandTool(
			"Godot_GetRuntimeErrors", "runtime_errors",
			"Return errors and warnings the running game has reported, with file, line and "
			"function. Godot_ReadOutputLog gives the same events as prose; this keeps the "
			"structure, which is what a diagnosis actually needs.",
			MCP_CAP_READ_RUNTIME, clearable_schema("error list")))));

	registry->register_tool(Ref<MCPTool>(memnew(RuntimeCommandTool(
			"Godot_SetGameWindowSize", "resize_window",
			"Resize the running game's window, for walking a resolution matrix without "
			"relaunching per size. The result reports the size that was actually applied as "
			"well as the one requested - a window manager is free to refuse.",
			MCP_CAP_RUN_PROJECT, resize_schema()))));
}
