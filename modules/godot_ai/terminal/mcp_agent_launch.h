/**************************************************************************/
/*  mcp_agent_launch.h                                                    */
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

#pragma once

// Working out how to start a coding agent in the terminal panel: which relay to point
// it at, what to write in its MCP configuration, what environment it inherits.
//
// None of this needs a scene, a pty or a running editor, and all of it is the sort of
// thing that is wrong in a way nobody notices until an agent silently has no tools. So
// it lives out here as plain functions over plain data, and the tests exercise every
// branch.
//
// The shape differs from the `GodotBeamDev` branch it descends from, because this fork's
// transport does. That branch pointed the agent at an HTTP endpoint inside the editor and
// authenticated it with a bearer token generated from `Math::random()` - a predictable
// value guarding a live editor. Here the agent launches `godot-ai-relay` over stdio and
// the editor's own approval flow decides what it may do, so there is no long-lived
// credential to generate, leak into a config file, or forget to revoke.

#ifdef MCP_TERMINAL_ENABLED

#include "core/string/ustring.h"
#include "core/templates/vector.h"

// Everything needed to start one agent process.
struct MCPAgentLaunchPlan {
	String command; // The agent binary, e.g. "claude".
	Vector<String> arguments;
	Vector<String> environment; // "NAME=value" entries.
	String working_directory;
	String mcp_config_path; // Written before launch, removed after; empty if none.
	String mcp_config_json;
};

// Where the relay might be, in the order they are tried. Exposed so the panel can say
// exactly where it looked when it finds nothing, rather than "not found".
Vector<String> mcp_agent_relay_search_paths(const String &p_executable_dir, const String &p_env_override);

// The MCP client configuration for an agent driving this editor: one stdio server that
// runs the relay against this process id.
String mcp_agent_build_mcp_config(const String &p_relay_path, int p_editor_pid, const String &p_client_name, bool p_read_only);

// The command line for Claude Code, given a configuration file to read.
Vector<String> mcp_agent_build_claude_arguments(const String &p_mcp_config_path, const String &p_extra_system_prompt);

// True for a command this knows the command line of. Everything else is started bare.
bool mcp_agent_command_is_claude(const String &p_command);

// The command line for whatever the user asked to run.
//
// Only a command we recognise gets flags. The obvious shortcut - always pass
// `--mcp-config` - turns the panel into something that only ever works with one agent:
// point it at a shell, at Codex, at anything else, and the process dies at once on an
// option it has never heard of. An unrecognised command is started as the user typed it,
// and finds the configuration through GODOT_AI_MCP_CONFIG in its environment.
Vector<String> mcp_agent_build_arguments(const String &p_command, const String &p_mcp_config_path, const String &p_extra_system_prompt);

// The environment an agent inherits: a named allowlist plus the terminal's own settings.
// An allowlist rather than the whole environment, so an editor launched from a shell
// holding unrelated credentials does not hand them to a child by default.
//
// `p_mcp_config_path` is exported as GODOT_AI_MCP_CONFIG. That is how an agent this does
// not know the command line of still finds the editor: the user points it at the file.
Vector<String> mcp_agent_build_environment(const Vector<String> &p_inherited, const String &p_mcp_config_path = String());

// The variables carried over from the editor's environment, by name.
Vector<String> mcp_agent_inherited_variable_names();

#endif // MCP_TERMINAL_ENABLED
