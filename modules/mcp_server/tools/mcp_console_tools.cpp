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
	MCPDebuggerBridge *bridge = MCPDebuggerBridge::get_singleton();
	if (bridge) {
		return bridge;
	}
	// Fallback: try via protocol.
	MCPProtocol *protocol = MCPProtocol::get_singleton();
	if (protocol) {
		return protocol->get_debugger_bridge();
	}
	return nullptr;
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

// Execute GDScript code on the game side and return the result.
// Always uses send_execute_code (not send_evaluate) to access engine singletons.
static Dictionary _execute_game_code(const String &p_code, int p_timeout_ms = 10000) {
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Debugger bridge not available.");
	}
	return bridge->send_execute_code(p_code, p_timeout_ms);
}

// Extract the value from a successful execute_code result.
// The bridge returns "Type: value" format; this strips the type prefix.
static String _extract_value(const Dictionary &p_result) {
	String raw = p_result.get("value", "");
	int colon_pos = raw.find(": ");
	if (colon_pos >= 0) {
		return raw.substr(colon_pos + 2);
	}
	return raw;
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
				"Execute a command in the in-game debug console.\n"
				"\n"
				"Requires a running game with debug instrumentation (auto_expose or manual\n"
				"Debug.register_* calls). Common commands:\n"
				"  'help' — list all available commands\n"
				"  'list cvars' / 'list queries' / 'list actions' — enumerate by category\n"
				"  'query.<name>' — read a query value\n"
				"  'action.<name> param=value' — invoke an action\n"
				"  'set <cvar> <value>' — modify a CVar\n"
				"  'watch query.<name>' — pin to debug overlay\n"
				"\n"
				"Returns the console output text. For structured data, prefer the dedicated\n"
				"console/* tools (console/query, console/invoke, etc.) which return parsed\n"
				"JSON instead of text.",
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
				"Get the complete debug manifest from the running game.\n"
				"\n"
				"Returns a structured JSON object with all registered debug primitives:\n"
				"  - cvars: configuration variables with name, type, min/max, current value\n"
				"  - commands: named console commands\n"
				"  - queries: named read-only values (health, score, state) with return types\n"
				"  - actions: callable operations with parameter schemas\n"
				"  - events: signals being monitored with recent fire counts\n"
				"  - ui_pages: registered debug UI overlay pages\n"
				"\n"
				"This is the primary discovery tool — call it first to understand what the\n"
				"game exposes. If the manifest is empty, the game has no debug instrumentation.\n"
				"Requires a running game. Related: console/query, console/invoke, console/get_cvar.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPConsoleTools::handle_get_manifest));
	}

	// console/query — evaluate a named query
	{
		Dictionary props;
		props["name"] = make_prop("string",
				"Query name (e.g. 'current_state', 'score', 'lives', 'remaining'). "
				"Use console/get_manifest to discover available queries.");
		Array required;
		required.push_back("name");
		p_registry->register_tool(
				"console/query",
				"Evaluate Debug Query",
				"Evaluate a named debug query registered by the game. Returns the current "
				"value of the query (e.g., game state, score, health). Much faster and more "
				"reliable than using runtime/evaluate with Debug.evaluate_query().",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPConsoleTools::handle_query));
	}

	// console/invoke — invoke a named action
	{
		Dictionary props;
		props["name"] = make_prop("string",
				"Action name (e.g. 'add_score', 'set_lives', 'teleport', 'clear_all_instant'). "
				"Use console/get_manifest to discover available actions.");
		props["params"] = make_prop("object",
				"Parameters to pass to the action as a JSON object (e.g. {\"amount\": 50}). "
				"Check the action's params_schema in the manifest for expected parameters.");
		Array required;
		required.push_back("name");
		p_registry->register_tool(
				"console/invoke",
				"Invoke Debug Action",
				"Invoke a named debug action registered by the game. Actions can modify game "
				"state (add score, set lives, teleport, spawn entities, etc.).",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPConsoleTools::handle_invoke_action));
	}

	// console/get_cvar
	{
		Dictionary props;
		props["name"] = make_prop("string",
				"CVar name (e.g. 'ship.speed', 'ball.initial_speed', 'game.starting_lives'). "
				"Use console/get_manifest to discover available CVars.");
		Array required;
		required.push_back("name");
		p_registry->register_tool(
				"console/get_cvar",
				"Get CVar Value",
				"Read the current value of a configuration variable (CVar). Returns the live "
				"value including any runtime modifications.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPConsoleTools::handle_get_cvar));
	}

	// console/set_cvar
	{
		Dictionary props;
		props["name"] = make_prop("string",
				"CVar name (e.g. 'ship.speed', 'ball.initial_speed').");
		props["value"] = make_prop("string",
				"New value as a string. Will be coerced to the CVar's registered type "
				"and clamped to min/max bounds if specified.");
		Array required;
		required.push_back("name");
		required.push_back("value");
		p_registry->register_tool(
				"console/set_cvar",
				"Set CVar Value",
				"Set the value of a configuration variable (CVar). The value is type-coerced "
				"and clamped to registered bounds. For auto_expose CVars, the change is "
				"written through to the actual game object property immediately.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPConsoleTools::handle_set_cvar));
	}

	// console/get_events
	{
		Dictionary props;
		props["count"] = make_prop("integer",
				"Maximum number of recent events to return (default: 20, max: 200).");
		Array required;
		p_registry->register_tool(
				"console/get_events",
				"Get Recent Events",
				"Get the most recent events from the debug event ring buffer. Events are "
				"logged when registered signals fire (e.g., ball_fell, all_bricks_cleared). "
				"Returns newest-first.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPConsoleTools::handle_get_events));
	}

	// console/batch_query
	{
		Dictionary props;
		props["names"] = make_prop("array",
				"Array of query names to evaluate (e.g. ['current_state', 'score', 'lives']). "
				"All queries are evaluated in a single round-trip.");
		Array required;
		required.push_back("names");
		p_registry->register_tool(
				"console/batch_query",
				"Batch Evaluate Queries",
				"Evaluate multiple debug queries in a single call. Returns a dictionary mapping "
				"each query name to its current value. Much more efficient than calling "
				"console/query multiple times.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPConsoleTools::handle_batch_query));
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

	// Execute via the running game's DebugConsole singleton.
	// Must use execute_code (not evaluate) because the Expression evaluator
	// cannot resolve engine singletons like DebugConsole.
	String code = vformat(
			"_result = DebugConsole.get_singleton()._execute_command_string(\"%s\")",
			command.c_escape());

	Dictionary result = _execute_game_code(code, 15000);

	if (result.has("error")) {
		return make_tool_error(String(result["error"]));
	}

	if (!(bool)result.get("success", false)) {
		return make_tool_error("Console execute failed: " + String(result.get("value", "")));
	}

	String output = _extract_value(result);
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
	if (!bridge) {
		return make_tool_error("Debugger bridge not available.");
	}

	Dictionary result = bridge->send_debug_command("debug_get_manifest", Array(), 10000);

	if (!(bool)result.get("success", false)) {
		return make_tool_error("get_manifest() failed: " + String(result.get("value", "")));
	}

	String json_str = result.get("value", "");

	if (json_str.is_empty()) {
		return make_tool_error("get_manifest() returned empty result. Is the Debug singleton available?");
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

// ---------------------------------------------------------------------------
// console/query
// ---------------------------------------------------------------------------

Dictionary MCPConsoleTools::handle_query(const Dictionary &p_args) {
	Dictionary err = _require_game_running();
	if (!err.is_empty()) {
		return err;
	}

	String name = p_args.get("name", "");
	if (name.is_empty()) {
		return make_tool_error("Missing required argument: name");
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Debugger bridge not available.");
	}

	Array data;
	data.push_back(name);
	Dictionary result = bridge->send_debug_command("debug_query", data, 10000);

	if (!(bool)result.get("success", false)) {
		return make_tool_error("Query '" + name + "' failed: " + String(result.get("value", "")));
	}

	String value = _extract_value(result);

	Dictionary structured;
	structured["name"] = name;
	structured["value"] = value;

	return make_tool_result(name + " = " + value, structured);
}

// ---------------------------------------------------------------------------
// console/invoke
// ---------------------------------------------------------------------------

Dictionary MCPConsoleTools::handle_invoke_action(const Dictionary &p_args) {
	Dictionary err = _require_game_running();
	if (!err.is_empty()) {
		return err;
	}

	String name = p_args.get("name", "");
	if (name.is_empty()) {
		return make_tool_error("Missing required argument: name");
	}

	Dictionary params;
	if (p_args.has("params")) {
		params = p_args["params"];
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Debugger bridge not available.");
	}

	Array data;
	data.push_back(name);
	data.push_back(JSON::stringify(params));
	Dictionary result = bridge->send_debug_command("debug_invoke_action", data, 15000);

	if (!(bool)result.get("success", false)) {
		return make_tool_error("Action '" + name + "' failed: " + String(result.get("value", "")));
	}

	String value = result.get("value", "");

	Dictionary structured;
	structured["name"] = name;
	structured["result"] = value;

	return make_tool_result("Action '" + name + "': " + value, structured);
}

// ---------------------------------------------------------------------------
// console/get_cvar
// ---------------------------------------------------------------------------

Dictionary MCPConsoleTools::handle_get_cvar(const Dictionary &p_args) {
	Dictionary err = _require_game_running();
	if (!err.is_empty()) {
		return err;
	}

	String name = p_args.get("name", "");
	if (name.is_empty()) {
		return make_tool_error("Missing required argument: name");
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Debugger bridge not available.");
	}

	Array data;
	data.push_back(name);
	Dictionary result = bridge->send_debug_command("debug_get_cvar", data, 10000);

	if (!(bool)result.get("success", false)) {
		return make_tool_error("get_cvar '" + name + "' failed: " + String(result.get("value", "")));
	}

	String value = _extract_value(result);

	Dictionary structured;
	structured["name"] = name;
	structured["value"] = value;

	return make_tool_result(name + " = " + value, structured);
}

// ---------------------------------------------------------------------------
// console/set_cvar
// ---------------------------------------------------------------------------

Dictionary MCPConsoleTools::handle_set_cvar(const Dictionary &p_args) {
	Dictionary err = _require_game_running();
	if (!err.is_empty()) {
		return err;
	}

	String name = p_args.get("name", "");
	if (name.is_empty()) {
		return make_tool_error("Missing required argument: name");
	}

	String value_str = p_args.get("value", "");
	if (value_str.is_empty()) {
		return make_tool_error("Missing required argument: value");
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Debugger bridge not available.");
	}

	Array data;
	data.push_back(name);
	data.push_back(value_str);
	Dictionary result = bridge->send_debug_command("debug_set_cvar", data, 10000);

	if (!(bool)result.get("success", false)) {
		return make_tool_error("set_cvar '" + name + "' failed: " + String(result.get("value", "")));
	}

	String confirmed_value = _extract_value(result);

	Dictionary structured;
	structured["name"] = name;
	structured["value"] = confirmed_value;
	structured["requested"] = value_str;

	return make_tool_result(name + " = " + confirmed_value + " (was: " + value_str + ")", structured);
}

// ---------------------------------------------------------------------------
// console/get_events
// ---------------------------------------------------------------------------

Dictionary MCPConsoleTools::handle_get_events(const Dictionary &p_args) {
	Dictionary err = _require_game_running();
	if (!err.is_empty()) {
		return err;
	}

	int count = (int)p_args.get("count", 20);
	count = CLAMP(count, 1, 200);

	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Debugger bridge not available.");
	}

	Array data;
	data.push_back(count);
	Dictionary result = bridge->send_debug_command("debug_get_events", data, 10000);

	if (!(bool)result.get("success", false)) {
		return make_tool_error("get_events failed: " + String(result.get("value", "")));
	}

	String json_str = result.get("value", "");

	// Parse JSON array.
	JSON json;
	Array events_array;
	if (json.parse(json_str) == OK && json.get_data().get_type() == Variant::ARRAY) {
		events_array = json.get_data();
	}

	String text;
	if (events_array.is_empty()) {
		text = "No recent events.";
	} else {
		text = vformat("%d recent event(s):\n", events_array.size());
		for (int i = 0; i < events_array.size(); i++) {
			Dictionary ev = events_array[i];
			text += vformat("  [frame %d] %s\n",
					(int64_t)ev.get("frame", 0),
					String(ev.get("name", "")));
		}
	}

	Dictionary structured;
	structured["events"] = events_array;
	structured["count"] = events_array.size();

	return make_tool_result(text, structured);
}

// ---------------------------------------------------------------------------
// console/batch_query
// ---------------------------------------------------------------------------

Dictionary MCPConsoleTools::handle_batch_query(const Dictionary &p_args) {
	Dictionary err = _require_game_running();
	if (!err.is_empty()) {
		return err;
	}

	Array names;
	Variant names_var = p_args.get("names", Variant());
	if (names_var.get_type() == Variant::ARRAY) {
		names = names_var;
	} else {
		return make_tool_error("Missing or invalid argument: names (expected array of strings)");
	}

	if (names.is_empty()) {
		return make_tool_error("names array is empty");
	}

	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Debugger bridge not available.");
	}

	// Send as PackedStringArray.
	PackedStringArray name_arr;
	for (int i = 0; i < names.size(); i++) {
		name_arr.push_back(names[i]);
	}
	Array data;
	data.push_back(name_arr);
	Dictionary result = bridge->send_debug_command("debug_batch_query", data, 15000);

	if (!(bool)result.get("success", false)) {
		return make_tool_error("batch_query failed: " + String(result.get("value", "")));
	}

	String json_str = result.get("value", "");

	// Parse JSON dict.
	JSON json;
	Dictionary values;
	if (json.parse(json_str) == OK && json.get_data().get_type() == Variant::DICTIONARY) {
		values = json.get_data();
	}

	String text;
	for (int i = 0; i < names.size(); i++) {
		String name = names[i];
		String val = values.has(name) ? String(values[name]) : "<not found>";
		text += name + " = " + val + "\n";
	}

	Dictionary structured;
	structured["values"] = values;
	structured["count"] = names.size();

	return make_tool_result(text.strip_edges(), structured);
}
