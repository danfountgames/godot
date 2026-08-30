/**************************************************************************/
/*  mcp_permissions.cpp                                                   */
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

#include "mcp_permissions.h"

#include "mcp_unattended.h"

#ifdef TOOLS_ENABLED
#include "editor/settings/editor_settings.h"
#endif

static bool s_policy_override_set[MCP_CAP_MAX] = {};
static MCPPolicy s_policy_override[MCP_CAP_MAX] = {};

MCPPolicy MCPPermissions::get_default_policy(MCPCapability p_capability) {
	switch (p_capability) {
		case MCP_CAP_READ_PROJECT:
		case MCP_CAP_READ_RUNTIME:
			// Reading the project the user already opened is the baseline capability;
			// a prompt per read would make the tooling unusable.
			return MCP_POLICY_ALLOW;
		case MCP_CAP_EDIT_FILES:
		case MCP_CAP_EDIT_SCENE:
		case MCP_CAP_RUN_PROJECT:
		case MCP_CAP_SIMULATE_INPUT:
		case MCP_CAP_READ_USER_DATA:
		case MCP_CAP_EDIT_USER_DATA:
			return MCP_POLICY_ASK;
		case MCP_CAP_DANGEROUS_EXEC:
		default:
			return MCP_POLICY_DENY;
	}
}

static String policy_setting_name(MCPCapability p_capability) {
	return "network/godot_ai/policy_" + mcp_capability_to_string(p_capability);
}

void MCPPermissions::register_editor_settings() {
#ifdef TOOLS_ENABLED
	if (!EditorSettings::get_singleton()) {
		return;
	}
	for (int i = 0; i < MCP_CAP_MAX; i++) {
		const MCPCapability capability = (MCPCapability)i;
		if (capability == MCP_CAP_DANGEROUS_EXEC) {
			// Not user-configurable: there is no supported way to allow this class.
			continue;
		}
		const String name = policy_setting_name(capability);
		if (!EditorSettings::get_singleton()->has_setting(name)) {
			EditorSettings::get_singleton()->set_manually(name, mcp_policy_to_string(get_default_policy(capability)));
		}
		EditorSettings::get_singleton()->add_property_hint(
				PropertyInfo(Variant::STRING, name, PROPERTY_HINT_ENUM, "allow,ask,deny"));
	}
#endif
}

MCPPolicy MCPPermissions::get_policy(MCPCapability p_capability) {
	if (p_capability < 0 || p_capability >= MCP_CAP_MAX) {
		return MCP_POLICY_DENY;
	}
	if (s_policy_override_set[p_capability]) {
		return s_policy_override[p_capability];
	}
	if (p_capability == MCP_CAP_DANGEROUS_EXEC) {
		return MCP_POLICY_DENY;
	}
	// An unattended run has no settings dialog to state a policy in, so it states one
	// in the environment instead. It sits above editor settings deliberately: the
	// operator who launched this process is more authoritative about what this run may
	// do than whatever the last interactive session happened to leave behind.
	MCPPolicy declared = MCP_POLICY_DENY;
	if (MCPUnattended::policy_override(p_capability, declared)) {
		return declared;
	}
#ifdef TOOLS_ENABLED
	if (EditorSettings::get_singleton()) {
		const String name = policy_setting_name(p_capability);
		if (EditorSettings::get_singleton()->has_setting(name)) {
			MCPPolicy policy = MCP_POLICY_DENY;
			if (mcp_policy_from_string(EditorSettings::get_singleton()->get_setting(name), policy)) {
				return policy;
			}
			// An unreadable setting fails closed rather than falling back to allow.
			return MCP_POLICY_DENY;
		}
	}
#endif
	return get_default_policy(p_capability);
}

void MCPPermissions::set_policy_override(MCPCapability p_capability, MCPPolicy p_policy) {
	ERR_FAIL_INDEX(p_capability, MCP_CAP_MAX);
	s_policy_override_set[p_capability] = true;
	s_policy_override[p_capability] = p_policy;
}

void MCPPermissions::clear_policy_overrides() {
	for (int i = 0; i < MCP_CAP_MAX; i++) {
		s_policy_override_set[i] = false;
	}
}

MCPPermissions::Decision MCPPermissions::evaluate(const MCPSession &p_session, MCPCapability p_capability, const String &p_tool_name) {
	Decision decision;

	if (!p_session.client_approved) {
		decision.outcome = OUTCOME_DENY;
		decision.reason = vformat("client '%s' has not been approved for this project", p_session.client_name);
		return decision;
	}

	if (p_capability == MCP_CAP_DANGEROUS_EXEC) {
		decision.outcome = OUTCOME_DENY;
		decision.reason = vformat("'%s' requires the 'dangerous_exec' capability, which is always denied", p_tool_name);
		return decision;
	}

	// A read-only session refuses mutation regardless of stored policy: the client
	// asked for a restricted session and must not be able to escape it.
	if (p_session.read_only && mcp_capability_is_mutating(p_capability)) {
		decision.outcome = OUTCOME_DENY;
		decision.reason = vformat("'%s' would modify the project, but this session is read-only", p_tool_name);
		return decision;
	}

	const MCPPolicy policy = get_policy(p_capability);
	if (policy == MCP_POLICY_DENY) {
		decision.outcome = OUTCOME_DENY;
		decision.reason = vformat("the '%s' capability is set to deny in the editor's AI settings",
				mcp_capability_to_string(p_capability));
		return decision;
	}
	if (policy == MCP_POLICY_ALLOW) {
		decision.outcome = OUTCOME_ALLOW;
		return decision;
	}

	// Policy is "ask": the session's requested approval mode decides how that is
	// resolved. It may narrow the decision but never widen a deny.
	switch (p_session.approval_mode) {
		case MCP_POLICY_ALLOW:
			decision.outcome = OUTCOME_ALLOW;
			decision.reason = "auto-approved by the client's --approval-mode allow";
			return decision;
		case MCP_POLICY_DENY:
			decision.outcome = OUTCOME_DENY;
			decision.reason = vformat("'%s' needs approval and the client requested --approval-mode deny", p_tool_name);
			return decision;
		case MCP_POLICY_ASK:
		default:
			decision.outcome = OUTCOME_PROMPT;
			decision.reason = vformat("'%s' needs the user's approval", p_tool_name);
			return decision;
	}
}
