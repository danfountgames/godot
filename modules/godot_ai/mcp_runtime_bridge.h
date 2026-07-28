/**************************************************************************/
/*  mcp_runtime_bridge.h                                                  */
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

// The editor end of the runtime channel.
//
// A tool asks a question of the running game and gets an answer some frames later, so
// this owns the correlation: each request carries an id, the reply carries it back, and
// the pending entry holds the deferred token the protocol layer will answer.
//
// It is the same shape as MCPDeferred elsewhere in this module, for the same reason -
// the editor's main loop must keep running while the game is being asked.

#ifndef MCP_RUNTIME_BRIDGE_H
#define MCP_RUNTIME_BRIDGE_H

#include "mcp_deferred.h"

#include "core/object/ref_counted.h"
#include "core/variant/dictionary.h"
#include "editor/plugins/editor_debugger_plugin.h"

class MCPRuntimeBridge : public EditorDebuggerPlugin {
	GDCLASS(MCPRuntimeBridge, EditorDebuggerPlugin);

	struct Pending {
		String request_id;
		MCPDeferred::Token token = MCPDeferred::INVALID_TOKEN;
		double deadline = 0.0;
	};

	Vector<Pending> pending;
	int64_t next_request = 1;

	static MCPRuntimeBridge *singleton;

public:
	static MCPRuntimeBridge *get_singleton() { return singleton; }

	MCPRuntimeBridge();
	~MCPRuntimeBridge();

	virtual bool capture(const String &p_message, const Array &p_data, int p_session) override;
	virtual bool has_capture(const String &p_capture) const override;

	// True when a game is running and its runtime agent can be reached.
	bool is_game_reachable() const;

	// Sends a command and returns a deferred token the caller should hand back to the
	// protocol layer. Returns INVALID_TOKEN when no game is running.
	MCPDeferred::Token send(const String &p_command, const Dictionary &p_arguments, double p_timeout_seconds = 10.0);

	// Fails every pending request; called when the game goes away.
	void abandon_all(const String &p_reason);

	// Expires requests whose deadline has passed. Driven from the service poll.
	void poll();
};

#endif // MCP_RUNTIME_BRIDGE_H
