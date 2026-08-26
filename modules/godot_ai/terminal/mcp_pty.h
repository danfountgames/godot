/**************************************************************************/
/*  mcp_pty.h                                                             */
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

#ifndef MCP_PTY_H
#define MCP_PTY_H

#include "core/string/ustring.h"
#include "core/templates/vector.h"

// A pseudo-terminal child process, owned safely.
//
// Ported from the `GodotBeamDev` branch's `PTYManager`, which worked but was
// crash-prone. The rewrite exists for one reason, and it is worth stating plainly
// because it is the whole point of this class:
//
//   **The original reaped the child inside a `const` query and then signalled the same
//   pid afterwards.**
//
//   `is_running() const` called `waitpid(child_pid, ..., WNOHANG)` every frame. When the
//   child had exited, that call *reaped* it - but because the method was const it could
//   not clear `child_pid`. The pid was now free for the operating system to hand to
//   something else. A later `kill_child()` then sent SIGTERM and SIGKILL to
//   `child_pid`, which by then could be an unrelated process on the machine.
//
// Everything below follows from refusing to let that happen again:
//
//   * Reaping happens in `poll()`, which is not const, and `child_pid` is cleared in the
//     same breath. After a reap there is no pid to signal, so a stale signal is not a
//     bug that has to be avoided - it is unrepresentable.
//   * `is_running()` reads a flag. It performs no syscall and cannot change state.
//   * Termination is asynchronous. The original blocked the editor's main thread for
//     100ms in `usleep()` between SIGTERM and SIGKILL, per terminal. `request_stop()`
//     signals and returns; `poll()` escalates once the grace period is up.
//   * `close()` is idempotent and safe to call twice. The original's `stop_process()`
//     called `kill_child()` and then `close_pty()`, which called `kill_child()` again.
//
// POSIX only: this is `forkpty`. Windows needs ConPTY and is a separate implementation,
// so the whole class is compiled out elsewhere and callers must check
// `MCPPty::is_supported()`.
class MCPPty {
public:
	// True where a pty can actually be opened. False on platforms compiled without it,
	// so a caller can say so rather than failing at launch.
	static bool is_supported();

	enum StopPhase {
		STOP_NONE, // Not stopping.
		STOP_TERM_SENT, // SIGTERM sent, inside the grace period.
		STOP_KILL_SENT, // Grace expired, SIGKILL sent.
	};

	// Milliseconds a child gets to exit after SIGTERM before SIGKILL. Long enough for a
	// shell to write its scrollback, short enough not to feel stuck.
	static const int DEFAULT_GRACE_MSEC = 250;

	// Launches `p_command` under a new pty. `p_env` entries are "NAME=value". Returns
	// false and fills `r_error` on failure; the object is left closed and reusable.
	bool start(const String &p_command, const Vector<String> &p_args, const Vector<String> &p_env,
			const String &p_working_dir, String &r_error);

	// Advances state: reaps an exited child, escalates a pending stop. Call every frame.
	// Returns true when the child is still running afterwards.
	bool poll();

	// A flag read, not a syscall. Accurate as of the last poll().
	bool is_running() const { return child_pid > 0; }
	bool has_exited() const { return exited; }

	// Exit status once the child has been reaped, or -1 while it is still running.
	int get_exit_code() const { return exited ? exit_code : -1; }

	// Non-blocking. Returns bytes read, 0 when there is nothing to read, -1 when the pty
	// has closed.
	int read(uint8_t *p_buffer, int p_max_length);

	// Returns bytes written, or -1. A short write is not an error; the caller retries.
	int write(const uint8_t *p_data, int p_length);

	void resize(int p_rows, int p_cols);
	int get_rows() const { return rows; }
	int get_columns() const { return columns; }

	// Asks the child to exit. Returns immediately - `poll()` escalates to SIGKILL after
	// the grace period. Safe to call when nothing is running.
	void request_stop(int p_grace_msec = DEFAULT_GRACE_MSEC);

	StopPhase get_stop_phase() const { return stop_phase; }

	// Closes the pty and makes sure the child is gone. Blocks only as long as the child
	// takes to die after SIGKILL, which is the one place blocking is right: this is
	// called from a destructor, and leaking a child process is worse than a brief stall.
	// Idempotent.
	void close();

	int get_master_fd() const { return master_fd; }

	MCPPty() {}
	~MCPPty();

	MCPPty(const MCPPty &) = delete;
	MCPPty &operator=(const MCPPty &) = delete;

private:
	int master_fd = -1;
	// Zero or negative means "no child to signal". Cleared the instant one is reaped,
	// which is what makes signalling a recycled pid impossible.
	int child_pid = -1;
	int rows = 24;
	int columns = 80;

	bool exited = false;
	int exit_code = -1;

	StopPhase stop_phase = STOP_NONE;
	uint64_t stop_deadline_msec = 0;

	// Reaps if the child has exited. Clears child_pid and records the status. Returns
	// true when the child was reaped by this call.
	bool _reap_if_exited();
};

#endif // MCP_PTY_H
