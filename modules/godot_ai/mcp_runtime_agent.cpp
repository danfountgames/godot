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
#include "core/os/os.h"
#include "core/variant/variant_parser.h"
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

	Dictionary result;
	result["action"] = action;
	result["events"] = events;
	return result;
}

Dictionary MCPRuntimeAgent::_capture(const Dictionary &p_arguments, String &r_error) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		r_error = "the running game has no viewport to capture";
		return Dictionary();
	}
	Ref<ViewportTexture> texture = tree->get_root()->get_texture();
	if (texture.is_null()) {
		r_error = "the running game's viewport has no texture yet";
		return Dictionary();
	}
	Ref<Image> image = texture->get_image();
	if (image.is_null() || image->is_empty()) {
		r_error = "the running game has not rendered a frame yet";
		return Dictionary();
	}

	// Written to a file rather than returned inline: the debugger channel is a
	// message bus for small payloads, and a screenshot is not small. The editor reads
	// it back from disk and decides what to do with it.
	const String directory = OS::get_singleton()->get_user_data_dir().path_join("godot_ai_captures");
	Ref<DirAccess> access = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (access.is_valid() && !access->dir_exists(directory)) {
		access->make_dir_recursive(directory);
	}
	const String path = directory.path_join(vformat("frame_%d.png", OS::get_singleton()->get_ticks_msec()));
	const Error error = image->save_png(path);
	if (error != OK) {
		r_error = vformat("could not write the capture to %s", path);
		return Dictionary();
	}

	Dictionary result;
	result["path"] = path;
	result["width"] = image->get_width();
	result["height"] = image->get_height();
	result["frame"] = (int64_t)Engine::get_singleton()->get_process_frames();
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

bool MCPRuntimeAgent::_coerce(const Variant &p_value, Variant::Type p_target, Variant &r_out, String &r_error) {
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
	if (!_coerce(p_arguments["value"], current.get_type(), value, coerce_error)) {
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

#endif // TOOLS_ENABLED
