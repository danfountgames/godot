/**************************************************************************/
/*  mcp_status_data.h                                                     */
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

#include "core/os/mutex.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"

// ---------------------------------------------------------------------------
// MCPRequestEvent -- one captured request/response event for the status panel.
// ---------------------------------------------------------------------------

struct MCPRequestEvent {
	uint64_t seq = 0; // Monotonically increasing sequence number (set by MCPEventBuffer).
	uint64_t timestamp_usec = 0; // OS::get_ticks_usec() at emission time.
	String method; // "tools/call", "initialize", etc.
	String session_id; // First 8 chars of session ID (or "-").
	String client_ip; // Source IP (from TCP peer).
	int http_status = 0; // 200, 400, 404, 500, etc.
	bool is_error = false;
	uint64_t duration_usec = 0; // Time from request received to response sent.
	String request_json; // Full request body (truncated to 4KB for UI).
	String response_json; // Full response body (truncated to 4KB for UI).
	String tool_name; // For tools/call: which tool was invoked.
};

// ---------------------------------------------------------------------------
// MCPEventBuffer -- thread-safe ring buffer for MCPRequestEvent records.
//
// The poll thread pushes events via push(). The UI main thread reads via
// read_since() using a cursor (sequence number) from the previous read.
// ---------------------------------------------------------------------------

class MCPEventBuffer {
public:
	static const int CAPACITY = 2000;

private:
	Vector<MCPRequestEvent> events;
	uint64_t next_seq = 1;
	int write_pos = 0;
	int count = 0;
	mutable Mutex mutex;

public:
	MCPEventBuffer();

	// Push a new event into the buffer. Thread-safe.
	// The event's seq field is overwritten with the next sequence number.
	void push(const MCPRequestEvent &p_event);

	// Read all events with seq > p_cursor, up to p_limit events.
	// Returns a Vector of events in chronological order.
	Vector<MCPRequestEvent> read_since(uint64_t p_cursor, int p_limit = 200) const;

	// Return the latest sequence number (0 if no events).
	uint64_t latest_seq() const;

	// Clear all events.
	void clear();
};

// ---------------------------------------------------------------------------
// MCPClientSnapshot -- thread-safe snapshot of a connected client.
// ---------------------------------------------------------------------------

struct MCPClientSnapshot {
	String session_id; // First 8 chars + "...".
	String peer_address; // IP:port from TCP peer.
	uint64_t connected_since = 0; // Timestamp (usec).
	uint64_t last_activity = 0; // Timestamp (usec).
	bool is_sse_stream = false; // GET SSE stream vs regular HTTP.
};

// ---------------------------------------------------------------------------
// MCPToolStats -- aggregated statistics for a single tool.
// ---------------------------------------------------------------------------

struct MCPToolStats {
	int call_count = 0;
	uint64_t total_duration_usec = 0;
	uint64_t last_call_time_usec = 0;
	bool last_was_error = false;
};

// ---------------------------------------------------------------------------
// MCPTestEvent -- a single test event for the status panel.
// ---------------------------------------------------------------------------

struct MCPTestEvent {
	enum Type {
		TEST_RUN_STARTED,
		TEST_METHOD_RESULT,
		TEST_FILE_COMPILE_ERROR,
		TEST_RUN_COMPLETE,
	};

	uint64_t seq = 0;
	Type type = TEST_METHOD_RESULT;
	uint64_t timestamp_usec = 0;
	int run_number = 0;

	// TEST_METHOD_RESULT fields:
	String file_path;
	String method_name;
	String status; // "passed", "failed", "error", "skipped", "timeout", "pending"
	String message;
	String error_file;
	int error_line = 0;
	int duration_ms = 0;

	// TEST_FILE_COMPILE_ERROR fields:
	Array compile_errors; // [{line, column, message}, ...]

	// TEST_RUN_COMPLETE fields:
	int total = 0;
	int passed = 0;
	int failed = 0;
	int errors = 0;
	int skipped = 0;
	int total_duration_ms = 0;
};

// ---------------------------------------------------------------------------
// MCPTestEventBuffer -- thread-safe ring buffer for test events.
//
// The bridge pushes events as test results arrive. The UI main thread reads
// via read_since() using a cursor (sequence number) from the previous read.
// ---------------------------------------------------------------------------

class MCPTestEventBuffer {
public:
	static const int CAPACITY = 500;

	MCPTestEventBuffer();
	void push(const MCPTestEvent &p_event);
	Vector<MCPTestEvent> read_since(uint64_t p_cursor, int p_limit = 100) const;
	uint64_t latest_seq() const;
	void clear();

private:
	Vector<MCPTestEvent> events;
	uint64_t next_seq = 1;
	int write_pos = 0;
	int count = 0;
	mutable Mutex mutex;
};
