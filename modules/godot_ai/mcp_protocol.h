/**************************************************************************/
/*  mcp_protocol.h                                                        */
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

#ifndef MCP_PROTOCOL_H
#define MCP_PROTOCOL_H

#include "mcp_permissions.h"
#include "mcp_tool.h"

#include "core/variant/dictionary.h"

// Pure request handling for the editor side of the bridge.
//
// Deliberately free of sockets and editor globals: everything the protocol needs
// from the outside arrives through Delegate. That makes the whole JSON-RPC surface
// unit-testable, which is where protocol conformance is actually verified.
class MCPProtocol {
public:
	// JSON-RPC 2.0 codes plus the server-defined range used by this bridge.
	enum ErrorCode {
		ERROR_PARSE = -32700,
		ERROR_INVALID_REQUEST = -32600,
		ERROR_METHOD_NOT_FOUND = -32601,
		ERROR_INVALID_PARAMS = -32602,
		ERROR_INTERNAL = -32603,
		ERROR_NOT_INITIALIZED = -32010,
		ERROR_CLIENT_NOT_APPROVED = -32011,
		ERROR_BRIDGE_VERSION = -32012,
		ERROR_PERMISSION_DENIED = -32013,
	};

	// MCP revision implemented here.
	static const char *PROTOCOL_VERSION;
	// Framing/handshake version shared with godot-ai-relay. Bumped together.
	static const char *BRIDGE_VERSION;
	static const char *SERVER_NAME;

	// Side effects the protocol cannot perform on its own.
	class Delegate {
	public:
		virtual ~Delegate() {}

		// Decides whether an unknown client may use this editor. Returning false must
		// fill r_reason.
		virtual bool approve_client(MCPSession &p_session, String &r_reason) = 0;

		// Asks the user to approve one invocation. Only called when policy resolves to
		// "prompt".
		virtual bool prompt_for_tool(const MCPSession &p_session, const Ref<MCPTool> &p_tool, const Dictionary &p_arguments, String &r_reason) = 0;

		// Recorded for the audit trail. The summary is already redacted.
		virtual void record_invocation(const MCPSession &p_session, const String &p_tool_name, const String &p_summary, bool p_allowed, const String &p_reason) = 0;

		// A tool answered "later": hold this request id until the token completes.
		// Returning false means the transport cannot defer, and the caller turns it
		// into an immediate error rather than losing the request.
		virtual bool defer_response(const Variant &p_id, int64_t p_token, const Ref<MCPTool> &p_tool, const String &p_checkpoint) { return false; }

		virtual String get_project_path() const = 0;
		virtual String get_project_name() const = 0;
		virtual String get_editor_version() const = 0;
	};

	// Handles one decoded message. Returns true when r_response must be sent
	// (notifications produce no response).
	static bool handle_message(const Dictionary &p_message, MCPSession &r_session, Delegate *p_delegate, Dictionary &r_response);

	static Dictionary make_result(const Variant &p_id, const Variant &p_result);
	static Dictionary make_error(const Variant &p_id, int p_code, const String &p_message, const Dictionary &p_data = Dictionary());
	static Dictionary make_notification(const String &p_method, const Dictionary &p_params);

	// Builds the MCP result envelope for a tool's structured output. Public because a
	// deferred call is completed by the transport, after handle_message has returned.
	static Dictionary make_tool_result(const Dictionary &p_structured, const Dictionary &p_output_schema);
	static Dictionary make_tool_error_result(const MCPToolError &p_error);

private:
	static bool _handle_hello(const Dictionary &p_params, const Variant &p_id, MCPSession &r_session, Delegate *p_delegate, Dictionary &r_response);
	static bool _handle_initialize(const Dictionary &p_params, const Variant &p_id, MCPSession &r_session, Dictionary &r_response);
	static bool _handle_tools_list(const Variant &p_id, Dictionary &r_response);
	static bool _handle_tools_call(const Dictionary &p_params, const Variant &p_id, MCPSession &r_session, Delegate *p_delegate, Dictionary &r_response);

};

#endif // MCP_PROTOCOL_H
