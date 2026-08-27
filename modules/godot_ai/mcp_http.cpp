/**************************************************************************/
/*  mcp_http.cpp                                                          */
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

#include "mcp_http.h"

#include "core/os/os.h"

// A request head larger than this is nobody's honest MCP call.
static const int MAX_HEAD_CHARACTERS = 16 * 1024;
// Bodies carry one JSON-RPC message; a screenshot request is bytes, its *response* is
// large. Matches the bridge's own frame ceiling rather than inventing a second number.
static const int MAX_BODY_CHARACTERS = 8 * 1024 * 1024;

bool mcp_http_parse_request(String &p_buffer, MCPHttpRequest &r_request, String &r_error) {
	r_error = String();
	const int head_end = p_buffer.find("\r\n\r\n");
	if (head_end < 0) {
		if (p_buffer.length() > MAX_HEAD_CHARACTERS) {
			r_error = "request head too large";
		}
		return false;
	}

	const String head = p_buffer.substr(0, head_end);
	const Vector<String> lines = head.split("\r\n");
	if (lines.is_empty()) {
		r_error = "empty request head";
		return false;
	}

	const Vector<String> request_line = lines[0].split(" ");
	if (request_line.size() != 3 || !request_line[2].begins_with("HTTP/1.")) {
		r_error = "malformed request line";
		return false;
	}

	MCPHttpRequest request;
	request.method = request_line[0].to_upper();
	request.path = request_line[1];
	const int query = request.path.find("?");
	if (query >= 0) {
		request.path = request.path.substr(0, query);
	}

	for (int i = 1; i < lines.size(); i++) {
		const int colon = lines[i].find(":");
		if (colon <= 0) {
			r_error = "malformed header line";
			return false;
		}
		request.headers[lines[i].substr(0, colon).strip_edges().to_lower()] =
				lines[i].substr(colon + 1).strip_edges();
	}

	int content_length = 0;
	const String declared = request.header("content-length");
	if (!declared.is_empty()) {
		if (!declared.is_valid_int() || declared.to_int() < 0) {
			r_error = "malformed content-length";
			return false;
		}
		content_length = declared.to_int();
	}
	if (content_length > MAX_BODY_CHARACTERS) {
		r_error = "request body too large";
		return false;
	}
	// Chunked bodies are legal HTTP that no MCP client we serve emits; refusing loudly
	// beats misreading the framing and treating chunk sizes as JSON.
	if (request.header("transfer-encoding") != String()) {
		r_error = "transfer-encoding is not supported; send content-length";
		return false;
	}

	const int body_start = head_end + 4;
	if (p_buffer.length() - body_start < content_length) {
		return false; // The body is still arriving.
	}

	request.body = p_buffer.substr(body_start, content_length);
	p_buffer = p_buffer.substr(body_start + content_length);
	r_request = request;
	return true;
}

static String status_line(int p_status, const String &p_reason) {
	return vformat("HTTP/1.1 %d %s\r\n", p_status, p_reason);
}

String mcp_http_response_json(int p_status, const String &p_body, const String &p_session_id) {
	const CharString utf8 = p_body.utf8();
	String response = status_line(p_status, p_status == 200 ? "OK" : (p_status == 401 ? "Unauthorized" : (p_status == 400 ? "Bad Request" : "Error")));
	response += "Content-Type: application/json\r\n";
	if (!p_session_id.is_empty()) {
		response += "Mcp-Session-Id: " + p_session_id + "\r\n";
	}
	response += vformat("Content-Length: %d\r\n\r\n", utf8.length());
	response += p_body;
	return response;
}

String mcp_http_response_empty(int p_status, const String &p_reason) {
	return status_line(p_status, p_reason) + "Content-Length: 0\r\n\r\n";
}

bool mcp_http_token_matches(const String &p_authorization_header, const String &p_token) {
	if (p_token.is_empty()) {
		return false; // No token configured means nothing authenticates, never everything.
	}
	const String scheme = "Bearer ";
	if (!p_authorization_header.begins_with(scheme)) {
		return false;
	}
	const CharString presented = p_authorization_header.substr(scheme.length()).strip_edges().utf8();
	const CharString expected = p_token.utf8();
	// Fold the length difference into the accumulator instead of returning on it; the
	// loop always walks the expected token so timing does not depend on the input.
	unsigned int acc = (unsigned int)(presented.length() ^ expected.length());
	for (int i = 0; i < expected.length(); i++) {
		const char presented_char = i < presented.length() ? presented[i] : 0;
		acc |= (unsigned int)(presented_char ^ expected[i]);
	}
	return acc == 0;
}

String mcp_http_random_id() {
	uint8_t bytes[16];
	OS::get_singleton()->get_entropy(bytes, sizeof(bytes));
	String id;
	for (size_t i = 0; i < sizeof(bytes); i++) {
		id += vformat("%02x", bytes[i]);
	}
	return id;
}
