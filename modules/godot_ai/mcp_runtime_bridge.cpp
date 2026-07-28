/**************************************************************************/
/*  mcp_runtime_bridge.cpp                                                */
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

#include "mcp_runtime_bridge.h"

#include "mcp_runtime_agent.h"

#include "core/os/os.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"

MCPRuntimeBridge *MCPRuntimeBridge::singleton = nullptr;

MCPRuntimeBridge::MCPRuntimeBridge() {
	singleton = this;
}

MCPRuntimeBridge::~MCPRuntimeBridge() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

bool MCPRuntimeBridge::has_capture(const String &p_capture) const {
	return p_capture == MCP_RUNTIME_CHANNEL;
}

bool MCPRuntimeBridge::capture(const String &p_message, const Array &p_data, int p_session) {
	if (p_message != String(MCP_RUNTIME_CHANNEL) + ":reply") {
		return false;
	}
	// [request_id, ok, payload]
	if (p_data.size() < 3) {
		return true;
	}
	const String request_id = p_data[0];
	const bool ok = p_data[1];
	const Dictionary payload = p_data[2];

	for (int i = 0; i < pending.size(); i++) {
		if (pending[i].request_id != request_id) {
			continue;
		}
		const MCPDeferred::Token token = pending[i].token;
		const Callable transform = pending[i].transform;
		const Callable on_reply = pending[i].on_reply;
		pending.remove_at(i);
		if (on_reply.is_valid()) {
			on_reply.call(ok, payload);
			return true;
		}
		if (ok) {
			Dictionary answer = payload;
			if (transform.is_valid()) {
				const Variant transformed = transform.call(payload);
				if (transformed.get_type() == Variant::DICTIONARY) {
					answer = transformed;
				}
			}
			MCPDeferred::complete(token, answer);
		} else {
			MCPDeferred::fail(token, MCPToolError::INVALID_STATE,
					String(payload.get("message", "the running game refused the request")));
		}
		return true;
	}
	// A reply to something already abandoned. Swallowing it is correct: the caller has
	// been answered once already, and answering twice is worse than answering late.
	return true;
}

bool MCPRuntimeBridge::is_game_reachable() const {
	EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
	return debugger && debugger->get_default_debugger() && debugger->get_default_debugger()->is_session_active();
}

MCPDeferred::Token MCPRuntimeBridge::send(const String &p_command, const Dictionary &p_arguments, double p_timeout_seconds, const Callable &p_transform) {
	EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
	if (!debugger || !is_game_reachable()) {
		return MCPDeferred::INVALID_TOKEN;
	}

	Pending entry;
	entry.request_id = vformat("r%d", next_request++);
	entry.token = MCPDeferred::begin(p_timeout_seconds);
	entry.deadline = OS::get_singleton()->get_ticks_msec() / 1000.0 + p_timeout_seconds;
	entry.transform = p_transform;
	pending.push_back(entry);

	Array message;
	message.push_back(entry.request_id);
	message.push_back(p_arguments);
	debugger->get_default_debugger()->send_message(String(MCP_RUNTIME_CHANNEL) + ":" + p_command, message);
	return entry.token;
}

bool MCPRuntimeBridge::request(const String &p_command, const Dictionary &p_arguments, double p_timeout_seconds, const Callable &p_on_reply) {
	EditorDebuggerNode *debugger = EditorDebuggerNode::get_singleton();
	if (!debugger || !is_game_reachable()) {
		return false;
	}

	Pending entry;
	entry.request_id = vformat("r%d", next_request++);
	entry.deadline = OS::get_singleton()->get_ticks_msec() / 1000.0 + p_timeout_seconds;
	entry.on_reply = p_on_reply;
	pending.push_back(entry);

	Array message;
	message.push_back(entry.request_id);
	message.push_back(p_arguments);
	debugger->get_default_debugger()->send_message(String(MCP_RUNTIME_CHANNEL) + ":" + p_command, message);
	return true;
}

void MCPRuntimeBridge::abandon_all(const String &p_reason) {
	for (const Pending &entry : pending) {
		if (entry.on_reply.is_valid()) {
			Dictionary payload;
			payload["message"] = p_reason;
			entry.on_reply.call(false, payload);
		} else {
			MCPDeferred::fail(entry.token, MCPToolError::INVALID_STATE, p_reason);
		}
	}
	pending.clear();
}

void MCPRuntimeBridge::poll() {
	if (pending.is_empty()) {
		return;
	}
	// A game that stopped mid-request will never reply, and the deferred deadline
	// alone would make the caller wait for the full timeout to learn something the
	// editor already knows.
	if (!is_game_reachable()) {
		abandon_all("the game stopped before it answered");
		return;
	}
	const double now = OS::get_singleton()->get_ticks_msec() / 1000.0;
	for (int i = pending.size() - 1; i >= 0; i--) {
		if (pending[i].deadline > now) {
			continue;
		}
		const Pending entry = pending[i];
		pending.remove_at(i);
		if (entry.on_reply.is_valid()) {
			Dictionary payload;
			payload["message"] = "the running game did not answer in time";
			entry.on_reply.call(false, payload);
		} else {
			MCPDeferred::fail(entry.token, MCPToolError::INVALID_STATE,
					"the running game did not answer in time");
		}
	}
}
