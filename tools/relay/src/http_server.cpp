/**************************************************************************/
/*  http_server.cpp                                                       */
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

#include "http_server.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace godot_ai {

namespace {

// A request bigger than this is not a tool call, it is an attack or a bug.
const size_t MAX_REQUEST_BYTES = 8 * 1024 * 1024;
const size_t MAX_HEADER_BYTES = 64 * 1024;

double now_seconds() {
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() / 1000.0;
}

std::string lower(const std::string &p_text) {
	std::string out = p_text;
	for (char &character : out) {
		if (character >= 'A' && character <= 'Z') {
			character = (char)(character - 'A' + 'a');
		}
	}
	return out;
}

std::string trim(const std::string &p_text) {
	size_t start = 0;
	size_t end = p_text.size();
	while (start < end && (p_text[start] == ' ' || p_text[start] == '\t')) {
		start++;
	}
	while (end > start && (p_text[end - 1] == ' ' || p_text[end - 1] == '\t' || p_text[end - 1] == '\r')) {
		end--;
	}
	return p_text.substr(start, end - start);
}

// The path part of a request target, without any query string.
std::string target_path(const std::string &p_target) {
	const size_t question = p_target.find('?');
	return question == std::string::npos ? p_target : p_target.substr(0, question);
}

std::string status_text(int p_status) {
	switch (p_status) {
		case 200:
			return "OK";
		case 202:
			return "Accepted";
		case 204:
			return "No Content";
		case 400:
			return "Bad Request";
		case 401:
			return "Unauthorized";
		case 404:
			return "Not Found";
		case 405:
			return "Method Not Allowed";
		case 413:
			return "Payload Too Large";
		case 429:
			return "Too Many Requests";
		case 500:
			return "Internal Server Error";
		default:
			return "Error";
	}
}

std::string response(int p_status, const std::string &p_body, const std::string &p_content_type = "application/json", const std::string &p_extra_headers = std::string()) {
	std::string out = "HTTP/1.1 " + std::to_string(p_status) + " " + status_text(p_status) + "\r\n";
	if (!p_body.empty()) {
		out += "Content-Type: " + p_content_type + "\r\n";
	}
	out += "Content-Length: " + std::to_string(p_body.size()) + "\r\n";
	// One request per connection: keep-alive would buy throughput this transport does
	// not need, at the cost of state that has to be got right.
	out += "Connection: close\r\n";
	out += p_extra_headers;
	out += "\r\n";
	out += p_body;
	return out;
}

// A JSON-RPC error carried by an HTTP response, so a client sees one error shape
// whether the request died in the transport or in the editor.
std::string rpc_error(int p_status, int p_code, const std::string &p_message, const JSONValueRef &p_id = nullptr, const std::string &p_extra_headers = std::string()) {
	JSONValueRef body = JSONValue::make_object();
	body->set("jsonrpc", JSONValue::make_string("2.0"));
	body->set("id", p_id ? p_id : JSONValue::make_null());
	JSONValueRef error = JSONValue::make_object();
	error->set("code", JSONValue::make_number(p_code));
	error->set("message", JSONValue::make_string(p_message));
	body->set("error", error);
	return response(p_status, body->to_string(), "application/json", p_extra_headers);
}

} // namespace

std::string HttpRequest::header(const std::string &p_name) const {
	const auto found = headers.find(lower(p_name));
	return found == headers.end() ? std::string() : found->second;
}

bool http_parse_request(std::string &p_buffer, HttpRequest &r_request, bool &r_complete, std::string &r_error) {
	r_complete = false;

	const size_t header_end = p_buffer.find("\r\n\r\n");
	if (header_end == std::string::npos) {
		if (p_buffer.size() > MAX_HEADER_BYTES) {
			r_error = "request headers exceed " + std::to_string(MAX_HEADER_BYTES) + " bytes";
			return false;
		}
		return true; // Need more bytes.
	}

	const std::string head = p_buffer.substr(0, header_end);
	size_t line_start = 0;
	size_t line_end = head.find("\r\n");
	const std::string request_line = head.substr(0, line_end == std::string::npos ? head.size() : line_end);

	const size_t first_space = request_line.find(' ');
	const size_t second_space = first_space == std::string::npos ? std::string::npos : request_line.find(' ', first_space + 1);
	if (first_space == std::string::npos || second_space == std::string::npos) {
		r_error = "malformed request line";
		return false;
	}
	r_request.method = request_line.substr(0, first_space);
	r_request.target = request_line.substr(first_space + 1, second_space - first_space - 1);
	r_request.headers.clear();

	line_start = line_end == std::string::npos ? head.size() : line_end + 2;
	while (line_start < head.size()) {
		line_end = head.find("\r\n", line_start);
		const std::string line = head.substr(line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);
		const size_t colon = line.find(':');
		if (colon != std::string::npos) {
			r_request.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
		}
		if (line_end == std::string::npos) {
			break;
		}
		line_start = line_end + 2;
	}

	size_t content_length = 0;
	const std::string length_header = r_request.header("content-length");
	if (!length_header.empty()) {
		errno = 0;
		char *parse_end = nullptr;
		const long parsed = strtol(length_header.c_str(), &parse_end, 10);
		if (errno != 0 || parse_end == length_header.c_str() || parsed < 0) {
			r_error = "invalid Content-Length";
			return false;
		}
		content_length = (size_t)parsed;
	}
	if (content_length > MAX_REQUEST_BYTES) {
		r_error = "request body exceeds " + std::to_string(MAX_REQUEST_BYTES) + " bytes";
		return false;
	}
	// Chunked bodies are not accepted: an MCP client has no reason to stream a
	// request, and silently ignoring the encoding would truncate one.
	if (lower(r_request.header("transfer-encoding")).find("chunked") != std::string::npos) {
		r_error = "chunked request bodies are not supported";
		return false;
	}

	const size_t body_start = header_end + 4;
	if (p_buffer.size() < body_start + content_length) {
		return true; // Need more bytes.
	}
	r_request.body = p_buffer.substr(body_start, content_length);
	p_buffer.erase(0, body_start + content_length);
	r_complete = true;
	return true;
}

bool http_tokens_match(const std::string &p_a, const std::string &p_b) {
	// Length is not secret, but the comparison must not stop early on the first
	// differing byte, or the token can be guessed one character at a time.
	if (p_a.size() != p_b.size()) {
		return false;
	}
	unsigned char difference = 0;
	for (size_t i = 0; i < p_a.size(); i++) {
		difference |= (unsigned char)(p_a[i] ^ p_b[i]);
	}
	return difference == 0;
}

HttpServer::~HttpServer() {
	if (listener != platform::INVALID_SOCKET_HANDLE) {
		platform::socket_close(listener);
	}
}

void HttpServer::log(LogLevel p_level, const std::string &p_message) const {
	if (p_level > relay_options.log_level) {
		return;
	}
	static const char *names[] = { "error", "warn", "info", "debug" };
	fprintf(stderr, "[godot-ai-relay] %s: %s\n", names[p_level], p_message.c_str());
	fflush(stderr);
}

bool HttpServer::start(std::string &r_error) {
	if (options.port <= 0) {
		r_error = "--http-port requires a port number";
		return false;
	}
	if (options.host != "127.0.0.1" && !options.allow_remote) {
		r_error = "refusing to listen on " + options.host +
				": this endpoint runs editor tools, so binding it off loopback needs "
				"--http-allow-remote and a token you chose yourself";
		return false;
	}

	token = options.token;
	if (token.empty()) {
		bool strong = false;
		token = platform::random_token(24, strong);
		if (!strong) {
			r_error = "could not generate a secure token on this system; pass --http-token";
			return false;
		}
		// stderr, never stdout: stdout is protocol territory even here.
		log(LOG_WARN, "generated bearer token: " + token);
	}

	listener = platform::socket_listen(options.host, options.port, r_error);
	if (listener == platform::INVALID_SOCKET_HANDLE) {
		return false;
	}
	log(LOG_INFO, "listening on http://" + options.host + ":" + std::to_string(options.port) + options.path);
	return true;
}

std::string HttpServer::create_session() {
	bool strong = false;
	std::string id = platform::random_token(16, strong);
	Session session;
	session.relay.reset(new Relay(relay_options));
	session.last_used = now_seconds();
	sessions[id] = std::move(session);
	log(LOG_INFO, "session " + id + " opened (" + std::to_string(sessions.size()) + " active)");
	return id;
}

void HttpServer::expire_idle_sessions() {
	const double cutoff = now_seconds() - (double)options.session_idle_seconds;
	for (auto iterator = sessions.begin(); iterator != sessions.end();) {
		if (iterator->second.last_used < cutoff) {
			log(LOG_INFO, "session " + iterator->first + " expired");
			iterator = sessions.erase(iterator);
		} else {
			++iterator;
		}
	}
}

std::string HttpServer::handle(const HttpRequest &p_request) {
	if (target_path(p_request.target) != options.path) {
		return rpc_error(404, RELAY_ERROR_EDITOR_UNAVAILABLE,
				"no MCP endpoint at " + target_path(p_request.target) + "; it is at " + options.path);
	}

	// Authorisation first: an unauthorised caller learns nothing about sessions.
	const std::string authorization = p_request.header("authorization");
	const std::string prefix = "Bearer ";
	const bool has_bearer = authorization.size() > prefix.size() && authorization.compare(0, prefix.size(), prefix) == 0;
	if (!has_bearer || !http_tokens_match(authorization.substr(prefix.size()), token)) {
		return rpc_error(401, RELAY_ERROR_HANDSHAKE_REJECTED,
				"missing or invalid bearer token",
				nullptr, "WWW-Authenticate: Bearer\r\n");
	}

	if (p_request.method == "GET") {
		return rpc_error(405, RELAY_ERROR_EDITOR_UNAVAILABLE,
				"this endpoint does not open server-initiated streams; POST your requests");
	}

	if (p_request.method == "DELETE") {
		const std::string id = p_request.header("mcp-session-id");
		const auto found = sessions.find(id);
		if (found == sessions.end()) {
			return rpc_error(404, RELAY_ERROR_EDITOR_UNAVAILABLE, "unknown session");
		}
		sessions.erase(found);
		log(LOG_INFO, "session " + id + " closed by the client");
		return response(204, std::string());
	}

	if (p_request.method != "POST") {
		return rpc_error(405, RELAY_ERROR_EDITOR_UNAVAILABLE,
				p_request.method + " is not allowed on this endpoint");
	}

	std::string parse_error;
	JSONValueRef message = json_parse(p_request.body, &parse_error);
	if (!message) {
		return rpc_error(400, -32700, "Parse error: " + parse_error);
	}
	if (message->is_array()) {
		// Batching was removed from MCP in 2025-06-18; accepting it here would mean
		// inventing semantics the editor side does not have.
		return rpc_error(400, -32600, "batched requests are not supported; send one message per request");
	}
	if (!message->is_object()) {
		return rpc_error(400, -32600, "Invalid Request: expected a JSON-RPC object");
	}

	JSONValueRef id = message->get("id");
	JSONValueRef method_value = message->get("method");
	const std::string method = method_value && method_value->is_string() ? method_value->get_string() : std::string();

	expire_idle_sessions();

	// `initialize` opens a session; everything else must name one. That is what makes
	// two clients two clients, rather than two halves of one confused conversation.
	std::string session_id = p_request.header("mcp-session-id");
	std::string new_session_header;
	if (session_id.empty()) {
		if (method != "initialize") {
			return rpc_error(400, RELAY_ERROR_HANDSHAKE_REJECTED,
					"missing Mcp-Session-Id; send 'initialize' first", id);
		}
		if ((int)sessions.size() >= options.max_sessions) {
			return rpc_error(429, RELAY_ERROR_EDITOR_UNAVAILABLE,
					"too many open sessions (" + std::to_string(options.max_sessions) + ")", id);
		}
		session_id = create_session();
		new_session_header = "Mcp-Session-Id: " + session_id + "\r\n";
	}

	const auto found = sessions.find(session_id);
	if (found == sessions.end()) {
		return rpc_error(404, RELAY_ERROR_HANDSHAKE_REJECTED,
				"unknown or expired session; send 'initialize' again", id);
	}
	found->second.last_used = now_seconds();

	std::string error;
	JSONValueRef reply;
	if (!found->second.relay->exchange(message, reply, error)) {
		// The bridge failed, not the tool. Drop the session: its editor connection is
		// no longer trustworthy, and a client that retries should start cleanly.
		sessions.erase(found);
		return rpc_error(500, RELAY_ERROR_EDITOR_DISCONNECTED, error, id, new_session_header);
	}

	if (!reply) {
		// A notification: accepted, nothing to say back.
		return response(202, std::string(), "application/json", new_session_header);
	}
	return response(200, reply->to_string(), "application/json", new_session_header);
}

void HttpServer::serve(platform::SocketHandle p_client) {
	std::string buffer;
	HttpRequest request;
	bool complete = false;
	std::string error;

	// Bounded: a connection that never finishes a request must not pin the server.
	const int slice_ms = 200;
	int waited_ms = 0;
	const int read_timeout_ms = 10000;
	while (!complete) {
		if (!http_parse_request(buffer, request, complete, error)) {
			const std::string out = rpc_error(400, -32600, error);
			platform::socket_send(p_client, out.data(), out.size());
			platform::socket_close(p_client);
			return;
		}
		if (complete) {
			break;
		}
		std::vector<platform::SocketHandle> ready;
		if (!platform::wait_for_sockets({ p_client }, slice_ms, ready)) {
			platform::socket_close(p_client);
			return;
		}
		if (ready.empty()) {
			waited_ms += slice_ms;
			if (waited_ms >= read_timeout_ms || platform::is_terminating()) {
				platform::socket_close(p_client);
				return;
			}
			continue;
		}
		char chunk[8192];
		const long read_bytes = platform::socket_recv(p_client, chunk, sizeof(chunk));
		if (read_bytes <= 0) {
			platform::socket_close(p_client);
			return;
		}
		buffer.append(chunk, (size_t)read_bytes);
	}

	const std::string out = handle(request);
	platform::socket_send(p_client, out.data(), out.size());
	platform::socket_close(p_client);
}

int HttpServer::run() {
	platform::install_termination_handler();

	while (!platform::is_terminating()) {
		std::vector<platform::SocketHandle> ready;
		if (!platform::wait_for_sockets({ listener }, 500, ready)) {
			log(LOG_ERROR, "waiting for connections failed");
			return 2;
		}
		if (ready.empty()) {
			expire_idle_sessions();
			continue;
		}
		std::string error;
		const platform::SocketHandle client = platform::socket_accept(listener, error);
		if (client == platform::INVALID_SOCKET_HANDLE) {
			if (!error.empty()) {
				log(LOG_ERROR, error);
			}
			continue;
		}
		// One connection at a time. Every request here is a single editor round trip,
		// and the editor itself is single-threaded, so concurrency would queue in the
		// editor instead of here - with more ways to go wrong.
		serve(client);
	}
	log(LOG_INFO, "shutting down");
	return 0;
}

} // namespace godot_ai
