/**************************************************************************/
/*  mcp_http.h                                                            */
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

// The editor speaking Streamable HTTP MCP itself, so a client - the terminal's agent
// first among them - connects straight to the editor with no relay process between.
//
// The old path was: editor spawns agent, agent spawns godot-ai-relay, relay dials the
// editor's socket. For an agent the editor itself launched, the middle process earned
// nothing. The one real constraint - engine prints would corrupt a protocol stream on
// the editor's *stdio* - does not touch an HTTP listener, which owns its own socket.
//
// Everything here is plain parsing and formatting over Strings, deliberately free of
// sockets and the editor, so the tests can exercise every branch. The service owns the
// listener and calls in.
//
// Scope, on purpose:
// - POST /mcp carries one JSON-RPC message; the reply is the HTTP response.
// - DELETE /mcp ends the session named by Mcp-Session-Id.
// - No SSE and no server push: nothing here originates in the editor. A client that
//   wants notifications polls, exactly as it did through the relay's HTTP transport.
// - Loopback and bearer-token only. The token exists because browsers can POST to
//   localhost from any web page; the raw TCP bridge never needed one because a browser
//   cannot speak raw TCP.

#ifndef MCP_HTTP_H
#define MCP_HTTP_H

#include "core/string/ustring.h"
#include "core/templates/hash_map.h"

struct MCPHttpRequest {
	String method; // Uppercase: "POST", "DELETE", ...
	String path; // As sent, before any query string.
	HashMap<String, String> headers; // Keys lowercased; values stripped.
	String body;

	String header(const String &p_name, const String &p_default = String()) const {
		const String *found = headers.getptr(p_name.to_lower());
		return found ? *found : p_default;
	}
};

// Incremental parser over a connection's growing buffer. Returns true and consumes the
// request's bytes from p_buffer when one complete request is available; false when more
// bytes are needed. A malformed head sets r_error and leaves the caller to close the
// connection - HTTP has no way to resynchronise a broken framing.
bool mcp_http_parse_request(String &p_buffer, MCPHttpRequest &r_request, String &r_error);

// Response builders. Every response closes over its own framing; keep-alive is implied
// by HTTP/1.1 and honoured by leaving the connection open.
String mcp_http_response_json(int p_status, const String &p_body, const String &p_session_id = String());
String mcp_http_response_empty(int p_status, const String &p_reason);

// Bearer-token check in constant time. Comparing early-out would let a caller time
// their way to a token byte by byte.
bool mcp_http_token_matches(const String &p_authorization_header, const String &p_token);

// 32 hex characters of OS entropy: session ids and the per-run token.
String mcp_http_random_id();

#endif // MCP_HTTP_H
