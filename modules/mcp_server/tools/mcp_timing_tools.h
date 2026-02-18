/**************************************************************************/
/*  mcp_timing_tools.h                                                    */
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

#include "core/variant/dictionary.h"

class MCPToolRegistry;

class MCPTimingTools {
public:
	// Register all debug time control tools into the registry.
	static void register_tools(MCPToolRegistry *p_registry);

	// --- Time Scale ---
	static Dictionary handle_set_time_scale(const Dictionary &p_args);
	static Dictionary handle_get_time_scale(const Dictionary &p_args);

	// --- Suspend / Resume ---
	static Dictionary handle_suspend(const Dictionary &p_args);
	static Dictionary handle_resume(const Dictionary &p_args);

	// --- Frame Advance ---
	static Dictionary handle_next_frame(const Dictionary &p_args);
	static Dictionary handle_advance_frames(const Dictionary &p_args);

	// --- Debug Pause ---
	static Dictionary handle_set_debug_pause(const Dictionary &p_args);
	static Dictionary handle_set_debug_pause_tag(const Dictionary &p_args);
	static Dictionary handle_clear_debug_pause_hits(const Dictionary &p_args);

private:
	static class MCPDebuggerBridge *_get_bridge();
	static Dictionary _require_game_running();
};
