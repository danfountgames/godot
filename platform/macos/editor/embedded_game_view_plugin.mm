/**************************************************************************/
/*  embedded_game_view_plugin.mm                                          */
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

#include "embedded_game_view_plugin.h"

#include "embedded_process_macos.h"

#include "core/config/engine.h"
#include "core/input/input.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_node.h"
#include "editor/gui/window_wrapper.h"
#include "scene/main/window.h"
#include "servers/display/display_server.h"

HashMap<String, GameViewDebuggerMacOS::ParseMessageFunc> GameViewDebuggerMacOS::parse_message_handlers;

bool GameViewDebuggerMacOS::_msg_set_context_id(const Array &p_args, int p_session) {
	ERR_FAIL_COND_V_MSG(p_args.size() != 1, false, "set_context_id: invalid number of arguments.");

	EmbeddedProcessMacOS *embedded_process = _process_for_session(p_session);
	ERR_FAIL_NULL_V(embedded_process, true);

	embedded_process->set_context_id(p_args[0]);
	return true;
}

bool GameViewDebuggerMacOS::_msg_cursor_set_shape(const Array &p_args, int p_session) {
	ERR_FAIL_COND_V_MSG(p_args.size() != 1, false, "cursor_set_shape: invalid number of arguments.");

	EmbeddedProcessMacOS *embedded_process = _process_for_session(p_session);
	ERR_FAIL_NULL_V(embedded_process, true);

	Control::CursorShape shape = Control::CursorShape(p_args[0]);
	embedded_process->get_layer_host()->set_default_cursor_shape(static_cast<Control::CursorShape>(shape));

	return true;
}

bool GameViewDebuggerMacOS::_msg_cursor_set_custom_image(const Array &p_args, int p_session) {
	ERR_FAIL_COND_V_MSG(p_args.size() != 3, false, "cursor_set_custom_image: invalid number of arguments.");

	EmbeddedProcessMacOS *embedded_process = _process_for_session(p_session);
	ERR_FAIL_NULL_V(embedded_process, true);

	Ref<Image> image;
	image.instantiate();
	PackedByteArray cursor_data = p_args[0];
	if (!cursor_data.is_empty()) {
		image->load_png_from_buffer(cursor_data);
	}
	DisplayServerEnums::CursorShape shape = DisplayServerEnums::CursorShape(p_args[1]);
	Vector2 hotspot = p_args[2];

	embedded_process->get_layer_host()->cursor_set_custom_image(image, shape, hotspot);

	return true;
}

bool GameViewDebuggerMacOS::_msg_mouse_set_mode(const Array &p_args, int p_session) {
	ERR_FAIL_COND_V_MSG(p_args.size() != 1, false, "mouse_set_mode: invalid number of arguments.");

	EmbeddedProcessMacOS *embedded_process = _process_for_session(p_session);
	ERR_FAIL_NULL_V(embedded_process, true);

	DisplayServerEnums::MouseMode mode = DisplayServerEnums::MouseMode(p_args[0]);
	embedded_process->mouse_set_mode(mode);

	return true;
}

bool GameViewDebuggerMacOS::_msg_window_set_ime_active(const Array &p_args, int p_session) {
	ERR_FAIL_COND_V_MSG(p_args.size() != 1, false, "window_set_ime_active: invalid number of arguments.");

	EmbeddedProcessMacOS *embedded_process = _process_for_session(p_session);
	ERR_FAIL_NULL_V(embedded_process, true);

	bool active = p_args[0];
	DisplayServerEnums::WindowID wid = embedded_process->get_window()->get_window_id();
	DisplayServer::get_singleton()->window_set_ime_active(active, wid);
	return true;
}

bool GameViewDebuggerMacOS::_msg_window_set_ime_position(const Array &p_args, int p_session) {
	ERR_FAIL_COND_V_MSG(p_args.size() != 1, false, "window_set_ime_position: invalid number of arguments.");

	EmbeddedProcessMacOS *embedded_process = _process_for_session(p_session);
	ERR_FAIL_NULL_V(embedded_process, true);

	Point2i pos = p_args[0];
	Point2i xpos = embedded_process->get_layer_host()->get_global_transform_with_canvas().xform(pos);
	DisplayServerEnums::WindowID wid = embedded_process->get_window()->get_window_id();
	DisplayServer::get_singleton()->window_set_ime_position(xpos, wid);
	return true;
}

bool GameViewDebuggerMacOS::_msg_joy_start(const Array &p_args, int p_session) {
	// Vibration is a device, not a window: it belongs to no embedder.
	(void)p_session;
	ERR_FAIL_COND_V_MSG(p_args.size() != 3, false, "joy_start: invalid number of arguments.");

	int joy_id = p_args[0];
	float duration = p_args[1];
	Vector2 strength = p_args[2];
	Input::get_singleton()->start_joy_vibration(joy_id, strength.x, strength.y, duration);
	return true;
}

bool GameViewDebuggerMacOS::_msg_joy_stop(const Array &p_args, int p_session) {
	// Vibration is a device, not a window: it belongs to no embedder.
	(void)p_session;
	ERR_FAIL_COND_V_MSG(p_args.size() != 1, false, "joy_stop: invalid number of arguments.");

	int joy_id = p_args[0];
	Input::get_singleton()->stop_joy_vibration(joy_id);
	return true;
}

bool GameViewDebuggerMacOS::_msg_warp_mouse(const Array &p_args, int p_session) {
	ERR_FAIL_COND_V_MSG(p_args.size() != 1, false, "warp_mouse: invalid number of arguments.");

	EmbeddedProcessMacOS *embedded_process = _process_for_session(p_session);
	ERR_FAIL_NULL_V(embedded_process, true);

	// Position arrives in points; convert to editor pixels.
	Vector2i pos = p_args[0];
	float scale = DisplayServer::get_singleton()->screen_get_max_scale();
	embedded_process->get_layer_host()->warp_mouse(Vector2i(Vector2(pos) * scale));
	return true;
}

void GameViewDebuggerMacOS::_init_capture_message_handlers() {
	parse_message_handlers["game_view:set_context_id"] = &GameViewDebuggerMacOS::_msg_set_context_id;
	parse_message_handlers["game_view:cursor_set_shape"] = &GameViewDebuggerMacOS::_msg_cursor_set_shape;
	parse_message_handlers["game_view:cursor_set_custom_image"] = &GameViewDebuggerMacOS::_msg_cursor_set_custom_image;
	parse_message_handlers["game_view:mouse_set_mode"] = &GameViewDebuggerMacOS::_msg_mouse_set_mode;
	parse_message_handlers["game_view:window_set_ime_active"] = &GameViewDebuggerMacOS::_msg_window_set_ime_active;
	parse_message_handlers["game_view:window_set_ime_position"] = &GameViewDebuggerMacOS::_msg_window_set_ime_position;
	parse_message_handlers["game_view:warp_mouse"] = &GameViewDebuggerMacOS::_msg_warp_mouse;
}

EmbeddedProcessMacOS *GameViewDebuggerMacOS::_process_for_session(int p_session) const {
	// The same join `GameView::_attach_script_debugger()` makes, in the other direction:
	// a debugger session knows the pid it is attached to, and an embedder registers the
	// pid it is holding. Anything hosting embedders - the Game workspace, or a grid of
	// them - is found this way without either side knowing about the other.
	EditorDebuggerNode *node = EditorDebuggerNode::get_singleton();
	if (node) {
		ScriptEditorDebugger *debugger = node->get_debugger(p_session);
		if (debugger) {
			EmbeddedProcessMacOS *found = EmbeddedProcessMacOS::for_pid((ProcessID)debugger->get_remote_pid());
			if (found) {
				return found;
			}
		}
	}
	// No registered embedder for this session. The Game workspace's own embedder is
	// attached directly rather than through the registry, so it is the right fallback -
	// and it is what every message went to before this join existed.
	return embedded_process;
}

bool GameViewDebuggerMacOS::capture(const String &p_message, const Array &p_data, int p_session) {
	Ref<EditorDebuggerSession> session = get_session(p_session);
	ERR_FAIL_COND_V(session.is_null(), true);

	ParseMessageFunc *fn_ptr = parse_message_handlers.getptr(p_message);
	if (fn_ptr) {
		return (this->**fn_ptr)(p_data, p_session);
	} else {
		return GameViewDebugger::capture(p_message, p_data, p_session);
	}

	return true;
}

GameViewDebuggerMacOS::GameViewDebuggerMacOS(EmbeddedProcessMacOS *p_embedded_process) :
		embedded_process(p_embedded_process) {
	if (parse_message_handlers.is_empty()) {
		_init_capture_message_handlers();
	}
}

GameViewPluginMacOS::GameViewPluginMacOS() {
	if (Engine::get_singleton()->is_recovery_mode_hint()) {
		return;
	}

	EmbeddedProcessMacOS *embedded_process = memnew(EmbeddedProcessMacOS);

	Ref<GameViewDebuggerMacOS> debugger;
	debugger.instantiate(embedded_process);

	setup(debugger, embedded_process);
}

extern "C" GameViewPluginBase *get_game_view_plugin() {
	return memnew(GameViewPluginMacOS);
}

// Tell the rest of the editor which embedder this platform uses, so a host that is not
// the Game workspace still gets one that can embed here. Registered at static
// initialization rather than from `get_game_view_plugin()` so it cannot depend on which
// editor plugin happens to be constructed first; only the pointer is stored now, and it
// is called later, once there is an editor to call it.
static EmbeddedProcessBase *_create_embedded_process_macos() {
	return memnew(EmbeddedProcessMacOS);
}

namespace {
struct EmbeddedProcessMacOSRegistration {
	EmbeddedProcessMacOSRegistration() {
		EmbeddedProcessBase::set_create_function(&_create_embedded_process_macos);
	}
};
EmbeddedProcessMacOSRegistration embedded_process_macos_registration;
} // namespace
