/**************************************************************************/
/*  mcp_session.h                                                         */
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

#include "mcp_types.h"

#include "core/io/stream_peer_tcp.h"
#include "core/object/ref_counted.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"

// MCPSession represents one TCP connection from an MCP client.
// It follows the LSPeer pattern from the GDScript LSP module:
// NO GDCLASS macro -- this is internal state only, not exposed to ClassDB.
//
// It contains:
//   - The TCP stream peer (connection)
//   - An incremental HTTP request parser (state machine)
//   - A response output queue
//
// NOTE: MCP protocol session state (session ID, initialization flags) is
// tracked separately in MCPSessionState (defined in mcp_protocol.h), because
// MCP Streamable HTTP sessions persist across TCP connections.

struct MCPSession : public RefCounted {
	// -----------------------------------------------------------------------
	// HTTP parser state machine
	// -----------------------------------------------------------------------

	enum ParseState {
		READING_REQUEST_LINE,
		READING_HEADERS,
		READING_BODY,
		REQUEST_COMPLETE,
	};

	// -----------------------------------------------------------------------
	// Connection
	// -----------------------------------------------------------------------

	Ref<StreamPeerTCP> connection;

	// -----------------------------------------------------------------------
	// HTTP parser state
	// -----------------------------------------------------------------------

	uint8_t req_buf[MCP_MAX_BUFFER_SIZE];
	int req_pos = 0;
	ParseState parse_state = READING_REQUEST_LINE;

	// Parsed request fields (populated by the state machine).
	String http_method; // Normalized to UPPERCASE.
	String http_path; // e.g., "/mcp".
	HashMap<String, String> headers; // Keys normalized to LOWERCASE.
	int content_length = 0;
	String request_body;

	// -----------------------------------------------------------------------
	// Response queue
	// -----------------------------------------------------------------------

	// Each entry is a complete HTTP response (headers + body), ready to send.
	Vector<CharString> res_queue;
	int res_sent = 0; // Byte offset into the current response being sent.

	// -----------------------------------------------------------------------
	// SSE (Server-Sent Events) state
	// -----------------------------------------------------------------------

	// When true, this TCP connection is a long-lived GET SSE stream.
	// The initial HTTP response headers have been sent; subsequent data is
	// SSE event frames ("event: message\ndata: ...\n\n").
	bool is_sse_stream = false;

	// The MCP session ID associated with this SSE stream (set when GET /mcp
	// is accepted). Used to look up the MCPSessionState for notification delivery.
	String sse_session_id;

	// -----------------------------------------------------------------------
	// Timestamps for timeout / GC
	// -----------------------------------------------------------------------

	uint64_t last_activity = 0;

	// Slow-loris protection: time when we entered READING_BODY state.
	static const int BODY_READ_TIMEOUT_SEC = 30;
	uint64_t body_start_time = 0;

	// -----------------------------------------------------------------------
	// Methods
	// -----------------------------------------------------------------------

	// Feed incoming bytes into the HTTP parser state machine.
	// Returns OK when a complete request is available (parse_state == REQUEST_COMPLETE).
	// Returns ERR_BUSY when more data is needed.
	// Returns FAILED on protocol errors or buffer overflow.
	Error handle_data();

	// Attempt to send queued responses over the TCP connection.
	Error send_data();

	// Queue a complete HTTP response for sending.
	void queue_response(const String &p_status, const String &p_body,
			const String &p_origin, const HashMap<String, String> &p_extra_headers = HashMap<String, String>());

	// Begin an SSE stream response (sends HTTP headers, sets is_sse_stream flag).
	void begin_sse_stream(const String &p_session_id, const String &p_origin);

	// Queue a single SSE event frame for delivery on this stream.
	// The p_data string is a complete JSON-RPC message.
	void queue_sse_event(const String &p_data);

	// Reset the parser state for the next request on the same connection.
	void reset_request();

	MCPSession();
};
