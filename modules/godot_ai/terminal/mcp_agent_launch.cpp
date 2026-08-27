/**************************************************************************/
/*  mcp_agent_launch.cpp                                                  */
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

#ifdef MCP_TERMINAL_ENABLED

#include "mcp_agent_launch.h"

#include "core/io/json.h"
#include "core/os/os.h"

String mcp_agent_build_http_mcp_config(int p_http_port, const String &p_client_name, bool p_read_only) {
	// Straight to the editor: it spawned this agent, so it serves this agent (DEC-0014).
	// No relay process, no binary to find, no stdio bridge.
	Dictionary headers;
	// The value is an env-var reference on purpose. The editor sets GODOT_AI_MCP_TOKEN
	// in the child's environment; the configuration file never carries the secret, so a
	// copied or committed config leaks nothing.
	headers["Authorization"] = "Bearer ${GODOT_AI_MCP_TOKEN}";
	if (!p_client_name.is_empty()) {
		headers["X-Godot-AI-Client-Name"] = p_client_name;
	}
	if (p_read_only) {
		headers["X-Godot-AI-Read-Only"] = "1";
	}

	Dictionary server;
	server["type"] = "http";
	server["url"] = vformat("http://127.0.0.1:%d/mcp", p_http_port);
	server["headers"] = headers;

	Dictionary servers;
	servers["godot-ai"] = server;

	Dictionary config;
	config["mcpServers"] = servers;

	return JSON::stringify(config, "\t");
}

String mcp_agent_editor_briefing(bool p_read_only) {
	String briefing =
			"You are running inside the GodotAI editor's terminal, attached to the live editor "
			"through the godot-ai MCP tools. Those tools ARE the editor: the scene tree, the "
			"Inspector, the running game, its input, its output log and its tests. Prefer them "
			"over reading .tscn/.tres text or guessing from source.\n"
			"\n"
			"The loop that makes your work trustworthy: make the change, run the game, look at "
			"it, and only then say it is done.\n"
			"- Run: Godot_PlayMainScene / Godot_PlayScene, or Godot_LaunchInstance for an "
			"agent-owned instance you may pause and stop freely (Godot_StopAllInstances never "
			"touches the user's own game).\n"
			"- See: Godot_CaptureGame for the game's pixels, Godot_CaptureEditorControl / "
			"Godot_CaptureEditorWindow for the editor's. Screenshots are evidence; claims "
			"without them are guesses.\n"
			"- Inspect: Godot_GetRuntimeSceneTree and Godot_GetRuntimeProperty read the LIVE "
			"game; Godot_SetRuntimeProperty tries values without touching files, and "
			"Godot_PromoteRuntimeValue keeps the one that felt right.\n"
			"- Drive: Godot_SendPointerInput / Godot_SendActionInput deliver real input; "
			"Godot_WaitForRuntimeCondition waits on observed state instead of sleeping.\n"
			"- Check: Godot_RunSceneTests, Godot_ReadOutputLog (errors show up there first), "
			"and the playtest tools when a goal needs a verdict.\n"
			"\n"
			"When asked to change something: change it, run it, watch it behave, fix what you "
			"see, and iterate until it demonstrably works - do not hand back an untested edit. "
			"Persistent scene edits and runtime (play-mode) edits are different tools on "
			"purpose; keep them distinct. Every mutating tool takes a checkpoint first and the "
			"user can undo it, so prefer acting and verifying over asking for permission to "
			"act.\n"
			"\n"
			"Skills in ai_skills/ are step-by-step recipes for bigger jobs (crash "
			"investigation, performance regressions, menu traversal, tuning); read one before "
			"improvising its job.";
	if (p_read_only) {
		briefing +=
				"\n\nThis session is READ-ONLY: mutating tools will be refused. Observe, "
				"diagnose and propose; do not fight the refusals.";
	}
	return briefing;
}

Vector<String> mcp_agent_build_claude_arguments(const String &p_mcp_config_path, const String &p_extra_system_prompt) {
	Vector<String> arguments;

	if (!p_mcp_config_path.is_empty()) {
		arguments.push_back("--mcp-config");
		arguments.push_back(p_mcp_config_path);
		// Only this editor's tools. Without it the agent also loads whatever the user
		// has configured globally, and a tool named the same thing from somewhere else
		// would be indistinguishable in the activity log.
		arguments.push_back("--strict-mcp-config");
	}

	if (!p_extra_system_prompt.is_empty()) {
		arguments.push_back("--append-system-prompt");
		arguments.push_back(p_extra_system_prompt);
	}

	// Deliberately absent: any blanket tool pre-authorisation. The branch this descends
	// from passed `--allowedTools mcp__godot__*`, which turns off the client's own
	// confirmation for every editor tool at once. This fork already asks on the editor
	// side, where the user can see what is being asked for and revoke it afterwards;
	// silencing the client's prompt as well would leave nothing between an agent and the
	// project but one dialog it can be told to expect.
	return arguments;
}

bool mcp_agent_command_is_claude(const String &p_command) {
	// By file name, so an absolute path to a particular build is still recognised, and
	// case-insensitively for the platforms that do not care.
	const String name = p_command.strip_edges().get_file().to_lower();
	return name == "claude" || name == "claude.exe" || name == "claude.cmd";
}

Vector<String> mcp_agent_build_arguments(const String &p_command, const String &p_mcp_config_path, const String &p_extra_system_prompt) {
	if (mcp_agent_command_is_claude(p_command)) {
		return mcp_agent_build_claude_arguments(p_mcp_config_path, p_extra_system_prompt);
	}
	// Started exactly as the user typed it. Guessing at flags for a command we do not
	// know is how a terminal ends up unable to run a shell.
	return Vector<String>();
}

Vector<String> mcp_agent_inherited_variable_names() {
	Vector<String> names;
	names.push_back("PATH");
	names.push_back("HOME");
	names.push_back("USER");
	names.push_back("SHELL");
	names.push_back("LANG");
	names.push_back("LC_ALL");
	names.push_back("LC_CTYPE");
	names.push_back("TMPDIR");
	names.push_back("XDG_RUNTIME_DIR");
	names.push_back("XDG_DATA_HOME");
	names.push_back("XDG_CONFIG_HOME");
	names.push_back("XDG_CACHE_HOME");
	names.push_back("SSH_AUTH_SOCK");
	names.push_back("DISPLAY");
	names.push_back("WAYLAND_DISPLAY");
	// The relay reads this to find the editor's state directory. An editor started with a
	// non-default one would otherwise be invisible to the agent it just launched.
	names.push_back("GODOT_AI_HOME");
	return names;
}

Vector<String> mcp_agent_build_environment(const Vector<String> &p_inherited, const String &p_mcp_config_path) {
	Vector<String> environment = p_inherited;

	// How an agent we do not know the command line of finds this editor.
	if (!p_mcp_config_path.is_empty()) {
		environment.push_back("GODOT_AI_MCP_CONFIG=" + p_mcp_config_path);
	}

	// The emulator implements enough of xterm-256color to be honest about it, and a
	// child told nothing at all falls back to a dumb terminal with no colour and no
	// cursor addressing - which for an agent's output is close to unreadable.
	environment.push_back("TERM=xterm-256color");

	// Tells anything that checks that it is running inside this editor's panel. Cheap,
	// and it makes a shell prompt able to say so.
	environment.push_back("GODOT_AI_TERMINAL=1");

	return environment;
}

#endif // MCP_TERMINAL_ENABLED
