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

static String codex_toml_string(const String &p_value) {
	return "\"" + p_value.c_escape() + "\"";
}

Vector<String> mcp_agent_build_codex_arguments(int p_http_port, const String &p_client_name,
		bool p_read_only, bool p_allow_host_approval, const String &p_project_path,
		const String &p_developer_instructions) {
	Vector<String> arguments;

	if (!p_project_path.is_empty()) {
		arguments.push_back("--cd");
		arguments.push_back(p_project_path);
	}
	arguments.push_back("--sandbox");
	arguments.push_back(p_read_only ? "read-only" : "workspace-write");
	arguments.push_back("--ask-for-approval");
	arguments.push_back(p_allow_host_approval ? "on-request" : "never");
	// The terminal widget already owns scrollback. Inline mode makes it remain useful
	// after Codex redraws or exits instead of hiding the conversation in an alt screen.
	arguments.push_back("--no-alt-screen");
	arguments.push_back("--strict-config");

	const String prefix = "mcp_servers.godot-ai.";
	arguments.push_back("--config");
	arguments.push_back(prefix + "url=" + codex_toml_string(vformat("http://127.0.0.1:%d/mcp", p_http_port)));
	arguments.push_back("--config");
	arguments.push_back(prefix + "bearer_token_env_var=" + codex_toml_string("GODOT_AI_MCP_TOKEN"));

	String headers = prefix + "http_headers={";
	if (!p_client_name.is_empty()) {
		headers += codex_toml_string("X-Godot-AI-Client-Name") + "=" + codex_toml_string(p_client_name);
	}
	if (p_read_only) {
		if (!p_client_name.is_empty()) {
			headers += ",";
		}
		headers += codex_toml_string("X-Godot-AI-Read-Only") + "=" + codex_toml_string("1");
	}
	if (!p_client_name.is_empty() || p_read_only) {
		arguments.push_back("--config");
		arguments.push_back(headers + "}");
	}

	arguments.push_back("--config");
	arguments.push_back(prefix + "required=true");
	// The user has just approved this exact server and its Godot capability classes in
	// the setup dialog. Repeating a second prompt for every MCP call adds no boundary;
	// the editor remains authoritative and still refuses every denied capability.
	arguments.push_back("--config");
	arguments.push_back(prefix + "default_tools_approval_mode=" + codex_toml_string("approve"));

	if (!p_developer_instructions.is_empty()) {
		// This briefs Codex without supplying a positional user prompt. A positional
		// prompt starts a turn immediately, which made the terminal appear to ask its own
		// question before the user had typed anything.
		arguments.push_back("--config");
		arguments.push_back("developer_instructions=" + codex_toml_string(p_developer_instructions));
	}
	return arguments;
}

Vector<String> mcp_agent_build_arguments(MCPAgentKind p_kind, const String &p_mcp_config_path,
		const String &p_extra_system_prompt, int p_http_port, const String &p_client_name,
		bool p_read_only, bool p_allow_host_approval, const String &p_project_path) {
	if (p_kind == MCP_AGENT_CLAUDE) {
		return mcp_agent_build_claude_arguments(p_mcp_config_path, p_extra_system_prompt);
	}
	return mcp_agent_build_codex_arguments(p_http_port, p_client_name, p_read_only,
			p_allow_host_approval, p_project_path, p_extra_system_prompt);
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
