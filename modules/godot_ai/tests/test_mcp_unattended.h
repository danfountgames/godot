/**************************************************************************/
/*  test_mcp_unattended.h                                                 */
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

#ifndef TEST_MCP_UNATTENDED_H
#define TEST_MCP_UNATTENDED_H

#include "modules/godot_ai/mcp_permissions.h"
#include "modules/godot_ai/mcp_unattended.h"

#include "tests/test_macros.h"

namespace TestMCPUnattended {

// Restores the environment seam however the test leaves, so one failing assertion
// cannot make every later case run under a stray policy.
struct EnvironmentScope {
	explicit EnvironmentScope(const String &p_policy, bool p_unattended = true, bool p_approve = false) {
		MCPUnattended::set_environment_override(p_policy, p_unattended, p_approve);
	}
	~EnvironmentScope() {
		MCPUnattended::clear_environment_override();
		MCPPermissions::clear_policy_overrides();
	}
};

TEST_CASE("[godot_ai] An unattended policy is read per capability") {
	EnvironmentScope scope("read_project=allow,edit_files=deny,run_project=ask");

	MCPPolicy policy = MCP_POLICY_DENY;
	REQUIRE(MCPUnattended::policy_override(MCP_CAP_READ_PROJECT, policy));
	CHECK(policy == MCP_POLICY_ALLOW);

	REQUIRE(MCPUnattended::policy_override(MCP_CAP_EDIT_FILES, policy));
	CHECK(policy == MCP_POLICY_DENY);

	REQUIRE(MCPUnattended::policy_override(MCP_CAP_RUN_PROJECT, policy));
	CHECK(policy == MCP_POLICY_ASK);

	// A capability the operator said nothing about falls through to the ordinary
	// settings path rather than being silently denied or granted.
	CHECK_FALSE(MCPUnattended::policy_override(MCP_CAP_EDIT_SCENE, policy));
}

TEST_CASE("[godot_ai] The declared policy outranks the editor's stored settings") {
	EnvironmentScope scope("edit_files=allow");

	// The operator who launched this process is more authoritative about what this
	// run may do than whatever the last interactive session left behind.
	CHECK(MCPPermissions::get_policy(MCP_CAP_EDIT_FILES) == MCP_POLICY_ALLOW);
}

TEST_CASE("[godot_ai] dangerous_exec cannot be granted by a policy") {
	EnvironmentScope scope("dangerous_exec=allow");

	// Refused at parse time, so the whole policy is rejected rather than partly
	// applied - an operator who wrote this believed they were granting something.
	MCPPolicy policies[MCP_CAP_MAX];
	bool set[MCP_CAP_MAX];
	String error;
	CHECK_FALSE(MCPUnattended::parse_policy("dangerous_exec=allow", policies, set, error));
	CHECK(error.contains("dangerous_exec"));

	// And the capability itself stays denied through every path.
	CHECK(MCPPermissions::get_policy(MCP_CAP_DANGEROUS_EXEC) == MCP_POLICY_DENY);

	MCPSession session;
	session.client_approved = true;
	const MCPPermissions::Decision decision =
			MCPPermissions::evaluate(session, MCP_CAP_DANGEROUS_EXEC, "Godot_Hypothetical");
	CHECK(decision.outcome == MCPPermissions::OUTCOME_DENY);
}

TEST_CASE("[godot_ai] A malformed policy grants nothing") {
	MCPPolicy policies[MCP_CAP_MAX];
	bool set[MCP_CAP_MAX];
	String error;

	CHECK_FALSE(MCPUnattended::parse_policy("read_project", policies, set, error));
	CHECK(error.contains("capability=policy"));

	CHECK_FALSE(MCPUnattended::parse_policy("read_porject=allow", policies, set, error));
	CHECK(error.contains("not a capability"));

	CHECK_FALSE(MCPUnattended::parse_policy("read_project=maybe", policies, set, error));
	CHECK(error.contains("allow, ask or deny"));

	// Failing closed is the only safe reading of "the operator meant something and we
	// could not tell what": one bad entry grants nothing, including the good entries
	// beside it.
	EnvironmentScope scope("read_project=allow,edit_files=sometimes");
	ERR_PRINT_OFF;
	MCPPolicy policy = MCP_POLICY_DENY;
	CHECK_FALSE(MCPUnattended::policy_override(MCP_CAP_READ_PROJECT, policy));
	ERR_PRINT_ON;
}

TEST_CASE("[godot_ai] An empty or absent policy changes nothing") {
	MCPPolicy policies[MCP_CAP_MAX];
	bool set[MCP_CAP_MAX];
	String error;
	CHECK(MCPUnattended::parse_policy("", policies, set, error));
	for (int i = 0; i < MCP_CAP_MAX; i++) {
		CHECK_FALSE(set[i]);
	}

	EnvironmentScope scope("");
	MCPPolicy policy = MCP_POLICY_DENY;
	CHECK_FALSE(MCPUnattended::policy_override(MCP_CAP_EDIT_FILES, policy));
	CHECK(MCPPermissions::get_policy(MCP_CAP_EDIT_FILES) == MCPPermissions::get_default_policy(MCP_CAP_EDIT_FILES));
}

TEST_CASE("[godot_ai] Whitespace and trailing separators are tolerated") {
	MCPPolicy policies[MCP_CAP_MAX];
	bool set[MCP_CAP_MAX];
	String error;
	REQUIRE(MCPUnattended::parse_policy("  read_project = allow ,, edit_files=deny,  ",
			policies, set, error));
	CHECK(set[MCP_CAP_READ_PROJECT]);
	CHECK(policies[MCP_CAP_READ_PROJECT] == MCP_POLICY_ALLOW);
	CHECK(set[MCP_CAP_EDIT_FILES]);
	CHECK(policies[MCP_CAP_EDIT_FILES] == MCP_POLICY_DENY);
}

TEST_CASE("[godot_ai] A read-only session still refuses what the policy allows") {
	EnvironmentScope scope("edit_files=allow");

	// The declared policy widens what the *editor* permits; it must not let a client
	// escape the restricted session it asked for.
	MCPSession session;
	session.client_approved = true;
	session.read_only = true;
	const MCPPermissions::Decision decision =
			MCPPermissions::evaluate(session, MCP_CAP_EDIT_FILES, "Godot_WriteFile");
	CHECK(decision.outcome == MCPPermissions::OUTCOME_DENY);
	CHECK(decision.reason.contains("read-only"));
}

TEST_CASE("[godot_ai] An unapproved client is refused whatever the policy says") {
	EnvironmentScope scope("read_project=allow", true, false);

	MCPSession session;
	session.client_approved = false;
	const MCPPermissions::Decision decision =
			MCPPermissions::evaluate(session, MCP_CAP_READ_PROJECT, "Godot_ReadFile");
	CHECK(decision.outcome == MCPPermissions::OUTCOME_DENY);
	CHECK(decision.reason.contains("approved"));
}

TEST_CASE("[godot_ai] Client pre-approval is a separate decision from policy") {
	{
		EnvironmentScope scope("read_project=allow", true, false);
		CHECK_FALSE(MCPUnattended::clients_pre_approved());
	}
	{
		EnvironmentScope scope("", true, true);
		CHECK(MCPUnattended::clients_pre_approved());
	}
}

TEST_CASE("[godot_ai] The unattended state is reported for the log") {
	{
		EnvironmentScope scope("read_project=allow", true, true);
		const String described = MCPUnattended::describe();
		CHECK(described.contains("unattended"));
		CHECK(described.contains("clients pre-approved"));
		CHECK(described.contains("read_project=allow"));
	}
	{
		// A malformed policy must be visible in the log as ignored, not absent.
		EnvironmentScope scope("nonsense", true, false);
		CHECK(MCPUnattended::describe().contains("IGNORED"));
	}
	{
		EnvironmentScope scope("", false, false);
		CHECK(MCPUnattended::describe().is_empty());
	}
}

TEST_CASE("[godot_ai] A declared unattended run reports no user to ask") {
	EnvironmentScope scope("", true, false);
	CHECK(MCPUnattended::is_unattended());

	const String reason = MCPUnattended::no_user_reason("answer a question", "Decide it yourself.");
	CHECK(reason.contains("nobody"));
	CHECK(reason.contains("Decide it yourself."));
}

} // namespace TestMCPUnattended

#endif // TEST_MCP_UNATTENDED_H
