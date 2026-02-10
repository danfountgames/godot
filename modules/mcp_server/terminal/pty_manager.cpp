/**************************************************************************/
/*  pty_manager.cpp                                                       */
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

#include "pty_manager.h"

#ifdef MCP_TERMINAL_ENABLED

#include "core/os/os.h"
#include "core/string/ustring.h"

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

PTYManager::PTYManager() {
}

PTYManager::~PTYManager() {
	close_pty();
}

bool PTYManager::fork_and_exec(const String &p_command, const Vector<String> &p_args, const Vector<String> &p_env) {
	if (master_fd >= 0) {
		// Already running; close the previous session first.
		close_pty();
	}

	// Set up initial window size.
	struct winsize ws;
	memset(&ws, 0, sizeof(ws));
	ws.ws_row = rows;
	ws.ws_col = cols;

	pid_t pid = forkpty(&master_fd, nullptr, nullptr, &ws);

	if (pid < 0) {
		// forkpty() failed.
		master_fd = -1;
		return false;
	}

	if (pid == 0) {
		// ---- Child process ----

		// Build argv array: command + args.
		CharString cmd_utf8 = p_command.utf8();

		// We need persistent storage for the arg strings.
		Vector<CharString> args_utf8;
		args_utf8.resize(p_args.size());
		for (int i = 0; i < p_args.size(); i++) {
			args_utf8.write[i] = p_args[i].utf8();
		}

		// argv[0] = command, argv[1..n] = args, argv[n+1] = nullptr.
		Vector<char *> argv;
		argv.resize(1 + p_args.size() + 1);
		argv.write[0] = const_cast<char *>(cmd_utf8.get_data());
		for (int i = 0; i < p_args.size(); i++) {
			argv.write[1 + i] = const_cast<char *>(args_utf8[i].get_data());
		}
		argv.write[1 + p_args.size()] = nullptr;

		// Build environment: start with provided env vars.
		Vector<CharString> env_utf8;
		env_utf8.resize(p_env.size());
		for (int i = 0; i < p_env.size(); i++) {
			env_utf8.write[i] = p_env[i].utf8();
		}

		// Ensure TERM is set.
		bool has_term = false;
		for (int i = 0; i < p_env.size(); i++) {
			if (p_env[i].begins_with("TERM=")) {
				has_term = true;
				break;
			}
		}

		CharString term_env;
		if (!has_term) {
			term_env = String("TERM=xterm-256color").utf8();
		}

		// Build envp array for environ.
		Vector<char *> envp;
		envp.resize(p_env.size() + (has_term ? 0 : 1) + 1);
		int env_idx = 0;
		for (int i = 0; i < p_env.size(); i++) {
			envp.write[env_idx++] = const_cast<char *>(env_utf8[i].get_data());
		}
		if (!has_term) {
			envp.write[env_idx++] = const_cast<char *>(term_env.get_data());
		}
		envp.write[env_idx] = nullptr;

		// Set environ manually (macOS doesn't have execvpe).
		environ = envp.ptrw();

		execvp(cmd_utf8.get_data(), argv.ptrw());

		// If execvp returns, it failed.
		_exit(1);
	}

	// ---- Parent process ----
	child_pid = pid;

	// Set master_fd to non-blocking.
	int flags = fcntl(master_fd, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
	}

	return true;
}

int PTYManager::read_pty(uint8_t *p_buffer, int p_max_len) {
	if (master_fd < 0) {
		return -1;
	}

	ssize_t n = ::read(master_fd, p_buffer, p_max_len);

	if (n > 0) {
		return (int)n;
	}

	if (n == 0) {
		// EOF -- the slave side was closed.
		return -1;
	}

	// n < 0: check errno.
	if (errno == EAGAIN || errno == EWOULDBLOCK) {
		return 0;
	}

	if (errno == EINTR) {
		return 0;
	}

	// Actual error (e.g. EIO when child exits).
	return -1;
}

bool PTYManager::write_pty(const uint8_t *p_data, int p_len) {
	if (master_fd < 0) {
		return false;
	}

	int total_written = 0;
	while (total_written < p_len) {
		ssize_t n = ::write(master_fd, p_data + total_written, p_len - total_written);

		if (n > 0) {
			total_written += (int)n;
			continue;
		}

		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
				// Retry.
				continue;
			}
			// Actual write error.
			return false;
		}
	}

	return true;
}

void PTYManager::resize(int p_rows, int p_cols) {
	rows = p_rows;
	cols = p_cols;

	if (master_fd < 0) {
		return;
	}

	struct winsize ws;
	memset(&ws, 0, sizeof(ws));
	ws.ws_row = p_rows;
	ws.ws_col = p_cols;

	ioctl(master_fd, TIOCSWINSZ, &ws);
}

bool PTYManager::is_running() const {
	if (child_pid <= 0) {
		return false;
	}

	int status = 0;
	pid_t result = waitpid(child_pid, &status, WNOHANG);

	if (result == 0) {
		// Child is still running.
		return true;
	}

	// Child has exited or an error occurred.
	return false;
}

int PTYManager::get_exit_code() {
	if (child_pid <= 0) {
		return -1;
	}

	int status = 0;
	pid_t result = waitpid(child_pid, &status, WNOHANG);

	if (result == 0) {
		// Still running.
		return -1;
	}

	if (result < 0) {
		// Error or already reaped.
		return -1;
	}

	if (WIFEXITED(status)) {
		return WEXITSTATUS(status);
	}

	if (WIFSIGNALED(status)) {
		return 128 + WTERMSIG(status);
	}

	return -1;
}

void PTYManager::kill_child() {
	if (child_pid <= 0) {
		return;
	}

	// Try SIGTERM first.
	::kill(child_pid, SIGTERM);

	// Wait briefly for the child to exit.
	usleep(100000); // 100ms

	int status = 0;
	pid_t result = waitpid(child_pid, &status, WNOHANG);

	if (result == 0) {
		// Child still alive after SIGTERM; escalate to SIGKILL.
		::kill(child_pid, SIGKILL);
		waitpid(child_pid, &status, 0);
	}

	child_pid = -1;
}

void PTYManager::close_pty() {
	if (child_pid > 0) {
		kill_child();
	}

	if (master_fd >= 0) {
		::close(master_fd);
		master_fd = -1;
	}
}

#endif // MCP_TERMINAL_ENABLED
