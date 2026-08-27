/**************************************************************************/
/*  embedded_game_view_plugin.h                                           */
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

#pragma once

#include "editor/run/game_view_plugin.h"

class EmbeddedProcessMacOS;

class GameViewDebuggerMacOS : public GameViewDebugger {
	GDCLASS(GameViewDebuggerMacOS, GameViewDebugger);

	// The embedder this debugger was constructed with: the Game workspace's single one.
	// It stays the answer for any session that cannot be resolved to a pid, which keeps
	// the ordinary one-game path working exactly as it did.
	EmbeddedProcessMacOS *embedded_process = nullptr;

	/// Message handler function for capture.

	/// @brief A function pointer to the message handler function.
	///
	/// Every handler receives the session the message arrived on. It used to be dropped:
	/// `capture()` took `p_session`, checked the session existed and then dispatched
	/// without it, so each handler reached through the one `embedded_process` above. With
	/// a single embedded game that is invisible. With several, every game's
	/// `set_context_id` landed on the same embedder and the last one won, so at most one
	/// game could ever display.
	typedef bool (GameViewDebuggerMacOS::*ParseMessageFunc)(const Array &p_args, int p_session);

	/// @brief A map of message handlers.
	static HashMap<String, ParseMessageFunc> parse_message_handlers;

	/// @brief Initialize the message handlers.
	static void _init_capture_message_handlers();

	/// @brief The embedder that owns a session, by way of the pid they share.
	EmbeddedProcessMacOS *_process_for_session(int p_session) const;

	bool _msg_set_context_id(const Array &p_args, int p_session);
	bool _msg_cursor_set_shape(const Array &p_args, int p_session);
	bool _msg_cursor_set_custom_image(const Array &p_args, int p_session);
	bool _msg_mouse_set_mode(const Array &p_args, int p_session);
	bool _msg_window_set_ime_active(const Array &p_args, int p_session);
	bool _msg_window_set_ime_position(const Array &p_args, int p_session);
	bool _msg_joy_start(const Array &p_args, int p_session);
	bool _msg_joy_stop(const Array &p_args, int p_session);
	bool _msg_warp_mouse(const Array &p_args, int p_session);

public:
	virtual bool capture(const String &p_message, const Array &p_data, int p_session) override;

	GameViewDebuggerMacOS(EmbeddedProcessMacOS *p_embedded_process);
};

class GameViewPluginMacOS : public GameViewPluginBase {
	GDCLASS(GameViewPluginMacOS, GameViewPluginBase);

public:
	GameViewPluginMacOS();
};

extern "C" void register_game_view_plugin();
