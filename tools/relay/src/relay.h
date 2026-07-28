/**************************************************************************/
/*  relay.h                                                               */
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

#ifndef GODOT_AI_RELAY_RELAY_H
#define GODOT_AI_RELAY_RELAY_H

#include "json.h"

#include <string>
#include <utility>
#include <vector>

namespace godot_ai {

// Relay-specific JSON-RPC error codes, in the implementation-defined server range
// (-32099..-32000) reserved by JSON-RPC 2.0.
enum RelayErrorCode {
	RELAY_ERROR_EDITOR_UNAVAILABLE = -32001,
	RELAY_ERROR_EDITOR_DISCONNECTED = -32002,
	RELAY_ERROR_VERSION_MISMATCH = -32003,
	RELAY_ERROR_HANDSHAKE_REJECTED = -32004,
};

enum LogLevel {
	LOG_ERROR = 0,
	LOG_WARN = 1,
	LOG_INFO = 2,
	LOG_DEBUG = 3,
};

enum ApprovalMode {
	APPROVAL_ASK,
	APPROVAL_ALLOW,
	APPROVAL_DENY,
};

struct RelayOptions {
	std::string editor_host = "127.0.0.1";
	int editor_port = -1; // -1 means "discover".
	std::string project; // Filter discovery by project path.
	long instance_pid = -1; // Filter discovery by editor pid.
	LogLevel log_level = LOG_WARN;
	bool read_only = false;
	ApprovalMode approval_mode = APPROVAL_ASK;
	std::string client_name;
	std::string home; // Overrides GODOT_AI_HOME / ~/.godot-ai.
	// How long to wait for the editor to answer the bridge handshake. A hung editor
	// must not stall the relay's event loop, so this is deliberately short.
	int handshake_timeout_ms = 5000;

	// Parsed but not otherwise meaningful: the relay always speaks MCP.
	bool mcp = false;
};

// Parses argv. Returns false on a usage error (message in r_error).
// r_exit_immediately is set for --help / --version, handled by the caller.
bool relay_parse_options(int p_argc, char **p_argv, RelayOptions &r_options, std::string &r_error, bool &r_exit_immediately, std::string &r_immediate_output);

struct InstanceDescriptor {
	std::string path;
	long pid = -1;
	int port = -1;
	std::string project_path;
	std::string project_name;
	std::string editor_version;
	std::string protocol_version;
	double started_at = 0.0;
};

std::string relay_home_dir(const RelayOptions &p_options);
std::string relay_instances_dir(const RelayOptions &p_options);

// Reads every descriptor in the instances directory, newest first.
std::vector<InstanceDescriptor> relay_discover_instances(const RelayOptions &p_options);

// Applies --project / --instance filters. Returns false when nothing matches, or
// when several instances match and the caller has not said which one it wants.
bool relay_select_instance(const std::vector<InstanceDescriptor> &p_instances, const RelayOptions &p_options, InstanceDescriptor &r_selected, std::string &r_error);

// Framing/handshake version shared with the editor module. Bumped together.
extern const char *RELAY_BRIDGE_VERSION;
extern const char *RELAY_VERSION;

class Relay {
	RelayOptions options;

	int socket_fd = -1;
	bool connected = false;
	bool handshake_complete = false;
	std::string fatal_bridge_error; // Non-empty once the bridge is unusable.
	int fatal_bridge_code = RELAY_ERROR_EDITOR_UNAVAILABLE;

	std::string stdin_buffer;
	std::string socket_buffer;

	// Requests forwarded to the editor and not yet answered, so a mid-flight editor
	// loss can be reported to the client instead of hanging it forever.
	std::vector<std::pair<std::string, std::string>> pending; // key -> raw id JSON.

	InstanceDescriptor selected_instance;

	void log(LogLevel p_level, const std::string &p_message) const;

	void write_stdout_line(const std::string &p_line) const;
	void send_error(const JSONValueRef &p_id, int p_code, const std::string &p_message) const;

	bool ensure_connected(std::string &r_error);
	void disconnect(const std::string &p_reason, bool p_fail_pending);
	bool socket_send_line(const std::string &p_line);
	bool perform_handshake(std::string &r_error, int &r_error_code);

	void handle_client_line(const std::string &p_line);
	void handle_editor_line(const std::string &p_line);

	void fail_all_pending(int p_code, const std::string &p_message);

	static std::string id_key(const JSONValueRef &p_id);

public:
	explicit Relay(const RelayOptions &p_options) :
			options(p_options) {}
	~Relay();

	// Runs until stdin reaches EOF or a termination signal arrives. Returns the
	// process exit code.
	int run();
};

} // namespace godot_ai

#endif // GODOT_AI_RELAY_RELAY_H
