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
#include "core/io/dir_access.h"
#include "core/templates/local_vector.h"
#include "core/io/image.h"
#include "core/os/keyboard.h"
#include "main/performance.h"
#include "servers/audio_server.h"
#include "core/object/script_language.h"
#include "scene/gui/control.h"
#include "core/os/os.h"
#include "core/variant/variant_parser.h"
#include "core/io/resource.h"
#include "core/templates/hash_map.h"
#include "core/os/time.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"

bool MCPRuntimeAgent::installed = false;

// --------------------------------------------------------------- recording ---

namespace {

// Bounded: a trace is for the last few interactions, not a session-long log that grows
// until the game runs out of memory.
const int MAX_TRACE_ENTRIES = 256;
Array g_trace;

// Errors reported by the engine while the game runs. Godot_ReadOutputLog returns the
// Output panel as prose; this keeps the structure - file, line, function, whether it
// was fatal - which is what a diagnosis needs.
const int MAX_ERROR_ENTRIES = 128;
Array g_errors;

void error_handler(void *p_user, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_explanation, bool p_editor_notify, ErrorHandlerType p_type) {
	Dictionary entry;
	entry["function"] = String::utf8(p_function);
	entry["file"] = String::utf8(p_file);
	entry["line"] = p_line;
	entry["message"] = String::utf8(p_error);
	entry["detail"] = String::utf8(p_explanation);
	switch (p_type) {
		case ERR_HANDLER_WARNING:
			entry["kind"] = "warning";
			break;
		case ERR_HANDLER_SCRIPT:
			entry["kind"] = "script";
			break;
		case ERR_HANDLER_SHADER:
			entry["kind"] = "shader";
			break;
		default:
			entry["kind"] = "error";
			break;
	}
	if (g_errors.size() >= MAX_ERROR_ENTRIES) {
		g_errors.remove_at(0);
	}
	g_errors.push_back(entry);
}

ErrorHandlerList g_error_handler;
bool g_error_handler_installed = false;

} // namespace


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

	// Collect errors structurally while the game runs. The Output panel has them as
	// prose; a diagnosis wants the file, the line and the function.
	if (!g_error_handler_installed) {
		g_error_handler.errfunc = error_handler;
		g_error_handler.userdata = nullptr;
		add_error_handler(&g_error_handler);
		g_error_handler_installed = true;
	}
	installed = true;
}

MCPRuntimeWatcher *MCPRuntimeWatcher::singleton = nullptr;

void MCPRuntimeWatcher::create() {
	if (singleton) {
		return;
	}
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree) {
		return;
	}
	singleton = memnew(MCPRuntimeWatcher);
	// Checked once a frame, which is the finest granularity the game itself has.
	tree->connect("process_frame", callable_mp(singleton, &MCPRuntimeWatcher::on_frame));
}

void MCPRuntimeWatcher::destroy() {
	if (!singleton) {
		return;
	}
	if (SceneTree::get_singleton()) {
		SceneTree::get_singleton()->disconnect("process_frame", callable_mp(singleton, &MCPRuntimeWatcher::on_frame));
	}
	memdelete(singleton);
	singleton = nullptr;
}

void MCPRuntimeWatcher::add(const String &p_request_id, const String &p_path, const String &p_property, const Variant &p_expected, double p_timeout_seconds) {
	Watch watch;
	watch.request_id = p_request_id;
	watch.path = p_path;
	watch.property = p_property;
	watch.expected = p_expected;
	watch.deadline = OS::get_singleton()->get_ticks_msec() / 1000.0 + p_timeout_seconds;
	watches.push_back(watch);
	// Check immediately: a condition that is already true should not cost a frame, and
	// more importantly should not look like it took one.
	on_frame();
}

void MCPRuntimeWatcher::add_sequence(const String &p_request_id, int p_frames, int p_interval_frames) {
	Sequence sequence;
	sequence.request_id = p_request_id;
	sequence.remaining = p_frames;
	sequence.interval_frames = p_interval_frames;
	sequence.countdown = 0;
	sequences.push_back(sequence);
}

void MCPRuntimeWatcher::add_profile(const String &p_request_id, int p_frames, double p_budget_frame_ms) {
	Profile profile;
	profile.request_id = p_request_id;
	profile.remaining = p_frames;
	profile.budget_frame_ms = p_budget_frame_ms;
	profiles.push_back(profile);
}

void MCPRuntimeWatcher::add_scene_test(const String &p_request_id, double p_timeout_seconds) {
	SceneTest test;
	test.request_id = p_request_id;
	test.deadline = OS::get_singleton()->get_ticks_msec() / 1000.0 + p_timeout_seconds;
	scene_tests.push_back(test);
	// A scene that finished before anyone asked has still finished; checking now means
	// a fast test is not charged a frame it did not need.
	on_frame();
}

bool MCPRuntimeWatcher::read_scene_test(bool &r_finished, Array &r_cases) {
	SceneTree *tree = SceneTree::get_singleton();
	Node *root = tree ? tree->get_current_scene() : nullptr;
	if (!root) {
		return false;
	}

	// Metadata first, then a script property. A scene with no script can only carry
	// metadata, and requiring a script would make the simplest test scene impossible.
	bool declared = false;
	if (root->has_meta(SNAME("test_finished"))) {
		r_finished = root->get_meta(SNAME("test_finished"));
		declared = true;
	} else {
		bool valid = false;
		const Variant value = root->get(SNAME("test_finished"), &valid);
		if (valid) {
			r_finished = value;
			declared = true;
		}
	}
	if (!declared) {
		return false;
	}

	if (root->has_meta(SNAME("test_results"))) {
		const Variant value = root->get_meta(SNAME("test_results"));
		if (value.get_type() == Variant::ARRAY) {
			r_cases = value;
		}
	} else {
		bool valid = false;
		const Variant value = root->get(SNAME("test_results"), &valid);
		if (valid && value.get_type() == Variant::ARRAY) {
			r_cases = value;
		}
	}
	return true;
}

void MCPRuntimeWatcher::on_frame() {
	const double now_seconds = OS::get_singleton()->get_ticks_msec() / 1000.0;
	for (int i = scene_tests.size() - 1; i >= 0; i--) {
		bool finished = false;
		Array cases;
		const bool declared = read_scene_test(finished, cases);
		const bool expired = now_seconds >= scene_tests[i].deadline;
		if (!finished && !expired) {
			continue;
		}
		Dictionary result;
		result["finished"] = finished;
		result["declared"] = declared;
		result["cases"] = cases;
		result["scene"] = SceneTree::get_singleton() && SceneTree::get_singleton()->get_current_scene()
				? SceneTree::get_singleton()->get_current_scene()->get_scene_file_path()
				: String();
		const String request_id = scene_tests[i].request_id;
		scene_tests.remove_at(i);
		// Answered either way. A test that never finishes is a result, and the caller
		// needs to be told which of the two happened rather than left to time out.
		MCPRuntimeAgent::reply(request_id, result);
	}

	// Frame-driven jobs first: both want this frame, not the state after the watches
	// have possibly changed it.
	for (int i = sequences.size() - 1; i >= 0; i--) {
		Sequence &sequence = sequences.write[i];
		if (sequence.countdown > 0) {
			sequence.countdown--;
			continue;
		}
		String error;
		const String path = MCPRuntimeAgent::capture_frame(error);
		if (path.is_empty()) {
			const String request_id = sequence.request_id;
			sequences.remove_at(i);
			MCPRuntimeAgent::fail(request_id, error);
			continue;
		}
		Dictionary entry;
		entry["path"] = path;
		entry["frame"] = (int64_t)Engine::get_singleton()->get_process_frames();
		entry["msec"] = (int64_t)OS::get_singleton()->get_ticks_msec();
		sequence.paths.push_back(entry);
		sequence.countdown = sequence.interval_frames - 1;
		sequence.remaining--;

		if (sequence.remaining <= 0) {
			Dictionary result;
			result["frames"] = sequence.paths;
			result["count"] = sequence.paths.size();
			// The same provenance a single capture carries. A sequence is more likely to
			// be quoted as evidence about pacing than any one frame, which is exactly
			// when a time scale other than 1 makes it worthless.
			result["time_scale"] = Engine::get_singleton()->get_time_scale();
			if (SceneTree::get_singleton() && SceneTree::get_singleton()->get_root()) {
				const Size2i size = SceneTree::get_singleton()->get_root()->get_size();
				result["window_width"] = size.width;
				result["window_height"] = size.height;
			}
			const String request_id = sequence.request_id;
			sequences.remove_at(i);
			MCPRuntimeAgent::reply(request_id, result);
		}
	}

	for (int i = profiles.size() - 1; i >= 0; i--) {
		Profile &profile = profiles.write[i];
		Performance *performance = Performance::get_singleton();
		const double frame_ms = performance
				? performance->get_monitor(Performance::TIME_PROCESS) * 1000.0
				: 0.0;
		profile.total_ms += frame_ms;
		profile.worst_frame_ms = MAX(profile.worst_frame_ms, frame_ms);
		profile.samples++;
		profile.remaining--;

		if (profile.remaining <= 0) {
			Dictionary result;
			result["frames"] = profile.samples;
			result["mean_frame_ms"] = profile.samples > 0 ? profile.total_ms / profile.samples : 0.0;
			// The worst frame is the one a player notices. A mean that meets a budget
			// while one frame in sixty takes 40ms is a mean that is hiding a stutter.
			result["worst_frame_ms"] = profile.worst_frame_ms;
			if (profile.budget_frame_ms > 0.0) {
				result["budget_frame_ms"] = profile.budget_frame_ms;
				result["within_budget"] = profile.worst_frame_ms <= profile.budget_frame_ms;
				result["verdict"] = profile.worst_frame_ms <= profile.budget_frame_ms
						? "every frame in the window met the budget"
						: vformat("the worst frame took %.2fms against a budget of %.2fms",
								  profile.worst_frame_ms, profile.budget_frame_ms);
			}
			const String request_id = profile.request_id;
			profiles.remove_at(i);
			MCPRuntimeAgent::reply(request_id, result);
		}
	}

	if (watches.is_empty()) {
		return;
	}
	SceneTree *tree = SceneTree::get_singleton();
	const double now = OS::get_singleton()->get_ticks_msec() / 1000.0;

	for (int i = watches.size() - 1; i >= 0; i--) {
		const Watch watch = watches[i];
		Node *node = tree && tree->get_root() ? tree->get_root()->get_node_or_null(NodePath(watch.path)) : nullptr;

		if (node) {
			bool valid = false;
			const Variant current = node->get(watch.property, &valid);
			if (valid) {
				Variant expected;
				String ignored;
				// The expected value arrives as JSON, so it needs the same conversion a
				// write does before it can be compared with what the game holds.
				if (!MCPRuntimeAgent::coerce(watch.expected, current.get_type(), expected, ignored)) {
					expected = watch.expected;
				}
				if (current == expected) {
					Dictionary result;
					result["path"] = watch.path;
					result["property"] = watch.property;
					result["satisfied"] = true;
					String text;
					VariantWriter::write_to_string(current, text);
					result["text"] = text;
					watches.remove_at(i);
					MCPRuntimeAgent::reply(watch.request_id, result);
					continue;
				}
			}
		}

		if (watch.deadline <= now) {
			// Say what it was waiting for and what it found: "timed out" alone sends the
			// reader back to the game to work out which half was wrong.
			String wanted;
			VariantWriter::write_to_string(watch.expected, wanted);
			String found = "the node does not exist";
			if (node) {
				bool valid = false;
				const Variant current = node->get(watch.property, &valid);
				if (valid) {
					VariantWriter::write_to_string(current, found);
				} else {
					found = "the property does not exist";
				}
			}
			watches.remove_at(i);
			MCPRuntimeAgent::fail(watch.request_id,
					vformat("'%s.%s' did not become %s within the timeout; it is %s",
							watch.path, watch.property, wanted, found));
		}
	}
}

void MCPRuntimeAgent::uninstall() {
	if (!installed) {
		return;
	}
	if (EngineDebugger::is_active()) {
		EngineDebugger::unregister_message_capture(MCP_RUNTIME_CHANNEL);
	}
	MCPRuntimeWatcher::destroy();
	if (g_error_handler_installed) {
		remove_error_handler(&g_error_handler);
		g_error_handler_installed = false;
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

	// A wait answers when the game reaches the state, or when its deadline passes, so
	// it must not be answered here as well - exactly one reply per request.
	if (command == "capture_sequence" || command == "profile_window") {
		MCPRuntimeWatcher::create();
		MCPRuntimeWatcher *watcher = MCPRuntimeWatcher::get_singleton();
		if (!watcher) {
			_fail(request_id, "the running game cannot schedule frame work");
			return OK;
		}
		const int frames = arguments.has("frames") ? (int)arguments["frames"] : 10;
		if (frames < 1 || frames > 240) {
			_fail(request_id, "ask for between 1 and 240 frames");
			return OK;
		}
		if (command == "capture_sequence") {
			const int interval = arguments.has("every_n_frames") ? (int)arguments["every_n_frames"] : 1;
			watcher->add_sequence(request_id, frames, interval < 1 ? 1 : interval);
		} else {
			const double budget = arguments.has("budget_frame_ms") ? (double)arguments["budget_frame_ms"] : 0.0;
			watcher->add_profile(request_id, frames, budget);
		}
		return OK;
	}

	if (command == "watch_scene_test") {
		MCPRuntimeWatcher::create();
		MCPRuntimeWatcher *watcher = MCPRuntimeWatcher::get_singleton();
		if (!watcher) {
			_fail(request_id, "the running game cannot schedule frame work");
			return OK;
		}
		const double timeout = arguments.has("timeout_seconds")
				? (double)arguments["timeout_seconds"]
				: 60.0;
		watcher->add_scene_test(request_id, timeout);
		return OK;
	}

	if (command == "wait_for") {
		MCPRuntimeWatcher::create();
		MCPRuntimeWatcher *watcher = MCPRuntimeWatcher::get_singleton();
		const String path = arguments.has("path") ? String(arguments["path"]) : String();
		const String property = arguments.has("property") ? String(arguments["property"]) : String();
		if (!watcher || path.is_empty() || property.is_empty() || !arguments.has("equals")) {
			_fail(request_id, "waiting needs a node path, a property name and a value to wait for");
			return OK;
		}
		const double timeout = arguments.has("timeout_seconds") ? (double)arguments["timeout_seconds"] : 10.0;
		watcher->add(request_id, path, property, arguments["equals"], timeout);
		return OK;
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
	if (p_command == "send_key") {
		return _send_key(p_arguments, r_error);
	}
	if (p_command == "capture") {
		return _capture(p_arguments, r_error);
	}
	if (p_command == "get_property") {
		return _get_property(p_arguments, r_error);
	}
	if (p_command == "set_property") {
		return _set_property(p_arguments, r_error);
	}
	if (p_command == "node_info") {
		return _node_info(p_arguments, r_error);
	}
	if (p_command == "performance") {
		return _performance(p_arguments, r_error);
	}
	if (p_command == "window_info") {
		return _window_info(p_arguments, r_error);
	}
	if (p_command == "send_touch") {
		return _send_touch(p_arguments, r_error);
	}
	if (p_command == "send_gamepad") {
		return _send_gamepad(p_arguments, r_error);
	}
	if (p_command == "input_trace") {
		return _input_trace(p_arguments, r_error);
	}
	if (p_command == "runtime_errors") {
		return _runtime_errors(p_arguments, r_error);
	}
	if (p_command == "resize_window") {
		return _resize_window(p_arguments, r_error);
	}
	if (p_command == "time_scale") {
		return _time_scale(p_arguments, r_error);
	}
	if (p_command == "audio_state") {
		return _audio_state(p_arguments, r_error);
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
	const Vector2 destination(
			p_arguments.has("to_x") ? (float)p_arguments["to_x"] : position.x,
			p_arguments.has("to_y") ? (float)p_arguments["to_y"] : position.y);

	const Size2i window_size = tree->get_root()->get_size();
	auto off_window = [&](const Vector2 &p_point) {
		return p_point.x < 0 || p_point.y < 0 || p_point.x >= window_size.width ||
				p_point.y >= window_size.height;
	};
	if (off_window(position)) {
		r_error = vformat("(%d, %d) is outside the game window, which is %dx%d",
				(int)position.x, (int)position.y, window_size.width, window_size.height);
		return Dictionary();
	}
	if (action == "drag" && off_window(destination)) {
		r_error = vformat("a drag to (%d, %d) ends outside the game window, which is %dx%d",
				(int)destination.x, (int)destination.y, window_size.width, window_size.height);
		return Dictionary();
	}

	// Every event goes through Input::parse_input_event(), which is where the platform
	// layer delivers real hardware events. That is the whole point: a shortcut that
	// called the control directly would pass a signal-based test and fail a test that
	// looks for the InputEvent.
	// The pointer's own idea of where it is, tracked locally rather than read back from
	// Input between events. `relative` is the whole content of a drag for anything that
	// moves a camera or a slider, and a delta computed against a position that has not
	// caught up yet is silently zero.
	Vector2 previous = input->get_mouse_position();
	BitField<MouseButtonMask> held;

	auto motion_to = [&](const Vector2 &p_to) {
		Ref<InputEventMouseMotion> motion;
		motion.instantiate();
		motion->set_position(p_to);
		motion->set_global_position(p_to);
		motion->set_relative(p_to - previous);
		// Motion during a drag has to carry the button that is down. A game that asks
		// `event.button_mask` to tell a drag from a hover sees nothing without it.
		motion->set_button_mask(held);
		input->parse_input_event(motion);
		previous = p_to;
	};

	auto button_at = [&](const Vector2 &p_at, bool p_pressed) {
		held = p_pressed ? mouse_button_to_mask((MouseButton)button) : BitField<MouseButtonMask>();
		Ref<InputEventMouseButton> event;
		event.instantiate();
		event->set_position(p_at);
		event->set_global_position(p_at);
		event->set_button_index((MouseButton)button);
		event->set_pressed(p_pressed);
		event->set_button_mask(held);
		input->parse_input_event(event);
	};
	auto button_event = [&](bool p_pressed) { button_at(position, p_pressed); };

	auto wheel_tick = [&](MouseButton p_wheel, float p_factor) {
		// A wheel notch is a press and a release of a wheel "button", which is how the
		// engine models scrolling; a press with no release leaves ScrollContainer and
		// every custom handler waiting for the other half.
		for (int pressed = 1; pressed >= 0; pressed--) {
			Ref<InputEventMouseButton> event;
			event.instantiate();
			event->set_position(position);
			event->set_global_position(position);
			event->set_button_index(p_wheel);
			event->set_pressed(pressed == 1);
			event->set_factor(p_factor);
			event->set_button_mask(pressed == 1 ? mouse_button_to_mask(p_wheel)
												: BitField<MouseButtonMask>());
			input->parse_input_event(event);
		}
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
	} else if (action == "drag") {
		// A press, motion in steps, then a release. One call, because a drag split
		// across three calls is three chances for something else to move the pointer,
		// and because the intermediate motion is the part that carries the meaning.
		const int steps = MAX(1, p_arguments.has("steps") ? (int)p_arguments["steps"] : 8);
		motion_to(position);
		button_event(true);
		for (int step = 1; step <= steps; step++) {
			motion_to(position.lerp(destination, (float)step / (float)steps));
			// Flushed after each one, or `steps` means nothing. Godot coalesces motion
			// events within a frame - real hardware polls far faster than it draws - so
			// ten motions sent back to back arrive as one event with the summed delta,
			// and a game with a movement threshold sees a single jump straight past it
			// instead of the gradual drag it was asked for.
			input->flush_buffered_events();
		}
		button_at(destination, false);
		events = 3 + steps;
	} else if (action == "scroll") {
		const String direction = p_arguments.has("direction")
				? String(p_arguments["direction"])
				: String("down");
		const int amount = MAX(1, p_arguments.has("amount") ? (int)p_arguments["amount"] : 3);
		MouseButton wheel = MouseButton::WHEEL_DOWN;
		if (direction == "up") {
			wheel = MouseButton::WHEEL_UP;
		} else if (direction == "down") {
			wheel = MouseButton::WHEEL_DOWN;
		} else if (direction == "left") {
			wheel = MouseButton::WHEEL_LEFT;
		} else if (direction == "right") {
			wheel = MouseButton::WHEEL_RIGHT;
		} else {
			r_error = vformat("unknown scroll direction '%s'; expected up, down, left or right",
					direction);
			return Dictionary();
		}
		motion_to(position);
		for (int tick = 0; tick < amount; tick++) {
			wheel_tick(wheel, 1.0f);
		}
		events = 1 + amount * 2;
	} else {
		r_error = vformat("unknown pointer action '%s'; expected move, press, release, click, "
						  "drag or scroll",
				action);
		return Dictionary();
	}

	Dictionary detail;
	detail["action"] = action;
	detail["x"] = position.x;
	detail["y"] = position.y;
	detail["button"] = button;
	if (action == "drag") {
		detail["to_x"] = destination.x;
		detail["to_y"] = destination.y;
	}
	if (action == "scroll") {
		detail["direction"] = p_arguments.get("direction", "down");
		detail["amount"] = p_arguments.get("amount", 3);
	}
	_record("pointer", detail);

	Dictionary result = detail;
	result["events"] = events;
	result["window_width"] = window_size.width;
	result["window_height"] = window_size.height;
	return result;
}

Dictionary MCPRuntimeAgent::_send_key(const Dictionary &p_arguments, String &r_error) {
	Input *input = Input::get_singleton();
	if (!input || !SceneTree::get_singleton()) {
		r_error = "the running game cannot receive input";
		return Dictionary();
	}

	const String action = p_arguments.has("action") ? String(p_arguments["action"]) : String("type");
	const bool shift = p_arguments.has("shift") ? (bool)p_arguments["shift"] : false;
	const bool ctrl = p_arguments.has("ctrl") ? (bool)p_arguments["ctrl"] : false;
	const bool alt = p_arguments.has("alt") ? (bool)p_arguments["alt"] : false;
	const bool meta = p_arguments.has("meta") ? (bool)p_arguments["meta"] : false;

	auto make_event = [&](Key p_keycode, char32_t p_unicode, bool p_pressed) {
		Ref<InputEventKey> event;
		event.instantiate();
		event->set_keycode(p_keycode);
		event->set_physical_keycode(p_keycode);
		event->set_unicode(p_unicode);
		event->set_pressed(p_pressed);
		event->set_shift_pressed(shift);
		event->set_ctrl_pressed(ctrl);
		event->set_alt_pressed(alt);
		event->set_meta_pressed(meta);
		input->parse_input_event(event);
	};

	int events = 0;

	if (action == "type") {
		const String text = p_arguments.has("text") ? String(p_arguments["text"]) : String();
		if (text.is_empty()) {
			r_error = "typing needs some text";
			return Dictionary();
		}
		// One press and release per character, carrying the unicode value - which is
		// what a LineEdit reads. Sending only a keycode types nothing for most
		// characters, and sending only unicode leaves shortcuts unreachable.
		for (int i = 0; i < text.length(); i++) {
			const char32_t character = text[i];
			Key keycode = Key::NONE;
			if (character >= 'a' && character <= 'z') {
				keycode = (Key)((char32_t)Key::A + (character - 'a'));
			} else if (character >= 'A' && character <= 'Z') {
				keycode = (Key)((char32_t)Key::A + (character - 'A'));
			} else if (character >= '0' && character <= '9') {
				keycode = (Key)((char32_t)Key::KEY_0 + (character - '0'));
			} else if (character == ' ') {
				keycode = Key::SPACE;
			}
			make_event(keycode, character, true);
			make_event(keycode, character, false);
			events += 2;
		}
	} else {
		const String key_name = p_arguments.has("key") ? String(p_arguments["key"]) : String();
		if (key_name.is_empty()) {
			r_error = "press and release need a key name, such as Enter or Escape";
			return Dictionary();
		}
		const Key keycode = find_keycode(key_name);
		if (keycode == Key::NONE) {
			r_error = vformat("'%s' is not a key name this engine recognises", key_name);
			return Dictionary();
		}
		if (action == "press") {
			make_event(keycode, 0, true);
			events = 1;
		} else if (action == "release") {
			make_event(keycode, 0, false);
			events = 1;
		} else if (action == "tap") {
			make_event(keycode, 0, true);
			make_event(keycode, 0, false);
			events = 2;
		} else {
			r_error = vformat("unknown key action '%s'; expected type, press, release or tap", action);
			return Dictionary();
		}
	}

	Dictionary detail;
	detail["action"] = action;
	if (p_arguments.has("text")) {
		detail["text"] = p_arguments["text"];
	}
	if (p_arguments.has("key")) {
		detail["key"] = p_arguments["key"];
	}
	_record("key", detail);

	Dictionary result = detail;
	result["events"] = events;
	return result;
}

String MCPRuntimeAgent::capture_frame(String &r_error) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		r_error = "the running game has no viewport to capture";
		return String();
	}
	Ref<ViewportTexture> texture = tree->get_root()->get_texture();
	if (texture.is_null()) {
		r_error = "the running game's viewport has no texture yet";
		return String();
	}
	Ref<Image> image = texture->get_image();
	if (image.is_null() || image->is_empty()) {
		r_error = "the running game has not rendered a frame yet";
		return String();
	}

	// Written to a file rather than returned inline: the debugger channel is a
	// message bus for small payloads, and a screenshot is not small. The editor reads
	// it back from disk and decides what to do with it.
	const String directory = OS::get_singleton()->get_user_data_dir().path_join("godot_ai_captures");
	Ref<DirAccess> access = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (access.is_valid() && !access->dir_exists(directory)) {
		access->make_dir_recursive(directory);
	}
	const String path = directory.path_join(vformat("frame_%d_%d.png",
			(int64_t)Engine::get_singleton()->get_process_frames(),
			OS::get_singleton()->get_ticks_usec()));
	if (image->save_png(path) != OK) {
		r_error = vformat("could not write the capture to %s", path);
		return String();
	}
	return path;
}

Dictionary MCPRuntimeAgent::_capture(const Dictionary &p_arguments, String &r_error) {
	const String path = capture_frame(r_error);
	if (path.is_empty()) {
		return Dictionary();
	}
	Ref<Image> image = Image::load_from_file(path);

	Dictionary result;
	result["path"] = path;
	result["width"] = image.is_valid() ? image->get_width() : 0;
	result["height"] = image.is_valid() ? image->get_height() : 0;

	// What this is a picture of, gathered here because only the game process knows it.
	// The time scale in particular: a frame captured at 5x is not evidence about how the
	// game plays, and by the time the image reaches a report there is nothing left to
	// say that it was.
	result["frame"] = (int64_t)Engine::get_singleton()->get_process_frames();
	result["time_scale"] = Engine::get_singleton()->get_time_scale();
	result["game_time_seconds"] = Time::get_singleton()->get_ticks_msec() / 1000.0;
	SceneTree *tree = SceneTree::get_singleton();
	if (tree && tree->get_root()) {
		const Size2i size = tree->get_root()->get_size();
		result["window_width"] = size.width;
		result["window_height"] = size.height;
		Node *scene = tree->get_current_scene();
		result["scene"] = scene ? scene->get_scene_file_path() : String();
	}
	return result;
}

Dictionary MCPRuntimeAgent::_time_scale(const Dictionary &p_arguments, String &r_error) {
	const double scale = p_arguments.has("scale") ? (double)p_arguments["scale"] : 1.0;
	if (scale <= 0.0 || scale > 100.0) {
		r_error = "a time scale must be greater than 0 and no more than 100";
		return Dictionary();
	}
	Engine::get_singleton()->set_time_scale(scale);

	Dictionary result;
	result["scale"] = Engine::get_singleton()->get_time_scale();
	// Said plainly, because a fast-forwarded run is not a playtest: physics steps,
	// animation and input timing all change, and a bug that only appears at 1x is
	// exactly the kind this would hide.
	result["note"] = "time scale changes how the game runs; do not use it during a playtest "
					 "or a timing measurement";
	return result;
}

// Every kind of audio player, read through properties rather than by casting: the three
// player classes are siblings, not subclasses of one another, and a project may well
// have a fourth of its own.
static void collect_audio_players(Node *p_node, Array &r_players) {
	if (!p_node) {
		return;
	}
	if (p_node->is_class("AudioStreamPlayer") || p_node->is_class("AudioStreamPlayer2D") ||
			p_node->is_class("AudioStreamPlayer3D")) {
		Dictionary entry;
		entry["node_path"] = String(p_node->get_path());
		entry["class"] = p_node->get_class();
		const Ref<Resource> stream = p_node->get(SNAME("stream"));
		entry["stream"] = stream.is_valid() ? stream->get_path() : String();
		entry["playing"] = (bool)p_node->get(SNAME("playing"));
		entry["volume_db"] = (double)p_node->get(SNAME("volume_db"));
		entry["bus"] = String(p_node->get(SNAME("bus")));
		entry["autoplay"] = (bool)p_node->get(SNAME("autoplay"));
		entry["max_polyphony"] = (int)p_node->get(SNAME("max_polyphony"));
		entry["position_seconds"] = (double)p_node->call(SNAME("get_playback_position"));
		r_players.push_back(entry);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		collect_audio_players(p_node->get_child(i), r_players);
	}
}

Dictionary MCPRuntimeAgent::_audio_state(const Dictionary &p_arguments, String &r_error) {
	AudioServer *server = AudioServer::get_singleton();
	if (!server) {
		r_error = "the running game has no audio server";
		return Dictionary();
	}

	Array buses;
	for (int i = 0; i < server->get_bus_count(); i++) {
		Dictionary bus;
		bus["index"] = i;
		bus["name"] = server->get_bus_name(i);
		bus["volume_db"] = server->get_bus_volume_db(i);
		bus["mute"] = server->is_bus_mute(i);
		bus["solo"] = server->is_bus_solo(i);
		// The peak is how "did a sound actually play" gets answered without hearing it:
		// silence sits at the floor, and anything audible does not.
		bus["peak_left_db"] = server->get_bus_peak_volume_left_db(i, 0);
		bus["peak_right_db"] = server->get_bus_peak_volume_right_db(i, 0);
		buses.push_back(bus);
	}

	// Which players are sounding, and whether any sound is sounding twice.
	//
	// A bus peak cannot tell one loud playback from four stacked on top of each other,
	// and stacking is the audio bug an agent working without ears is most likely to
	// ship: a sound triggered from two places, or on every frame a button is held, is
	// loud and wrong and looks completely fine in every other measurement here.
	Array players;
	collect_audio_players(SceneTree::get_singleton() ? SceneTree::get_singleton()->get_root() : nullptr,
			players);

	HashMap<String, Array> sounding;
	for (int i = 0; i < players.size(); i++) {
		const Dictionary entry = players[i];
		if (!(bool)entry.get("playing", false)) {
			continue;
		}
		const String stream = entry.get("stream", String());
		if (stream.is_empty()) {
			// An unsaved or generated stream has no identity to compare against, so
			// counting it would invent duplicates out of unrelated sounds.
			continue;
		}
		if (!sounding.has(stream)) {
			sounding.insert(stream, Array());
		}
		sounding[stream].push_back(entry.get("node_path", String()));
	}

	Array stacked;
	for (const KeyValue<String, Array> &pair : sounding) {
		if (pair.value.size() < 2) {
			continue;
		}
		Dictionary entry;
		entry["stream"] = pair.key;
		entry["count"] = pair.value.size();
		entry["node_paths"] = pair.value;
		stacked.push_back(entry);
	}

	Dictionary result;
	result["buses"] = buses;
	result["players"] = players;
	result["stacked"] = stacked;
	result["output_latency_ms"] = server->get_output_latency() * 1000.0;

	String note = "an agent cannot hear this; peaks and bus state are structural evidence, "
				  "and whether it sounds right is not something this can tell you";
	if (!stacked.is_empty()) {
		note += vformat(". %d sound(s) are playing on more than one player at once, which is "
						"audible as one loud or doubled sound - see `stacked`",
				stacked.size());
	}
	// Said whether or not anything stacked, because the limit matters to how the empty
	// case should be read: a player with polyphony above 1 can stack with *itself*, and
	// nothing here can see inside it.
	note += ". A single player whose max_polyphony is above 1 can overlap itself, and that "
			"cannot be counted from outside it";
	result["note"] = note;
	return result;
}

Dictionary MCPRuntimeAgent::_get_property(const Dictionary &p_arguments, String &r_error) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		r_error = "the running game has no scene tree";
		return Dictionary();
	}
	const String path = p_arguments.has("path") ? String(p_arguments["path"]) : String();
	const String property = p_arguments.has("property") ? String(p_arguments["property"]) : String();
	if (path.is_empty() || property.is_empty()) {
		r_error = "reading a property needs a node path and a property name";
		return Dictionary();
	}

	Node *node = tree->get_root()->get_node_or_null(NodePath(path));
	if (!node) {
		r_error = vformat("no node at '%s' in the running game", path);
		return Dictionary();
	}

	bool valid = false;
	const Variant value = node->get(property, &valid);
	if (!valid) {
		r_error = vformat("'%s' has no property '%s'", path, property);
		return Dictionary();
	}

	Dictionary result;
	result["path"] = path;
	result["property"] = property;
	result["type"] = Variant::get_type_name(value.get_type());
	// Scalars survive JSON untouched; everything else would either be mangled or
	// refused, so it goes back in Godot's own text form, which round-trips.
	switch (value.get_type()) {
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
		case Variant::STRING:
		case Variant::STRING_NAME:
			result["value"] = value;
			break;
		default:
			result["value"] = String(value);
			break;
	}
	String text;
	VariantWriter::write_to_string(value, text);
	result["text"] = text;
	return result;
}

bool MCPRuntimeAgent::coerce(const Variant &p_value, Variant::Type p_target, Variant &r_out, String &r_error) {
	if (p_value.get_type() == p_target) {
		r_out = p_value;
		return true;
	}

	// A value that arrived as JSON has only arrays, numbers, strings and booleans to
	// work with, so a Vector2 shows up as [x, y]. Building the real type from that is
	// the difference between setting a property and silently doing nothing: Object::set
	// does not convert, it just refuses, and it refuses quietly.
	if (p_value.get_type() == Variant::ARRAY) {
		const Array values = p_value;
		Vector<Variant> arguments;
		for (int i = 0; i < values.size(); i++) {
			arguments.push_back(values[i]);
		}
		LocalVector<const Variant *> pointers;
		for (uint32_t i = 0; i < (uint32_t)arguments.size(); i++) {
			pointers.push_back(&arguments[i]);
		}
		Callable::CallError call_error;
		Variant constructed;
		Variant::construct(p_target, constructed, pointers.ptr(), (int)pointers.size(), call_error);
		if (call_error.error == Callable::CallError::CALL_OK) {
			r_out = constructed;
			return true;
		}
		r_error = vformat("cannot build a %s from %d values", Variant::get_type_name(p_target), values.size());
		return false;
	}

	// Numbers and strings convert where Godot itself says they can.
	if (Variant::can_convert(p_value.get_type(), p_target)) {
		Callable::CallError call_error;
		const Variant *argument = &p_value;
		Variant converted;
		Variant::construct(p_target, converted, &argument, 1, call_error);
		if (call_error.error == Callable::CallError::CALL_OK) {
			r_out = converted;
			return true;
		}
	}

	r_error = vformat("cannot use a %s as a %s",
			Variant::get_type_name(p_value.get_type()), Variant::get_type_name(p_target));
	return false;
}

Dictionary MCPRuntimeAgent::_set_property(const Dictionary &p_arguments, String &r_error) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		r_error = "the running game has no scene tree";
		return Dictionary();
	}
	const String path = p_arguments.has("path") ? String(p_arguments["path"]) : String();
	const String property = p_arguments.has("property") ? String(p_arguments["property"]) : String();
	if (path.is_empty() || property.is_empty()) {
		r_error = "setting a property needs a node path and a property name";
		return Dictionary();
	}
	if (!p_arguments.has("value")) {
		r_error = "setting a property needs a value";
		return Dictionary();
	}

	Node *node = tree->get_root()->get_node_or_null(NodePath(path));
	if (!node) {
		r_error = vformat("no node at '%s' in the running game", path);
		return Dictionary();
	}

	bool valid = false;
	const Variant current = node->get(property, &valid);
	if (!valid) {
		r_error = vformat("'%s' has no property '%s'", path, property);
		return Dictionary();
	}

	Variant value;
	String coerce_error;
	if (!coerce(p_arguments["value"], current.get_type(), value, coerce_error)) {
		r_error = vformat("'%s.%s' is a %s: %s", path, property,
				Variant::get_type_name(current.get_type()), coerce_error);
		return Dictionary();
	}

	node->set(property, value);

	// Read back rather than assume. Object::set is silent about refusing, and a tool
	// that reports success it did not verify is worse than one that fails.
	const Variant applied = node->get(property, &valid);
	if (!valid || applied != value) {
		String wanted;
		String got;
		VariantWriter::write_to_string(value, wanted);
		VariantWriter::write_to_string(applied, got);
		r_error = vformat("'%s.%s' did not take the value: wanted %s, holds %s",
				path, property, wanted, got);
		return Dictionary();
	}

	Dictionary result;
	result["path"] = path;
	result["property"] = property;
	result["persistent"] = false;
	result["type"] = Variant::get_type_name(applied.get_type());
	String text;
	VariantWriter::write_to_string(applied, text);
	result["text"] = text;
	return result;
}

Dictionary MCPRuntimeAgent::_node_info(const Dictionary &p_arguments, String &r_error) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		r_error = "the running game has no scene tree";
		return Dictionary();
	}
	const String path = p_arguments.has("path") ? String(p_arguments["path"]) : String();
	Node *node = path.is_empty() ? nullptr : tree->get_root()->get_node_or_null(NodePath(path));
	if (!node) {
		r_error = vformat("no node at '%s' in the running game", path);
		return Dictionary();
	}

	Dictionary result;
	result["path"] = path;
	result["name"] = node->get_name();
	result["type"] = node->get_class();
	const Ref<Script> script = node->get_script();
	result["script"] = script.is_valid() ? script->get_path() : String();

	Array groups;
	List<Node::GroupInfo> group_list;
	node->get_groups(&group_list);
	for (const Node::GroupInfo &group : group_list) {
		groups.push_back(String(group.name));
	}
	result["groups"] = groups;

	Array children;
	for (int i = 0; i < node->get_child_count(); i++) {
		children.push_back(node->get_child(i)->get_name());
	}
	result["children"] = children;

	// Geometry, when the node has any. An agent aiming input needs to know where a
	// control is far more often than it needs the rest of this.
	CanvasItem *canvas_item = Object::cast_to<CanvasItem>(node);
	result["visible"] = canvas_item ? canvas_item->is_visible() : true;
	Control *control = Object::cast_to<Control>(node);
	if (control) {
		const Rect2 rect = control->get_global_rect();
		Dictionary geometry;
		geometry["x"] = rect.position.x;
		geometry["y"] = rect.position.y;
		geometry["width"] = rect.size.width;
		geometry["height"] = rect.size.height;
		geometry["center_x"] = rect.position.x + rect.size.width / 2.0;
		geometry["center_y"] = rect.position.y + rect.size.height / 2.0;
		result["rect"] = geometry;
	}
	return result;
}

Dictionary MCPRuntimeAgent::_performance(const Dictionary &p_arguments, String &r_error) {
	Performance *performance = Performance::get_singleton();
	if (!performance) {
		r_error = "this build has no performance monitors";
		return Dictionary();
	}
	Dictionary result;
	result["fps"] = performance->get_monitor(Performance::TIME_FPS);
	result["frame_time_ms"] = performance->get_monitor(Performance::TIME_PROCESS) * 1000.0;
	result["physics_time_ms"] = performance->get_monitor(Performance::TIME_PHYSICS_PROCESS) * 1000.0;
	result["static_memory_bytes"] = performance->get_monitor(Performance::MEMORY_STATIC);
	result["object_count"] = performance->get_monitor(Performance::OBJECT_COUNT);
	result["node_count"] = performance->get_monitor(Performance::OBJECT_NODE_COUNT);
	result["draw_calls"] = performance->get_monitor(Performance::RENDER_TOTAL_DRAW_CALLS_IN_FRAME);
	result["frame"] = (int64_t)Engine::get_singleton()->get_process_frames();
	// One sample is a data point, not a measurement. Say so rather than let a caller
	// treat an instant as a budget verdict.
	result["note"] = "a single sample; measure a window of frames before judging a budget";
	return result;
}

Dictionary MCPRuntimeAgent::_window_info(const Dictionary &p_arguments, String &r_error) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		r_error = "the running game has no window";
		return Dictionary();
	}
	Window *window = tree->get_root();
	Dictionary result;
	const Size2i size = window->get_size();
	result["width"] = size.width;
	result["height"] = size.height;
	const Size2i visible = window->get_visible_rect().size;
	result["viewport_width"] = visible.width;
	result["viewport_height"] = visible.height;
	result["aspect"] = size.height > 0 ? (double)size.width / (double)size.height : 0.0;
	result["content_scale_factor"] = window->get_content_scale_factor();
	return result;
}

void MCPRuntimeAgent::_record(const String &p_kind, const Dictionary &p_detail) {
	Dictionary entry = p_detail;
	entry["kind"] = p_kind;
	entry["frame"] = (int64_t)Engine::get_singleton()->get_process_frames();
	entry["msec"] = (int64_t)OS::get_singleton()->get_ticks_msec();
	if (g_trace.size() >= MAX_TRACE_ENTRIES) {
		g_trace.remove_at(0);
	}
	g_trace.push_back(entry);
}

Dictionary MCPRuntimeAgent::_input_trace(const Dictionary &p_arguments, String &r_error) {
	const bool clear = p_arguments.has("clear") ? (bool)p_arguments["clear"] : false;
	Dictionary result;
	result["events"] = g_trace.duplicate();
	result["count"] = g_trace.size();
	if (clear) {
		g_trace.clear();
	}
	return result;
}

Dictionary MCPRuntimeAgent::_runtime_errors(const Dictionary &p_arguments, String &r_error) {
	const bool clear = p_arguments.has("clear") ? (bool)p_arguments["clear"] : false;
	Dictionary result;
	result["errors"] = g_errors.duplicate();
	result["count"] = g_errors.size();
	if (clear) {
		g_errors.clear();
	}
	return result;
}

Dictionary MCPRuntimeAgent::_send_touch(const Dictionary &p_arguments, String &r_error) {
	SceneTree *tree = SceneTree::get_singleton();
	Input *input = Input::get_singleton();
	if (!tree || !tree->get_root() || !input) {
		r_error = "the running game cannot receive input";
		return Dictionary();
	}

	const String action = p_arguments.has("action") ? String(p_arguments["action"]) : String("tap");
	const int index = p_arguments.has("index") ? (int)p_arguments["index"] : 0;
	const Vector2 position(
			p_arguments.has("x") ? (float)p_arguments["x"] : 0.0f,
			p_arguments.has("y") ? (float)p_arguments["y"] : 0.0f);

	const Size2i window_size = tree->get_root()->get_size();
	if (position.x < 0 || position.y < 0 || position.x >= window_size.width || position.y >= window_size.height) {
		r_error = vformat("(%d, %d) is outside the game window, which is %dx%d",
				(int)position.x, (int)position.y, window_size.width, window_size.height);
		return Dictionary();
	}

	// A cancel is `pressed = false` *and* `canceled = true`, which is how the engine
	// itself models it: `InputEvent::is_released()` is `!pressed && !canceled`, so a
	// cancelled touch is deliberately not a release. That distinction is the whole
	// point - a game that treats the two alike fires the button the player was dragging
	// away from.
	auto touch = [&](bool p_pressed, bool p_canceled) {
		Ref<InputEventScreenTouch> event;
		event.instantiate();
		event->set_index(index);
		event->set_position(position);
		event->set_pressed(p_pressed);
		event->set_canceled(p_canceled);
		input->parse_input_event(event);
	};

	int events = 0;
	if (action == "down") {
		touch(true, false);
		events = 1;
	} else if (action == "up") {
		touch(false, false);
		events = 1;
	} else if (action == "cancel") {
		// The touch the operating system takes away: a notification, an incoming call,
		// a gesture the system claimed. A game has to survive it, and until now there
		// was no way to put one into that state.
		touch(false, true);
		events = 1;
	} else if (action == "tap") {
		touch(true, false);
		touch(false, false);
		events = 2;
	} else if (action == "drag") {
		Ref<InputEventScreenDrag> event;
		event.instantiate();
		event->set_index(index);
		event->set_position(position);
		event->set_relative(Vector2(
				p_arguments.has("relative_x") ? (float)p_arguments["relative_x"] : 0.0f,
				p_arguments.has("relative_y") ? (float)p_arguments["relative_y"] : 0.0f));
		input->parse_input_event(event);
		events = 1;
	} else {
		r_error = vformat("unknown touch action '%s'; expected down, up, cancel, tap or drag",
				action);
		return Dictionary();
	}

	Dictionary detail;
	detail["action"] = action;
	detail["index"] = index;
	detail["x"] = position.x;
	detail["y"] = position.y;
	_record("touch", detail);

	Dictionary result = detail;
	result["events"] = events;
	return result;
}

Dictionary MCPRuntimeAgent::_send_gamepad(const Dictionary &p_arguments, String &r_error) {
	Input *input = Input::get_singleton();
	if (!input) {
		r_error = "the running game cannot receive input";
		return Dictionary();
	}

	const String action = p_arguments.has("action") ? String(p_arguments["action"]) : String("tap");
	const int device = p_arguments.has("device") ? (int)p_arguments["device"] : 0;

	int events = 0;
	if (action == "connect" || action == "disconnect") {
		// Device changes are a required test in the production template: a controller
		// unplugged mid-game must not strand the player in a menu they cannot leave.
		input->joy_connection_changed(device, action == "connect",
				action == "connect" ? "Simulated Controller" : String());
		events = 1;
	} else if (action == "axis") {
		const int axis = p_arguments.has("axis") ? (int)p_arguments["axis"] : 0;
		const float value = p_arguments.has("value") ? (float)p_arguments["value"] : 0.0f;
		Ref<InputEventJoypadMotion> event;
		event.instantiate();
		event->set_device(device);
		event->set_axis((JoyAxis)axis);
		event->set_axis_value(value);
		input->parse_input_event(event);
		events = 1;
	} else {
		const int button = p_arguments.has("button") ? (int)p_arguments["button"] : 0;
		auto press = [&](bool p_pressed) {
			Ref<InputEventJoypadButton> event;
			event.instantiate();
			event->set_device(device);
			event->set_button_index((JoyButton)button);
			event->set_pressed(p_pressed);
			input->parse_input_event(event);
		};
		if (action == "press") {
			press(true);
			events = 1;
		} else if (action == "release") {
			press(false);
			events = 1;
		} else if (action == "tap") {
			press(true);
			press(false);
			events = 2;
		} else {
			r_error = vformat("unknown gamepad action '%s'; expected press, release, tap, axis, connect or disconnect", action);
			return Dictionary();
		}
	}

	Dictionary detail;
	detail["action"] = action;
	detail["device"] = device;
	_record("gamepad", detail);

	Dictionary result = detail;
	result["events"] = events;
	return result;
}

Dictionary MCPRuntimeAgent::_resize_window(const Dictionary &p_arguments, String &r_error) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		r_error = "the running game has no window";
		return Dictionary();
	}
	const int width = p_arguments.has("width") ? (int)p_arguments["width"] : 0;
	const int height = p_arguments.has("height") ? (int)p_arguments["height"] : 0;
	if (width < 64 || height < 64) {
		r_error = "a window smaller than 64x64 is not a resolution anybody supports";
		return Dictionary();
	}
	tree->get_root()->set_size(Size2i(width, height));

	// Read back: a window manager is free to refuse, and a resolution matrix built on
	// sizes that were requested rather than applied proves nothing.
	const Size2i applied = tree->get_root()->get_size();
	Dictionary result;
	result["requested_width"] = width;
	result["requested_height"] = height;
	result["width"] = applied.width;
	result["height"] = applied.height;
	result["applied"] = (applied.width == width && applied.height == height);
	return result;
}

#endif // TOOLS_ENABLED
