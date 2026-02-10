/**************************************************************************/
/*  mcp_signal_tools.cpp                                                  */
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

#include "mcp_signal_tools.h"

#include "../mcp_debugger_bridge.h"
#include "../mcp_protocol.h"
#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/input/shortcut.h"
#include "core/object/script_language.h"

// ============================================================================
// Common Helpers
// ============================================================================

MCPDebuggerBridge *MCPSignalTools::_get_bridge() {
	MCPProtocol *protocol = MCPProtocol::get_singleton();
	if (!protocol) {
		return nullptr;
	}
	return protocol->get_debugger_bridge();
}

Dictionary MCPSignalTools::_require_game_running() {
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Internal error: debugger bridge not available.");
	}
	if (!bridge->is_game_running()) {
		return make_tool_error(
				"No game is currently running.\n\n"
				"If the game stopped unexpectedly, check debug/get_errors for runtime errors.\n"
				"To start a game: debug/run_project (main scene) or debug/run_scene (specific scene).\n"
				"To check status: debug/get_status (includes stop_reason when stopped).");
	}
	return Dictionary();
}

// ============================================================================
// Tool Registration
// ============================================================================

void MCPSignalTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ---- debug/get_node_signals ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Absolute node path in the running scene tree. "
				"Example: '/root/Player' or '/root/Main/HUD'");
		props["include_inherited"] = make_prop("boolean",
				"Whether to include signals inherited from parent classes "
				"(default: true). Set to false to see only signals defined "
				"on the node's own script or class.");
		props["signal_name"] = make_prop("string",
				"Optional: filter to a specific signal name. "
				"If provided, only that signal's info is returned.");
		Array required;
		required.push_back("node_path");
		p_registry->register_tool(
				"debug/get_node_signals", "Get Node Signals",
				"Get signals on a runtime node with argument definitions and "
				"connection info. Shows signal names, parameter types, source "
				"(script/class/user), and all connected methods with flags.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPSignalTools::handle_get_node_signals));
	}

	// ---- debug/emit_signal ----
	{
		Dictionary props;
		props["node_path"] = make_prop("string",
				"Absolute node path in the running scene tree. "
				"Example: '/root/Player' or '/root/Main/HUD'");
		props["signal_name"] = make_prop("string",
				"Name of the signal to emit. "
				"Example: 'health_changed' or 'item_collected'");
		Dictionary args_prop = make_prop("array",
				"Optional array of arguments to pass with the signal. "
				"Arguments are passed in order to all connected methods. "
				"Example: [50, \"damage\"]");
		props["args"] = args_prop;
		Array required;
		required.push_back("node_path");
		required.push_back("signal_name");
		p_registry->register_tool(
				"debug/emit_signal", "Emit Signal",
				"Emit a signal on a runtime node, triggering all connected "
				"methods. Use debug/get_node_signals first to discover available "
				"signals and their expected arguments.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/false),
				callable_mp_static(&MCPSignalTools::handle_emit_signal));
	}
}

// ============================================================================
// handle_get_node_signals
// ============================================================================

Dictionary MCPSignalTools::handle_get_node_signals(const Dictionary &p_args) {
	// 1. Check game is running.
	Dictionary err = _require_game_running();
	if (!err.is_empty()) {
		return err;
	}

	// 2. Validate node_path.
	String node_path = p_args.get("node_path", "");
	if (node_path.is_empty()) {
		return make_tool_error("'node_path' is required and cannot be empty.");
	}

	// Validate characters: alphanumeric + _/.@:- and space.
	for (int i = 0; i < node_path.length(); i++) {
		char32_t c = node_path[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
					(c >= '0' && c <= '9') || c == '_' || c == '/' ||
					c == '.' || c == '@' || c == ':' || c == '-' || c == ' ')) {
			return make_tool_error(vformat(
					"'node_path' contains invalid character '%c' at position %d. "
					"Allowed: alphanumeric, _ / . @ : - and space.",
					(char)c, i));
		}
	}

	// 3. Get optional parameters.
	bool include_inherited = p_args.get("include_inherited", true);
	String signal_name = p_args.get("signal_name", "");

	// 4. Send request to the running game via the debugger bridge.
	MCPDebuggerBridge *bridge = _get_bridge();
	Dictionary result = bridge->send_get_node_signals(node_path, include_inherited, signal_name);

	// 5. Check for errors.
	if (!(bool)result.get("success", false)) {
		String error_msg = result.get("error", "Failed to get node signals.");
		return make_tool_error(error_msg);
	}

	// 6. Build text output.
	String node_type = result.get("node_type", "Node");
	Array signals = result.get("signals", Array());

	String text = vformat("Signals for %s (%s): %d signal(s)",
			node_path, node_type, signals.size());

	for (int i = 0; i < signals.size(); i++) {
		Dictionary sig = signals[i];
		String sig_name = sig.get("name", "");
		Array args = sig.get("arguments", Array());
		String source = sig.get("origin", "");

		// Build argument list.
		String args_str;
		for (int j = 0; j < args.size(); j++) {
			Dictionary arg = args[j];
			String arg_name = arg.get("name", "");
			String arg_type = arg.get("type", "");
			if (j > 0) {
				args_str += ", ";
			}
			args_str += arg_name + ": " + arg_type;
		}

		text += vformat("\n  %s(%s)", sig_name, args_str);
		if (!source.is_empty()) {
			text += vformat(" [%s]", source);
		}

		// Show connections.
		Array connections = sig.get("connections", Array());
		for (int j = 0; j < connections.size(); j++) {
			Dictionary conn = connections[j];
			String target = conn.get("target_path", "");
			String method = conn.get("method", "");
			String flags_str = conn.get("flags_desc", "");

			text += vformat("\n    -> %s::%s", target, method);
			if (!flags_str.is_empty()) {
				text += vformat(" [%s]", flags_str);
			}
		}
	}

	// 7. Build structured output.
	Dictionary structured;
	structured["node_path"] = node_path;
	structured["node_type"] = node_type;
	structured["signal_count"] = signals.size();
	structured["signals"] = signals;

	return make_tool_result(text, structured);
}

// ============================================================================
// handle_emit_signal
// ============================================================================

Dictionary MCPSignalTools::handle_emit_signal(const Dictionary &p_args) {
	// 1. Check game is running.
	Dictionary err = _require_game_running();
	if (!err.is_empty()) {
		return err;
	}

	// 2. Validate node_path.
	String node_path = p_args.get("node_path", "");
	if (node_path.is_empty()) {
		return make_tool_error("'node_path' is required and cannot be empty.");
	}

	// Validate characters: alphanumeric + _/.@:- and space.
	for (int i = 0; i < node_path.length(); i++) {
		char32_t c = node_path[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
					(c >= '0' && c <= '9') || c == '_' || c == '/' ||
					c == '.' || c == '@' || c == ':' || c == '-' || c == ' ')) {
			return make_tool_error(vformat(
					"'node_path' contains invalid character '%c' at position %d. "
					"Allowed: alphanumeric, _ / . @ : - and space.",
					(char)c, i));
		}
	}

	// 3. Validate signal_name.
	String signal_name = p_args.get("signal_name", "");
	if (signal_name.is_empty()) {
		return make_tool_error("'signal_name' is required and cannot be empty.");
	}

	// Signal names: alphanumeric + underscore only.
	for (int i = 0; i < signal_name.length(); i++) {
		char32_t c = signal_name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
					(c >= '0' && c <= '9') || c == '_')) {
			return make_tool_error(vformat(
					"'signal_name' contains invalid character '%c' at position %d. "
					"Allowed: alphanumeric and underscore only.",
					(char)c, i));
		}
	}

	// 4. Get args array (default empty).
	Array args = p_args.get("args", Array());

	// 5. Send request to the running game via the debugger bridge.
	MCPDebuggerBridge *bridge = _get_bridge();
	Dictionary result = bridge->send_emit_signal(node_path, signal_name, args);

	// 6. Check for errors.
	if (!(bool)result.get("success", false)) {
		String error_msg = result.get("error", "Failed to emit signal.");
		return make_tool_error(error_msg);
	}

	// 7. Build text output.
	int connections_triggered = result.get("connections_triggered", 0);
	String text = vformat("Signal '%s' emitted on %s\nConnections triggered: %d",
			signal_name, node_path, connections_triggered);

	// 8. Build structured output.
	Dictionary structured;
	structured["node_path"] = node_path;
	structured["signal_name"] = signal_name;
	structured["connections_triggered"] = connections_triggered;
	structured["success"] = true;

	return make_tool_result(text, structured);
}
