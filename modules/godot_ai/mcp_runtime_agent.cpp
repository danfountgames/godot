/**************************************************************************/
/*  mcp_runtime_agent.cpp                                                 */
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

#include "mcp_runtime_agent.h"

#ifdef TOOLS_ENABLED

#include "core/config/engine.h"
#include "core/debugger/engine_debugger.h"
#include "core/input/input.h"
#include "core/input/input_event.h"
#include "core/os/os.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"

bool MCPRuntimeAgent::installed = false;

void MCPRuntimeAgent::install() {
	if (installed) {
		return;
	}
	// An editor process has no business answering these: its own tools already reach
	// the editor directly, and a handler in both processes would make it ambiguous
	// which one replied.
	if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if (!EngineDebugger::is_active()) {
		return;
	}
	EngineDebugger::register_message_capture(
			MCP_RUNTIME_CHANNEL, EngineDebugger::Capture(nullptr, &MCPRuntimeAgent::parse_message));
	installed = true;
}

void MCPRuntimeAgent::uninstall() {
	if (!installed) {
		return;
	}
	if (EngineDebugger::is_active()) {
		EngineDebugger::unregister_message_capture(MCP_RUNTIME_CHANNEL);
	}
	installed = false;
}

void MCPRuntimeAgent::_reply(const String &p_request_id, const Dictionary &p_result) {
	Array message;
	message.push_back(p_request_id);
	message.push_back(true);
	message.push_back(p_result);
	EngineDebugger::get_singleton()->send_message(String(MCP_RUNTIME_CHANNEL) + ":reply", message);
}

void MCPRuntimeAgent::_fail(const String &p_request_id, const String &p_message) {
	Array message;
	message.push_back(p_request_id);
	message.push_back(false);
	Dictionary error;
	error["message"] = p_message;
	message.push_back(error);
	EngineDebugger::get_singleton()->send_message(String(MCP_RUNTIME_CHANNEL) + ":reply", message);
}

Error MCPRuntimeAgent::parse_message(void *p_user, const String &p_message, const Array &p_args, bool &r_captured) {
	// The two ends of this channel disagree about what a message name is, and it is
	// worth stating plainly because it costs an afternoon otherwise. The engine strips
	// the capture prefix before calling us, so `godot_ai:send_pointer` arrives here as
	// `send_pointer`. The *editor* side, EditorDebuggerPlugin::capture(), is handed the
	// full `godot_ai:reply`. Neither is wrong; they are simply different conventions in
	// the same conversation.
	r_captured = true;
	const String command = p_message;
	// [request_id, arguments]
	const String request_id = p_args.size() > 0 ? String(p_args[0]) : String();
	Dictionary arguments;
	if (p_args.size() > 1 && p_args[1].get_type() == Variant::DICTIONARY) {
		arguments = p_args[1];
	}

	String error;
	const Dictionary result = _handle(command, arguments, error);
	if (error.is_empty()) {
		_reply(request_id, result);
	} else {
		_fail(request_id, error);
	}
	return OK;
}

Dictionary MCPRuntimeAgent::_handle(const String &p_command, const Dictionary &p_arguments, String &r_error) {
	if (p_command == "ping") {
		return _ping(p_arguments, r_error);
	}
	if (p_command == "send_pointer") {
		return _send_pointer(p_arguments, r_error);
	}
	r_error = vformat("unknown runtime command '%s'", p_command);
	return Dictionary();
}

Dictionary MCPRuntimeAgent::_ping(const Dictionary &p_arguments, String &r_error) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree) {
		r_error = "the running game has no scene tree";
		return Dictionary();
	}

	Dictionary result;
	result["alive"] = true;
	result["frame"] = (int64_t)Engine::get_singleton()->get_process_frames();
	Window *window = tree->get_root();
	if (window) {
		const Size2i size = window->get_size();
		result["window_width"] = size.width;
		result["window_height"] = size.height;
	}
	return result;
}

// -------------------------------------------------------------------- input ---

Dictionary MCPRuntimeAgent::_send_pointer(const Dictionary &p_arguments, String &r_error) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		r_error = "the running game has no window to receive input";
		return Dictionary();
	}
	Input *input = Input::get_singleton();
	if (!input) {
		r_error = "the running game has no input singleton";
		return Dictionary();
	}

	// has() rather than operator[]: reading a missing key through a const Dictionary
	// inserts a null, and these arguments are echoed back in the result.
	const String action = p_arguments.has("action") ? String(p_arguments["action"]) : String("click");
	const Vector2 position(
			p_arguments.has("x") ? (float)p_arguments["x"] : 0.0f,
			p_arguments.has("y") ? (float)p_arguments["y"] : 0.0f);
	const int button = p_arguments.has("button") ? (int)p_arguments["button"] : (int)MouseButton::LEFT;

	const Size2i window_size = tree->get_root()->get_size();
	if (position.x < 0 || position.y < 0 || position.x >= window_size.width || position.y >= window_size.height) {
		r_error = vformat("(%d, %d) is outside the game window, which is %dx%d",
				(int)position.x, (int)position.y, window_size.width, window_size.height);
		return Dictionary();
	}

	// Every event goes through Input::parse_input_event(), which is where the platform
	// layer delivers real hardware events. That is the whole point: a shortcut that
	// called the control directly would pass a signal-based test and fail a test that
	// looks for the InputEvent.
	auto motion_to = [&](const Vector2 &p_to) {
		Ref<InputEventMouseMotion> motion;
		motion.instantiate();
		motion->set_position(p_to);
		motion->set_global_position(p_to);
		motion->set_relative(p_to - input->get_mouse_position());
		input->parse_input_event(motion);
	};

	auto button_event = [&](bool p_pressed) {
		Ref<InputEventMouseButton> event;
		event.instantiate();
		event->set_position(position);
		event->set_global_position(position);
		event->set_button_index((MouseButton)button);
		event->set_pressed(p_pressed);
		event->set_button_mask(p_pressed ? mouse_button_to_mask((MouseButton)button) : BitField<MouseButtonMask>());
		input->parse_input_event(event);
	};

	int events = 0;
	if (action == "move") {
		motion_to(position);
		events = 1;
	} else if (action == "press") {
		motion_to(position);
		button_event(true);
		events = 2;
	} else if (action == "release") {
		button_event(false);
		events = 1;
	} else if (action == "click") {
		// A click is a press and a release, in that order, at the same point. Sending
		// only one of them leaves controls latched and is a common source of tests
		// that pass once and then poison every later interaction.
		motion_to(position);
		button_event(true);
		button_event(false);
		events = 3;
	} else {
		r_error = vformat("unknown pointer action '%s'; expected move, press, release or click", action);
		return Dictionary();
	}

	Dictionary result;
	result["action"] = action;
	result["x"] = position.x;
	result["y"] = position.y;
	result["button"] = button;
	result["events"] = events;
	result["window_width"] = window_size.width;
	result["window_height"] = window_size.height;
	return result;
}

#endif // TOOLS_ENABLED
