/**************************************************************************/
/*  mcp_types.cpp                                                         */
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

#include "mcp_types.h"

String mcp_capability_to_string(MCPCapability p_capability) {
	switch (p_capability) {
		case MCP_CAP_READ_PROJECT:
			return "read_project";
		case MCP_CAP_READ_RUNTIME:
			return "read_runtime";
		case MCP_CAP_EDIT_FILES:
			return "edit_files";
		case MCP_CAP_EDIT_SCENE:
			return "edit_scene";
		case MCP_CAP_RUN_PROJECT:
			return "run_project";
		case MCP_CAP_SIMULATE_INPUT:
			return "simulate_input";
		case MCP_CAP_DANGEROUS_EXEC:
			return "dangerous_exec";
		default:
			return "unknown";
	}
}

bool mcp_capability_from_string(const String &p_name, MCPCapability &r_capability) {
	for (int i = 0; i < MCP_CAP_MAX; i++) {
		if (mcp_capability_to_string((MCPCapability)i) == p_name) {
			r_capability = (MCPCapability)i;
			return true;
		}
	}
	return false;
}

bool mcp_capability_is_mutating(MCPCapability p_capability) {
	switch (p_capability) {
		case MCP_CAP_EDIT_FILES:
		case MCP_CAP_EDIT_SCENE:
		case MCP_CAP_DANGEROUS_EXEC:
			return true;
		default:
			// Running the project changes runtime state but nothing persistent, so it
			// is gated by policy without being treated as a mutation.
			return false;
	}
}

String mcp_policy_to_string(MCPPolicy p_policy) {
	switch (p_policy) {
		case MCP_POLICY_ALLOW:
			return "allow";
		case MCP_POLICY_ASK:
			return "ask";
		case MCP_POLICY_DENY:
		default:
			return "deny";
	}
}

bool mcp_policy_from_string(const String &p_name, MCPPolicy &r_policy) {
	if (p_name == "allow") {
		r_policy = MCP_POLICY_ALLOW;
	} else if (p_name == "ask") {
		r_policy = MCP_POLICY_ASK;
	} else if (p_name == "deny") {
		r_policy = MCP_POLICY_DENY;
	} else {
		return false;
	}
	return true;
}

bool mcp_is_sensitive_key(const String &p_key) {
	const String key = p_key.to_lower();
	static const char *needles[] = { "password", "passwd", "secret", "token", "api_key", "apikey", "credential", "private_key" };
	for (const char *needle : needles) {
		if (key.contains(needle)) {
			return true;
		}
	}
	return false;
}

String MCPToolError::kind_to_string() const {
	switch (kind) {
		case NONE:
			return "none";
		case INVALID_ARGUMENTS:
			return "invalid_arguments";
		case NOT_FOUND:
			return "not_found";
		case INVALID_STATE:
			return "invalid_state";
		case PERMISSION_DENIED:
			return "permission_denied";
		case UNSUPPORTED:
			return "unsupported";
		case FAILED:
		default:
			return "failed";
	}
}
