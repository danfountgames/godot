/**************************************************************************/
/*  mcp_console_tools.cpp                                                 */
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

#include "mcp_console_tools.h"

#include "core/object/script_language.h"

#include "../mcp_debugger_bridge.h"
#include "../mcp_protocol.h"
#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/io/json.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static MCPDebuggerBridge *_get_bridge() {
	MCPProtocol *protocol = MCPProtocol::get_singleton();
	if (!protocol) {
		return nullptr;
	}
	return protocol->get_debugger_bridge();
}

static Dictionary _require_game_running() {
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Debugger bridge not available.");
	}
	if (!bridge->is_game_running()) {
		return make_tool_error("Game is not running. Use runtime/run_project first.");
	}
	return Dictionary();
}

// ---------------------------------------------------------------------------
// Tool Registration
// ---------------------------------------------------------------------------

void MCPConsoleTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// console/execute
	{
		Dictionary props;
		props["command"] = make_prop("string",
				"Raw console command string (e.g. 'help', 'list cvars', 'pause', 'node /root').");
		Array required;
		required.push_back("command");
		p_registry->register_tool(
				"console/execute",
				"Execute Console Command",
				"Execute a command in the in-game debug console. Use 'help' for available commands.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPConsoleTools::handle_execute));
	}

	// console/get_manifest
	{
		Dictionary props;
		Array required;
		p_registry->register_tool(
				"console/get_manifest",
				"Get Debug Manifest",
				"Get all registered debug actions, queries, cvars, commands, events, and UI pages from the running game.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPConsoleTools::handle_get_manifest));
	}
}

// ---------------------------------------------------------------------------
// console/execute
// ---------------------------------------------------------------------------

Dictionary MCPConsoleTools::handle_execute(const Dictionary &p_args) {
	Dictionary err = _require_game_running();
	if (!err.is_empty()) {
		return err;
	}

	String command = p_args.get("command", "");
	if (command.is_empty()) {
		return make_tool_error("Missing required argument: command");
	}

	MCPDebuggerBridge *bridge = _get_bridge();

	// Evaluate via the running game's DebugConsole singleton.
	// _execute_command_string is private, so we use the public execute path:
	// We build a GDScript expression that tokenizes and dispatches the command
	// the same way the console does internally.
	String expr = vformat(
			"DebugConsole.get_singleton()._execute_command_string(\"%s\")",
			command.c_escape());

	Dictionary result = bridge->send_evaluate(expr, 15000);

	if (result.has("error")) {
		return make_tool_error(String(result["error"]));
	}

	String output;
	if (result.has("value")) {
		output = String(result["value"]);
	}

	if (output.is_empty()) {
		output = "(ok)";
	}

	Dictionary structured;
	structured["command"] = command;
	structured["output"] = output;
	return make_tool_result(output, structured);
}

// ---------------------------------------------------------------------------
// console/get_manifest
// ---------------------------------------------------------------------------

Dictionary MCPConsoleTools::handle_get_manifest(const Dictionary &p_args) {
	Dictionary err = _require_game_running();
	if (!err.is_empty()) {
		return err;
	}

	MCPDebuggerBridge *bridge = _get_bridge();

	// Call Debug.get_manifest() on the running game.
	String expr = "JSON.stringify(Debug.get_manifest())";
	Dictionary result = bridge->send_evaluate(expr, 10000);

	if (result.has("error")) {
		return make_tool_error(String(result["error"]));
	}

	String json_str;
	if (result.has("value")) {
		json_str = String(result["value"]);
	}

	if (json_str.is_empty()) {
		return make_tool_error("get_manifest() returned empty result.");
	}

	// Parse the JSON back to a Dictionary for structured content.
	JSON json;
	Error parse_err = json.parse(json_str);

	Dictionary structured;
	if (parse_err == OK && json.get_data().get_type() == Variant::DICTIONARY) {
		structured = json.get_data();
	}

	return make_tool_result(json_str, structured);
}
