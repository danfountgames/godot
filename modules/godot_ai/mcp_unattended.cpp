/**************************************************************************/
/*  mcp_unattended.cpp                                                    */
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

#include "mcp_unattended.h"

#include "core/os/os.h"
#include "servers/display/display_server.h"

const char *MCPUnattended::ENV_POLICY = "GODOT_AI_POLICY";
const char *MCPUnattended::ENV_UNATTENDED = "GODOT_AI_UNATTENDED";
const char *MCPUnattended::ENV_APPROVE_CLIENTS = "GODOT_AI_APPROVE_CLIENTS";
const char *MCPUnattended::ENV_AUTO_APPROVE = "GODOT_AI_AUTO_APPROVE";

namespace {

bool s_override_active = false;
String s_override_policy;
bool s_override_unattended = false;
bool s_override_approve_clients = false;

String read_env(const char *p_name) {
	if (s_override_active) {
		return String();
	}
	return OS::get_singleton() ? OS::get_singleton()->get_environment(p_name) : String();
}

bool env_flag(const char *p_name) {
	return read_env(p_name) == "1";
}

} // namespace

bool MCPUnattended::is_unattended() {
	if (s_override_active) {
		return s_override_unattended;
	}
	if (env_flag(ENV_UNATTENDED)) {
		return true;
	}
	// The display server, not the editor, is what decides whether anything can be
	// shown. `--headless --editor` has a complete EditorNode and nobody looking at it.
	DisplayServer *display = DisplayServer::get_singleton();
	if (!display) {
		return true;
	}
	return display->get_name() == "headless";
}

String MCPUnattended::no_user_reason(const String &p_what, const String &p_alternative) {
	String reason = vformat(
			"there is nobody here to %s: this editor is running unattended, so a dialog would "
			"wait for its whole timeout and then be dismissed by nothing",
			p_what);
	if (!p_alternative.is_empty()) {
		reason += ". " + p_alternative;
	}
	return reason;
}

bool MCPUnattended::clients_pre_approved() {
	if (s_override_active) {
		return s_override_approve_clients;
	}
	return env_flag(ENV_APPROVE_CLIENTS) || env_flag(ENV_AUTO_APPROVE);
}

bool MCPUnattended::parse_policy(const String &p_text, MCPPolicy *r_policies, bool *r_set, String &r_error) {
	for (int i = 0; i < MCP_CAP_MAX; i++) {
		r_set[i] = false;
		r_policies[i] = MCP_POLICY_DENY;
	}

	const String text = p_text.strip_edges();
	if (text.is_empty()) {
		return true;
	}

	for (const String &raw : text.split(",", false)) {
		const String entry = raw.strip_edges();
		if (entry.is_empty()) {
			continue;
		}
		const int equals = entry.find_char('=');
		if (equals <= 0) {
			r_error = vformat("'%s' is not 'capability=policy'", entry);
			return false;
		}
		const String capability_name = entry.substr(0, equals).strip_edges();
		const String policy_name = entry.substr(equals + 1).strip_edges();

		MCPCapability capability = MCP_CAP_MAX;
		if (!mcp_capability_from_string(capability_name, capability)) {
			r_error = vformat("'%s' is not a capability", capability_name);
			return false;
		}
		if (capability == MCP_CAP_DANGEROUS_EXEC) {
			// Refusing loudly beats accepting and ignoring: an operator who wrote this
			// believed they were granting something.
			r_error = "'dangerous_exec' cannot be granted, here or anywhere";
			return false;
		}
		MCPPolicy policy = MCP_POLICY_DENY;
		if (!mcp_policy_from_string(policy_name, policy)) {
			r_error = vformat("'%s' is not a policy; use allow, ask or deny", policy_name);
			return false;
		}
		r_policies[capability] = policy;
		r_set[capability] = true;
	}
	return true;
}

bool MCPUnattended::policy_override(MCPCapability p_capability, MCPPolicy &r_policy) {
	if (p_capability < 0 || p_capability >= MCP_CAP_MAX) {
		return false;
	}
	const String text = s_override_active ? s_override_policy : read_env(ENV_POLICY);
	if (text.strip_edges().is_empty()) {
		return false;
	}

	MCPPolicy policies[MCP_CAP_MAX];
	bool set[MCP_CAP_MAX];
	String error;
	if (!parse_policy(text, policies, set, error)) {
		// A malformed policy grants nothing. Failing closed is the only safe reading
		// of "the operator meant something and we could not tell what".
		ERR_PRINT(vformat("Godot AI: %s is malformed (%s); no capability is granted by it.",
				ENV_POLICY, error));
		return false;
	}
	if (!set[p_capability]) {
		return false;
	}
	r_policy = policies[p_capability];
	return true;
}

String MCPUnattended::describe() {
	Vector<String> parts;
	if (is_unattended()) {
		parts.push_back("unattended (no display, or declared)");
	}
	if (clients_pre_approved()) {
		parts.push_back("clients pre-approved");
	}

	const String text = s_override_active ? s_override_policy : read_env(ENV_POLICY);
	if (!text.strip_edges().is_empty()) {
		MCPPolicy policies[MCP_CAP_MAX];
		bool set[MCP_CAP_MAX];
		String error;
		if (!parse_policy(text, policies, set, error)) {
			parts.push_back(vformat("policy IGNORED, malformed: %s", error));
		} else {
			String listed;
			for (int i = 0; i < MCP_CAP_MAX; i++) {
				if (!set[i]) {
					continue;
				}
				listed += (listed.is_empty() ? "" : " ") +
						mcp_capability_to_string((MCPCapability)i) + "=" +
						mcp_policy_to_string(policies[i]);
			}
			parts.push_back("policy " + listed);
		}
	}

	if (parts.is_empty()) {
		return String();
	}
	String out;
	for (const String &part : parts) {
		out += (out.is_empty() ? "" : "; ") + part;
	}
	return out;
}

void MCPUnattended::set_environment_override(const String &p_policy, bool p_unattended, bool p_approve_clients) {
	s_override_active = true;
	s_override_policy = p_policy;
	s_override_unattended = p_unattended;
	s_override_approve_clients = p_approve_clients;
}

void MCPUnattended::clear_environment_override() {
	s_override_active = false;
	s_override_policy = String();
	s_override_unattended = false;
	s_override_approve_clients = false;
}
