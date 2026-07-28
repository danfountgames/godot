/**************************************************************************/
/*  platform.h                                                            */
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

// Platform seam for godot-ai-relay.
//
// The relay is a socket-and-stdio pump, which is exactly the part of a program that
// POSIX and Windows disagree about. Everything platform-specific lives behind this
// header so relay.cpp reads the same on both, and so a Windows regression cannot
// hide inside the protocol logic.
//
// The awkward difference is waiting: on POSIX a socket and stdin are both pollable
// file descriptors, while on Windows stdin is a HANDLE that WSAPoll cannot watch.
// wait_for_input() hides that - the Windows backend runs a reader thread for stdin
// and polls only the socket.

#ifndef GODOT_AI_RELAY_PLATFORM_H
#define GODOT_AI_RELAY_PLATFORM_H

#include <cstddef>
#include <string>
#include <vector>

namespace godot_ai {
namespace platform {

#ifdef _WIN32
typedef unsigned long long SocketHandle;
#else
typedef int SocketHandle;
#endif

extern const SocketHandle INVALID_SOCKET_HANDLE;

// Called once at startup and shutdown. Winsock needs this; POSIX does not, and its
// implementation is empty rather than absent so the call sites stay identical.
bool initialize(std::string &r_error);
void finalize();

// Connects to a loopback host. Returns INVALID_SOCKET_HANDLE and fills r_error on
// failure, with a message the user can act on.
SocketHandle socket_connect(const std::string &p_host, int p_port, std::string &r_error);
void socket_close(SocketHandle p_socket);

// Returns the number of bytes sent, or -1 on error. Retries short writes.
long socket_send(SocketHandle p_socket, const char *p_data, size_t p_length);
// Returns bytes read, 0 on a clean close, -1 on error.
long socket_recv(SocketHandle p_socket, char *p_buffer, size_t p_length);

// Waits until stdin or the socket has data, or the timeout elapses. Pass
// INVALID_SOCKET_HANDLE when there is no socket yet. Returns false on a wait error.
bool wait_for_input(SocketHandle p_socket, int p_timeout_ms, bool p_watch_stdin, bool &r_stdin_ready, bool &r_socket_ready);

// stdin/stdout. Returns bytes moved, 0 on EOF, -1 on error.
long read_stdin(char *p_buffer, size_t p_length);
void write_stdout(const char *p_data, size_t p_length);

// Termination signalling, so a client killing the relay is a clean shutdown.
void install_termination_handler();
bool is_terminating();

// Filesystem bits the instance registry needs.
std::vector<std::string> list_directory(const std::string &p_path);
bool read_file(const std::string &p_path, std::string &r_contents);
void remove_file(const std::string &p_path);
std::string real_path(const std::string &p_path);
std::string environment(const std::string &p_name);
std::string home_directory();
long process_id();

} // namespace platform
} // namespace godot_ai

#endif // GODOT_AI_RELAY_PLATFORM_H
