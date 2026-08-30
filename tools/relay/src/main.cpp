/**************************************************************************/
/*  main.cpp                                                              */
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
#include "http_server.h"
#include "relay.h"

#include <cstdio>
#include <string>

static int run(godot_ai::RelayOptions &p_options, std::string &r_error);

int main(int argc, char **argv) {
	godot_ai::RelayOptions options;
	std::string error;
	bool exit_immediately = false;
	std::string immediate_output;

	if (!godot_ai::relay_parse_options(argc, argv, options, error, exit_immediately, immediate_output)) {
		// Usage diagnostics never touch stdout: a client may already be reading it as
		// a protocol stream.
		fprintf(stderr, "godot-ai-relay: %s\n", error.c_str());
		fprintf(stderr, "Run 'godot-ai-relay --help' for usage.\n");
		return 2;
	}
	if (exit_immediately) {
		fputs(immediate_output.c_str(), stderr);
		return 0;
	}

	// Nothing called this before, which POSIX forgave and Windows would not have:
	// without WSAStartup every socket call there fails with WSANOTINITIALISED. The
	// backend has always had the call; the entry point simply never made it.
	if (!godot_ai::platform::initialize(error)) {
		fprintf(stderr, "godot-ai-relay: %s\n", error.c_str());
		return 2;
	}

	const int status = run(options, error);
	godot_ai::platform::finalize();
	return status;
}

static int run(godot_ai::RelayOptions &p_options, std::string &r_error) {
	godot_ai::RelayOptions &options = p_options;
	std::string &error = r_error;
	// Configuration management runs before anything connects: these commands are
	// about a file on disk, and must work with no editor running.
	if (options.list_backends) {
		fputs(godot_ai::agent_backends_listing().c_str(), stdout);
		return 0;
	}
	if (options.check_backends) {
		bool up_to_date = false;
		std::string summary;
		if (!godot_ai::backend_check(options.backend_config, up_to_date, summary, error)) {
			fprintf(stderr, "godot-ai-relay: %s\n", error.c_str());
			return 2;
		}
		printf("%s\n", summary.c_str());
		return up_to_date ? 0 : 1;
	}
	if (!options.install_backend.empty()) {
		godot_ai::AgentBackend backend;
		if (!godot_ai::agent_backend_by_name(options.install_backend, backend)) {
			fprintf(stderr, "godot-ai-relay: unknown backend '%s'\n", options.install_backend.c_str());
			fputs(godot_ai::agent_backends_listing().c_str(), stderr);
			return 2;
		}
		std::string summary;
		if (!godot_ai::backend_install(backend, options, options.backend_config, summary, error)) {
			fprintf(stderr, "godot-ai-relay: %s\n", error.c_str());
			return 2;
		}
		printf("%s\n", summary.c_str());
		return 0;
	}

	if (options.http_port > 0) {
		godot_ai::HttpOptions http;
		http.host = options.http_host;
		http.port = options.http_port;
		http.path = options.http_path;
		http.token = options.http_token;
		http.allow_remote = options.http_allow_remote;

		godot_ai::HttpServer server(options, http);
		if (!server.start(error)) {
			fprintf(stderr, "godot-ai-relay: %s\n", error.c_str());
			return 2;
		}
		return server.run();
	}

	godot_ai::Relay relay(options);
	// Every mode that answers once and exits. A mode missing from this list does not
	// fail: it falls through to serving MCP on stdin, reads EOF, and exits 0 - a silent
	// success that produced no output and looked exactly like a working call that had
	// nothing to say. Adding a one-shot mode means adding it here too.
	if (!options.call_tool.empty() || options.list_tools || options.batch ||
			options.list_prompts || !options.prompt_name.empty() ||
			!options.describe_tool.empty()) {
		return relay.run_one_shot();
	}
	return relay.run();
}
