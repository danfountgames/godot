/**************************************************************************/
/*  backends.cpp                                                          */
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

#include "backends.h"

#include "platform.h"

namespace godot_ai {

const char *BACKEND_SERVER_KEY = "godot-ai";
const char *BACKEND_METADATA_KEY = "x-godot-ai";

std::vector<AgentBackend> agent_backends() {
	std::vector<AgentBackend> backends;

	AgentBackend stdio;
	stdio.name = "stdio";
	stdio.description = "Launch this relay as a child process (the usual MCP setup).";
	stdio.transport = "stdio";
	backends.push_back(stdio);

	AgentBackend http;
	http.name = "http";
	http.description = "Point at a relay already serving --http-port on this machine.";
	http.transport = "http";
	http.token_variable = "GODOT_AI_HTTP_TOKEN";
	backends.push_back(http);

	return backends;
}

bool agent_backend_by_name(const std::string &p_name, AgentBackend &r_backend) {
	for (const AgentBackend &backend : agent_backends()) {
		if (backend.name == p_name) {
			r_backend = backend;
			return true;
		}
	}
	return false;
}

std::string agent_backends_listing() {
	std::string out = "Available backends:\n";
	for (const AgentBackend &backend : agent_backends()) {
		out += "  " + backend.name;
		out += std::string(10 - (backend.name.size() < 10 ? backend.name.size() : 9), ' ');
		out += backend.description + "\n";
		if (!backend.token_variable.empty()) {
			out += "             reads its bearer token from $" + backend.token_variable +
					" at run time; the token is never written to the file\n";
		}
	}
	out += "\nWrite one with:\n"
		   "  godot-ai-relay --install-backend <name> --backend-config <path to the client's config>\n";
	return out;
}

JSONValueRef backend_entry(const AgentBackend &p_backend, const RelayOptions &p_options, const std::string &p_executable) {
	JSONValueRef entry = JSONValue::make_object();

	if (p_backend.transport == "http") {
		entry->set("type", JSONValue::make_string("http"));
		entry->set("url", JSONValue::make_string(
									 "http://" + p_options.http_host + ":" +
									 std::to_string(p_options.http_port > 0 ? p_options.http_port : 7345) +
									 p_options.http_path));
		JSONValueRef headers = JSONValue::make_object();
		// The variable name, not the value. A client that expands environment
		// references reads the real token at run time; one that does not fails loudly
		// rather than authenticating with a secret checked into a dotfile.
		headers->set("Authorization", JSONValue::make_string("Bearer ${" + p_backend.token_variable + "}"));
		entry->set("headers", headers);
	} else {
		entry->set("type", JSONValue::make_string("stdio"));
		entry->set("command", JSONValue::make_string(p_executable));

		JSONValueRef args = JSONValue::make_array();
		args->push_back(JSONValue::make_string("--mcp"));
		// Only carry over the options that change which editor is driven, or under
		// what policy. Anything else is this invocation's business, not the client's.
		if (!p_options.project.empty()) {
			args->push_back(JSONValue::make_string("--project"));
			args->push_back(JSONValue::make_string(p_options.project));
		}
		if (p_options.read_only) {
			args->push_back(JSONValue::make_string("--read-only"));
		}
		if (!p_options.client_name.empty()) {
			args->push_back(JSONValue::make_string("--client-name"));
			args->push_back(JSONValue::make_string(p_options.client_name));
		}
		entry->set("args", args);
	}

	// What wrote this, and against which bridge. --check-backends reads it back.
	JSONValueRef metadata = JSONValue::make_object();
	metadata->set("relay_version", JSONValue::make_string(RELAY_VERSION));
	metadata->set("bridge_version", JSONValue::make_string(RELAY_BRIDGE_VERSION));
	metadata->set("backend", JSONValue::make_string(p_backend.name));
	entry->set(BACKEND_METADATA_KEY, metadata);

	return entry;
}

bool backend_install(const AgentBackend &p_backend, const RelayOptions &p_options, const std::string &p_path, std::string &r_summary, std::string &r_error) {
	if (p_path.empty()) {
		r_error = "--install-backend needs --backend-config <path>: every MCP client keeps "
				  "its configuration somewhere different, and guessing wrong would write a "
				  "file nothing reads";
		return false;
	}
	if (p_backend.transport == "http" && !p_options.http_token.empty()) {
		r_error = "refusing to write a configuration containing --http-token: the entry "
				  "reads $" +
				p_backend.token_variable + " at run time instead, so the secret stays out of a "
										   "file that gets copied around";
		return false;
	}

	const std::string executable = platform::executable_path();
	if (p_backend.transport == "stdio" && executable.empty()) {
		r_error = "could not determine this binary's own path";
		return false;
	}

	// Merge rather than replace: a client's configuration usually names several
	// servers, and clobbering the others would be a rude way to install one.
	JSONValueRef document;
	std::string existing;
	if (platform::read_file(p_path, existing) && !existing.empty()) {
		std::string parse_error;
		document = json_parse(existing, &parse_error);
		if (!document || !document->is_object()) {
			r_error = "existing configuration at " + p_path + " is not a JSON object" +
					(parse_error.empty() ? "" : ": " + parse_error);
			return false;
		}
	}
	if (!document) {
		document = JSONValue::make_object();
	}

	JSONValueRef servers = document->get("mcpServers");
	if (servers && !servers->is_object()) {
		r_error = "existing configuration at " + p_path + " has a non-object 'mcpServers'";
		return false;
	}
	if (!servers) {
		servers = JSONValue::make_object();
	}

	const bool replaced = servers->get(BACKEND_SERVER_KEY) != nullptr;
	servers->set(BACKEND_SERVER_KEY, backend_entry(p_backend, p_options, executable));
	document->set("mcpServers", servers);

	// The directory may not exist yet if the client has never been configured.
	const size_t separator = p_path.find_last_of("/\\");
	if (separator != std::string::npos && separator > 0) {
		platform::make_directories(p_path.substr(0, separator));
	}
	if (!platform::write_file(p_path, document->to_string() + "\n", r_error)) {
		return false;
	}

	r_summary = std::string(replaced ? "updated" : "added") + " '" + BACKEND_SERVER_KEY +
			"' (" + p_backend.name + ") in " + p_path;
	return true;
}

bool backend_check(const std::string &p_path, bool &r_up_to_date, std::string &r_summary, std::string &r_error) {
	r_up_to_date = false;
	if (p_path.empty()) {
		r_error = "--check-backends needs --backend-config <path>";
		return false;
	}

	std::string contents;
	if (!platform::read_file(p_path, contents)) {
		r_summary = "no configuration at " + p_path;
		return true;
	}
	std::string parse_error;
	JSONValueRef document = json_parse(contents, &parse_error);
	if (!document || !document->is_object()) {
		r_error = "configuration at " + p_path + " is not a JSON object";
		return false;
	}
	JSONValueRef servers = document->get("mcpServers");
	JSONValueRef entry = servers && servers->is_object() ? servers->get(BACKEND_SERVER_KEY) : nullptr;
	if (!entry || !entry->is_object()) {
		r_summary = "no '" + std::string(BACKEND_SERVER_KEY) + "' entry in " + p_path +
				"; run --install-backend";
		return true;
	}

	JSONValueRef metadata = entry->get(BACKEND_METADATA_KEY);
	const std::string relay_version = metadata && metadata->is_object()
			? metadata->get_string_or("relay_version", std::string())
			: std::string();
	const std::string bridge_version = metadata && metadata->is_object()
			? metadata->get_string_or("bridge_version", std::string())
			: std::string();

	if (relay_version.empty()) {
		r_summary = "the entry in " + p_path + " was not written by this tool; "
											   "run --install-backend to replace it";
		return true;
	}
	if (bridge_version != RELAY_BRIDGE_VERSION) {
		r_summary = "the entry in " + p_path + " targets bridge version " + bridge_version +
				", but this relay speaks " + RELAY_BRIDGE_VERSION + "; run --install-backend";
		return true;
	}
	if (relay_version != RELAY_VERSION) {
		r_summary = "the entry in " + p_path + " was written by relay " + relay_version +
				" and this is " + RELAY_VERSION + "; re-running --install-backend keeps them in step";
		return true;
	}

	r_up_to_date = true;
	r_summary = "the entry in " + p_path + " is current (relay " + RELAY_VERSION +
			", bridge " + RELAY_BRIDGE_VERSION + ")";
	return true;
}

} // namespace godot_ai
