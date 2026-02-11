/**************************************************************************/
/*  mcp_session.cpp                                                       */
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

#include "mcp_session.h"

#include "mcp_progress.h"

#include "core/os/os.h"
#include "core/string/print_string.h"

MCPSession::MCPSession() {
	req_buf.resize(16384); // 16 KB initial capacity -- grows on demand up to MCP_MAX_BUFFER_SIZE.
	memset(req_buf.ptrw(), 0, req_buf.size());
	last_activity = OS::get_singleton()->get_ticks_usec();
}

void MCPSession::reset_request() {
	req_pos = 0;
	parse_state = READING_REQUEST_LINE;
	http_method = String();
	http_path = String();
	headers.clear();
	content_length = 0;
	request_body = String();
	body_start_time = 0;
	request_start_time = 0;
}

Error MCPSession::handle_data() {
	int read = 0;

	// Update activity timestamp on any incoming data.
	uint64_t now = OS::get_singleton()->get_ticks_usec();
	last_activity = now;

	// Start the request timeout on the first byte of a new request.
	if (request_start_time == 0 && parse_state != REQUEST_COMPLETE) {
		request_start_time = now;
	}

	// Slow-loris protection for all phases (request line, headers, body).
	if (request_start_time > 0) {
		uint64_t elapsed_sec = (now - request_start_time) / 1000000;
		if (elapsed_sec > REQUEST_READ_TIMEOUT_SEC) {
			reset_request();
			ERR_FAIL_V_MSG(FAILED, "MCP: Request read timeout (slow-loris protection).");
		}
	}

	while (true) {
		switch (parse_state) {
			case READING_REQUEST_LINE: {
				while (true) {
					if (req_pos >= MCP_MAX_BUFFER_SIZE) {
						reset_request();
						ERR_FAIL_V_MSG(FAILED, "MCP: Request line too long, exceeds buffer size.");
					}
					if (req_pos >= req_buf.size()) {
						req_buf.resize(MIN((int)req_buf.size() * 2, MCP_MAX_BUFFER_SIZE));
					}
					Error err = connection->get_partial_data(req_buf.ptrw() + req_pos, 1, read);
					if (err != OK) {
						return FAILED;
					}
					if (read != 1) {
						return ERR_BUSY;
					}

					// Check for end of request line: \r\n
					if (req_pos > 0 && req_buf[req_pos - 1] == '\r' && req_buf[req_pos] == '\n') {
						req_buf.ptrw()[req_pos - 1] = '\0';
						String request_line = String::utf8((const char *)req_buf.ptr(), req_pos - 1);
						Vector<String> parts = request_line.split(" ");
						if (parts.size() >= 2) {
							http_method = parts[0].to_upper();
							http_path = parts[1];
						} else {
							reset_request();
							ERR_FAIL_V_MSG(FAILED, "MCP: Malformed HTTP request line.");
						}

						req_pos = 0;
						parse_state = READING_HEADERS;
						break;
					}

					req_pos++;
				}
			} break;

			case READING_HEADERS: {
				while (true) {
					if (req_pos >= MCP_MAX_BUFFER_SIZE) {
						reset_request();
						ERR_FAIL_V_MSG(FAILED, "MCP: Headers too long, exceeds buffer size.");
					}
					if (req_pos >= req_buf.size()) {
						req_buf.resize(MIN((int)req_buf.size() * 2, MCP_MAX_BUFFER_SIZE));
					}
					Error err = connection->get_partial_data(req_buf.ptrw() + req_pos, 1, read);
					if (err != OK) {
						return FAILED;
					}
					if (read != 1) {
						return ERR_BUSY;
					}

					// Check for end of headers: \r\n\r\n
					int l = req_pos;
					if (l >= 3 &&
							req_buf[l] == '\n' &&
							req_buf[l - 1] == '\r' &&
							req_buf[l - 2] == '\n' &&
							req_buf[l - 3] == '\r') {
						req_buf.ptrw()[l - 3] = '\0';
						String header_block = String::utf8((const char *)req_buf.ptr(), l - 3);
						Vector<String> header_lines = header_block.split("\r\n");

						for (int i = 0; i < header_lines.size(); i++) {
							int colon_pos = header_lines[i].find(":");
							if (colon_pos > 0) {
								String header_name = header_lines[i].substr(0, colon_pos);
								String header_value = header_lines[i].substr(colon_pos + 1);
								headers[header_name.to_lower()] = header_value.strip_edges();
							}
						}

						if (headers.has("content-length")) {
							content_length = headers["content-length"].to_int();
							if (content_length < 0) {
								content_length = 0;
							}
						} else {
							content_length = 0;
						}

						req_pos = 0;
						if (content_length > 0) {
							if (content_length > MCP_MAX_BUFFER_SIZE) {
								reset_request();
								ERR_FAIL_V_MSG(FAILED, "MCP: Content-Length exceeds maximum buffer size.");
							}
							// Pre-allocate buffer for the body now that we know the size.
							if (content_length > req_buf.size()) {
								req_buf.resize(content_length);
							}
							parse_state = READING_BODY;
							body_start_time = OS::get_singleton()->get_ticks_usec();
						} else {
							parse_state = REQUEST_COMPLETE;
							return OK;
						}
						break;
					}

					req_pos++;
				}
			} break;

			case READING_BODY: {
				// Check body read timeout (slow-loris protection).
				if (body_start_time > 0) {
					uint64_t elapsed_sec = (OS::get_singleton()->get_ticks_usec() - body_start_time) / 1000000;
					if (elapsed_sec > (uint64_t)BODY_READ_TIMEOUT_SEC) {
						reset_request();
						ERR_FAIL_V_MSG(FAILED, "MCP: Body read timeout exceeded (" + itos(BODY_READ_TIMEOUT_SEC) + "s).");
					}
				}

				while (req_pos < content_length) {
					if (req_pos >= MCP_MAX_BUFFER_SIZE) {
						reset_request();
						ERR_FAIL_V_MSG(FAILED, "MCP: Body exceeds maximum buffer size.");
					}
					if (req_pos >= req_buf.size()) {
						req_buf.resize(MIN((int)req_buf.size() * 2, MCP_MAX_BUFFER_SIZE));
					}
					int bytes_remaining = content_length - req_pos;
					int buf_space = req_buf.size() - req_pos;
					int to_read = MIN(bytes_remaining, buf_space);
					Error err = connection->get_partial_data(req_buf.ptrw() + req_pos, to_read, read);
					if (err != OK) {
						return FAILED;
					}
					if (read == 0) {
						return ERR_BUSY;
					}
					req_pos += read;
				}

				request_body = String::utf8((const char *)req_buf.ptr(), req_pos);
				req_pos = 0;
				body_start_time = 0;
				parse_state = REQUEST_COMPLETE;
				return OK;
			} break;

			case REQUEST_COMPLETE: {
				return OK;
			} break;
		}
	}
}

Error MCPSession::send_data() {
	int sent = 0;
	while (res_read_pos < res_queue.size()) {
		const CharString &c_res = res_queue[res_read_pos];
		if (res_sent < c_res.size()) {
			Error err = connection->put_partial_data(
					(const uint8_t *)c_res.get_data() + res_sent,
					c_res.size() - res_sent - 1, // -1 for null terminator.
					sent);
			if (err != OK) {
				return err;
			}
			res_sent += sent;
		}
		if (res_sent >= c_res.size() - 1) {
			res_sent = 0;
			res_read_pos++;
		} else {
			return ERR_BUSY;
		}
	}
	// Compact: only when the queue is fully drained.
	if (res_read_pos > 0) {
		res_queue.clear();
		res_read_pos = 0;
	}
	return OK;
}

String MCPSession::_build_cors_headers(const String &p_origin) {
	String h;
	// Validated origin echo-back, never wildcards.
	// Reject origins containing CRLF to prevent header injection.
	if (!p_origin.is_empty() && p_origin.find("\r") == -1 && p_origin.find("\n") == -1) {
		h += "Access-Control-Allow-Origin: " + p_origin + "\r\n";
		h += "Vary: Origin\r\n";
	}
	h += "Access-Control-Allow-Headers: Content-Type, Accept, Authorization, Mcp-Session-Id, MCP-Protocol-Version\r\n";
	h += "Access-Control-Allow-Methods: POST, GET, DELETE, OPTIONS\r\n";
	h += "Access-Control-Expose-Headers: Mcp-Session-Id\r\n";
	return h;
}

void MCPSession::queue_response(const String &p_status, const String &p_body,
		const String &p_origin, const HashMap<String, String> &p_extra_headers) {
	// Prevent unbounded queue growth from slow/stalled clients.
	if (res_queue.size() - res_read_pos > MAX_RES_QUEUE_SIZE) {
		ERR_PRINT_ONCE("MCP: Response queue full, dropping response.");
		return;
	}
	String response;

	response += "HTTP/1.1 " + p_status + "\r\n";
	response += "Connection: close\r\n";

	// CORS headers.
	response += _build_cors_headers(p_origin);

	for (const KeyValue<String, String> &E : p_extra_headers) {
		response += E.key + ": " + E.value + "\r\n";
	}

	if (!p_body.is_empty()) {
		CharString body_utf8 = p_body.utf8();
		response += "Content-Type: application/json\r\n";
		response += "Content-Length: " + itos(body_utf8.length()) + "\r\n";
		response += "\r\n";
		// Encode headers to UTF-8, then combine with the already-encoded body
		// to avoid a redundant second UTF-8 encode of the body.
		CharString header_utf8 = response.utf8();
		int header_len = header_utf8.length();
		int body_len = body_utf8.length();
		CharString combined;
		combined.resize_uninitialized(header_len + body_len + 1); // +1 for null terminator.
		memcpy(combined.ptrw(), header_utf8.get_data(), header_len);
		memcpy(combined.ptrw() + header_len, body_utf8.get_data(), body_len);
		combined.set(header_len + body_len, '\0');
		res_queue.push_back(combined);
	} else {
		response += "Content-Length: 0\r\n";
		response += "\r\n";
		res_queue.push_back(response.utf8());
	}
	close_after_send = true;
}

void MCPSession::begin_sse_stream(const String &p_session_id, const String &p_origin) {
	sse_session_id = p_session_id;
	response_mode = RESPONSE_SSE_GET;
	sse_headers_sent = true;

	String response;
	response += "HTTP/1.1 200 OK\r\n";
	response += "Content-Type: text/event-stream\r\n";
	response += "Cache-Control: no-cache\r\n";
	response += "Connection: keep-alive\r\n";
	response += "Mcp-Session-Id: " + p_session_id + "\r\n";

	// CORS headers.
	response += _build_cors_headers(p_origin);
	response += "\r\n";

	res_queue.push_back(response.utf8());
}

void MCPSession::begin_sse_response(const String &p_session_id, const String &p_origin) {
	ERR_FAIL_COND(sse_headers_sent); // Double-begin guard.

	response_mode = RESPONSE_SSE_POST;
	sse_headers_sent = true;

	String response;
	response += "HTTP/1.1 200 OK\r\n";
	response += "Content-Type: text/event-stream\r\n";
	response += "Cache-Control: no-cache\r\n";
	response += "Connection: close\r\n"; // POST SSE: close after final event.

	if (!p_session_id.is_empty()) {
		response += "Mcp-Session-Id: " + p_session_id + "\r\n";
	}

	// CORS headers.
	response += _build_cors_headers(p_origin);
	response += "\r\n";

	res_queue.push_back(response.utf8());
}

String MCPSession::_format_sse_frame(const String &p_data) {
	// SSE event frame format (HTML Living Standard, Server-Sent Events):
	// Multi-line data must prefix each line with "data: ".
	//   event: message\n
	//   data: <line1>\n
	//   data: <line2>\n
	//   \n
	String frame = "event: message\n";
	Vector<String> lines = p_data.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		frame += "data: " + lines[i] + "\n";
	}
	frame += "\n"; // Empty line terminates the event.
	return frame;
}

void MCPSession::queue_sse_event(const String &p_data) {
	res_queue.push_back(_format_sse_frame(p_data).utf8());
}

void MCPSession::queue_sse_event_threadsafe(const String &p_data) {
	MutexLock lock(sse_mutex);

	// Backpressure: cap the queue.
	if (sse_event_queue.size() >= MCP_MAX_EVENT_QUEUE_SIZE) {
		sse_event_queue.remove_at(0); // Drop oldest event.
	}

	sse_event_queue.push_back(_format_sse_frame(p_data));
}

Error MCPSession::flush_sse_events() {
	MutexLock lock(sse_mutex);

	if (sse_event_queue.is_empty()) {
		return OK;
	}

	for (const String &event : sse_event_queue) {
		CharString utf8 = event.utf8();
		res_queue.push_back(utf8);
	}
	sse_event_queue.clear();
	return OK;
}

void MCPSession::end_sse_stream() {
	// Flush any remaining events before closing.
	flush_sse_events();

	response_mode = RESPONSE_NONE;
	sse_headers_sent = false;

	// For POST SSE: allow the connection to accept a new HTTP request.
	reset_request();
}

// ---------------------------------------------------------------------------
// ProgressContext::_queue_event -- implemented here (not in mcp_progress.h)
// to break the circular header dependency: mcp_progress.h forward-declares
// MCPSession, while mcp_session.h does not include mcp_progress.h.
// ---------------------------------------------------------------------------

void ProgressContext::_queue_event(const String &p_json) {
	if (session) {
		session->queue_sse_event_threadsafe(p_json);
	}
}
