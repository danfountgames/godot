/**************************************************************************/
/*  test_mcp_agent_state.h                                                */
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

#ifndef TEST_MCP_AGENT_STATE_H
#define TEST_MCP_AGENT_STATE_H

#include "modules/godot_ai/mcp_activity.h"
#include "modules/godot_ai/mcp_agent_state.h"

#include "tests/test_macros.h"

namespace TestMCPAgentState {

struct AgentStateFixture {
	AgentStateFixture() {
		MCPAgentControl::resume();
		MCPAgentIntent::clear();
	}
	~AgentStateFixture() {
		MCPAgentControl::resume();
		MCPAgentIntent::clear();
	}
};

TEST_CASE("[godot_ai] A declared goal and activity are readable back") {
	AgentStateFixture fixture;

	CHECK(MCPAgentIntent::get_goal().is_empty());
	MCPAgentIntent::set_goal("Improve the player jump");
	MCPAgentIntent::set_activity("Measuring the current jump height");
	CHECK(MCPAgentIntent::get_goal() == "Improve the player jump");
	CHECK(MCPAgentIntent::get_activity() == "Measuring the current jump height");

	// Cleared when the work ends, because a stale goal sitting over unrelated work is
	// worse than no goal at all.
	MCPAgentIntent::clear();
	CHECK(MCPAgentIntent::get_goal().is_empty());
	CHECK(MCPAgentIntent::get_activity().is_empty());
}

TEST_CASE("[godot_ai] Activity records carry the intent that was declared at the time") {
	AgentStateFixture fixture;
	MCPActivity::clear();

	MCPAgentIntent::set_goal("Improve the player jump");
	MCPAgentIntent::set_activity("Measuring the current jump height");
	MCPActivity::finish(
			MCPActivity::begin("c", "Godot_GetRuntimeProperty", MCP_CAP_READ_RUNTIME, "s", Array()),
			"ok", String(), String());

	// The next step declares something different; the earlier record must keep what was
	// true when it ran, not what is true now.
	MCPAgentIntent::set_activity("Trying a higher value");
	MCPActivity::finish(
			MCPActivity::begin("c", "Godot_SetRuntimeProperty", MCP_CAP_READ_RUNTIME, "s", Array()),
			"ok", String(), String());

	const Array records = MCPActivity::snapshot();
	REQUIRE(records.size() == 2);
	CHECK(String(Dictionary(records[0])["intent"]) == "Measuring the current jump height");
	CHECK(String(Dictionary(records[1])["intent"]) == "Trying a higher value");
	CHECK(String(Dictionary(records[0])["goal"]) == "Improve the player jump");

	MCPActivity::clear();
}

TEST_CASE("[godot_ai] A running agent may run anything") {
	AgentStateFixture fixture;
	String reason;
	CHECK(MCPAgentControl::get_state() == MCPAgentControl::STATE_RUNNING);
	CHECK(MCPAgentControl::may_run("Godot_ManageNode", true, reason));
	CHECK(MCPAgentControl::may_run("Godot_ReadTextFile", false, reason));
	CHECK(reason.is_empty());
}

TEST_CASE("[godot_ai] A paused agent is refused, and told enough to explain itself") {
	AgentStateFixture fixture;
	MCPAgentControl::pause();
	CHECK(MCPAgentControl::get_state() == MCPAgentControl::STATE_PAUSED);

	String reason;
	CHECK_FALSE(MCPAgentControl::may_run("Godot_ManageNode", true, reason));
	// A model handed a bare "denied" retries. One told a person pressed Pause can say so
	// and wait, which is the whole point of the control.
	CHECK(reason.contains("paused"));
	CHECK(reason.contains("Nothing was changed"));
	CHECK(reason.contains("Godot_GetActivity"));

	// Reading the project is still working, so it is held too.
	CHECK_FALSE(MCPAgentControl::may_run("Godot_ReadTextFile", false, reason));

	MCPAgentControl::resume();
	CHECK(MCPAgentControl::may_run("Godot_ManageNode", true, reason));
}

TEST_CASE("[godot_ai] A held agent can still be asked what it already did") {
	AgentStateFixture fixture;

	for (int pass = 0; pass < 2; pass++) {
		// Identical for pause and stop: the user who just halted an agent is exactly the
		// user who needs to see what it changed.
		if (pass == 0) {
			MCPAgentControl::pause();
		} else {
			MCPAgentControl::stop();
		}
		String reason;
		CHECK(MCPAgentControl::may_run("Godot_GetActivity", false, reason));
		CHECK(MCPAgentControl::may_run("Godot_ListCheckpoints", false, reason));
		CHECK(MCPAgentControl::may_run("Godot_DiffCheckpoint", false, reason));
		CHECK(MCPAgentControl::may_run("Godot_GetEditorStatus", false, reason));
		CHECK(MCPAgentControl::may_run("Godot_ListInstances", false, reason));
		// But not a write, and not a fresh read of the project.
		CHECK_FALSE(MCPAgentControl::may_run("Godot_WriteTextFile", true, reason));
		CHECK_FALSE(MCPAgentControl::may_run("Godot_SearchProject", false, reason));
	}
}

TEST_CASE("[godot_ai] Stop carries its reason and needs an explicit resume") {
	AgentStateFixture fixture;
	MCPAgentControl::stop("the user stopped it mid-refactor");

	CHECK(MCPAgentControl::get_state() == MCPAgentControl::STATE_STOPPED);
	CHECK(MCPAgentControl::get_reason() == "the user stopped it mid-refactor");

	String reason;
	CHECK_FALSE(MCPAgentControl::may_run("Godot_ManageNode", true, reason));
	CHECK(reason.contains("mid-refactor"));

	const Dictionary state = MCPAgentControl::to_dictionary();
	CHECK(String(state["state"]) == "stopped");
	CHECK(String(state["reason"]).contains("mid-refactor"));

	MCPAgentControl::resume();
	CHECK(MCPAgentControl::get_state() == MCPAgentControl::STATE_RUNNING);
	CHECK(MCPAgentControl::get_reason().is_empty());
}

TEST_CASE("[godot_ai] A refused call still appears in the activity stream") {
	// The trail has to show attempts, not only successes - otherwise a user who paused an
	// agent sees nothing and cannot tell whether it is still trying.
	AgentStateFixture fixture;
	MCPActivity::clear();

	String reason;
	MCPAgentControl::pause();
	MCPAgentControl::may_run("Godot_ManageNode", true, reason);
	MCPActivity::refuse("c", "Godot_ManageNode", MCP_CAP_EDIT_SCENE, "s", Array(), reason);

	const Array records = MCPActivity::snapshot();
	REQUIRE(records.size() == 1);
	const Dictionary record = records[0];
	CHECK(String(record["outcome"]) == "refused");
	CHECK(String(record["detail"]).contains("paused"));

	MCPActivity::clear();
}

} // namespace TestMCPAgentState

#endif // TEST_MCP_AGENT_STATE_H
