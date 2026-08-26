/**************************************************************************/
/*  mcp_pty.cpp                                                           */
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

#include "mcp_pty.h"

#include "core/os/os.h"

#if defined(UNIX_ENABLED) && !defined(WEB_ENABLED)
#define MCP_PTY_POSIX
#endif

#ifdef MCP_PTY_POSIX

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <util.h>
#else
#include <pty.h>
#endif

#endif // MCP_PTY_POSIX

bool MCPPty::is_supported() {
#ifdef MCP_PTY_POSIX
	return true;
#else
	return false;
#endif
}

#ifndef MCP_PTY_POSIX

// Everything below has a POSIX body and a stub. The stubs refuse rather than pretend, so
// a caller on an unsupported platform gets a message instead of a terminal that never
// prints anything.

bool MCPPty::start(const String &p_command, const Vector<String> &p_args,
		const Vector<String> &p_env, const String &p_working_dir, String &r_error) {
	r_error = "a pseudo-terminal is not available on this platform";
	return false;
}

bool MCPPty::poll() { return false; }
bool MCPPty::_reap_if_exited() { return false; }
int MCPPty::read(uint8_t *p_buffer, int p_max_length) { return -1; }
int MCPPty::write(const uint8_t *p_data, int p_length) { return -1; }
void MCPPty::resize(int p_rows, int p_columns) {}
void MCPPty::request_stop(int p_grace_msec) {}
void MCPPty::close() {}
MCPPty::~MCPPty() {}

#else

bool MCPPty::start(const String &p_command, const Vector<String> &p_args,
		const Vector<String> &p_env, const String &p_working_dir, String &r_error) {
	if (master_fd >= 0 || child_pid > 0) {
		// Reusing a live object would strand the previous child. Close first, so the
		// caller cannot leak one by accident.
		close();
	}
	exited = false;
	exit_code = -1;
	stop_phase = STOP_NONE;
	stop_deadline_msec = 0;

	struct winsize size;
	memset(&size, 0, sizeof(size));
	size.ws_row = rows;
	size.ws_col = columns;

	// Everything the child needs must be built *before* the fork. After it, only
	// async-signal-safe work is legal, and Godot's String/Vector allocate.
	const CharString command_utf8 = p_command.utf8();

	Vector<CharString> argument_storage;
	argument_storage.resize(p_args.size());
	for (int i = 0; i < p_args.size(); i++) {
		argument_storage.write[i] = p_args[i].utf8();
	}
	Vector<char *> argv;
	argv.resize(p_args.size() + 2);
	argv.write[0] = const_cast<char *>(command_utf8.get_data());
	for (int i = 0; i < p_args.size(); i++) {
		argv.write[i + 1] = const_cast<char *>(argument_storage[i].get_data());
	}
	argv.write[p_args.size() + 1] = nullptr;

	bool has_term = false;
	for (int i = 0; i < p_env.size(); i++) {
		if (p_env[i].begins_with("TERM=")) {
			has_term = true;
			break;
		}
	}
	Vector<CharString> environment_storage;
	environment_storage.resize(p_env.size() + (has_term ? 0 : 1));
	for (int i = 0; i < p_env.size(); i++) {
		environment_storage.write[i] = p_env[i].utf8();
	}
	if (!has_term) {
		// Without TERM the child assumes a dumb terminal and emits no escape sequences,
		// which makes the emulator look broken rather than empty.
		environment_storage.write[p_env.size()] = String("TERM=xterm-256color").utf8();
	}
	Vector<char *> envp;
	envp.resize(environment_storage.size() + 1);
	for (int i = 0; i < environment_storage.size(); i++) {
		envp.write[i] = const_cast<char *>(environment_storage[i].get_data());
	}
	envp.write[environment_storage.size()] = nullptr;

	const CharString working_dir_utf8 = p_working_dir.utf8();

	int fd = -1;
	const pid_t pid = forkpty(&fd, nullptr, nullptr, &size);
	if (pid < 0) {
		master_fd = -1;
		r_error = vformat("could not open a pseudo-terminal: %s", strerror(errno));
		return false;
	}

	if (pid == 0) {
		// ---- child ----
		// Only async-signal-safe calls from here. Any failure ends in _exit(): returning
		// would leave a second copy of the editor running its own main loop.

		// Give the child a clean signal state before exec.
		//
		// A forked child inherits the parent's signal mask, and dispositions set to
		// SIG_IGN survive exec. The editor blocks and ignores signals for its own
		// reasons, so without this the shell starts with SIGTERM *already ignored* - and
		// POSIX says a shell cannot even install a trap for a signal it inherited as
		// ignored. The symptom is a terminal that shrugs off every polite stop and can
		// only be killed, which looks like a bug in the stop logic and is not. Measured:
		// with this block absent, `trap 'exit 42' TERM` never fires and the child always
		// ends on SIGKILL.
		sigset_t no_signals_blocked;
		sigemptyset(&no_signals_blocked);
		sigprocmask(SIG_SETMASK, &no_signals_blocked, nullptr);
		for (int signal_number = 1; signal_number < NSIG; signal_number++) {
			// SIGKILL and SIGSTOP cannot be reset; the failures are harmless.
			signal(signal_number, SIG_DFL);
		}

		if (!p_working_dir.is_empty()) {
			if (chdir(working_dir_utf8.get_data()) != 0) {
				_exit(127);
			}
		}
		execvpe(command_utf8.get_data(), argv.ptrw(), envp.ptrw());
		_exit(127);
	}

	// ---- parent ----
	master_fd = fd;
	child_pid = pid;

	// Non-blocking, so a poll on a quiet terminal costs one EAGAIN rather than stalling
	// the editor's frame.
	const int flags = fcntl(master_fd, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
	}
	return true;
}

bool MCPPty::_reap_if_exited() {
	if (child_pid <= 0) {
		return false;
	}
	int status = 0;
	const pid_t result = waitpid(child_pid, &status, WNOHANG);
	if (result == 0) {
		return false; // Still running.
	}

	// Reaped, or gone in a way we can no longer wait on. Either way the pid must stop
	// being addressable *now*: the moment a pid is reaped the kernel may reuse it, and a
	// later signal would land on whatever process inherited the number. This single
	// assignment is the fix for the original's worst bug.
	child_pid = -1;
	exited = true;
	stop_phase = STOP_NONE;

	if (result > 0) {
		if (WIFEXITED(status)) {
			exit_code = WEXITSTATUS(status);
		} else if (WIFSIGNALED(status)) {
			// Report a signal the way a shell does, so "137" reads as SIGKILL.
			exit_code = 128 + WTERMSIG(status);
		} else {
			exit_code = -1;
		}
	} else {
		// ECHILD: something else reaped it. The exit status is unknowable, and saying so
		// beats inventing a zero.
		exit_code = -1;
	}
	return true;
}

bool MCPPty::poll() {
	if (child_pid <= 0) {
		return false;
	}
	if (_reap_if_exited()) {
		return false;
	}

	if (stop_phase == STOP_TERM_SENT && OS::get_singleton()->get_ticks_msec() >= stop_deadline_msec) {
		// The grace period is over. Escalating here rather than sleeping between the two
		// signals is what keeps the editor's frame free; the original blocked 100ms in
		// usleep() for every terminal it closed.
		::kill((pid_t)child_pid, SIGKILL);
		stop_phase = STOP_KILL_SENT;
	}
	return child_pid > 0;
}

int MCPPty::read(uint8_t *p_buffer, int p_max_length) {
	if (master_fd < 0 || !p_buffer || p_max_length <= 0) {
		return -1;
	}
	const ssize_t count = ::read(master_fd, p_buffer, (size_t)p_max_length);
	if (count > 0) {
		return (int)count;
	}
	if (count == 0) {
		// The child closed its end.
		return -1;
	}
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
		return 0;
	}
	// EIO is the ordinary way a pty reports that the child has gone.
	return -1;
}

int MCPPty::write(const uint8_t *p_data, int p_length) {
	if (master_fd < 0 || !p_data || p_length <= 0) {
		return -1;
	}
	const ssize_t count = ::write(master_fd, p_data, (size_t)p_length);
	if (count >= 0) {
		return (int)count;
	}
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
		return 0;
	}
	return -1;
}

void MCPPty::resize(int p_rows, int p_columns) {
	rows = MAX(1, p_rows);
	columns = MAX(1, p_columns);
	if (master_fd < 0) {
		return;
	}
	struct winsize size;
	memset(&size, 0, sizeof(size));
	size.ws_row = (unsigned short)rows;
	size.ws_col = (unsigned short)columns;
	ioctl(master_fd, TIOCSWINSZ, &size);
}

void MCPPty::request_stop(int p_grace_msec) {
	if (child_pid <= 0 || stop_phase != STOP_NONE) {
		return;
	}
	::kill((pid_t)child_pid, SIGTERM);
	stop_phase = STOP_TERM_SENT;
	stop_deadline_msec = OS::get_singleton()->get_ticks_msec() + (uint64_t)MAX(0, p_grace_msec);
}

void MCPPty::close() {
	if (child_pid > 0) {
		// Do not wait politely here: close() runs from teardown, and a child that
		// outlives the editor is worse than a signal it did not expect. request_stop()
		// is the polite path and the caller had their chance to use it.
		if (stop_phase != STOP_KILL_SENT) {
			::kill((pid_t)child_pid, SIGKILL);
			stop_phase = STOP_KILL_SENT;
		}
		// The one blocking wait in this class, and the one place it is right: leaking a
		// process is worse than a brief stall, and the child has already had SIGKILL.
		int status = 0;
		waitpid((pid_t)child_pid, &status, 0);
		child_pid = -1;
		exited = true;
		if (exit_code < 0 && WIFSIGNALED(status)) {
			exit_code = 128 + WTERMSIG(status);
		}
	}
	if (master_fd >= 0) {
		::close(master_fd);
		master_fd = -1;
	}
	stop_phase = STOP_NONE;
}

MCPPty::~MCPPty() {
	close();
}

#endif // MCP_PTY_POSIX
