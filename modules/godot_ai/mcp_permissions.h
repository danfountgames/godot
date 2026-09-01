/**************************************************************************/
/*  mcp_permissions.h                                                     */
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

#ifndef MCP_PERMISSIONS_H
#define MCP_PERMISSIONS_H

#include "mcp_types.h"

// Per-connection state established by the bridge handshake. Permission decisions are
// a function of this plus the tool's capability class, so they can be reasoned about
// (and unit-tested) without a live socket.
struct MCPSession {
	String client_name = "unknown-client";
	String client_id; // Stable identity used for stored approvals.
	int64_t relay_pid = 0;
	bool read_only = false;
	MCPPolicy approval_mode = MCP_POLICY_ASK;

	// Set once the user has approved this client for this project.
	bool client_approved = false;

	// MCP session state.
	bool initialized = false;
	String protocol_version;
	String mcp_client_name;
	String mcp_client_version;

	// The client offered `sampling` at initialize. Recorded because the client said it,
	// not because anything asks: the in-editor chat that consumed it is gone - one
	// conversation surface, the agent terminal - and this stays only as an honest note
	// of what the session negotiated.
	bool supports_sampling = false;
};

// Resolves "may this client run this tool right now?" from user policy, the session's
// requested mode, and the tool's capability class.
class MCPPermissions {
public:
	enum Outcome {
		OUTCOME_ALLOW,
		OUTCOME_PROMPT, // Requires an interactive decision before running.
		OUTCOME_DENY,
	};

	struct Decision {
		Outcome outcome = OUTCOME_DENY;
		String reason;

		bool is_allowed() const { return outcome == OUTCOME_ALLOW; }
	};

	// Editor-settings-backed policy, with the specification's defaults when no
	// EditorSettings exists (headless runs and unit tests).
	static MCPPolicy get_policy(MCPCapability p_capability);
	// Persists the editor policy selected by the user. Dangerous execution is not a
	// configurable capability and is always refused.
	static bool set_policy(MCPCapability p_capability, MCPPolicy p_policy);
	static void set_policy_override(MCPCapability p_capability, MCPPolicy p_policy);
	static void clear_policy_overrides();
	static MCPPolicy get_default_policy(MCPCapability p_capability);

	static Decision evaluate(const MCPSession &p_session, MCPCapability p_capability, const String &p_tool_name);

	// Registers the editor settings this module reads. Called by the service.
	static void register_editor_settings();
};

#endif // MCP_PERMISSIONS_H
