/**************************************************************************/
/*  test_mcp_runtime_instances.h                                          */
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

#ifndef TEST_MCP_RUNTIME_INSTANCES_H
#define TEST_MCP_RUNTIME_INSTANCES_H

#include "modules/godot_ai/mcp_runtime_instances.h"

#include "tests/test_macros.h"

namespace TestMCPRuntimeInstances {

struct InstancesFixture {
	InstancesFixture() { MCPRuntimeInstances::clear(); }
	~InstancesFixture() { MCPRuntimeInstances::clear(); }
};

TEST_CASE("[godot_ai] A runtime instance is registered before it has a process") {
	InstancesFixture fixture;

	const String id = MCPRuntimeInstances::create("Jump Variant A", "candidate", "compare jumps",
			MCPRuntimeInstances::RETENTION_INTERACTIVE);
	CHECK_FALSE(id.is_empty());
	CHECK(MCPRuntimeInstances::exists(id));

	MCPRuntimeInstances::Instance instance;
	REQUIRE(MCPRuntimeInstances::get(id, instance));
	CHECK(instance.label == "Jump Variant A");
	CHECK(instance.role == "candidate");
	CHECK(instance.lifecycle == MCPRuntimeInstances::LIFECYCLE_QUEUED);
	// An instance is the agent's until a human takes it, never the other way round.
	CHECK(instance.control == MCPRuntimeInstances::CONTROL_AGENT);
	CHECK(instance.pid == 0);
	// Registered but not yet launched is not live.
	CHECK(MCPRuntimeInstances::live().is_empty());
}

TEST_CASE("[godot_ai] Binding a process makes an instance findable by pid") {
	InstancesFixture fixture;

	const String id = MCPRuntimeInstances::create("A", "candidate", "t",
			MCPRuntimeInstances::RETENTION_EPHEMERAL);
	REQUIRE(MCPRuntimeInstances::bind_pid(id, 4242));
	REQUIRE(MCPRuntimeInstances::set_lifecycle(id, MCPRuntimeInstances::LIFECYCLE_RUNNING));

	CHECK(MCPRuntimeInstances::find_by_pid(4242) == id);
	CHECK(MCPRuntimeInstances::is_agent_owned(4242));
	CHECK(MCPRuntimeInstances::live().size() == 1);

	// The human's own run is not in the registry, so it must not look agent-owned. This
	// is the check that keeps a "stop all agent instances" action off their game.
	CHECK_FALSE(MCPRuntimeInstances::is_agent_owned(9999));
	CHECK(MCPRuntimeInstances::find_by_pid(9999).is_empty());
}

TEST_CASE("[godot_ai] One process cannot be claimed by two instances") {
	InstancesFixture fixture;

	const String first = MCPRuntimeInstances::create("A", "candidate", "t",
			MCPRuntimeInstances::RETENTION_EPHEMERAL);
	const String second = MCPRuntimeInstances::create("B", "candidate", "t",
			MCPRuntimeInstances::RETENTION_EPHEMERAL);

	REQUIRE(MCPRuntimeInstances::bind_pid(first, 4242));
	// Two registrations on one pid would make "stop that one" ambiguous, and the wrong
	// game would die.
	CHECK_FALSE(MCPRuntimeInstances::bind_pid(second, 4242));
	CHECK(MCPRuntimeInstances::find_by_pid(4242) == first);

	// Re-binding the same pid to the instance that already owns it is not a conflict.
	CHECK(MCPRuntimeInstances::bind_pid(first, 4242));
}

TEST_CASE("[godot_ai] A closed instance releases its pid but keeps its record") {
	InstancesFixture fixture;

	const String id = MCPRuntimeInstances::create("A", "candidate", "t",
			MCPRuntimeInstances::RETENTION_EPHEMERAL);
	REQUIRE(MCPRuntimeInstances::bind_pid(id, 4242));
	REQUIRE(MCPRuntimeInstances::set_lifecycle(id, MCPRuntimeInstances::LIFECYCLE_CLOSED, "test finished"));

	MCPRuntimeInstances::Instance instance;
	REQUIRE(MCPRuntimeInstances::get(id, instance));
	CHECK(instance.lifecycle == MCPRuntimeInstances::LIFECYCLE_CLOSED);
	CHECK(instance.detail == "test finished");
	// The record survives so evidence can still be attributed to it...
	CHECK(MCPRuntimeInstances::list().size() == 1);
	CHECK(MCPRuntimeInstances::live().is_empty());
	// ...but the pid is released, or a later lookup could match a pid the operating
	// system has since given to something else.
	CHECK(instance.pid == 0);
	CHECK(MCPRuntimeInstances::find_by_pid(4242).is_empty());
}

TEST_CASE("[godot_ai] A failed instance is not live and records why") {
	InstancesFixture fixture;

	const String id = MCPRuntimeInstances::create("A", "candidate", "t",
			MCPRuntimeInstances::RETENTION_EPHEMERAL);
	REQUIRE(MCPRuntimeInstances::bind_pid(id, 77));
	REQUIRE(MCPRuntimeInstances::set_lifecycle(id, MCPRuntimeInstances::LIFECYCLE_FAILED,
			"the game crashed on startup"));

	CHECK(MCPRuntimeInstances::live().is_empty());
	MCPRuntimeInstances::Instance instance;
	REQUIRE(MCPRuntimeInstances::get(id, instance));
	CHECK(instance.detail.contains("crashed"));
}

TEST_CASE("[godot_ai] Control ownership is explicit and switchable") {
	InstancesFixture fixture;

	const String id = MCPRuntimeInstances::create("A", "candidate", "t",
			MCPRuntimeInstances::RETENTION_INTERACTIVE);
	REQUIRE(MCPRuntimeInstances::set_control(id, MCPRuntimeInstances::CONTROL_HUMAN));

	MCPRuntimeInstances::Instance instance;
	REQUIRE(MCPRuntimeInstances::get(id, instance));
	CHECK(instance.control == MCPRuntimeInstances::CONTROL_HUMAN);
	CHECK(MCPRuntimeInstances::control_to_string(instance.control) == "human");

	REQUIRE(MCPRuntimeInstances::set_control(id, MCPRuntimeInstances::CONTROL_AGENT));
	REQUIRE(MCPRuntimeInstances::get(id, instance));
	CHECK(instance.control == MCPRuntimeInstances::CONTROL_AGENT);
}

TEST_CASE("[godot_ai] Instances can be grouped, as a multiplayer test needs") {
	InstancesFixture fixture;

	const String server = MCPRuntimeInstances::create("Server", "server", "lobby test",
			MCPRuntimeInstances::RETENTION_PERSISTENT);
	const String client = MCPRuntimeInstances::create("Client 1", "client", "lobby test",
			MCPRuntimeInstances::RETENTION_PERSISTENT);
	REQUIRE(MCPRuntimeInstances::set_group(server, "lobby"));
	REQUIRE(MCPRuntimeInstances::set_group(client, "lobby"));

	int in_group = 0;
	for (const MCPRuntimeInstances::Instance &instance : MCPRuntimeInstances::list()) {
		if (instance.group == "lobby") {
			in_group++;
		}
	}
	CHECK(in_group == 2);
}

TEST_CASE("[godot_ai] Routing refuses rather than guessing when there is no editor") {
	InstancesFixture fixture;

	const String id = MCPRuntimeInstances::create("A", "candidate", "t",
			MCPRuntimeInstances::RETENTION_EPHEMERAL);
	REQUIRE(MCPRuntimeInstances::bind_pid(id, 4242));

	// No EditorDebuggerNode in a headless test. Every targeted control must say it could
	// not deliver rather than silently broadcasting or pretending it worked - the whole
	// point of this layer is that one instance is addressed and no other is touched.
	CHECK(MCPRuntimeInstances::debugger_for(id) == nullptr);
	CHECK_FALSE(MCPRuntimeInstances::set_suspended(id, true));
	CHECK_FALSE(MCPRuntimeInstances::next_frame(id));
	CHECK_FALSE(MCPRuntimeInstances::set_time_scale(id, 4.0));
	CHECK_FALSE(MCPRuntimeInstances::set_muted(id, true));
	CHECK_FALSE(MCPRuntimeInstances::send_to(id, "scene:next_frame", Array()));

	// An unknown instance is refused too, not treated as "send to whatever is there".
	CHECK_FALSE(MCPRuntimeInstances::set_suspended("inst-does-not-exist", true));
}

TEST_CASE("[godot_ai] Instance ids are unique and ordered") {
	InstancesFixture fixture;

	const String first = MCPRuntimeInstances::create("A", "r", "t",
			MCPRuntimeInstances::RETENTION_EPHEMERAL);
	const String second = MCPRuntimeInstances::create("B", "r", "t",
			MCPRuntimeInstances::RETENTION_EPHEMERAL);
	CHECK(first != second);
	// clear() does not reset the counter: a reader comparing ids from before and after
	// a reset must not see the same id mean two different instances.
	MCPRuntimeInstances::clear();
	const String third = MCPRuntimeInstances::create("C", "r", "t",
			MCPRuntimeInstances::RETENTION_EPHEMERAL);
	CHECK(third != first);
	CHECK(third != second);
}

TEST_CASE("[godot_ai] Instances serialise for the dock without losing state") {
	InstancesFixture fixture;

	const String id = MCPRuntimeInstances::create("Jump Variant B", "candidate", "compare jumps",
			MCPRuntimeInstances::RETENTION_INTERACTIVE);
	REQUIRE(MCPRuntimeInstances::bind_pid(id, 1234));
	REQUIRE(MCPRuntimeInstances::set_lifecycle(id, MCPRuntimeInstances::LIFECYCLE_RUNNING));
	REQUIRE(MCPRuntimeInstances::set_control(id, MCPRuntimeInstances::CONTROL_HUMAN));

	const Array serialised = MCPRuntimeInstances::to_array();
	REQUIRE(serialised.size() == 1);
	const Dictionary entry = serialised[0];
	CHECK(String(entry["instance_id"]) == id);
	CHECK(String(entry["label"]) == "Jump Variant B");
	CHECK(String(entry["lifecycle"]) == "running");
	CHECK(String(entry["control"]) == "human");
	CHECK(String(entry["retention"]) == "interactive");
	CHECK((int64_t)entry["pid"] == 1234);
}

} // namespace TestMCPRuntimeInstances

#endif // TEST_MCP_RUNTIME_INSTANCES_H
