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

#include "../mcp_capture_metadata.h"
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
		actions.push_back("drag");
		actions.push_back("scroll");
		properties["action"] = MCPSchema::enum_property(
				"A click is a press and a release at the same point; sending a press alone "
				"leaves the control latched. A drag is a press, motion in steps, and a release, "
				"in one call - the motion between the ends is what a slider or a camera reads.",
				actions, "click");
		properties["x"] = MCPSchema::integer_property("Horizontal position in game window pixels.");
		properties["y"] = MCPSchema::integer_property("Vertical position in game window pixels.");
		properties["button"] = MCPSchema::integer_property(
				"Mouse button index: 1 left, 2 right, 3 middle.", 1);
		properties["to_x"] = MCPSchema::integer_property("Where a drag ends, horizontally.");
		properties["to_y"] = MCPSchema::integer_property("Where a drag ends, vertically.");
		properties["steps"] = MCPSchema::integer_property(
				"How many motion events a drag is broken into. More is smoother and slower; a "
				"game with a movement threshold needs enough that no single step jumps past it.",
				8);
		Vector<String> directions;
		directions.push_back("up");
		directions.push_back("down");
		directions.push_back("left");
		directions.push_back("right");
		properties["direction"] = MCPSchema::enum_property("Which way to scroll.", directions, "down");
		properties["amount"] = MCPSchema::integer_property("How many wheel notches to send.", 3);
		properties["frames_per_step"] = MCPSchema::integer_property(
				"Frames to leave between the events of a multi-event gesture. 1 is one event per "
				"frame, which is what real hardware looks like; raise it to slow a gesture down "
				"for something that samples every few frames.", 1);
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
		properties["frames_per_step"] = MCPSchema::integer_property(
				"Frames to leave between the events of a multi-event gesture. Typing a string is a "
				"gesture too: a game that reads one character per frame, or debounces input, sees a "
				"whole word arriving in one frame very differently from someone typing it.", 1);
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
		properties["time_scale"] = MCPSchema::number_property(
				"The game's time scale when the frame was taken.");
		properties["game_time_seconds"] = MCPSchema::number_property(
				"How long the game process had been running.");
		properties["window_width"] = MCPSchema::integer_property("The game window's width.");
		properties["window_height"] = MCPSchema::integer_property("The game window's height.");
		properties["scene"] = MCPSchema::string_property("Scene the game was running.");
		properties["inlined"] = MCPSchema::bool_property("True when the image is also in the response.");
		MCPCaptureMetadata::declare(properties);
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
		// The flag is bound into the transform rather than read from the reply: the game
		// echoes back what it was asked, not what the caller asked for, and this tool
		// used to inline unconditionally - so a caller who said `inline_image: false`
		// got megabytes of base64 anyway.
		const bool inline_image = (bool)p_arguments.get("inline_image", true);
		return MCPDeferred::make_deferred_result(
				bridge->send("capture", p_arguments, 20.0,
						callable_mp_static(&CaptureGameTool::_inline_image).bind(inline_image)));
	}

	// Runs when the game's reply arrives, before the client sees it.
	static Dictionary _inline_image(const Dictionary &p_result, bool p_inline) {
		Dictionary result = p_result;
		result["inlined"] = false;

		// The game reported the facts only it can know; this is where they become a
		// statement about what the image is worth.
		MCPCaptureMetadata::stamp(result, MCPCaptureMetadata::SOURCE_GAME_WINDOW,
				vformat("the running game's window at frame %d",
						(int64_t)result.get("frame", 0)));
		const double scale = result.get("time_scale", 1.0);
		if (!Math::is_equal_approx(scale, 1.0)) {
			// Worth saying loudly. A frame taken at 5x is not evidence about how the game
			// plays, and nothing in the image itself will ever reveal that it was.
			MCPCaptureMetadata::add_note(result,
					vformat("The game was running at %.2fx speed, so this frame is not evidence "
							"about pacing, animation or anything a player would see at normal "
							"speed.",
							scale));
		}
		const String path = result.get("path", String());
		if (!p_inline || path.is_empty() || !FileAccess::exists(path)) {
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
	Callable transform;
	// Some questions have a true answer when no game is running, and refusing them makes
	// a whole pattern inexpressible - see `answer_without_game` at the call site.
	Dictionary answer_without_game;
	bool answers_without_game = false;

public:
	RuntimeCommandTool(const String &p_name, const String &p_command, const String &p_description,
			MCPCapability p_capability, const Dictionary &p_schema, double p_timeout = 10.0,
			const Callable &p_transform = Callable()) :
			tool_name(p_name), command(p_command), description(p_description),
			capability(p_capability), schema(p_schema), timeout(p_timeout),
			transform(p_transform) {}

	void set_answer_without_game(const Dictionary &p_answer) {
		answer_without_game = p_answer;
		answers_without_game = true;
	}

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
			if (answers_without_game) {
				r_error = MCPToolError();
				return answer_without_game;
			}
			return Dictionary();
		}
		return MCPDeferred::make_deferred_result(
				bridge->send(command, p_arguments, timeout, transform));
	}
};

// Provenance for a frame sequence, applied when the game's reply comes back. A sequence
// is more likely than any single frame to be quoted as evidence about pacing, which is
// exactly when a time scale other than 1 makes it worthless.
Dictionary stamp_sequence(const Dictionary &p_result) {
	Dictionary result = p_result;
	MCPCaptureMetadata::stamp(result, MCPCaptureMetadata::SOURCE_GAME_WINDOW,
			vformat("%d consecutive frames of the running game", (int)result.get("count", 0)));
	const double scale = result.get("time_scale", 1.0);
	if (!Math::is_equal_approx(scale, 1.0)) {
		MCPCaptureMetadata::add_note(result,
				vformat("The game was running at %.2fx speed, so the timing between these frames "
						"is not the timing a player would see.",
						scale));
	}
	return result;
}

Dictionary touch_schema() {
	Dictionary properties;
	Vector<String> actions;
	actions.push_back("down");
	actions.push_back("up");
	actions.push_back("cancel");
	actions.push_back("tap");
	actions.push_back("drag");
	properties["action"] = MCPSchema::enum_property(
			"Touch phase. 'cancel' is the touch the operating system takes away - a "
			"notification, an incoming call - which a game must not treat as a release: the "
			"engine's own is_released() is false for it, and a game that ignores the "
			"difference fires the button the player was dragging away from.",
			actions, "tap");
	properties["x"] = MCPSchema::integer_property("Horizontal position in game window pixels.");
	properties["y"] = MCPSchema::integer_property("Vertical position in game window pixels.");
	properties["index"] = MCPSchema::integer_property(
			"Finger index, so multi-touch can be exercised.", 0);
	properties["to_x"] = MCPSchema::integer_property(
			"Where a swept drag ends, horizontally. Given this (or to_y), action='drag' is a "
			"whole gesture: a touch down, motion in steps, and a release at the destination.");
	properties["to_y"] = MCPSchema::integer_property("Where a swept drag ends, vertically.");
	properties["steps"] = MCPSchema::integer_property(
			"How many motion events a swept drag is broken into. The motion between the ends "
			"is the whole content of a drag, so give it enough steps that no single one jumps "
			"past whatever threshold the thing being dragged uses.", 8);
	properties["relative_x"] = MCPSchema::integer_property(
			"Horizontal movement for a single drag event, when assembling a gesture by hand "
			"rather than using to_x/to_y.", 0);
	properties["relative_y"] = MCPSchema::integer_property("Vertical movement, likewise.", 0);
	properties["frames_per_step"] = MCPSchema::integer_property(
			"Frames to leave between the events of a multi-event gesture. 1 is one event per "
			"frame, which is what real hardware looks like; raise it to slow a gesture down "
			"for something that samples every few frames.", 1);
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
	properties["value"] = MCPSchema::number_property("Axis value from -1 to 1.", 0);
	properties["frames_per_step"] = MCPSchema::integer_property(
			"Frames to leave between the events of a multi-event gesture. 1 is one event per "
			"frame, which is what real hardware looks like; raise it to slow a gesture down "
			"for something that samples every few frames.", 1);
	return MCPSchema::object_schema(properties);
}

Dictionary clearable_schema(const String &p_what) {
	Dictionary properties;
	properties["clear"] = MCPSchema::bool_property(
			vformat("Empty the %s after returning it, so the next call starts fresh.", p_what), false);
	return MCPSchema::object_schema(properties);
}

Dictionary sequence_schema() {
	Dictionary properties;
	properties["frames"] = MCPSchema::integer_property(
			"How many frames to capture, 1 to 240.", 10);
	properties["every_n_frames"] = MCPSchema::integer_property(
			"Capture one frame out of every N, to cover a longer span.", 1);
	return MCPSchema::object_schema(properties);
}

Dictionary profile_schema() {
	Dictionary properties;
	properties["frames"] = MCPSchema::integer_property(
			"How many frames to measure, 1 to 240.", 60);
	properties["budget_frame_ms"] = MCPSchema::number_property(
			"Frame-time budget in milliseconds. When given, the result carries a verdict "
			"judged on the worst frame rather than the mean.", 0);
	return MCPSchema::object_schema(properties);
}

Dictionary time_scale_schema() {
	Dictionary properties;
	properties["scale"] = MCPSchema::number_property(
			"Multiplier for the passage of time: 2 runs twice as fast, 1 is normal.", 1);
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
			"Deliver real touch events to the running game: tap, press, release, cancel or drag, "
			"with a finger index so multi-touch and gesture thresholds can be exercised. A game "
			"that only ever receives mouse events has not been tested on a touch device, and one "
			"that has never had a touch cancelled has not been tested against a notification "
			"arriving mid-gesture.",
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

	{
		RuntimeCommandTool *errors = memnew(RuntimeCommandTool(
			"Godot_GetRuntimeErrors", "runtime_errors",
			"Return errors and warnings the running game has reported, with file, line, "
			"function, and the script call stack as it stood when the error was raised. "
			"Godot_ReadOutputLog gives the same events as prose; this keeps the structure, "
			"which is what a diagnosis actually needs - the call site says where it broke, and "
			"the stack says which caller got it there. Each entry carries `raised_in` "
			"(`engine` or `script`) and `project_frames`, which describe where it came from. "
			"Neither says whether the project could have avoided it: an autoload with any "
			"member variable logs one unavoidable engine error per run that *does* carry "
			"project frames, and push_error() from a script is raised inside the engine. That "
			"distinction is not available from the data, so this tool does not pretend to it - "
			"read the stack.",
			MCP_CAP_READ_RUNTIME, clearable_schema("error list")));
		// "Restart the game, then assert it logged nothing" needs this to answer rather
		// than refuse. The errors live in the game process, so once it is gone there are
		// genuinely none - and a fresh game starts with an empty buffer, which is exactly
		// what this says.
		Dictionary empty;
		empty["errors"] = Array();
		empty["count"] = 0;
		empty["note"] = "no game is running, so there are no runtime errors; a game started "
						"from here begins with an empty buffer";
		errors->set_answer_without_game(empty);
		registry->register_tool(Ref<MCPTool>(errors));
	}


	registry->register_tool(Ref<MCPTool>(memnew(RuntimeCommandTool(
			"Godot_CaptureFrameSequence", "capture_sequence",
			"Capture several consecutive frames of the running game, each tagged with its frame "
			"number and time. One screenshot cannot show whether feedback began within 100ms or "
			"whether a transition dropped a frame; a sequence can.",
			MCP_CAP_READ_RUNTIME, sequence_schema(), 60.0,
			callable_mp_static(&stamp_sequence)))));

	registry->register_tool(Ref<MCPTool>(memnew(RuntimeCommandTool(
			"Godot_ProfileWindow", "profile_window",
			"Measure frame time across a window of frames and report the mean and the worst "
			"frame. Judge against the worst: a mean that meets a budget while one frame in sixty "
			"takes 40ms is a mean hiding a stutter the player can feel. Give budget_frame_ms to "
			"get a verdict rather than numbers to interpret.",
			MCP_CAP_READ_RUNTIME, profile_schema(), 60.0))));

	registry->register_tool(Ref<MCPTool>(memnew(RuntimeCommandTool(
			"Godot_GetAudioState", "audio_state",
			"Report the running game's audio: bus volumes, mute and solo state, current peak "
			"levels, every audio player in the scene, and any sound playing on more than one "
			"player at once. That last one is what a peak cannot tell you - four stacked "
			"playbacks and one loud playback look identical on a meter, and a sound triggered "
			"twice is the audio bug you are most likely to ship without ears. This does not "
			"pretend to more: whether it sounds right is not something it can tell you.",
			MCP_CAP_READ_RUNTIME, MCPSchema::object_schema(Dictionary())))));

	{
		Dictionary properties;
		properties["frames"] = MCPSchema::integer_property(
				"How many frames to watch. Send the input burst you are worried about while "
				"this is sampling.",
				60);
		registry->register_tool(Ref<MCPTool>(memnew(RuntimeCommandTool(
				"Godot_DetectAudioStacking", "audio_window",
				"Watch every frame for a window and report any sound that played on more than "
				"one player at the same time, with the frame it happened on. Godot_GetAudioState "
				"answers for the instant you ask; a sound triggered twice by one button press "
				"can double and finish before the next call arrives, and this is the only thing "
				"that will see it. Send the input burst while this is sampling - pointed at an "
				"idle game it correctly reports nothing.",
				MCP_CAP_READ_RUNTIME, MCPSchema::object_schema(properties), 60.0))));
	}

	registry->register_tool(Ref<MCPTool>(memnew(RuntimeCommandTool(
			"Godot_SetTimeScale", "time_scale",
			"Speed up or slow down the running game, for walking a long route without waiting "
			"through it. **It changes the simulation, not merely the pace.** Godot multiplies "
			"the physics delta by the time scale, so a physics scene run at 5x is a *coarser* "
			"scene, not a faster one: bodies travel further per step, land differently, and "
			"produce different outcomes. Never measure anything physical at a raised scale, and "
			"never gather balance numbers with it. Not for a playtest and not for a timing "
			"measurement either: physics steps, "
			"animation and input timing all change, and a bug that only appears at normal speed "
			"is exactly the kind this hides.",
			MCP_CAP_RUN_PROJECT, time_scale_schema()))));

	registry->register_tool(Ref<MCPTool>(memnew(RuntimeCommandTool(
			"Godot_SetGameWindowSize", "resize_window",
			"Resize the running game's window, for walking a resolution matrix without "
			"relaunching per size. The result reports the size that was actually applied as "
			"well as the one requested - a window manager is free to refuse.",
			MCP_CAP_RUN_PROJECT, resize_schema()))));
}
