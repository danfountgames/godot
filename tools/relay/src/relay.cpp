/**************************************************************************/
/*  relay.cpp                                                             */
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

#include "relay.h"

#include <iostream>

#include "platform.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace godot_ai {

const char *RELAY_VERSION = "0.1.0";
const char *RELAY_BRIDGE_VERSION = "1";

// A single frame is one line of JSON. Anything larger is a protocol violation
// rather than something to buffer indefinitely.
static const size_t MAX_FRAME_BYTES = 32u * 1024u * 1024u;

static const char *HELLO_ID = "godot-ai-relay/hello";

// ----------------------------------------------------------------- options ---

static const char *USAGE =
		"godot-ai-relay - MCP stdio bridge to a running Godot editor\n"
		"\n"
		"Usage: godot-ai-relay [options]\n"
		"\n"
		"Options:\n"
		"  --mcp                      Serve the Model Context Protocol over stdio (default).\n"
		"  --editor-socket <spec>     Connect to <port> or <host>:<port> instead of discovering.\n"
		"  --project <path>           Select the editor instance editing this project.\n"
		"  --instance <pid>           Select the editor instance with this process id.\n"
		"  --log-level <level>        error | warn | info | debug (default: warn).\n"
		"  --read-only                Request a session that refuses every mutating tool.\n"
		"  --approval-mode <mode>     ask | allow | deny (default: ask).\n"
		"  --client-name <name>       Identify this client to the editor's approval UI.\n"
		"  --home <path>              Override the relay state directory (GODOT_AI_HOME).\n"
		"  --handshake-timeout <ms>   Editor handshake timeout in milliseconds (default: 5000).\n"
		"  --call <tool>              Run one tool and print its result as JSON, then exit.\n"
		"  --list-tools               Print every available tool as JSON, then exit.\n"
		"  --list-prompts             Print the skills the editor offers as prompts, then\n"
		"                             exit. These are the intended way in: a named job\n"
		"                             such as 'run a performance investigation', with the\n"
		"                             primitives composed underneath. Only skills the user\n"
		"                             has allowed are listed.\n"
		"  --prompt <name>            Print one skill's instructions as JSON, then exit.\n"
		"  --context <text>           Optional subject for --prompt: what to apply it to.\n"
		"  --batch                    Read a JSON array of tool calls on stdin and run\n"
		"                             them all over ONE connection. --call pays a process\n"
		"                             launch, a connect and a handshake every time - about\n"
		"                             half a second - which is fine for three calls and\n"
		"                             ruinous for a thousand. It is also the only way to\n"
		"                             read several properties of a running game close\n"
		"                             enough together to mean anything. Use this, or --mcp.\n"
		"  --continue-on-error        With --batch, keep going after a failed entry.\n"
		"                             The default stops, because a batch is a sequence and\n"
		"                             a call after a failed gate proceeds on an assumption\n"
		"                             that never held.\n"
		"  --arguments <json>         Arguments object for --call (default: {}).\n"
		"  --http-port <port>         Serve MCP over HTTP on this port instead of stdio.\n"
		"  --http-host <host>         Address to bind (default: 127.0.0.1).\n"
		"  --http-path <path>         Endpoint path (default: /mcp).\n"
		"  --http-token <token>       Bearer token clients must present (default: generated).\n"
		"  --http-allow-remote        Permit binding somewhere other than loopback.\n"
		"  --list-backends            List the MCP client configurations this can write.\n"
		"  --install-backend <name>   Write this relay into a client's configuration.\n"
		"  --check-backends           Report whether that configuration is still current.\n"
		"  --backend-config <path>    The client configuration file to write or check.\n"
		"  --version                  Print the relay version and exit.\n"
		"  --help                     Print this message and exit.\n"
		"\n"
		"Diagnostics are always written to stderr; stdout carries protocol traffic only.\n"
		"With --call, stdout carries the tool's result instead - that mode is for scripts,\n"
		"not for an MCP client.\n";

static bool parse_log_level(const std::string &p_value, LogLevel &r_level) {
	if (p_value == "error") {
		r_level = LOG_ERROR;
	} else if (p_value == "warn" || p_value == "warning") {
		r_level = LOG_WARN;
	} else if (p_value == "info") {
		r_level = LOG_INFO;
	} else if (p_value == "debug") {
		r_level = LOG_DEBUG;
	} else {
		return false;
	}
	return true;
}

bool relay_parse_options(int p_argc, char **p_argv, RelayOptions &r_options, std::string &r_error, bool &r_exit_immediately, std::string &r_immediate_output) {
	r_exit_immediately = false;
	for (int i = 1; i < p_argc; i++) {
		const std::string arg = p_argv[i];
		auto next = [&](std::string &r_value) -> bool {
			if (i + 1 >= p_argc) {
				r_error = arg + " requires a value";
				return false;
			}
			r_value = p_argv[++i];
			return true;
		};

		if (arg == "--mcp") {
			r_options.mcp = true;
		} else if (arg == "--read-only") {
			r_options.read_only = true;
		} else if (arg == "--help" || arg == "-h") {
			r_exit_immediately = true;
			r_immediate_output = USAGE;
			return true;
		} else if (arg == "--version") {
			r_exit_immediately = true;
			r_immediate_output = std::string("godot-ai-relay ") + RELAY_VERSION + " (bridge protocol " + RELAY_BRIDGE_VERSION + ")\n";
			return true;
		} else if (arg == "--editor-socket") {
			std::string value;
			if (!next(value)) {
				return false;
			}
			const size_t colon = value.rfind(':');
			std::string port_part = value;
			if (colon != std::string::npos) {
				r_options.editor_host = value.substr(0, colon);
				port_part = value.substr(colon + 1);
			}
			char *end = nullptr;
			const long port = strtol(port_part.c_str(), &end, 10);
			if (!end || *end != '\0' || port <= 0 || port > 65535) {
				r_error = "invalid --editor-socket value: " + value;
				return false;
			}
			r_options.editor_port = (int)port;
		} else if (arg == "--project") {
			if (!next(r_options.project)) {
				return false;
			}
		} else if (arg == "--instance") {
			std::string value;
			if (!next(value)) {
				return false;
			}
			char *end = nullptr;
			const long pid = strtol(value.c_str(), &end, 10);
			if (!end || *end != '\0' || pid <= 0) {
				r_error = "invalid --instance value: " + value;
				return false;
			}
			r_options.instance_pid = pid;
		} else if (arg == "--log-level") {
			std::string value;
			if (!next(value)) {
				return false;
			}
			if (!parse_log_level(value, r_options.log_level)) {
				r_error = "invalid --log-level value: " + value;
				return false;
			}
		} else if (arg == "--approval-mode") {
			std::string value;
			if (!next(value)) {
				return false;
			}
			if (value == "ask") {
				r_options.approval_mode = APPROVAL_ASK;
			} else if (value == "allow") {
				r_options.approval_mode = APPROVAL_ALLOW;
			} else if (value == "deny") {
				r_options.approval_mode = APPROVAL_DENY;
			} else {
				r_error = "invalid --approval-mode value: " + value;
				return false;
			}
		} else if (arg == "--client-name") {
			if (!next(r_options.client_name)) {
				return false;
			}
		} else if (arg == "--list-tools") {
			r_options.list_tools = true;
		} else if (arg == "--list-prompts") {
			r_options.list_prompts = true;
		} else if (arg == "--prompt") {
			if (!next(r_options.prompt_name)) {
				return false;
			}
		} else if (arg == "--context") {
			if (!next(r_options.prompt_context)) {
				return false;
			}
		} else if (arg == "--batch") {
			r_options.batch = true;
		} else if (arg == "--continue-on-error") {
			r_options.batch_continue_on_error = true;
		} else if (arg == "--call") {
			if (!next(r_options.call_tool)) {
				return false;
			}
		} else if (arg == "--arguments") {
			if (!next(r_options.call_arguments)) {
				return false;
			}
			std::string parse_error;
			JSONValueRef parsed = json_parse(r_options.call_arguments, &parse_error);
			if (!parsed || !parsed->is_object()) {
				r_error = "--arguments must be a JSON object: " + parse_error;
				return false;
			}
		} else if (arg == "--http-port") {
			std::string value;
			if (!next(value)) {
				return false;
			}
			char *end = nullptr;
			const long port = strtol(value.c_str(), &end, 10);
			if (!end || *end != '\0' || port < 1 || port > 65535) {
				r_error = "invalid --http-port value: " + value;
				return false;
			}
			r_options.http_port = (int)port;
		} else if (arg == "--http-host") {
			if (!next(r_options.http_host)) {
				return false;
			}
		} else if (arg == "--http-path") {
			if (!next(r_options.http_path)) {
				return false;
			}
			if (r_options.http_path.empty() || r_options.http_path[0] != '/') {
				r_error = "--http-path must start with '/'";
				return false;
			}
		} else if (arg == "--http-token") {
			if (!next(r_options.http_token)) {
				return false;
			}
		} else if (arg == "--http-allow-remote") {
			r_options.http_allow_remote = true;
		} else if (arg == "--list-backends") {
			r_options.list_backends = true;
		} else if (arg == "--install-backend") {
			if (!next(r_options.install_backend)) {
				return false;
			}
		} else if (arg == "--check-backends") {
			r_options.check_backends = true;
		} else if (arg == "--backend-config") {
			if (!next(r_options.backend_config)) {
				return false;
			}
		} else if (arg == "--home") {
			if (!next(r_options.home)) {
				return false;
			}
		} else if (arg == "--handshake-timeout") {
			std::string value;
			if (!next(value)) {
				return false;
			}
			char *end = nullptr;
			const long timeout = strtol(value.c_str(), &end, 10);
			if (!end || *end != '\0' || timeout < 1) {
				r_error = "invalid --handshake-timeout value: " + value;
				return false;
			}
			r_options.handshake_timeout_ms = (int)timeout;
		} else {
			r_error = "unknown option: " + arg;
			return false;
		}
	}
	return true;
}

// --------------------------------------------------------------- discovery ---

std::string relay_home_dir(const RelayOptions &p_options) {
	if (!p_options.home.empty()) {
		return p_options.home;
	}
	const std::string env = platform::environment("GODOT_AI_HOME");
	if (!env.empty()) {
		return env;
	}
	const std::string home = platform::home_directory();
	if (!home.empty()) {
		return home + "/.godot-ai";
	}
	return ".godot-ai";
}

std::string relay_instances_dir(const RelayOptions &p_options) {
	return relay_home_dir(p_options) + "/instances";
}

std::vector<InstanceDescriptor> relay_discover_instances(const RelayOptions &p_options) {
	std::vector<InstanceDescriptor> instances;
	const std::string dir_path = relay_instances_dir(p_options);
	for (const std::string &name : platform::list_directory(dir_path)) {
		if (name.size() < 6 || name.compare(name.size() - 5, 5, ".json") != 0) {
			continue;
		}
		const std::string path = dir_path + "/" + name;
		std::string contents;
		if (!platform::read_file(path, contents)) {
			continue;
		}
		JSONValueRef value = json_parse(contents);
		if (!value || !value->is_object()) {
			// A descriptor being written right now, or left behind corrupted, must not
			// break discovery for every other instance.
			continue;
		}
		JSONValueRef port = value->get("port");
		if (!port || !port->is_number()) {
			continue;
		}
		InstanceDescriptor descriptor;
		descriptor.path = path;
		descriptor.port = (int)port->get_number();
		JSONValueRef pid = value->get("pid");
		if (pid && pid->is_number()) {
			descriptor.pid = (long)pid->get_number();
		}
		JSONValueRef started = value->get("started_at");
		if (started && started->is_number()) {
			descriptor.started_at = started->get_number();
		}
		descriptor.project_path = value->get_string_or("project_path", "");
		descriptor.project_name = value->get_string_or("project_name", "");
		descriptor.editor_version = value->get_string_or("editor_version", "");
		descriptor.protocol_version = value->get_string_or("protocol_version", "");
		// An editor that has exited leaves its descriptor behind. Left in place it
		// poisons discovery for everything else: the relay reports "several editor
		// instances are running" and lists processes that died hours ago, and --project
		// cannot disambiguate two stale entries naming the same folder. Checking the pid
		// costs nothing and removes the whole failure.
		if (!platform::process_is_alive(descriptor.pid)) {
			platform::remove_file(path);
			continue;
		}
		instances.push_back(descriptor);
	}

	std::sort(instances.begin(), instances.end(), [](const InstanceDescriptor &a, const InstanceDescriptor &b) {
		return a.started_at > b.started_at;
	});
	return instances;
}

static std::string normalize_path(const std::string &p_path) {
	if (p_path.empty()) {
		return p_path;
	}
	return platform::real_path(p_path);
}

bool relay_select_instance(const std::vector<InstanceDescriptor> &p_instances, const RelayOptions &p_options, InstanceDescriptor &r_selected, std::string &r_error) {
	std::vector<InstanceDescriptor> candidates;
	const std::string wanted_project = normalize_path(p_options.project);
	for (const InstanceDescriptor &instance : p_instances) {
		if (p_options.instance_pid > 0 && instance.pid != p_options.instance_pid) {
			continue;
		}
		if (!wanted_project.empty() && normalize_path(instance.project_path) != wanted_project) {
			continue;
		}
		candidates.push_back(instance);
	}

	if (candidates.empty()) {
		if (p_instances.empty()) {
			r_error = "no running Godot editor with the AI service enabled was found (looked in " + relay_instances_dir(p_options) + ")";
		} else if (p_options.instance_pid > 0) {
			r_error = "no running editor instance with pid " + std::to_string(p_options.instance_pid);
		} else {
			r_error = "no running editor instance is editing project '" + p_options.project + "'";
		}
		return false;
	}
	if (candidates.size() > 1 && p_options.project.empty() && p_options.instance_pid <= 0) {
		// Guessing which editor the user meant would be worse than asking.
		std::string detail;
		for (const InstanceDescriptor &instance : candidates) {
			detail += "\n  pid " + std::to_string(instance.pid) + " port " + std::to_string(instance.port) + " project " + instance.project_path;
		}
		r_error = "several editor instances are running; select one with --project or --instance:" + detail;
		return false;
	}
	r_selected = candidates.front();
	return true;
}

// ------------------------------------------------------------------- relay ---

Relay::~Relay() {
	if (socket != platform::INVALID_SOCKET_HANDLE) {
		platform::socket_close(socket);
		socket = platform::INVALID_SOCKET_HANDLE;
	}
}

void Relay::log(LogLevel p_level, const std::string &p_message) const {
	if (p_level > options.log_level) {
		return;
	}
	static const char *names[] = { "error", "warn", "info", "debug" };
	fprintf(stderr, "[godot-ai-relay] %s: %s\n", names[p_level], p_message.c_str());
	fflush(stderr);
}

void Relay::write_stdout_line(const std::string &p_line) const {
	// stdout carries protocol frames only; everything else goes to stderr.
	const std::string frame = p_line + "\n";
	platform::write_stdout(frame.data(), frame.size());
}

std::string Relay::id_key(const JSONValueRef &p_id) {
	if (!p_id) {
		return std::string();
	}
	return p_id->to_string();
}

void Relay::send_error(const JSONValueRef &p_id, int p_code, const std::string &p_message) const {
	JSONValueRef response = JSONValue::make_object();
	response->set("jsonrpc", JSONValue::make_string("2.0"));
	response->set("id", p_id ? p_id : JSONValue::make_null());
	JSONValueRef error = JSONValue::make_object();
	error->set("code", JSONValue::make_number(p_code));
	error->set("message", JSONValue::make_string(p_message));
	response->set("error", error);
	write_stdout_line(response->to_string());
}

void Relay::fail_all_pending(int p_code, const std::string &p_message) {
	std::vector<std::pair<std::string, std::string>> flushed;
	flushed.swap(pending);
	for (const auto &entry : flushed) {
		JSONValueRef id = json_parse(entry.second);
		send_error(id ? id : JSONValue::make_null(), p_code, p_message);
	}
}

bool Relay::ensure_connected(std::string &r_error) {
	if (connected) {
		return true;
	}
	if (!fatal_bridge_error.empty()) {
		r_error = fatal_bridge_error;
		return false;
	}

	int port = options.editor_port;
	const std::string host = options.editor_host;
	if (port <= 0) {
		const std::vector<InstanceDescriptor> instances = relay_discover_instances(options);
		InstanceDescriptor selected;
		if (!relay_select_instance(instances, options, selected, r_error)) {
			return false;
		}
		selected_instance = selected;
		port = selected.port;
		log(LOG_INFO, "selected editor instance pid " + std::to_string(selected.pid) + " on port " + std::to_string(port));
	}

	std::string reason;
	const platform::SocketHandle handle = platform::socket_connect(host, port, reason);
	if (handle == platform::INVALID_SOCKET_HANDLE) {
		if (!selected_instance.path.empty()) {
			// The descriptor pointed at a port nothing is listening on: the editor died
			// without cleaning up. Prune it so the next attempt is accurate.
			log(LOG_WARN, "pruning stale instance descriptor " + selected_instance.path);
			platform::remove_file(selected_instance.path);
			selected_instance = InstanceDescriptor();
		}
		r_error = "could not connect to the Godot editor on " + host + ":" + std::to_string(port) + " (" + reason + ")";
		return false;
	}

	socket = handle;
	connected = true;
	socket_buffer.clear();
	log(LOG_INFO, "connected to editor on " + host + ":" + std::to_string(port));

	std::string handshake_error;
	int handshake_code = RELAY_ERROR_HANDSHAKE_REJECTED;
	if (!perform_handshake(handshake_error, handshake_code)) {
		disconnect(handshake_error, false);
		// A rejection or version mismatch will not resolve itself by retrying, so it is
		// remembered and reported verbatim. Transient failures stay retryable.
		if (handshake_code != RELAY_ERROR_EDITOR_UNAVAILABLE) {
			fatal_bridge_error = handshake_error;
			fatal_bridge_code = handshake_code;
		}
		r_error = handshake_error;
		return false;
	}
	return true;
}

bool Relay::socket_send_line(const std::string &p_line) {
	if (socket == platform::INVALID_SOCKET_HANDLE) {
		return false;
	}
	const std::string frame = p_line + "\n";
	return platform::socket_send(socket, frame.data(), frame.size()) >= 0;
}

bool Relay::perform_handshake(std::string &r_error, int &r_error_code) {
	r_error_code = RELAY_ERROR_EDITOR_UNAVAILABLE;

	JSONValueRef params = JSONValue::make_object();
	params->set("relay_version", JSONValue::make_string(RELAY_VERSION));
	JSONValueRef versions = JSONValue::make_array();
	versions->push_back(JSONValue::make_string(RELAY_BRIDGE_VERSION));
	params->set("bridge_versions", versions);
	params->set("read_only", JSONValue::make_bool(options.read_only));
	const char *modes[] = { "ask", "allow", "deny" };
	params->set("approval_mode", JSONValue::make_string(modes[options.approval_mode]));
	std::string client_name = options.client_name;
	if (client_name.empty()) {
		client_name = platform::environment("GODOT_AI_CLIENT_NAME");
		if (client_name.empty()) {
			client_name = "unknown-client";
		}
	}
	params->set("client_name", JSONValue::make_string(client_name));
	params->set("pid", JSONValue::make_number((double)platform::process_id()));

	JSONValueRef request = JSONValue::make_object();
	request->set("jsonrpc", JSONValue::make_string("2.0"));
	request->set("id", JSONValue::make_string(HELLO_ID));
	request->set("method", JSONValue::make_string("godot/hello"));
	request->set("params", params);

	if (!socket_send_line(request->to_string())) {
		r_error = "failed to send the bridge handshake to the editor";
		return false;
	}

	// The handshake is synchronous: nothing else may be forwarded until the editor
	// has accepted this client. Poll in short slices so a signal or a hung editor
	// cannot pin the relay for the whole timeout.
	std::string buffer;
	const int slice_ms = 100;
	int waited_ms = 0;
	while (true) {
		if (platform::is_terminating()) {
			r_error = "interrupted while waiting for the editor handshake";
			return false;
		}
		bool stdin_ready = false;
		bool socket_ready = false;
		if (!platform::wait_for_input(socket, slice_ms, false, stdin_ready, socket_ready)) {
			r_error = "waiting for the editor handshake failed";
			return false;
		}
		if (!socket_ready) {
			waited_ms += slice_ms;
			if (waited_ms >= options.handshake_timeout_ms) {
				r_error = "the editor accepted the connection but did not answer the bridge handshake within " +
						std::to_string(options.handshake_timeout_ms) + "ms";
				return false;
			}
			continue;
		}

		char chunk[4096];
		const long read_bytes = platform::socket_recv(socket, chunk, sizeof(chunk));
		if (read_bytes <= 0) {
			r_error = "the editor closed the connection during the bridge handshake";
			return false;
		}
		buffer.append(chunk, (size_t)read_bytes);
		const size_t newline = buffer.find('\n');
		if (newline == std::string::npos) {
			if (buffer.size() > MAX_FRAME_BYTES) {
				r_error = "handshake response exceeded the maximum frame size";
				return false;
			}
			continue;
		}
		const std::string line = buffer.substr(0, newline);
		// Anything after the handshake response belongs to the normal stream.
		socket_buffer = buffer.substr(newline + 1);

		JSONValueRef response = json_parse(line);
		if (!response || !response->is_object()) {
			r_error = "the editor sent a malformed bridge handshake response";
			return false;
		}
		JSONValueRef error = response->get("error");
		if (error && error->is_object()) {
			r_error = "the editor rejected this client: " + error->get_string_or("message", "no reason given");
			r_error_code = RELAY_ERROR_HANDSHAKE_REJECTED;
			return false;
		}
		JSONValueRef result = response->get("result");
		if (!result || !result->is_object()) {
			r_error = "the editor sent a bridge handshake response without a result";
			return false;
		}
		const std::string bridge_version = result->get_string_or("bridge_version", "");
		if (bridge_version != RELAY_BRIDGE_VERSION) {
			r_error = "bridge protocol mismatch: relay speaks " + std::string(RELAY_BRIDGE_VERSION) +
					", editor speaks " + (bridge_version.empty() ? "an unknown version" : bridge_version) +
					"; update the relay and the editor together";
			r_error_code = RELAY_ERROR_VERSION_MISMATCH;
			return false;
		}
		handshake_complete = true;
		log(LOG_INFO, "bridge handshake accepted by editor " + result->get_string_or("editor_version", "?") +
						" for project " + result->get_string_or("project_path", "?"));
		return true;
	}
}

void Relay::disconnect(const std::string &p_reason, bool p_fail_pending) {
	if (socket != platform::INVALID_SOCKET_HANDLE) {
		platform::socket_close(socket);
		socket = platform::INVALID_SOCKET_HANDLE;
	}
	connected = false;
	handshake_complete = false;
	socket_buffer.clear();
	if (!p_reason.empty()) {
		log(LOG_WARN, "editor connection closed: " + p_reason);
	}
	if (p_fail_pending) {
		fail_all_pending(RELAY_ERROR_EDITOR_DISCONNECTED, "the Godot editor closed the connection: " + p_reason);
	} else {
		pending.clear();
	}
}

void Relay::handle_client_line(const std::string &p_line) {
	// Ignore blank keep-alive lines rather than treating them as parse errors.
	bool only_whitespace = true;
	for (char character : p_line) {
		if (character != ' ' && character != '\t' && character != '\r') {
			only_whitespace = false;
			break;
		}
	}
	if (only_whitespace) {
		return;
	}

	std::string parse_error;
	JSONValueRef message = json_parse(p_line, &parse_error);
	if (!message) {
		log(LOG_WARN, "dropping malformed client frame: " + parse_error);
		send_error(JSONValue::make_null(), -32700, "Parse error: " + parse_error);
		return;
	}
	if (!message->is_object()) {
		send_error(JSONValue::make_null(), -32600, "Invalid Request: expected a JSON-RPC object");
		return;
	}

	JSONValueRef id = message->get("id");
	const bool is_request = id != nullptr && !id->is_null();

	std::string connect_error;
	if (!ensure_connected(connect_error)) {
		if (is_request) {
			const int code = fatal_bridge_error.empty() ? RELAY_ERROR_EDITOR_UNAVAILABLE : fatal_bridge_code;
			send_error(id, code, connect_error);
		} else {
			// A notification has no reply channel; fabricating one would be a lie.
			log(LOG_WARN, "dropping notification, editor unavailable: " + connect_error);
		}
		return;
	}

	if (is_request) {
		pending.emplace_back(id_key(id), id->to_string());
	}

	if (!socket_send_line(p_line)) {
		disconnect("write failed", false);
		if (is_request) {
			pending.pop_back();
			send_error(id, RELAY_ERROR_EDITOR_DISCONNECTED, "failed to forward the request to the Godot editor");
		}
		return;
	}
	log(LOG_DEBUG, "-> editor: " + p_line);
}

void Relay::handle_editor_line(const std::string &p_line) {
	if (p_line.empty()) {
		return;
	}
	std::string parse_error;
	JSONValueRef message = json_parse(p_line, &parse_error);
	if (!message || !message->is_object()) {
		// Never forward anything that is not a valid JSON object: stdout purity is the
		// relay's contract with the client.
		log(LOG_ERROR, "dropping malformed editor frame: " + (parse_error.empty() ? "not a JSON object" : parse_error));
		return;
	}

	JSONValueRef id = message->get("id");
	if (id && id->is_string() && id->get_string() == HELLO_ID) {
		// Bridge-level traffic is never exposed to the MCP client.
		log(LOG_DEBUG, "ignoring late handshake frame");
		return;
	}

	if (id && !id->is_null()) {
		const std::string key = id_key(id);
		for (size_t i = 0; i < pending.size(); i++) {
			if (pending[i].first == key) {
				pending.erase(pending.begin() + (long)i);
				break;
			}
		}
	}

	log(LOG_DEBUG, "<- editor: " + p_line);
	write_stdout_line(p_line);
}

bool Relay::request(const std::string &p_method, const std::string &p_id, const JSONValueRef &p_params, JSONValueRef &r_response, std::string &r_error) {
	JSONValueRef message = JSONValue::make_object();
	message->set("jsonrpc", JSONValue::make_string("2.0"));
	if (!p_id.empty()) {
		message->set("id", JSONValue::make_string(p_id));
	}
	message->set("method", JSONValue::make_string(p_method));
	if (p_params) {
		message->set("params", p_params);
	}

	if (!socket_send_line(message->to_string())) {
		r_error = "failed to send '" + p_method + "' to the editor";
		return false;
	}
	if (p_id.empty()) {
		// A notification has no reply to wait for.
		return true;
	}
	return wait_for_response(id_key(JSONValue::make_string(p_id)), "'" + p_method + "'", r_response, r_error);
}

bool Relay::exchange(const JSONValueRef &p_message, JSONValueRef &r_response, std::string &r_error) {
	if (!p_message || !p_message->is_object()) {
		r_error = "message is not a JSON-RPC object";
		return false;
	}
	if (!ensure_connected(r_error)) {
		return false;
	}
	if (!socket_send_line(p_message->to_string())) {
		r_error = "failed to send the request to the editor";
		return false;
	}

	const std::string key = id_key(p_message->get("id"));
	if (key.empty()) {
		// A notification: accepted, with nothing to wait for.
		return true;
	}

	JSONValueRef method = p_message->get("method");
	const std::string what = method && method->is_string()
			? "'" + method->get_string() + "'"
			: std::string("the request");
	return wait_for_response(key, what, r_response, r_error);
}

bool Relay::wait_for_response(const std::string &p_id_key, const std::string &p_what, JSONValueRef &r_response, std::string &r_error) {
	if (p_id_key.empty()) {
		return true;
	}

	// Bounded so a wedged editor fails the caller instead of hanging it.
	const int slice_ms = 100;
	int waited_ms = 0;
	const int timeout_ms = 30000;
	while (true) {
		size_t newline = socket_buffer.find('\n');
		while (newline != std::string::npos) {
			const std::string line = socket_buffer.substr(0, newline);
			socket_buffer.erase(0, newline + 1);
			JSONValueRef parsed = json_parse(line);
			if (parsed && parsed->is_object()) {
				if (id_key(parsed->get("id")) == p_id_key) {
					r_response = parsed;
					return true;
				}
				// Notifications and unrelated frames are not what we are waiting for.
				log(LOG_DEBUG, "ignoring " + line);
			}
			newline = socket_buffer.find('\n');
		}

		if (platform::is_terminating()) {
			r_error = "interrupted";
			return false;
		}
		bool stdin_ready = false;
		bool socket_ready = false;
		if (!platform::wait_for_input(socket, slice_ms, false, stdin_ready, socket_ready)) {
			r_error = "waiting for the editor failed";
			return false;
		}
		if (!socket_ready) {
			waited_ms += slice_ms;
			if (waited_ms >= timeout_ms) {
				r_error = "the editor did not answer " + p_what + " within 30s";
				return false;
			}
			continue;
		}
		char chunk[8192];
		const long read_bytes = platform::socket_recv(socket, chunk, sizeof(chunk));
		if (read_bytes <= 0) {
			disconnect("the editor closed the connection", false);
			r_error = "the editor closed the connection while handling " + p_what;
			return false;
		}
		socket_buffer.append(chunk, (size_t)read_bytes);
	}
}

int Relay::run_one_shot() {
	platform::install_termination_handler();

	std::string error;
	if (!ensure_connected(error)) {
		log(LOG_ERROR, error);
		return 2;
	}

	JSONValueRef initialize_params = JSONValue::make_object();
	initialize_params->set("protocolVersion", JSONValue::make_string("2025-06-18"));
	initialize_params->set("capabilities", JSONValue::make_object());
	JSONValueRef client_info = JSONValue::make_object();
	client_info->set("name", JSONValue::make_string("godot-ai-relay --call"));
	client_info->set("version", JSONValue::make_string(RELAY_VERSION));
	initialize_params->set("clientInfo", client_info);

	JSONValueRef response;
	if (!request("initialize", "one-shot-init", initialize_params, response, error)) {
		log(LOG_ERROR, error);
		return 2;
	}
	if (response->get("error")) {
		write_stdout_line(response->to_string());
		return 1;
	}
	if (!request("notifications/initialized", std::string(), nullptr, response, error)) {
		log(LOG_ERROR, error);
		return 2;
	}

	if (options.list_tools) {
		JSONValueRef listed;
		if (!request("tools/list", "one-shot-list", JSONValue::make_object(), listed, error)) {
			log(LOG_ERROR, error);
			return 2;
		}
		JSONValueRef list_result = listed->get("result");
		write_stdout_line(list_result ? list_result->to_string() : listed->to_string());
		return list_result ? 0 : 1;
	}

	if (options.list_prompts) {
		JSONValueRef listed;
		if (!request("prompts/list", "one-shot-prompts", JSONValue::make_object(), listed, error)) {
			log(LOG_ERROR, error);
			return 2;
		}
		JSONValueRef list_result = listed->get("result");
		write_stdout_line(list_result ? list_result->to_string() : listed->to_string());
		return list_result ? 0 : 1;
	}

	if (!options.prompt_name.empty()) {
		JSONValueRef params = JSONValue::make_object();
		params->set("name", JSONValue::make_string(options.prompt_name));
		if (!options.prompt_context.empty()) {
			JSONValueRef arguments = JSONValue::make_object();
			arguments->set("context", JSONValue::make_string(options.prompt_context));
			params->set("arguments", arguments);
		}
		JSONValueRef fetched;
		if (!request("prompts/get", "one-shot-prompt", params, fetched, error)) {
			log(LOG_ERROR, error);
			return 2;
		}
		JSONValueRef prompt_result = fetched->get("result");
		write_stdout_line(prompt_result ? prompt_result->to_string() : fetched->to_string());
		return prompt_result ? 0 : 1;
	}

	if (options.batch) {
		// One connection, one handshake, every call. See Options::batch.
		std::string input;
		std::string line;
		while (std::getline(std::cin, line)) {
			input += line;
			input += "\n";
		}
		std::string parse_error;
		JSONValueRef requests = json_parse(input, &parse_error);
		if (!requests || !requests->is_array()) {
			log(LOG_ERROR, "--batch expects a JSON array of {\"name\":…, \"arguments\":…} on stdin");
			return 2;
		}
		JSONValueRef results = JSONValue::make_array();
		int failures = 0;
		const std::vector<JSONValueRef> &entries = requests->get_array();
		for (size_t i = 0; i < entries.size(); i++) {
			JSONValueRef entry = entries[i];
			JSONValueRef name = entry ? entry->get("name") : nullptr;
			if (!name || !name->is_string()) {
				log(LOG_ERROR, "every batch entry needs a string \"name\"");
				return 2;
			}
			JSONValueRef params = JSONValue::make_object();
			params->set("name", JSONValue::make_string(name->get_string()));
			JSONValueRef entry_arguments = entry->get("arguments");
			params->set("arguments",
					entry_arguments && entry_arguments->is_object() ? entry_arguments
																   : JSONValue::make_object());
			JSONValueRef reply;
			if (!request("tools/call", "batch-" + std::to_string(i), params, reply, error)) {
				log(LOG_ERROR, error);
				return 2;
			}
			JSONValueRef entry_result = reply->get("result");
			JSONValueRef entry_error = reply->get("error");
			JSONValueRef wrapped = JSONValue::make_object();
			wrapped->set("name", JSONValue::make_string(name->get_string()));
			bool entry_failed = false;
			std::string entry_message;
			if (entry_error) {
				wrapped->set("error", entry_error);
				entry_failed = true;
				JSONValueRef message = entry_error->get("message");
				if (message && message->is_string()) {
					entry_message = message->get_string();
				}
			} else if (entry_result) {
				wrapped->set("result", entry_result);
				JSONValueRef is_error = entry_result->get("isError");
				if (is_error && is_error->is_bool() && is_error->get_bool()) {
					entry_failed = true;
					JSONValueRef content = entry_result->get("content");
					if (content && content->is_array() && !content->get_array().empty()) {
						JSONValueRef first = content->get_array()[0];
						JSONValueRef text = first ? first->get("text") : nullptr;
						if (text && text->is_string()) {
							entry_message = text->get_string();
						}
					}
				}
			}
			results->push_back(wrapped);

			if (entry_failed) {
				failures++;
				// Named on stderr, not only in the JSON. A caller reading selected fields out
				// of the results array will not notice an `error` key it did not look for, and
				// a batch is a *sequence*: an entry that did not run is usually a gate, and
				// every later call then proceeds on an assumption that never held.
				log(LOG_ERROR, "batch entry " + std::to_string(i) + " (" + name->get_string() +
										") failed: " + entry_message);
				if (!options.batch_continue_on_error) {
					log(LOG_ERROR, "stopping: " + std::to_string(entries.size() - i - 1) +
											" later call(s) not attempted. Pass "
											"--continue-on-error to run them anyway.");
					write_stdout_line(results->to_string());
					return 1;
				}
			}
		}
		write_stdout_line(results->to_string());
		return failures > 0 ? 1 : 0;
	}

	JSONValueRef call_params = JSONValue::make_object();
	call_params->set("name", JSONValue::make_string(options.call_tool));
	JSONValueRef arguments = options.call_arguments.empty()
			? JSONValue::make_object()
			: json_parse(options.call_arguments);
	call_params->set("arguments", arguments ? arguments : JSONValue::make_object());

	if (!request("tools/call", "one-shot-call", call_params, response, error)) {
		log(LOG_ERROR, error);
		return 2;
	}

	// In this mode stdout carries the result, not protocol traffic: the caller is a
	// script, not an MCP client.
	JSONValueRef result = response->get("result");
	JSONValueRef failure = response->get("error");
	if (failure) {
		write_stdout_line(failure->to_string());
		return 1;
	}
	if (result) {
		write_stdout_line(result->to_string());
		JSONValueRef is_error = result->get("isError");
		if (is_error && is_error->is_bool() && is_error->get_bool()) {
			return 1;
		}
		return 0;
	}

	write_stdout_line(response->to_string());
	return 1;
}

int Relay::run() {
	platform::install_termination_handler();

	// Connecting eagerly turns "the editor is not running" into an immediate, clearly
	// worded stderr diagnostic instead of a confusing failure on the first request.
	std::string connect_error;
	if (!ensure_connected(connect_error)) {
		log(LOG_WARN, connect_error);
		log(LOG_WARN, "serving stdio anyway; requests will fail until the editor is reachable");
	}

	bool stdin_open = true;
	while (stdin_open && !platform::is_terminating()) {
		bool stdin_ready = false;
		bool socket_ready = false;
		const platform::SocketHandle watched = connected ? socket : platform::INVALID_SOCKET_HANDLE;
		if (!platform::wait_for_input(watched, 1000, true, stdin_ready, socket_ready)) {
			log(LOG_ERROR, "waiting for input failed");
			break;
		}

		// The editor side is drained first so a disconnect is observed before new
		// client traffic is forwarded into a dead socket.
		if (socket_ready) {
			char chunk[8192];
			const long read_bytes = platform::socket_recv(socket, chunk, sizeof(chunk));
			if (read_bytes > 0) {
				socket_buffer.append(chunk, (size_t)read_bytes);
				size_t newline = std::string::npos;
				while ((newline = socket_buffer.find('\n')) != std::string::npos) {
					std::string line = socket_buffer.substr(0, newline);
					socket_buffer.erase(0, newline + 1);
					if (!line.empty() && line.back() == '\r') {
						line.pop_back();
					}
					handle_editor_line(line);
				}
				if (socket_buffer.size() > MAX_FRAME_BYTES) {
					disconnect("editor frame exceeded the maximum frame size", true);
				}
			} else {
				disconnect(read_bytes == 0 ? "editor disconnected" : "read error", true);
			}
		}

		if (stdin_ready) {
			char chunk[8192];
			const long read_bytes = platform::read_stdin(chunk, sizeof(chunk));
			if (read_bytes > 0) {
				stdin_buffer.append(chunk, (size_t)read_bytes);
				size_t newline = std::string::npos;
				while ((newline = stdin_buffer.find('\n')) != std::string::npos) {
					std::string line = stdin_buffer.substr(0, newline);
					stdin_buffer.erase(0, newline + 1);
					if (!line.empty() && line.back() == '\r') {
						line.pop_back();
					}
					handle_client_line(line);
				}
				if (stdin_buffer.size() > MAX_FRAME_BYTES) {
					log(LOG_ERROR, "client frame exceeded the maximum frame size; dropping buffer");
					stdin_buffer.clear();
					send_error(JSONValue::make_null(), -32700, "Parse error: frame exceeded the maximum size");
				}
			} else if (read_bytes == 0) {
				log(LOG_INFO, "client closed stdin; shutting down");
				stdin_open = false;
			} else if (read_bytes == -1) {
				// -2 means "nothing yet", which is normal; -1 is a real failure.
				log(LOG_ERROR, "stdin read failed");
				stdin_open = false;
			}
		}
	}

	if (platform::is_terminating()) {
		log(LOG_INFO, "terminating on signal");
	}
	disconnect("", false);
	return 0;
}

} // namespace godot_ai
