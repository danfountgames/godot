/**************************************************************************/
/*  test_mcp_http.h                                                       */
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

#ifndef TEST_MCP_HTTP_H
#define TEST_MCP_HTTP_H

#include "../mcp_http.h"

#include "tests/test_macros.h"

namespace TestMCPHttp {

TEST_CASE("[godot_ai] An HTTP request parses only when all of it has arrived") {
	String buffer = "POST /mcp HTTP/1.1\r\nContent-Length: 11\r\nAuthorization: Bearer tok\r\n\r\n";
	MCPHttpRequest request;
	String error;
	// Head complete, body absent: wait, not fail.
	CHECK_FALSE(mcp_http_parse_request(buffer, request, error));
	CHECK(error.is_empty());

	buffer += "hello world";
	CHECK(mcp_http_parse_request(buffer, request, error));
	CHECK(request.method == "POST");
	CHECK(request.path == "/mcp");
	CHECK(request.body == "hello world");
	CHECK(request.header("authorization") == "Bearer tok");
	CHECK(buffer.is_empty());
}

TEST_CASE("[godot_ai] Header names are case-insensitive and query strings are stripped") {
	String buffer = "POST /mcp?session=1 HTTP/1.1\r\nCoNtEnT-LeNgTh: 2\r\nMcp-Session-Id: abc\r\n\r\nok";
	MCPHttpRequest request;
	String error;
	CHECK(mcp_http_parse_request(buffer, request, error));
	CHECK(request.path == "/mcp");
	CHECK(request.header("mcp-session-id") == "abc");
	CHECK(request.header("MCP-SESSION-ID") == "abc");
}

TEST_CASE("[godot_ai] Two pipelined requests parse one at a time, in order") {
	String buffer =
			"POST /mcp HTTP/1.1\r\nContent-Length: 1\r\n\r\nA"
			"DELETE /mcp HTTP/1.1\r\nMcp-Session-Id: s\r\n\r\n";
	MCPHttpRequest request;
	String error;
	CHECK(mcp_http_parse_request(buffer, request, error));
	CHECK(request.method == "POST");
	CHECK(request.body == "A");
	CHECK(mcp_http_parse_request(buffer, request, error));
	CHECK(request.method == "DELETE");
	CHECK(request.body.is_empty());
	CHECK_FALSE(mcp_http_parse_request(buffer, request, error));
	CHECK(error.is_empty());
}

TEST_CASE("[godot_ai] Malformed framing is an error, not a wait") {
	MCPHttpRequest request;
	String error;

	String bad_line = "NONSENSE\r\n\r\n";
	CHECK_FALSE(mcp_http_parse_request(bad_line, request, error));
	CHECK_FALSE(error.is_empty());

	String bad_length = "POST /mcp HTTP/1.1\r\nContent-Length: many\r\n\r\n";
	CHECK_FALSE(mcp_http_parse_request(bad_length, request, error));
	CHECK_FALSE(error.is_empty());

	// Chunked framing is refused loudly rather than misread.
	String chunked = "POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
	CHECK_FALSE(mcp_http_parse_request(chunked, request, error));
	CHECK(error.contains("content-length"));
}

TEST_CASE("[godot_ai] The bearer token matches exactly or not at all") {
	CHECK(mcp_http_token_matches("Bearer secret", "secret"));
	CHECK(mcp_http_token_matches("Bearer  secret ", "secret")); // Value is stripped.
	CHECK_FALSE(mcp_http_token_matches("Bearer secre", "secret"));
	CHECK_FALSE(mcp_http_token_matches("Bearer secrets", "secret"));
	CHECK_FALSE(mcp_http_token_matches("secret", "secret")); // Scheme required.
	CHECK_FALSE(mcp_http_token_matches("", "secret"));
	// An empty configured token authenticates nothing - never everything.
	CHECK_FALSE(mcp_http_token_matches("Bearer ", ""));
	CHECK_FALSE(mcp_http_token_matches("", ""));
}

TEST_CASE("[godot_ai] Responses carry their framing and the session id only when given") {
	const String with_session = mcp_http_response_json(200, "{}", "sid-1");
	CHECK(with_session.begins_with("HTTP/1.1 200 OK\r\n"));
	CHECK(with_session.contains("Mcp-Session-Id: sid-1\r\n"));
	CHECK(with_session.contains("Content-Length: 2\r\n"));
	CHECK(with_session.ends_with("{}"));

	const String without = mcp_http_response_json(200, "{}");
	CHECK_FALSE(without.contains("Mcp-Session-Id"));

	const String accepted = mcp_http_response_empty(202, "Accepted");
	CHECK(accepted.begins_with("HTTP/1.1 202 Accepted\r\n"));
	CHECK(accepted.contains("Content-Length: 0\r\n"));
}

TEST_CASE("[godot_ai] Random ids are hex, the right length, and not each other") {
	const String a = mcp_http_random_id();
	const String b = mcp_http_random_id();
	CHECK(a.length() == 32);
	CHECK(b.length() == 32);
	CHECK(a != b);
	for (int i = 0; i < a.length(); i++) {
		CHECK(String("0123456789abcdef").contains(String::chr(a[i])));
	}
}

} // namespace TestMCPHttp

#endif // TEST_MCP_HTTP_H
