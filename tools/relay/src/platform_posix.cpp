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

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

SocketHandle socket_listen(const std::string &p_host, int p_port, std::string &r_error) {
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		r_error = std::string("socket() failed: ") + strerror(errno);
		return INVALID_SOCKET_HANDLE;
	}

	int one = 1;
	// Without this, restarting the relay on the same port fails for as long as the
	// previous socket sits in TIME_WAIT, which is exactly when a restart happens.
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)p_port);
	if (inet_pton(AF_INET, p_host.c_str(), &address.sin_addr) != 1) {
		close(fd);
		r_error = "invalid listen address: " + p_host;
		return INVALID_SOCKET_HANDLE;
	}
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		r_error = std::string("bind() failed: ") + strerror(errno);
		close(fd);
		return INVALID_SOCKET_HANDLE;
	}
	if (listen(fd, 16) != 0) {
		r_error = std::string("listen() failed: ") + strerror(errno);
		close(fd);
		return INVALID_SOCKET_HANDLE;
	}
	return fd;
}

SocketHandle socket_accept(SocketHandle p_listener, std::string &r_error) {
	const int fd = accept((int)p_listener, nullptr, nullptr);
	if (fd < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
			r_error = std::string("accept() failed: ") + strerror(errno);
		}
		return INVALID_SOCKET_HANDLE;
	}
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return fd;
}

bool wait_for_sockets(const std::vector<SocketHandle> &p_sockets, int p_timeout_ms, std::vector<SocketHandle> &r_ready) {
	r_ready.clear();
	if (p_sockets.empty()) {
		return true;
	}
	std::vector<struct pollfd> fds;
	fds.reserve(p_sockets.size());
	for (SocketHandle handle : p_sockets) {
		struct pollfd entry;
		entry.fd = (int)handle;
		entry.events = POLLIN;
		entry.revents = 0;
		fds.push_back(entry);
	}
	const int result = poll(fds.data(), (nfds_t)fds.size(), p_timeout_ms);
	if (result < 0) {
		return errno == EINTR;
	}
	for (size_t i = 0; i < fds.size(); i++) {
		if (fds[i].revents != 0) {
			r_ready.push_back(p_sockets[i]);
		}
	}
	return true;
}

std::string random_token(size_t p_bytes, bool &r_strong) {
	static const char *HEX = "0123456789abcdef";
	std::string out;
	out.reserve(p_bytes * 2);
	r_strong = false;

	FILE *source = fopen("/dev/urandom", "rb");
	if (source) {
		std::vector<unsigned char> buffer(p_bytes);
		const size_t read_count = fread(buffer.data(), 1, p_bytes, source);
		fclose(source);
		if (read_count == p_bytes) {
			r_strong = true;
			for (size_t i = 0; i < p_bytes; i++) {
				out.push_back(HEX[buffer[i] >> 4]);
				out.push_back(HEX[buffer[i] & 0xF]);
			}
			return out;
		}
	}

	// Predictable, and the caller is told so: a token nobody can trust is worse than
	// no token at all if it is presented as one.
	unsigned long seed = (unsigned long)getpid() ^ (unsigned long)time(nullptr);
	for (size_t i = 0; i < p_bytes; i++) {
		seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
		const unsigned char byte = (unsigned char)((seed >> 33) & 0xFF);
		out.push_back(HEX[byte >> 4]);
		out.push_back(HEX[byte & 0xF]);
	}
	return out;
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

std::string executable_path() {
	char buffer[4096];
	const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
	if (length > 0) {
		buffer[length] = '\0';
		return std::string(buffer);
	}
	// macOS has no /proc; _NSGetExecutablePath is the documented route there.
#ifdef __APPLE__
	uint32_t size = sizeof(buffer);
	if (_NSGetExecutablePath(buffer, &size) == 0) {
		return real_path(buffer);
	}
#endif
	return std::string();
}

bool make_directories(const std::string &p_path) {
	if (p_path.empty()) {
		return false;
	}
	std::string built;
	size_t position = 0;
	while (position <= p_path.size()) {
		const size_t slash = p_path.find('/', position);
		const size_t end = slash == std::string::npos ? p_path.size() : slash;
		built = p_path.substr(0, end);
		position = end + 1;
		if (built.empty()) {
			continue;
		}
		if (mkdir(built.c_str(), 0700) != 0 && errno != EEXIST) {
			return false;
		}
		if (slash == std::string::npos) {
			break;
		}
	}
	return true;
}

bool write_file(const std::string &p_path, const std::string &p_contents, std::string &r_error) {
	FILE *file = fopen(p_path.c_str(), "wb");
	if (!file) {
		r_error = "cannot write " + p_path + ": " + strerror(errno);
		return false;
	}
	const size_t written = fwrite(p_contents.data(), 1, p_contents.size(), file);
	fclose(file);
	if (written != p_contents.size()) {
		r_error = "short write to " + p_path;
		return false;
	}
	return true;
}

long process_id() {
	return (long)getpid();
}

bool process_is_alive(long p_pid) {
	if (p_pid <= 0) {
		// No pid recorded: assume alive rather than prune something real.
		return true;
	}
	// Signal 0 performs the existence and permission checks without delivering
	// anything. EPERM means it exists and belongs to someone else, which still counts.
	if (::kill((pid_t)p_pid, 0) == 0) {
		return true;
	}
	return errno == EPERM;
}

} // namespace platform
} // namespace godot_ai

#endif // !_WIN32
