/**************************************************************************/
/*  mcp_protocol.cpp                                                      */
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

#include "mcp_protocol.h"

#include "mcp_tool_registry.h"

#include "core/io/json.h"
#include "core/variant/array.h"
#include "core/version.h"

const char *MCPProtocol::PROTOCOL_VERSION = "2025-06-18";
const char *MCPProtocol::BRIDGE_VERSION = "1";
const char *MCPProtocol::SERVER_NAME = "godot-ai";

// Revisions this server can speak. The newest is preferred.
static const char *SUPPORTED_PROTOCOL_VERSIONS[] = {
	"2025-06-18",
	"2025-03-26",
	"2024-11-05",
};

Dictionary MCPProtocol::make_result(const Variant &p_id, const Variant &p_result) {
	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = p_id;
	response["result"] = p_result;
	return response;
}

Dictionary MCPProtocol::make_error(const Variant &p_id, int p_code, const String &p_message, const Dictionary &p_data) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	if (!p_data.is_empty()) {
		error["data"] = p_data;
	}
	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = p_id;
	response["error"] = error;
	return response;
}

Dictionary MCPProtocol::make_notification(const String &p_method, const Dictionary &p_params) {
	Dictionary notification;
	notification["jsonrpc"] = "2.0";
	notification["method"] = p_method;
	if (!p_params.is_empty()) {
		notification["params"] = p_params;
	}
	return notification;
}

Dictionary MCPProtocol::make_tool_result(const Dictionary &p_structured, const Dictionary &p_output_schema) {
	Dictionary result;

	// Every result carries a text rendering: clients that cannot consume structured
	// content still get something a model can read.
	Dictionary text_content;
	text_content["type"] = "text";
	text_content["text"] = JSON::stringify(p_structured, "  ");
	Array content;
	content.push_back(text_content);

	result["content"] = content;
	result["isError"] = false;
	// structuredContent is only meaningful when the tool declared an output schema.
	if (!p_output_schema.is_empty()) {
		result["structuredContent"] = p_structured;
	}
	return result;
}

Dictionary MCPProtocol::make_tool_error_result(const MCPToolError &p_error) {
	Dictionary text_content;
	text_content["type"] = "text";
	text_content["text"] = p_error.message;

	Array content;
	content.push_back(text_content);

	Dictionary result;
	result["content"] = content;
	result["isError"] = true;

	Dictionary meta;
	meta["kind"] = p_error.kind_to_string();
	if (!p_error.data.is_empty()) {
		meta["data"] = p_error.data;
	}
	result["_meta"] = meta;
	return result;
}

bool MCPProtocol::_handle_hello(const Dictionary &p_params, const Variant &p_id, MCPSession &r_session, Delegate *p_delegate, Dictionary &r_response) {
	// Version check first: an incompatible relay must be told precisely that, rather
	// than failing later in a confusing way.
	bool version_supported = false;
	if (p_params.has("bridge_versions")) {
		const Array versions = p_params["bridge_versions"];
		for (int i = 0; i < versions.size(); i++) {
			if (String(versions[i]) == BRIDGE_VERSION) {
				version_supported = true;
				break;
			}
		}
	}
	if (!version_supported) {
		Dictionary data;
		data["editor_bridge_version"] = BRIDGE_VERSION;
		r_response = make_error(p_id, ERROR_BRIDGE_VERSION,
				vformat("this editor speaks bridge protocol %s; update godot-ai-relay to match", BRIDGE_VERSION), data);
		return true;
	}

	if (p_params.has("client_name")) {
		r_session.client_name = p_params["client_name"];
	}
	if (p_params.has("read_only")) {
		r_session.read_only = p_params["read_only"];
	}
	if (p_params.has("pid")) {
		r_session.relay_pid = (int64_t)p_params["pid"];
	}
	if (p_params.has("approval_mode")) {
		MCPPolicy mode = MCP_POLICY_ASK;
		if (!mcp_policy_from_string(p_params["approval_mode"], mode)) {
			r_response = make_error(p_id, ERROR_INVALID_PARAMS,
					vformat("unknown approval_mode '%s'", String(p_params["approval_mode"])));
			return true;
		}
		r_session.approval_mode = mode;
	}
	r_session.client_id = r_session.client_name;

	String reason;
	if (!p_delegate->approve_client(r_session, reason)) {
		r_session.client_approved = false;
		r_response = make_error(p_id, ERROR_CLIENT_NOT_APPROVED,
				reason.is_empty() ? String("this client is not approved to use the editor") : reason);
		return true;
	}
	r_session.client_approved = true;

	Dictionary result;
	result["bridge_version"] = BRIDGE_VERSION;
	result["protocol_version"] = PROTOCOL_VERSION;
	result["editor_version"] = p_delegate->get_editor_version();
	result["project_path"] = p_delegate->get_project_path();
	result["project_name"] = p_delegate->get_project_name();
	result["read_only"] = r_session.read_only;
	r_response = make_result(p_id, result);
	return true;
}

bool MCPProtocol::_handle_initialize(const Dictionary &p_params, const Variant &p_id, MCPSession &r_session, Dictionary &r_response) {
	const String requested = p_params.has("protocolVersion") ? String(p_params["protocolVersion"]) : String();

	// MCP requires the server to answer with a version it supports; the client then
	// decides whether it can proceed. Echo the request back when it is supported.
	String negotiated = PROTOCOL_VERSION;
	for (const char *version : SUPPORTED_PROTOCOL_VERSIONS) {
		if (requested == version) {
			negotiated = requested;
			break;
		}
	}
	r_session.protocol_version = negotiated;

	if (p_params.has("clientInfo")) {
		const Dictionary client_info = p_params["clientInfo"];
		r_session.mcp_client_name = client_info.has("name") ? String(client_info["name"]) : String();
		r_session.mcp_client_version = client_info.has("version") ? String(client_info["version"]) : String();
	}

	Dictionary tools_capability;
	tools_capability["listChanged"] = true;
	Dictionary capabilities;
	capabilities["tools"] = tools_capability;

	Dictionary server_info;
	server_info["name"] = SERVER_NAME;
	server_info["version"] = String(VERSION_BRANCH);

	Dictionary result;
	result["protocolVersion"] = negotiated;
	result["capabilities"] = capabilities;
	result["serverInfo"] = server_info;
	result["instructions"] =
			"Tools prefixed with Godot_ drive the running Godot editor. Reads are allowed "
			"by default; anything that modifies the project or starts the game is subject "
			"to the user's permission settings and may be refused. Edits made while the "
			"game is running are not persistent - use the scene tools for changes that "
			"must survive stopping the game.";
	if (!requested.is_empty() && requested != negotiated) {
		// Tell the client its request was not honoured, rather than silently differing.
		Dictionary meta;
		meta["requestedProtocolVersion"] = requested;
		result["_meta"] = meta;
	}

	r_session.initialized = true;
	r_response = make_result(p_id, result);
	return true;
}

bool MCPProtocol::_handle_tools_list(const Variant &p_id, Dictionary &r_response) {
	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	Dictionary result;
	result["tools"] = registry ? registry->get_tool_descriptors() : Array();
	r_response = make_result(p_id, result);
	return true;
}

bool MCPProtocol::_handle_tools_call(const Dictionary &p_params, const Variant &p_id, MCPSession &r_session, Delegate *p_delegate, Dictionary &r_response) {
	if (!p_params.has("name")) {
		r_response = make_error(p_id, ERROR_INVALID_PARAMS, "tools/call requires a 'name' parameter");
		return true;
	}
	const String tool_name = p_params["name"];

	MCPToolRegistry *registry = MCPToolRegistry::get_singleton();
	if (!registry) {
		r_response = make_error(p_id, ERROR_INTERNAL, "the tool registry is not available");
		return true;
	}

	const Ref<MCPTool> tool = registry->get_tool(tool_name);
	if (tool.is_null()) {
		// An unknown tool is a protocol-level error: the client asked for something
		// that does not exist, which no amount of model reasoning can fix.
		r_response = make_error(p_id, ERROR_METHOD_NOT_FOUND, vformat("unknown tool '%s'", tool_name));
		return true;
	}

	Dictionary arguments;
	if (p_params.has("arguments")) {
		const Variant arguments_value = p_params["arguments"];
		if (arguments_value.get_type() != Variant::DICTIONARY && arguments_value.get_type() != Variant::NIL) {
			r_response = make_error(p_id, ERROR_INVALID_PARAMS, "tools/call 'arguments' must be an object");
			return true;
		}
		if (arguments_value.get_type() == Variant::DICTIONARY) {
			arguments = arguments_value;
		}
	}

	MCPPermissions::Decision decision = MCPPermissions::evaluate(r_session, tool->get_capability(), tool_name);
	if (decision.outcome == MCPPermissions::OUTCOME_PROMPT) {
		String reason;
		if (p_delegate->prompt_for_tool(r_session, tool, arguments, reason)) {
			decision.outcome = MCPPermissions::OUTCOME_ALLOW;
			decision.reason = reason;
		} else {
			decision.outcome = MCPPermissions::OUTCOME_DENY;
			decision.reason = reason.is_empty() ? vformat("the user declined to run '%s'", tool_name) : reason;
		}
	}

	const String summary = tool->describe_invocation(arguments);
	if (decision.outcome != MCPPermissions::OUTCOME_ALLOW) {
		// Refusals are audited too, so the trail shows what was attempted.
		p_delegate->record_invocation(r_session, tool_name, summary, false, decision.reason);
		Dictionary data;
		data["capability"] = mcp_capability_to_string(tool->get_capability());
		r_response = make_error(p_id, ERROR_PERMISSION_DENIED, decision.reason, data);
		return true;
	}

	MCPToolError error;
	const Dictionary structured = registry->call_tool(tool_name, arguments, error);

	p_delegate->record_invocation(r_session, tool_name, summary, true,
			error.has_error() ? error.message : String("ok"));

	if (error.has_error()) {
		// Invalid arguments are the caller's protocol mistake; everything else is a
		// tool-level failure the model should see and can react to.
		if (error.kind == MCPToolError::INVALID_ARGUMENTS) {
			r_response = make_error(p_id, ERROR_INVALID_PARAMS, error.message);
		} else {
			r_response = make_result(p_id, make_tool_error_result(error));
		}
		return true;
	}

	r_response = make_result(p_id, make_tool_result(structured, tool->get_output_schema()));
	return true;
}

bool MCPProtocol::handle_message(const Dictionary &p_message, MCPSession &r_session, Delegate *p_delegate, Dictionary &r_response) {
	ERR_FAIL_NULL_V(p_delegate, false);

	const Variant id = p_message.has("id") ? p_message["id"] : Variant();
	const bool is_request = id.get_type() != Variant::NIL;

	if (!p_message.has("method")) {
		if (is_request) {
			r_response = make_error(id, ERROR_INVALID_REQUEST, "message has no 'method'");
			return true;
		}
		// A response to something we never sent: ignore rather than answer.
		return false;
	}
	const String method = p_message["method"];

	if (p_message.has("jsonrpc") && String(p_message["jsonrpc"]) != "2.0") {
		if (is_request) {
			r_response = make_error(id, ERROR_INVALID_REQUEST, "only JSON-RPC 2.0 is supported");
			return true;
		}
		return false;
	}

	Dictionary params;
	if (p_message.has("params") && Variant(p_message["params"]).get_type() == Variant::DICTIONARY) {
		params = p_message["params"];
	}

	if (method == "godot/hello") {
		if (!is_request) {
			return false;
		}
		return _handle_hello(params, id, r_session, p_delegate, r_response);
	}

	// Everything past the handshake requires an approved client. This is the single
	// gate: no tool method can be reached without passing through here.
	if (!r_session.client_approved) {
		if (is_request) {
			r_response = make_error(id, ERROR_CLIENT_NOT_APPROVED,
					"this connection has not completed the editor handshake");
			return true;
		}
		return false;
	}

	if (method == "initialize") {
		if (!is_request) {
			return false;
		}
		return _handle_initialize(params, id, r_session, r_response);
	}

	if (method == "notifications/initialized") {
		r_session.initialized = true;
		return false;
	}

	if (method.begins_with("notifications/")) {
		// Cancellation and progress notifications are accepted and ignored: every tool
		// here runs to completion on the editor's main thread.
		return false;
	}

	if (method == "ping") {
		if (!is_request) {
			return false;
		}
		r_response = make_result(id, Dictionary());
		return true;
	}

	if (!r_session.initialized) {
		if (is_request) {
			r_response = make_error(id, ERROR_NOT_INITIALIZED,
					vformat("'%s' was called before 'initialize'", method));
			return true;
		}
		return false;
	}

	if (method == "tools/list") {
		if (!is_request) {
			return false;
		}
		return _handle_tools_list(id, r_response);
	}

	if (method == "tools/call") {
		if (!is_request) {
			return false;
		}
		return _handle_tools_call(params, id, r_session, p_delegate, r_response);
	}

	if (is_request) {
		r_response = make_error(id, ERROR_METHOD_NOT_FOUND, vformat("unknown method '%s'", method));
		return true;
	}
	return false;
}
