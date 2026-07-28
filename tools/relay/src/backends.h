/**************************************************************************/
/*  backends.h                                                            */
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

// Wiring an MCP client up to this relay, without anyone hand-editing JSON.
//
// Every MCP client keeps a configuration file naming the servers it may talk to. The
// entry is short but easy to get subtly wrong - a relative binary path that resolves
// to nothing later, a missing --project on a machine with two editors open - and the
// mistakes surface as "the tools just are not there".
//
// So the relay writes the entry itself, describing the binary that is actually
// running. Two rules shape what it writes:
//
//   * **No credentials, ever.** A generated entry references an environment variable
//     for the HTTP bearer token; it never contains the token. A config file is
//     copied, synced and pasted into bug reports, and a secret in one is a secret
//     that has escaped. `--install-backend` refuses outright if asked to bake one in.
//   * **Pin what generated it.** Each entry records the relay and bridge versions it
//     was written for, so `--check-backends` can say "this was written by an older
//     relay, re-run --install-backend" rather than leaving a client to fail against a
//     bridge it cannot speak.

#ifndef GODOT_AI_RELAY_BACKENDS_H
#define GODOT_AI_RELAY_BACKENDS_H

#include "json.h"
#include "relay.h"

#include <string>
#include <vector>

namespace godot_ai {

// The key the generated entry is stored under, and the marker that identifies an
// entry as ours so re-running an install replaces it rather than duplicating it.
extern const char *BACKEND_SERVER_KEY;
extern const char *BACKEND_METADATA_KEY;

struct AgentBackend {
	std::string name;
	std::string description;
	// "stdio" launches this binary as a child process; "http" points at a running
	// relay's endpoint.
	std::string transport;
	// The environment variable a generated HTTP entry reads its token from. Named
	// here rather than in the file, because the file must not hold the value.
	std::string token_variable;
};

// The backends this relay knows how to configure.
std::vector<AgentBackend> agent_backends();
bool agent_backend_by_name(const std::string &p_name, AgentBackend &r_backend);

// Human-readable listing for --list-backends.
std::string agent_backends_listing();

// Builds the server entry a client configuration needs. Exposed for testing.
JSONValueRef backend_entry(const AgentBackend &p_backend, const RelayOptions &p_options, const std::string &p_executable);

// Merges the entry into the client configuration at p_path, creating the file and its
// directory when absent, and preserving every other server already configured.
bool backend_install(const AgentBackend &p_backend, const RelayOptions &p_options, const std::string &p_path, std::string &r_summary, std::string &r_error);

// Reports whether the entry at p_path was written by this relay version.
// r_up_to_date is false for a stale or absent entry; r_summary always explains.
bool backend_check(const std::string &p_path, bool &r_up_to_date, std::string &r_summary, std::string &r_error);

} // namespace godot_ai

#endif // GODOT_AI_RELAY_BACKENDS_H
