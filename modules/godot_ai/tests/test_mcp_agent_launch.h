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

static Dictionary parse_config(const String &p_json) {
	JSON json;
	REQUIRE(json.parse(p_json) == OK);
	return json.get_data();
}

static Dictionary godot_server(const String &p_json) {
	const Dictionary config = parse_config(p_json);
	REQUIRE(config.has("mcpServers"));
	const Dictionary servers = config["mcpServers"];
	REQUIRE(servers.has("godot-ai"));
	return servers["godot-ai"];
}

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

TEST_CASE("[godot_ai] Nothing pre-authorises the editor's tools") {
	// The branch passed `--allowedTools mcp__godot__*`, which silences the client's own
	// confirmation for every editor tool at once. The editor asks too, but leaving only
	// one gate between an agent and the project is not a decision to make by accident.
	const Vector<String> arguments = mcp_agent_build_claude_arguments("/tmp/agent.json", String());
	for (int i = 0; i < arguments.size(); i++) {
		CHECK(arguments[i] != "--allowedTools");
		CHECK(arguments[i] != "--dangerously-skip-permissions");
		CHECK_FALSE(arguments[i].contains("bypassPermissions"));
	}
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

TEST_CASE("[godot_ai] Only a command we recognise is given a command line") {
	// The panel used to hand every command Claude Code's flags. Point it at a shell and
	// the process died at once on an option it had never heard of - which is exactly how
	// the first real run of the panel failed.
	CHECK(mcp_agent_command_is_claude("claude"));
	CHECK(mcp_agent_command_is_claude("/usr/local/bin/claude"));
	CHECK(mcp_agent_command_is_claude("  claude  "));
	CHECK_FALSE(mcp_agent_command_is_claude("sh"));
	CHECK_FALSE(mcp_agent_command_is_claude("/bin/bash"));
	CHECK_FALSE(mcp_agent_command_is_claude("codex"));
	CHECK_FALSE(mcp_agent_command_is_claude(""));

	CHECK(mcp_agent_build_arguments("claude", "/tmp/a.json", String()).size() > 0);
	CHECK(mcp_agent_build_arguments("/opt/bin/claude", "/tmp/a.json", String()).size() > 0);

	// Anything else starts exactly as the user typed it.
	CHECK(mcp_agent_build_arguments("sh", "/tmp/a.json", String()).is_empty());
	CHECK(mcp_agent_build_arguments("codex", "/tmp/a.json", String()).is_empty());
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
