/**************************************************************************/
/*  mcp_progress.h                                                        */
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

#include "core/io/json.h"
#include "core/string/ustring.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"

class MCPSession; // Forward declaration; do NOT include mcp_session.h to avoid circular deps.

// ProgressContext bridges tool execution with the SSE event queue.
// Stack-allocated by dispatch_tool_with_progress(); valid for the duration
// of tool execution. Tool handlers use report_progress() to send SSE progress
// events and is_cancelled() to cooperatively abort.
struct ProgressContext {
	String token; // From request's _meta.progressToken (may be empty).
	MCPSession *session = nullptr; // Weak pointer; valid for the duration of tool execution.
	SafeFlag cancelled; // Set by the cancellation handler (atomic bool).

	// Report progress. No-op if token is empty or session is null.
	// Thread-safe: calls session->queue_sse_event() which is guarded by sse_mutex.
	void report_progress(int p_progress, int p_total, const String &p_message = "") {
		if (token.is_empty() || !session) {
			return;
		}

		Dictionary params;
		params["progressToken"] = token;
		params["progress"] = p_progress;
		params["total"] = p_total;
		if (!p_message.is_empty()) {
			params["message"] = p_message;
		}

		Dictionary notification;
		notification["jsonrpc"] = "2.0";
		notification["method"] = "notifications/progress";
		notification["params"] = params;

		// _queue_event is implemented in mcp_session.cpp to break the header
		// dependency cycle (mcp_progress.h forward-declares MCPSession).
		_queue_event(JSON::stringify(notification));
	}

	bool is_cancelled() const {
		return cancelled.is_set();
	}

private:
	// Implemented in mcp_session.cpp to break the circular dependency.
	// The implementation simply calls session->queue_sse_event(p_json).
	void _queue_event(const String &p_json);
};
