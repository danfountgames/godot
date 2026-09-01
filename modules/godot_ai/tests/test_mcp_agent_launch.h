/**************************************************************************/
/*  test_mcp_agent_launch.h                                               */
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

#ifndef TEST_MCP_AGENT_LAUNCH_H
#define TEST_MCP_AGENT_LAUNCH_H

#ifdef MCP_TERMINAL_ENABLED

#include "modules/godot_ai/terminal/mcp_agent_launch.h"

#include "core/io/json.h"

#include "tests/test_macros.h"

namespace TestMCPAgentLaunch {

static int index_of(const Vector<String> &p_list, const String &p_value) {
	for (int i = 0; i < p_list.size(); i++) {
		if (p_list[i] == p_value) {
			return i;
		}
	}
	return -1;
}

TEST_CASE("[godot_ai] Claude Code is pointed at that configuration and nothing else") {
	const Vector<String> arguments = mcp_agent_build_claude_arguments("/tmp/agent.json", String());

	const int config_flag = index_of(arguments, "--mcp-config");
	REQUIRE(config_flag >= 0);
	CHECK(arguments[config_flag + 1] == "/tmp/agent.json");

	// Without this the agent also loads whatever the user configured globally, and a
	// tool of the same name from elsewhere would be indistinguishable in the activity log.
	CHECK(index_of(arguments, "--strict-mcp-config") >= 0);
}

TEST_CASE("[godot_ai] Claude receives no blanket permission bypass") {
	// Claude has no shared pre-launch permission schema, so its own confirmation remains
	// intact. Codex is different: the Agent Setup dialog explicitly configures both
	// layers before its per-server tool prompts are suppressed.
	const Vector<String> arguments = mcp_agent_build_claude_arguments("/tmp/agent.json", String());
	for (int i = 0; i < arguments.size(); i++) {
		CHECK(arguments[i] != "--allowedTools");
		CHECK(arguments[i] != "--dangerously-skip-permissions");
		CHECK_FALSE(arguments[i].contains("bypassPermissions"));
	}
}

TEST_CASE("[godot_ai] Codex receives this editor as a required Streamable HTTP MCP server") {
	const Vector<String> arguments = mcp_agent_build_codex_arguments(6110,
			"Godot Agent Terminal (codex)", false, true, "/tmp/project", "Use the editor.");

	const int directory_flag = index_of(arguments, "--cd");
	REQUIRE(directory_flag >= 0);
	CHECK(arguments[directory_flag + 1] == "/tmp/project");
	const int sandbox_flag = index_of(arguments, "--sandbox");
	REQUIRE(sandbox_flag >= 0);
	CHECK(arguments[sandbox_flag + 1] == "workspace-write");
	const int approval_flag = index_of(arguments, "--ask-for-approval");
	REQUIRE(approval_flag >= 0);
	CHECK(arguments[approval_flag + 1] == "on-request");
	CHECK(index_of(arguments, "--no-alt-screen") >= 0);
	CHECK(index_of(arguments, "--strict-config") >= 0);

	bool has_url = false;
	bool has_token_environment = false;
	bool has_client_name = false;
	bool is_required = false;
	bool mcp_is_preapproved = false;
	bool has_briefing = false;
	for (int i = 0; i < arguments.size(); i++) {
		const String argument = arguments[i];
		has_url |= argument == "mcp_servers.godot-ai.url=\"http://127.0.0.1:6110/mcp\"";
		has_token_environment |= argument == "mcp_servers.godot-ai.bearer_token_env_var=\"GODOT_AI_MCP_TOKEN\"";
		has_client_name |= argument.contains("X-Godot-AI-Client-Name") && argument.contains("Godot Agent Terminal (codex)");
		is_required |= argument == "mcp_servers.godot-ai.required=true";
		mcp_is_preapproved |= argument == "mcp_servers.godot-ai.default_tools_approval_mode=\"approve\"";
		has_briefing |= argument == "developer_instructions=\"Use the editor.\"";
		CHECK_FALSE(argument.contains("test-secret"));
		CHECK(argument != "--dangerously-bypass-approvals-and-sandbox");
	}
	CHECK(has_url);
	CHECK(has_token_environment);
	CHECK(has_client_name);
	CHECK(is_required);
	CHECK(mcp_is_preapproved);
	CHECK(has_briefing);
	// A positional prompt starts a turn before the user asks for anything. The briefing
	// belongs in developer instructions instead.
	CHECK(index_of(arguments, "Use the editor.") == -1);
}

TEST_CASE("[godot_ai] Codex read-only and no-approval choices reach both security boundaries") {
	const Vector<String> arguments = mcp_agent_build_codex_arguments(6111,
			"Godot Agent Terminal (codex)", true, false, "/tmp/project", String());

	const int sandbox_flag = index_of(arguments, "--sandbox");
	REQUIRE(sandbox_flag >= 0);
	CHECK(arguments[sandbox_flag + 1] == "read-only");
	const int approval_flag = index_of(arguments, "--ask-for-approval");
	REQUIRE(approval_flag >= 0);
	CHECK(arguments[approval_flag + 1] == "never");

	bool has_read_only_header = false;
	for (int i = 0; i < arguments.size(); i++) {
		has_read_only_header |= arguments[i].contains("X-Godot-AI-Read-Only") && arguments[i].contains("\"1\"");
	}
	CHECK(has_read_only_header);
}

TEST_CASE("[godot_ai] The HTTP configuration points at the editor and carries no secret") {
	const String json = mcp_agent_build_http_mcp_config(6110, "Godot Agent Terminal (claude)", false);
	const Variant parsed = JSON::parse_string(json);
	REQUIRE(parsed.get_type() == Variant::DICTIONARY);
	const Dictionary servers = Dictionary(parsed)["mcpServers"];
	REQUIRE(servers.has("godot-ai"));
	const Dictionary server = servers["godot-ai"];
	CHECK(String(server["type"]) == "http");
	CHECK(String(server["url"]) == "http://127.0.0.1:6110/mcp");
	const Dictionary headers = server["headers"];
	// The env-var reference and nothing else: a committed config must leak nothing.
	CHECK(String(headers["Authorization"]) == "Bearer ${GODOT_AI_MCP_TOKEN}");
	CHECK(String(headers["X-Godot-AI-Client-Name"]) == "Godot Agent Terminal (claude)");
	CHECK_FALSE(headers.has("X-Godot-AI-Read-Only"));
	// No relay anywhere in it.
	CHECK_FALSE(json.contains("relay"));
	CHECK_FALSE(server.has("command"));

	const String read_only = mcp_agent_build_http_mcp_config(6110, "x", true);
	const Dictionary ro_headers = Dictionary(Dictionary(Dictionary(JSON::parse_string(read_only))["mcpServers"])["godot-ai"])["headers"];
	CHECK(String(ro_headers["X-Godot-AI-Read-Only"]) == "1");
}

TEST_CASE("[godot_ai] The briefing teaches the loop, not just the tools") {
	const String briefing = mcp_agent_editor_briefing(false);
	// The point of injecting anything is that the agent runs and looks at the game
	// rather than reporting an untested edit. If these disappear from the text, the
	// injection has stopped doing its one job.
	CHECK(briefing.contains("run the game"));
	CHECK(briefing.contains("Godot_CaptureGame"));
	CHECK(briefing.contains("Godot_GetRuntimeSceneTree"));
	CHECK(briefing.contains("iterate"));
	CHECK(briefing.contains("ai_skills/"));
	// Compact is a requirement, not a taste: past a screenful, models skim.
	CHECK(briefing.length() < 3000);
	// It must not name project-specific files; it is injected into every project.
	CHECK(!briefing.contains("GAME_SPEC"));
	CHECK(!briefing.contains("res://scenes"));

	const String read_only = mcp_agent_editor_briefing(true);
	CHECK(read_only.contains("READ-ONLY"));
	CHECK(!mcp_agent_editor_briefing(false).contains("READ-ONLY"));
}

TEST_CASE("[godot_ai] A system prompt is appended only when there is one") {
	CHECK(index_of(mcp_agent_build_claude_arguments("/tmp/a.json", String()), "--append-system-prompt") == -1);

	const Vector<String> with_prompt = mcp_agent_build_claude_arguments("/tmp/a.json", "You are in Godot.");
	const int flag = index_of(with_prompt, "--append-system-prompt");
	REQUIRE(flag >= 0);
	CHECK(with_prompt[flag + 1] == "You are in Godot.");
}

TEST_CASE("[godot_ai] The selected backend controls arguments independently of its executable path") {
	const Vector<String> claude = mcp_agent_build_arguments(MCP_AGENT_CLAUDE,
			"/tmp/a.json", "brief", 6110, "client", false, true, "/tmp/project");
	CHECK(index_of(claude, "--mcp-config") >= 0);
	CHECK(index_of(claude, "--append-system-prompt") >= 0);
	CHECK(index_of(claude, "--sandbox") == -1);

	const Vector<String> codex = mcp_agent_build_arguments(MCP_AGENT_CODEX,
			String(), "brief", 6110, "client", false, true, "/tmp/project");
	CHECK(index_of(codex, "--mcp-config") == -1);
	CHECK(index_of(codex, "--sandbox") >= 0);
	CHECK(index_of(codex, "developer_instructions=\"brief\"") >= 0);
}

TEST_CASE("[godot_ai] An agent we do not know finds the configuration in its environment") {
	Vector<String> inherited;
	const Vector<String> environment = mcp_agent_build_environment(inherited, "/tmp/agent.json");
	CHECK(index_of(environment, "GODOT_AI_MCP_CONFIG=/tmp/agent.json") >= 0);

	// And nothing is exported when there is no configuration to point at, rather than an
	// empty variable that reads as a path to a file that is not there.
	const Vector<String> without = mcp_agent_build_environment(inherited, String());
	for (int i = 0; i < without.size(); i++) {
		CHECK_FALSE(without[i].begins_with("GODOT_AI_MCP_CONFIG="));
	}
}

TEST_CASE("[godot_ai] The agent's environment is an allowlist, plus the terminal's own settings") {
	const Vector<String> names = mcp_agent_inherited_variable_names();

	// PATH decides which agent binary runs at all; GODOT_AI_HOME decides whether the
	// relay can find this editor. Losing either turns into a confusing failure much
	// later than here.
	CHECK(index_of(names, "PATH") >= 0);
	CHECK(index_of(names, "HOME") >= 0);
	CHECK(index_of(names, "GODOT_AI_HOME") >= 0);

	Vector<String> inherited;
	inherited.push_back("PATH=/usr/bin");
	const Vector<String> environment = mcp_agent_build_environment(inherited);

	CHECK(index_of(environment, "PATH=/usr/bin") >= 0);
	// Without TERM the child falls back to a dumb terminal: no colour, no cursor
	// addressing, and an agent's output close to unreadable.
	CHECK(index_of(environment, "TERM=xterm-256color") >= 0);
	CHECK(index_of(environment, "GODOT_AI_TERMINAL=1") >= 0);
}

} // namespace TestMCPAgentLaunch

#endif // MCP_TERMINAL_ENABLED

#endif // TEST_MCP_AGENT_LAUNCH_H
