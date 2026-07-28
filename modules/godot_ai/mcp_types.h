/**************************************************************************/
/*  mcp_types.h                                                           */
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

#ifndef MCP_TYPES_H
#define MCP_TYPES_H

#include "core/string/ustring.h"
#include "core/variant/dictionary.h"

// Capability classes from the specification's security model. Every tool declares
// exactly one; policy is applied per class rather than per tool, so a new tool
// cannot quietly gain unreviewed reach.
enum MCPCapability {
	MCP_CAP_READ_PROJECT, // Reads project files, assets and edited scene state.
	MCP_CAP_READ_RUNTIME, // Reads state from the running game.
	MCP_CAP_EDIT_FILES, // Writes files inside the project.
	MCP_CAP_EDIT_SCENE, // Mutates the edited scene or resources.
	MCP_CAP_RUN_PROJECT, // Starts, stops or drives play mode.
	MCP_CAP_DANGEROUS_EXEC, // Anything with arbitrary reach. Denied by default.
	MCP_CAP_MAX,
};

// Policy a user can apply to a capability class.
enum MCPPolicy {
	MCP_POLICY_ALLOW,
	MCP_POLICY_ASK,
	MCP_POLICY_DENY,
};

String mcp_capability_to_string(MCPCapability p_capability);
bool mcp_capability_from_string(const String &p_name, MCPCapability &r_capability);
// True for classes that can change persistent project state; these are refused
// outright in read-only sessions.
bool mcp_capability_is_mutating(MCPCapability p_capability);

String mcp_policy_to_string(MCPPolicy p_policy);
bool mcp_policy_from_string(const String &p_name, MCPPolicy &r_policy);

// True for argument names whose values must never reach the audit log or an
// approval prompt (tokens, passwords, keys).
bool mcp_is_sensitive_key(const String &p_key);

// Error carried out of a tool invocation. Protocol-level problems become JSON-RPC
// errors; tool-level failures become MCP results with `isError` set.
struct MCPToolError {
	enum Kind {
		NONE,
		INVALID_ARGUMENTS, // Schema validation failed.
		NOT_FOUND, // The referenced scene/node/file does not exist.
		INVALID_STATE, // The editor is not in a state where this makes sense.
		PERMISSION_DENIED,
		FAILED, // The operation was attempted and did not succeed.
		UNSUPPORTED, // Not available in this build/session (e.g. headless).
	};

	Kind kind = NONE;
	String message;
	Dictionary data;

	bool has_error() const { return kind != NONE; }
	void set(Kind p_kind, const String &p_message) {
		kind = p_kind;
		message = p_message;
	}
	void clear() {
		kind = NONE;
		message = String();
		data = Dictionary();
	}

	String kind_to_string() const;
};

#endif // MCP_TYPES_H
