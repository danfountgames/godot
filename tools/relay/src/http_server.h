/**************************************************************************/
/*  http_server.h                                                         */
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

// An HTTP transport for MCP, for the case the stdio relay cannot serve: a client
// that is not a child process, and more than one of them at once.
//
// This is the "Streamable HTTP" shape from the MCP specification, reduced to the part
// that is meaningful here: one endpoint, JSON request in, JSON response out, sessions
// identified by an `Mcp-Session-Id` header. Server-initiated streaming (the SSE half)
// is deliberately absent - nothing in this toolset pushes to a client, so opening that
// door would be untested surface rather than a feature.
//
// Two properties matter more than throughput, and shape the design:
//
//   * **Isolation.** Every session gets its own `Relay`, and therefore its own socket
//     to the editor, which is what stops one session reading another's replies. The
//     editor already accepts several peers, so this costs nothing there.
//   * **Authorisation.** The endpoint is loopback-only and bearer-token-gated by
//     default. An MCP endpoint reachable from the network with no token is a remote
//     code execution service, so neither is optional and both are refused loudly.

#ifndef GODOT_AI_RELAY_HTTP_SERVER_H
#define GODOT_AI_RELAY_HTTP_SERVER_H

#include "platform.h"
#include "relay.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace godot_ai {

struct HttpOptions {
	std::string host = "127.0.0.1";
	int port = 0;
	std::string path = "/mcp";
	std::string token; // Empty means "generate one and print it".
	// Refusing to listen off-loopback unless the operator insists is the difference
	// between a local convenience and an open door.
	bool allow_remote = false;
	int max_sessions = 32;
	// A session with no traffic for this long is dropped, so an abandoned client does
	// not hold an editor connection open forever.
	int session_idle_seconds = 900;
};

// Parsed HTTP request. Only what this server actually uses.
struct HttpRequest {
	std::string method;
	std::string target;
	std::map<std::string, std::string> headers; // Keys lower-cased.
	std::string body;

	std::string header(const std::string &p_name) const;
};

// Splits a request off the front of `p_buffer`. Returns false when more bytes are
// needed; sets r_error for a request that can never be valid.
bool http_parse_request(std::string &p_buffer, HttpRequest &r_request, bool &r_complete, std::string &r_error);

// Constant-time comparison, so a token cannot be recovered one byte at a time.
bool http_tokens_match(const std::string &p_a, const std::string &p_b);

class HttpServer {
	RelayOptions relay_options;
	HttpOptions options;

	platform::SocketHandle listener = platform::INVALID_SOCKET_HANDLE;
	std::string token;

	struct Session {
		std::unique_ptr<Relay> relay;
		double last_used = 0.0;
	};
	std::map<std::string, Session> sessions;

	void log(LogLevel p_level, const std::string &p_message) const;

	std::string create_session();
	void expire_idle_sessions();

	// Serves one connection: read a request, answer it, close.
	void serve(platform::SocketHandle p_client);
	std::string handle(const HttpRequest &p_request);

public:
	HttpServer(RelayOptions p_relay_options, HttpOptions p_options) :
			relay_options(std::move(p_relay_options)), options(std::move(p_options)) {}
	~HttpServer();

	// Binds the endpoint. Returns false with a usable message on failure.
	bool start(std::string &r_error);

	// The token clients must present. Valid after start().
	const std::string &get_token() const { return token; }

	// Serves until terminated. Returns the process exit code.
	int run();
};

} // namespace godot_ai

#endif // GODOT_AI_RELAY_HTTP_SERVER_H
