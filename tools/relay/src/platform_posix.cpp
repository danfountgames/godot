/**************************************************************************/
/*  platform_posix.cpp                                                    */
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

#ifndef _WIN32

#include "platform.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace godot_ai {
namespace platform {

const SocketHandle INVALID_SOCKET_HANDLE = -1;

static volatile sig_atomic_t g_terminate = 0;

static void handle_signal(int) {
	g_terminate = 1;
}

bool initialize(std::string &r_error) {
	(void)r_error;
	// Nothing to start up; the symmetry with the Windows backend is the point.
	return true;
}

void finalize() {}

SocketHandle socket_connect(const std::string &p_host, int p_port, std::string &r_error) {
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		r_error = std::string("socket() failed: ") + strerror(errno);
		return INVALID_SOCKET_HANDLE;
	}

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)p_port);
	if (inet_pton(AF_INET, p_host.c_str(), &address.sin_addr) != 1) {
		close(fd);
		r_error = "invalid editor host address: " + p_host;
		return INVALID_SOCKET_HANDLE;
	}
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		r_error = strerror(errno);
		close(fd);
		return INVALID_SOCKET_HANDLE;
	}

	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return fd;
}

void socket_close(SocketHandle p_socket) {
	if (p_socket != INVALID_SOCKET_HANDLE) {
		close((int)p_socket);
	}
}

long socket_send(SocketHandle p_socket, const char *p_data, size_t p_length) {
	size_t written = 0;
	while (written < p_length) {
		const ssize_t result = send((int)p_socket, p_data + written, p_length - written, MSG_NOSIGNAL);
		if (result < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		written += (size_t)result;
	}
	return (long)written;
}

long socket_recv(SocketHandle p_socket, char *p_buffer, size_t p_length) {
	const ssize_t result = recv((int)p_socket, p_buffer, p_length, 0);
	return (long)result;
}

bool wait_for_input(SocketHandle p_socket, int p_timeout_ms, bool p_watch_stdin, bool &r_stdin_ready, bool &r_socket_ready) {
	r_stdin_ready = false;
	r_socket_ready = false;

	struct pollfd fds[2];
	int count = 0;
	int stdin_index = -1;
	int socket_index = -1;
	if (p_watch_stdin) {
		fds[count].fd = STDIN_FILENO;
		fds[count].events = POLLIN;
		fds[count].revents = 0;
		stdin_index = count++;
	}
	if (p_socket != INVALID_SOCKET_HANDLE) {
		fds[count].fd = (int)p_socket;
		fds[count].events = POLLIN;
		fds[count].revents = 0;
		socket_index = count++;
	}
	if (count == 0) {
		return true;
	}

	const int ready = poll(fds, (nfds_t)count, p_timeout_ms);
	if (ready < 0) {
		return errno == EINTR;
	}
	if (stdin_index >= 0) {
		r_stdin_ready = (fds[stdin_index].revents & (POLLIN | POLLHUP | POLLERR)) != 0;
	}
	if (socket_index >= 0) {
		r_socket_ready = (fds[socket_index].revents & (POLLIN | POLLHUP | POLLERR)) != 0;
	}
	return true;
}

long read_stdin(char *p_buffer, size_t p_length) {
	const ssize_t result = read(STDIN_FILENO, p_buffer, p_length);
	if (result < 0 && (errno == EINTR || errno == EAGAIN)) {
		return -2; // Retryable.
	}
	return (long)result;
}

void write_stdout(const char *p_data, size_t p_length) {
	size_t written = 0;
	while (written < p_length) {
		const ssize_t result = write(STDOUT_FILENO, p_data + written, p_length - written);
		if (result < 0) {
			if (errno == EINTR) {
				continue;
			}
			return;
		}
		written += (size_t)result;
	}
}

void install_termination_handler() {
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = handle_signal;
	sigaction(SIGINT, &action, nullptr);
	sigaction(SIGTERM, &action, nullptr);
	// A client that closes the pipe must not kill us mid-write.
	signal(SIGPIPE, SIG_IGN);
}

bool is_terminating() {
	return g_terminate != 0;
}

std::vector<std::string> list_directory(const std::string &p_path) {
	std::vector<std::string> entries;
	DIR *dir = opendir(p_path.c_str());
	if (!dir) {
		return entries;
	}
	struct dirent *entry = nullptr;
	while ((entry = readdir(dir)) != nullptr) {
		entries.push_back(entry->d_name);
	}
	closedir(dir);
	return entries;
}

bool read_file(const std::string &p_path, std::string &r_contents) {
	FILE *file = fopen(p_path.c_str(), "rb");
	if (!file) {
		return false;
	}
	char buffer[4096];
	size_t read_bytes = 0;
	r_contents.clear();
	while ((read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
		r_contents.append(buffer, read_bytes);
	}
	fclose(file);
	return true;
}

void remove_file(const std::string &p_path) {
	unlink(p_path.c_str());
}

std::string real_path(const std::string &p_path) {
	char resolved[4096];
	if (realpath(p_path.c_str(), resolved)) {
		return resolved;
	}
	std::string out = p_path;
	while (out.size() > 1 && out.back() == '/') {
		out.pop_back();
	}
	return out;
}

std::string environment(const std::string &p_name) {
	const char *value = getenv(p_name.c_str());
	return value ? value : "";
}

std::string home_directory() {
	return environment("HOME");
}

long process_id() {
	return (long)getpid();
}

} // namespace platform
} // namespace godot_ai

#endif // !_WIN32
