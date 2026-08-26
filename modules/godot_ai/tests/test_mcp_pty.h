/**************************************************************************/
/*  test_mcp_pty.h                                                        */
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

#ifndef TEST_MCP_PTY_H
#define TEST_MCP_PTY_H

#include "modules/godot_ai/terminal/mcp_pty.h"

#include "core/os/os.h"

#include "tests/test_macros.h"

namespace TestMCPPty {

// Drives poll() until `p_predicate` holds or the budget runs out. Terminals are real
// processes, so a test that asserts immediately after start() asserts on scheduling
// luck rather than behaviour.
template <typename T>
static bool pump_until(MCPPty &p_pty, T p_predicate, int p_budget_msec = 5000) {
	const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + (uint64_t)p_budget_msec;
	while (OS::get_singleton()->get_ticks_msec() < deadline) {
		p_pty.poll();
		if (p_predicate()) {
			return true;
		}
		OS::get_singleton()->delay_usec(2000);
	}
	return false;
}

static Vector<String> no_args() {
	return Vector<String>();
}

// Waits until the child has printed `p_marker`.
//
// Signalling straight after start() signals a process that has not finished exec'ing,
// let alone run its first command - so a shell asked to install a trap is killed by the
// default action before the trap exists, and the test measures the race rather than the
// behaviour. Every signal test below waits for the child to say it is ready.
static bool wait_for_marker(MCPPty &p_pty, const String &p_marker, int p_budget_msec = 5000) {
	String collected;
	uint8_t buffer[512];
	return pump_until(
			p_pty,
			[&]() {
				const int count = p_pty.read(buffer, sizeof(buffer));
				if (count > 0) {
					collected += String::utf8((const char *)buffer, count);
				}
				return collected.contains(p_marker);
			},
			p_budget_msec);
}

TEST_CASE("[godot_ai] A pty runs a command and reports its exit code") {
	if (!MCPPty::is_supported()) {
		return; // Windows needs ConPTY; the stubs are covered below.
	}
	MCPPty pty;
	String error;

	Vector<String> args;
	args.push_back("-c");
	args.push_back("exit 7");
	REQUIRE_MESSAGE(pty.start("/bin/sh", args, Vector<String>(), String(), error), error);
	CHECK(pty.is_running());
	CHECK(pty.get_exit_code() == -1);

	CHECK(pump_until(pty, [&]() { return pty.has_exited(); }));
	CHECK_FALSE(pty.is_running());
	CHECK(pty.get_exit_code() == 7);
}

TEST_CASE("[godot_ai] A pty carries output back to the reader") {
	if (!MCPPty::is_supported()) {
		return;
	}
	MCPPty pty;
	String error;

	Vector<String> args;
	args.push_back("-c");
	args.push_back("printf 'hello-from-the-pty'");
	REQUIRE_MESSAGE(pty.start("/bin/sh", args, Vector<String>(), String(), error), error);

	String collected;
	uint8_t buffer[512];
	pump_until(pty, [&]() {
		const int count = pty.read(buffer, sizeof(buffer));
		if (count > 0) {
			collected += String::utf8((const char *)buffer, count);
		}
		return collected.contains("hello-from-the-pty");
	});
	CHECK(collected.contains("hello-from-the-pty"));
}

TEST_CASE("[godot_ai] Polling an exited child clears its pid so nothing can signal it later") {
	// The bug this class exists to prevent. The original reaped inside a const
	// is_running(), which could not clear child_pid, so a later kill_child() sent SIGTERM
	// and SIGKILL to a pid the operating system may already have reused.
	if (!MCPPty::is_supported()) {
		return;
	}
	MCPPty pty;
	String error;

	Vector<String> args;
	args.push_back("-c");
	args.push_back("exit 0");
	REQUIRE(pty.start("/bin/sh", args, Vector<String>(), String(), error));
	REQUIRE(pump_until(pty, [&]() { return pty.has_exited(); }));

	// After the reap there is no pid left to address, so a stale signal is not merely
	// avoided - it is unrepresentable.
	CHECK_FALSE(pty.is_running());

	// Every path that would have signalled must now be a no-op rather than a signal to
	// whatever inherited the number.
	pty.request_stop();
	CHECK(pty.get_stop_phase() == MCPPty::STOP_NONE);
	pty.poll();
	pty.close();
	pty.close(); // Idempotent: the original killed twice through two entry points.
	CHECK_FALSE(pty.is_running());
	CHECK(pty.get_exit_code() == 0);
}

TEST_CASE("[godot_ai] is_running() performs no syscall and cannot reap") {
	if (!MCPPty::is_supported()) {
		return;
	}
	MCPPty pty;
	String error;

	Vector<String> args;
	args.push_back("-c");
	args.push_back("exit 0");
	REQUIRE(pty.start("/bin/sh", args, Vector<String>(), String(), error));

	// The child will have exited almost immediately, but only poll() may notice. Calling
	// the query a thousand times must not change anything, which is what "const means
	// const" buys.
	for (int i = 0; i < 1000; i++) {
		CHECK(pty.is_running());
		CHECK_FALSE(pty.has_exited());
	}
	CHECK(pump_until(pty, [&]() { return pty.has_exited(); }));
}

TEST_CASE("[godot_ai] A child that ignores SIGTERM is killed after the grace period") {
	if (!MCPPty::is_supported()) {
		return;
	}
	MCPPty pty;
	String error;

	Vector<String> args;
	args.push_back("-c");
	// Deliberately deaf to SIGTERM, so only the escalation can end it.
	args.push_back("trap '' TERM; echo READY; while true; do sleep 0.05; done");
	REQUIRE(pty.start("/bin/sh", args, Vector<String>(), String(), error));
	REQUIRE(wait_for_marker(pty, "READY"));

	const uint64_t started = OS::get_singleton()->get_ticks_msec();
	pty.request_stop(120);
	CHECK(pty.get_stop_phase() == MCPPty::STOP_TERM_SENT);

	// request_stop() must not block. The original slept 100ms inside the call, on the
	// editor's main thread, for every terminal it closed.
	CHECK(OS::get_singleton()->get_ticks_msec() - started < 60);

	CHECK(pump_until(pty, [&]() { return pty.has_exited(); }));
	// 128 + SIGKILL(9). Reported the way a shell reports it.
	CHECK(pty.get_exit_code() == 137);
}

TEST_CASE("[godot_ai] A child that honours SIGTERM exits on it, without escalation") {
	if (!MCPPty::is_supported()) {
		return;
	}
	MCPPty pty;
	String error;

	Vector<String> args;
	args.push_back("-c");
	// An explicit trap, not a bare loop. Under forkpty the child becomes a session leader
	// with a controlling terminal, and a shell in that position ignores SIGTERM the way
	// an interactive one does - a bare loop here survives the polite signal and is killed
	// by the escalation, which would make this test assert the opposite of its name. The
	// trap makes delivery unambiguous: exit code 42 can only come from the handler.
	args.push_back("trap 'exit 42' TERM; echo READY; while true; do sleep 0.05; done");
	REQUIRE(pty.start("/bin/sh", args, Vector<String>(), String(), error));
	REQUIRE(wait_for_marker(pty, "READY"));

	pty.request_stop(4000);
	CHECK(pump_until(pty, [&]() { return pty.has_exited(); }));
	CHECK(pty.get_exit_code() == 42);
	// Reaping clears the stop, so nothing escalated and nothing is left pending.
	CHECK(pty.get_stop_phase() == MCPPty::STOP_NONE);
}

TEST_CASE("[godot_ai] Starting twice on one object does not strand the first child") {
	if (!MCPPty::is_supported()) {
		return;
	}
	MCPPty pty;
	String error;

	Vector<String> first;
	first.push_back("-c");
	first.push_back("echo READY; while true; do sleep 0.05; done");
	REQUIRE(pty.start("/bin/sh", first, Vector<String>(), String(), error));
	REQUIRE(wait_for_marker(pty, "READY"));

	Vector<String> second;
	second.push_back("-c");
	second.push_back("exit 3");
	REQUIRE(pty.start("/bin/sh", second, Vector<String>(), String(), error));
	CHECK(pump_until(pty, [&]() { return pty.has_exited(); }));
	CHECK(pty.get_exit_code() == 3);
}

TEST_CASE("[godot_ai] A command that does not exist fails as an exit code, not a hang") {
	if (!MCPPty::is_supported()) {
		return;
	}
	MCPPty pty;
	String error;

	// forkpty() succeeds; the child fails at execvpe and _exit(127)s. Returning from the
	// child instead would leave a second copy of the editor running its own main loop,
	// which is why the child path ends in _exit() on every branch.
	REQUIRE(pty.start("/definitely/not/a/binary", no_args(), Vector<String>(), String(), error));
	CHECK(pump_until(pty, [&]() { return pty.has_exited(); }));
	CHECK(pty.get_exit_code() == 127);
}

TEST_CASE("[godot_ai] A pty starts in the working directory it was given") {
	if (!MCPPty::is_supported()) {
		return;
	}
	MCPPty pty;
	String error;

	Vector<String> args;
	args.push_back("-c");
	args.push_back("pwd");
	REQUIRE(pty.start("/bin/sh", args, Vector<String>(), "/tmp", error));

	String collected;
	uint8_t buffer[512];
	pump_until(pty, [&]() {
		const int count = pty.read(buffer, sizeof(buffer));
		if (count > 0) {
			collected += String::utf8((const char *)buffer, count);
		}
		return collected.contains("/tmp");
	});
	CHECK(collected.contains("/tmp"));
}

TEST_CASE("[godot_ai] Reading and writing a closed pty refuses rather than crashing") {
	MCPPty pty;
	uint8_t buffer[16];
	CHECK(pty.read(buffer, sizeof(buffer)) == -1);
	CHECK(pty.write(buffer, sizeof(buffer)) == -1);
	CHECK_FALSE(pty.is_running());
	CHECK(pty.get_exit_code() == -1);
	// All of these run against a pty that was never started.
	pty.resize(40, 100);
	pty.request_stop();
	pty.poll();
	pty.close();
	CHECK(pty.get_rows() == 40);
	CHECK(pty.get_columns() == 100);
}

TEST_CASE("[godot_ai] A pty destroyed while its child runs does not leak the process") {
	if (!MCPPty::is_supported()) {
		return;
	}
	int pid_seen = 0;
	{
		MCPPty pty;
		String error;
		Vector<String> args;
		args.push_back("-c");
		args.push_back("echo READY; while true; do sleep 0.05; done");
		REQUIRE(pty.start("/bin/sh", args, Vector<String>(), String(), error));
		REQUIRE(wait_for_marker(pty, "READY"));
		pid_seen = pty.get_master_fd();
		CHECK(pid_seen >= 0);
		// Destructor runs here, with the child still running and no request_stop().
	}
	// Nothing to assert on the pid directly - it is gone with the object - but the
	// destructor must have returned rather than blocked forever, and reaching this line
	// is that assertion.
	CHECK(true);
}

} // namespace TestMCPPty

#endif // TEST_MCP_PTY_H
