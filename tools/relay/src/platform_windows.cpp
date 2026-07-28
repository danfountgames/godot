/**************************************************************************/
/*  platform_windows.cpp                                                  */
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

#ifdef _WIN32

// WSAPoll, GetTickCount64 and BCryptGenRandom are Vista/Windows 7 APIs, and mingw
// declares them only when the target version asks for them. Toolchains disagree about
// the default: a recent one exposes them anyway, an older one does not, so the same
// source cross-compiled cleanly here and failed on CI with "WSAPoll was not declared".
// State the target rather than inherit it. This must precede every Windows header.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06010000
#endif

#include "platform.h"

#include <atomic>
#include <condition_variable>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <winsock2.h>
// windows.h must come after winsock2.h, or it pulls in the winsock 1 declarations.
#include <ws2tcpip.h>

#include <bcrypt.h>

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>

namespace godot_ai {
namespace platform {

const SocketHandle INVALID_SOCKET_HANDLE = (SocketHandle)INVALID_SOCKET;

namespace {

std::atomic<bool> g_terminate{ false };

// Windows cannot wait on a console/pipe handle and a socket in the same call, so
// stdin is read by a dedicated thread that hands bytes to the main loop. This is the
// whole reason wait_for_input() exists as a seam rather than a poll() call.
struct StdinPump {
	std::thread thread;
	std::mutex mutex;
	std::condition_variable condition;
	std::string buffer;
	bool eof = false;
	bool failed = false;
	bool started = false;

	void start() {
		if (started) {
			return;
		}
		started = true;
		thread = std::thread([this]() {
			char chunk[8192];
			while (true) {
				const int result = _read(_fileno(stdin), chunk, sizeof(chunk));
				std::lock_guard<std::mutex> lock(mutex);
				if (result > 0) {
					buffer.append(chunk, (size_t)result);
				} else if (result == 0) {
					eof = true;
				} else {
					failed = true;
				}
				condition.notify_all();
				if (result <= 0) {
					return;
				}
			}
		});
		thread.detach();
	}

	bool has_input() {
		std::lock_guard<std::mutex> lock(mutex);
		return !buffer.empty() || eof || failed;
	}

	long take(char *p_buffer, size_t p_length) {
		std::lock_guard<std::mutex> lock(mutex);
		if (!buffer.empty()) {
			const size_t count = buffer.size() < p_length ? buffer.size() : p_length;
			memcpy(p_buffer, buffer.data(), count);
			buffer.erase(0, count);
			return (long)count;
		}
		if (eof) {
			return 0;
		}
		if (failed) {
			return -1;
		}
		return -2; // Nothing yet; retryable.
	}
};

StdinPump g_stdin;

BOOL WINAPI console_handler(DWORD p_type) {
	if (p_type == CTRL_C_EVENT || p_type == CTRL_BREAK_EVENT || p_type == CTRL_CLOSE_EVENT) {
		g_terminate = true;
		return TRUE;
	}
	return FALSE;
}

std::string last_socket_error() {
	const int code = WSAGetLastError();
	char *message = nullptr;
	FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, (DWORD)code, 0, (char *)&message, 0, nullptr);
	std::string out = message ? message : "unknown socket error";
	if (message) {
		LocalFree(message);
	}
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
		out.pop_back();
	}
	return out;
}

} // namespace

bool initialize(std::string &r_error) {
	WSADATA data;
	const int result = WSAStartup(MAKEWORD(2, 2), &data);
	if (result != 0) {
		r_error = "WSAStartup failed with code " + std::to_string(result);
		return false;
	}
	// Binary mode: a JSON frame must not have its newlines translated to CRLF.
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	return true;
}

void finalize() {
	WSACleanup();
}

SocketHandle socket_connect(const std::string &p_host, int p_port, std::string &r_error) {
	const SOCKET handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (handle == INVALID_SOCKET) {
		r_error = "socket() failed: " + last_socket_error();
		return INVALID_SOCKET_HANDLE;
	}

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)p_port);
	if (InetPtonA(AF_INET, p_host.c_str(), &address.sin_addr) != 1) {
		closesocket(handle);
		r_error = "invalid editor host address: " + p_host;
		return INVALID_SOCKET_HANDLE;
	}
	if (connect(handle, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
		r_error = last_socket_error();
		closesocket(handle);
		return INVALID_SOCKET_HANDLE;
	}

	BOOL one = TRUE;
	setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
	return (SocketHandle)handle;
}

void socket_close(SocketHandle p_socket) {
	if (p_socket != INVALID_SOCKET_HANDLE) {
		closesocket((SOCKET)p_socket);
	}
}

long socket_send(SocketHandle p_socket, const char *p_data, size_t p_length) {
	size_t written = 0;
	while (written < p_length) {
		const int result = send((SOCKET)p_socket, p_data + written, (int)(p_length - written), 0);
		if (result == SOCKET_ERROR) {
			return -1;
		}
		written += (size_t)result;
	}
	return (long)written;
}

long socket_recv(SocketHandle p_socket, char *p_buffer, size_t p_length) {
	const int result = recv((SOCKET)p_socket, p_buffer, (int)p_length, 0);
	if (result == SOCKET_ERROR) {
		return -1;
	}
	return (long)result;
}

bool wait_for_input(SocketHandle p_socket, int p_timeout_ms, bool p_watch_stdin, bool &r_stdin_ready, bool &r_socket_ready) {
	r_stdin_ready = false;
	r_socket_ready = false;

	if (p_watch_stdin) {
		g_stdin.start();
		if (g_stdin.has_input()) {
			r_stdin_ready = true;
			// Still check the socket, but do not block: there is work to do already.
			p_timeout_ms = 0;
		}
	}

	if (p_socket != INVALID_SOCKET_HANDLE) {
		WSAPOLLFD descriptor;
		descriptor.fd = (SOCKET)p_socket;
		descriptor.events = POLLRDNORM;
		descriptor.revents = 0;
		const int ready = WSAPoll(&descriptor, 1, p_timeout_ms);
		if (ready == SOCKET_ERROR) {
			return false;
		}
		r_socket_ready = ready > 0 && (descriptor.revents & (POLLRDNORM | POLLHUP | POLLERR)) != 0;
	} else if (p_timeout_ms > 0) {
		// Nothing to poll; wait for the stdin thread rather than spinning.
		Sleep((DWORD)p_timeout_ms);
	}

	if (p_watch_stdin && !r_stdin_ready) {
		r_stdin_ready = g_stdin.has_input();
	}
	return true;
}

SocketHandle socket_listen(const std::string &p_host, int p_port, std::string &r_error) {
	const SOCKET handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (handle == INVALID_SOCKET) {
		r_error = "socket() failed: " + last_socket_error();
		return INVALID_SOCKET_HANDLE;
	}

	// Deliberately not SO_REUSEADDR: on Windows that permits two sockets to bind the
	// same port, which would silently steal an endpoint rather than report a clash.
	BOOL exclusive = TRUE;
	setsockopt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive));

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)p_port);
	if (InetPtonA(AF_INET, p_host.c_str(), &address.sin_addr) != 1) {
		closesocket(handle);
		r_error = "invalid listen address: " + p_host;
		return INVALID_SOCKET_HANDLE;
	}
	if (bind(handle, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
		r_error = "bind() failed: " + last_socket_error();
		closesocket(handle);
		return INVALID_SOCKET_HANDLE;
	}
	if (listen(handle, 16) == SOCKET_ERROR) {
		r_error = "listen() failed: " + last_socket_error();
		closesocket(handle);
		return INVALID_SOCKET_HANDLE;
	}
	return (SocketHandle)handle;
}

SocketHandle socket_accept(SocketHandle p_listener, std::string &r_error) {
	const SOCKET handle = accept((SOCKET)p_listener, nullptr, nullptr);
	if (handle == INVALID_SOCKET) {
		const int error = WSAGetLastError();
		if (error != WSAEWOULDBLOCK && error != WSAEINTR) {
			r_error = "accept() failed: " + last_socket_error();
		}
		return INVALID_SOCKET_HANDLE;
	}
	BOOL one = TRUE;
	setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
	return (SocketHandle)handle;
}

bool wait_for_sockets(const std::vector<SocketHandle> &p_sockets, int p_timeout_ms, std::vector<SocketHandle> &r_ready) {
	r_ready.clear();
	if (p_sockets.empty()) {
		return true;
	}
	// WSAPoll handles sockets only - which is fine here, because unlike the stdio
	// path this waits on nothing else.
	std::vector<WSAPOLLFD> fds;
	fds.reserve(p_sockets.size());
	for (SocketHandle handle : p_sockets) {
		WSAPOLLFD entry;
		entry.fd = (SOCKET)handle;
		entry.events = POLLRDNORM;
		entry.revents = 0;
		fds.push_back(entry);
	}
	const int result = WSAPoll(fds.data(), (ULONG)fds.size(), p_timeout_ms);
	if (result == SOCKET_ERROR) {
		return false;
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

	std::vector<unsigned char> buffer(p_bytes);
	if (BCryptGenRandom(nullptr, buffer.data(), (ULONG)p_bytes,
				BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
		r_strong = true;
	} else {
		unsigned long long seed = (unsigned long long)GetCurrentProcessId() ^ (unsigned long long)GetTickCount64();
		for (size_t i = 0; i < p_bytes; i++) {
			seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
			buffer[i] = (unsigned char)((seed >> 33) & 0xFF);
		}
	}
	for (size_t i = 0; i < p_bytes; i++) {
		out.push_back(HEX[buffer[i] >> 4]);
		out.push_back(HEX[buffer[i] & 0xF]);
	}
	return out;
}

std::string executable_path() {
	char buffer[4096];
	const DWORD length = GetModuleFileNameA(nullptr, buffer, (DWORD)sizeof(buffer));
	if (length == 0 || length >= sizeof(buffer)) {
		return std::string();
	}
	return std::string(buffer, length);
}

bool make_directories(const std::string &p_path) {
	if (p_path.empty()) {
		return false;
	}
	std::string built;
	size_t position = 0;
	while (position <= p_path.size()) {
		size_t separator = p_path.find_first_of("/\\", position);
		const size_t end = separator == std::string::npos ? p_path.size() : separator;
		built = p_path.substr(0, end);
		position = end + 1;
		// "C:" is a drive, not a directory to create.
		if (built.empty() || (built.size() == 2 && built[1] == ':')) {
			if (separator == std::string::npos) {
				break;
			}
			continue;
		}
		if (_mkdir(built.c_str()) != 0 && errno != EEXIST) {
			return false;
		}
		if (separator == std::string::npos) {
			break;
		}
	}
	return true;
}

bool write_file(const std::string &p_path, const std::string &p_contents, std::string &r_error) {
	FILE *file = fopen(p_path.c_str(), "wb");
	if (!file) {
		r_error = "cannot write " + p_path;
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

long read_stdin(char *p_buffer, size_t p_length) {
	g_stdin.start();
	return g_stdin.take(p_buffer, p_length);
}

void write_stdout(const char *p_data, size_t p_length) {
	size_t written = 0;
	while (written < p_length) {
		const int result = _write(_fileno(stdout), p_data + written, (unsigned int)(p_length - written));
		if (result <= 0) {
			return;
		}
		written += (size_t)result;
	}
	fflush(stdout);
}

void install_termination_handler() {
	SetConsoleCtrlHandler(console_handler, TRUE);
}

bool is_terminating() {
	return g_terminate.load();
}

std::vector<std::string> list_directory(const std::string &p_path) {
	std::vector<std::string> entries;
	WIN32_FIND_DATAA data;
	const HANDLE handle = FindFirstFileA((p_path + "\\*").c_str(), &data);
	if (handle == INVALID_HANDLE_VALUE) {
		return entries;
	}
	do {
		entries.push_back(data.cFileName);
	} while (FindNextFileA(handle, &data));
	FindClose(handle);
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
	DeleteFileA(p_path.c_str());
}

std::string real_path(const std::string &p_path) {
	char resolved[MAX_PATH];
	const DWORD length = GetFullPathNameA(p_path.c_str(), MAX_PATH, resolved, nullptr);
	if (length > 0 && length < MAX_PATH) {
		std::string out = resolved;
		// Normalise separators so comparisons against descriptor paths behave.
		for (char &character : out) {
			if (character == '\\') {
				character = '/';
			}
		}
		while (out.size() > 1 && out.back() == '/') {
			out.pop_back();
		}
		return out;
	}
	return p_path;
}

std::string environment(const std::string &p_name) {
	// GetEnvironmentVariableA rather than getenv: it reflects changes made after the
	// process started, and _dupenv_s is MSVC-only so it would not build under mingw.
	const DWORD length = GetEnvironmentVariableA(p_name.c_str(), nullptr, 0);
	if (length == 0) {
		return "";
	}
	std::string value(length, '\0');
	const DWORD written = GetEnvironmentVariableA(p_name.c_str(), &value[0], length);
	value.resize(written);
	return value;
}

std::string home_directory() {
	const std::string profile = environment("USERPROFILE");
	if (!profile.empty()) {
		return profile;
	}
	const std::string drive = environment("HOMEDRIVE");
	const std::string path = environment("HOMEPATH");
	if (!drive.empty() && !path.empty()) {
		return drive + path;
	}
	return "";
}

long process_id() {
	return (long)GetCurrentProcessId();
}

} // namespace platform
} // namespace godot_ai

#endif // _WIN32
