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

#include "core/os/os.h"
#include "core/string/print_string.h"

MCPSession::MCPSession() {
	memset(req_buf, 0, sizeof(req_buf));
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
}

Error MCPSession::handle_data() {
	int read = 0;

	// Update activity timestamp on any incoming data.
	last_activity = OS::get_singleton()->get_ticks_usec();

	while (true) {
		switch (parse_state) {
			case READING_REQUEST_LINE: {
				while (true) {
					if (req_pos >= MCP_MAX_BUFFER_SIZE) {
						reset_request();
						ERR_FAIL_V_MSG(FAILED, "MCP: Request line too long, exceeds buffer size.");
					}
					Error err = connection->get_partial_data(&req_buf[req_pos], 1, read);
					if (err != OK) {
						return FAILED;
					}
					if (read != 1) {
						return ERR_BUSY;
					}

					// Check for end of request line: \r\n
					if (req_pos > 0 && req_buf[req_pos - 1] == '\r' && req_buf[req_pos] == '\n') {
						req_buf[req_pos - 1] = '\0';
						String request_line = String::utf8((const char *)req_buf, req_pos - 1);
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
					Error err = connection->get_partial_data(&req_buf[req_pos], 1, read);
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
						req_buf[l - 3] = '\0';
						String header_block = String::utf8((const char *)req_buf, l - 3);
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
						} else {
							content_length = 0;
						}

						req_pos = 0;
						if (content_length > 0) {
							if (content_length > MCP_MAX_BUFFER_SIZE) {
								reset_request();
								ERR_FAIL_V_MSG(FAILED, "MCP: Content-Length exceeds maximum buffer size.");
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
					Error err = connection->get_partial_data(&req_buf[req_pos], 1, read);
					if (err != OK) {
						return FAILED;
					}
					if (read != 1) {
						return ERR_BUSY;
					}
					req_pos++;
				}

				request_body = String::utf8((const char *)req_buf, req_pos);
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
	while (!res_queue.is_empty()) {
		CharString c_res = res_queue[0];
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
			res_queue.remove_at(0);
		} else {
			return ERR_BUSY;
		}
	}
	return OK;
}

void MCPSession::queue_response(const String &p_status, const String &p_body,
		const String &p_origin, const HashMap<String, String> &p_extra_headers) {
	String response;

	response += "HTTP/1.1 " + p_status + "\r\n";

	// CORS headers -- validated origin echo-back, never wildcards.
	if (!p_origin.is_empty()) {
		response += "Access-Control-Allow-Origin: " + p_origin + "\r\n";
		response += "Vary: Origin\r\n";
	}
	response += "Access-Control-Allow-Headers: Content-Type, Accept, Authorization, Mcp-Session-Id, MCP-Protocol-Version\r\n";
	response += "Access-Control-Allow-Methods: POST, GET, DELETE, OPTIONS\r\n";
	response += "Access-Control-Expose-Headers: Mcp-Session-Id\r\n";

	for (const KeyValue<String, String> &E : p_extra_headers) {
		response += E.key + ": " + E.value + "\r\n";
	}

	if (!p_body.is_empty()) {
		CharString body_utf8 = p_body.utf8();
		response += "Content-Type: application/json\r\n";
		response += "Content-Length: " + itos(body_utf8.length()) + "\r\n";
		response += "\r\n";
		response += p_body;
	} else {
		response += "Content-Length: 0\r\n";
		response += "\r\n";
	}

	res_queue.push_back(response.utf8());
}

void MCPSession::begin_sse_stream(const String &p_session_id, const String &p_origin) {
	is_sse_stream = true;
	sse_session_id = p_session_id;

	String response;
	response += "HTTP/1.1 200 OK\r\n";
	response += "Content-Type: text/event-stream\r\n";
	response += "Cache-Control: no-cache\r\n";
	response += "Connection: keep-alive\r\n";
	response += "Mcp-Session-Id: " + p_session_id + "\r\n";

	// CORS headers.
	if (!p_origin.is_empty()) {
		response += "Access-Control-Allow-Origin: " + p_origin + "\r\n";
		response += "Vary: Origin\r\n";
	}
	response += "Access-Control-Allow-Headers: Content-Type, Accept, Authorization, Mcp-Session-Id, MCP-Protocol-Version\r\n";
	response += "Access-Control-Allow-Methods: POST, GET, DELETE, OPTIONS\r\n";
	response += "Access-Control-Expose-Headers: Mcp-Session-Id\r\n";
	response += "\r\n";

	res_queue.push_back(response.utf8());
}

void MCPSession::queue_sse_event(const String &p_data) {
	// SSE event frame format (RFC 9001 / HTML Living Standard):
	//   event: message\n
	//   data: <json>\n
	//   \n
	String frame = "event: message\ndata: " + p_data + "\n\n";
	res_queue.push_back(frame.utf8());
}
