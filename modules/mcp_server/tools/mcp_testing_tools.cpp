/**************************************************************************/
/*  mcp_testing_tools.cpp                                                 */
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

#include "mcp_testing_tools.h"

#include "../mcp_debugger_bridge.h"
#include "../mcp_progress.h"
#include "../mcp_protocol.h"
#include "../mcp_tool_registry.h"
#include "../mcp_types.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/script_language.h"
#include "editor/run/editor_run_bar.h"
#include "modules/gdscript/gdscript.h"

// ============================================================================
// Embedded runner file contents.
//
// These are copied into res://addons/mcp_test/ before each test run so that
// the game process can load them. They are cleaned up after the run completes.
// ============================================================================

static const char *RUNNER_TSCN_CONTENT = R"GD([gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://addons/mcp_test/runner.gd" id="1"]

[node name="MCPTestRunner" type="Node"]
script = ExtResource("1")
)GD";

static const char *RUNNER_GD_CONTENT = R"GD(# res://addons/mcp_test/runner.gd
# MCP Test Runner -- launched by the test/run tool.
# Reads the test manifest, executes test methods, reports results via debugger messages.
extends Node

const TestContext = preload("res://addons/mcp_test/test_context.gd")

var _results: Array = []
var _total := 0
var _passed := 0
var _failed := 0
var _errors := 0
var _skipped := 0
var _start_time := 0

func _ready():
	_start_time = Time.get_ticks_msec()

	var manifest := _load_manifest()
	if manifest.is_empty():
		_send_complete()
		get_tree().quit()
		return

	var files: Array = manifest.get("files", [])
	var filter: String = manifest.get("filter", "")
	var method_timeout: int = manifest.get("method_timeout_ms", 5000)
	var total_timeout: int = manifest.get("timeout_ms", 30000)

	for file_path in files:
		var elapsed := Time.get_ticks_msec() - _start_time
		if elapsed >= total_timeout:
			# Remaining files get timeout status.
			_send_method_result(file_path, "<timeout>", "timeout",
				"Total test run timeout exceeded (%dms)" % total_timeout,
				"", 0, 0, [])
			_errors += 1
			_total += 1
			continue
		await _run_test_file(file_path, filter, method_timeout)

	_send_complete()
	# Small delay to ensure messages are flushed before quit.
	await get_tree().create_timer(0.1).timeout
	get_tree().quit()

func _load_manifest() -> Dictionary:
	var path := "user://mcp_test_manifest.json"
	if not FileAccess.file_exists(path):
		push_error("MCP test manifest not found: " + path)
		return {}
	var file := FileAccess.open(path, FileAccess.READ)
	var json := JSON.new()
	if json.parse(file.get_as_text()) != OK:
		push_error("Failed to parse test manifest: " + json.get_error_message())
		return {}
	return json.data

func _run_test_file(file_path: String, filter: String, method_timeout: int):
	var script := load(file_path) as GDScript
	if script == null:
		_send_method_result(file_path, "<load>", "error",
			"Failed to load script: " + file_path, file_path, 0, 0, [])
		_errors += 1
		_total += 1
		return

	# Get test methods.
	var methods := _get_test_methods(script)
	if not filter.is_empty():
		methods = methods.filter(func(m): return m.match(filter))

	# Instantiate.
	var instance = script.new()
	var is_node := instance is Node
	if is_node:
		add_child(instance)

	# Inject test context.
	var ctx := TestContext.new()
	ctx._scene_tree = get_tree()
	if "_test" in instance:
		instance._test = ctx
	else:
		instance.set("_test", ctx)

	# before_all
	if instance.has_method("before_all"):
		instance.before_all()

	for method_name in methods:
		_total += 1
		ctx._reset()

		# before_each
		if instance.has_method("before_each"):
			instance.before_each()

		var method_start := Time.get_ticks_msec()
		var status := "passed"
		var message := ""
		var error_file := ""
		var error_line := 0

		# Execute test method with error catching.
		var result = instance.call(method_name)

		var duration := Time.get_ticks_msec() - method_start

		# Check context for failures.
		if ctx._skipped:
			status = "skipped"
			message = ctx._skip_reason
			_skipped += 1
		elif ctx._failed:
			status = "failed"
			message = ctx._failure_message
			error_file = ctx._failure_file
			error_line = ctx._failure_line
			_failed += 1
		else:
			_passed += 1

		# after_each
		if instance.has_method("after_each"):
			instance.after_each()

		ctx._cleanup()

		_send_method_result(file_path, method_name, status, message,
			error_file, error_line, duration, ctx._output)

	# after_all
	if instance.has_method("after_all"):
		instance.after_all()

	# Cleanup.
	if is_node:
		instance.queue_free()
		await get_tree().process_frame

func _get_test_methods(script: GDScript) -> Array:
	var methods := []
	for m in script.get_script_method_list():
		if m.name.begins_with("test_"):
			methods.append(m.name)
	methods.sort()
	return methods

func _send_method_result(file: String, method: String, status: String,
		message: String, error_file: String, error_line: int,
		duration_ms: int, output: Array):
	EngineDebugger.send_message("mcp:test_method_result", [
		file, method, status, message, error_file, error_line,
		duration_ms, output
	])

func _send_complete():
	var duration := Time.get_ticks_msec() - _start_time
	EngineDebugger.send_message("mcp:test_complete", [
		_total, _passed, _failed, _errors, _skipped, duration
	])
)GD";

static const char *TEST_CONTEXT_GD_CONTENT = R"GD(# res://addons/mcp_test/test_context.gd
# Assertion helper object injected as _test into test instances.
class_name MCPTestContext
extends RefCounted

# --- Internal state (reset per test method) ---
var _failed := false
var _failure_message := ""
var _failure_file := ""
var _failure_line := 0
var _skipped := false
var _skip_reason := ""
var _pending := false
var _pending_message := ""
var _output := []
var _watched_signals := {}
var _autofree_objects := []
var _autoqfree_nodes := []
var _orphan_count_before := 0
var _scene_tree: SceneTree

func _reset():
	_failed = false
	_failure_message = ""
	_failure_file = ""
	_failure_line = 0
	_skipped = false
	_skip_reason = ""
	_pending = false
	_pending_message = ""
	_output = []
	_watched_signals = {}
	_autofree_objects = []
	_autoqfree_nodes = []
	_orphan_count_before = Performance.get_monitor(Performance.OBJECT_ORPHAN_NODE_COUNT)

func _cleanup():
	for obj in _autofree_objects:
		if is_instance_valid(obj):
			obj.free()
	for node in _autoqfree_nodes:
		if is_instance_valid(node):
			node.queue_free()
	_autofree_objects.clear()
	_autoqfree_nodes.clear()

func _record_failure(message: String):
	if _failed:
		return
	_failed = true
	_failure_message = message
	var stack := get_stack()
	for i in range(1, stack.size()):
		if not stack[i].source.ends_with("test_context.gd"):
			_failure_file = stack[i].source
			_failure_line = stack[i].line
			break

# --- Assertions ---

func assert_eq(a, b, msg := ""):
	if a != b:
		_record_failure(_fmt("Expected %s == %s", [a, b], msg))

func assert_ne(a, b, msg := ""):
	if a == b:
		_record_failure(_fmt("Expected %s != %s", [a, b], msg))

func assert_true(val, msg := ""):
	if not val:
		_record_failure(_fmt("Expected true, got %s", [val], msg))

func assert_false(val, msg := ""):
	if val:
		_record_failure(_fmt("Expected false, got %s", [val], msg))

func assert_gt(a, b, msg := ""):
	if not (a > b):
		_record_failure(_fmt("Expected %s > %s", [a, b], msg))

func assert_lt(a, b, msg := ""):
	if not (a < b):
		_record_failure(_fmt("Expected %s < %s", [a, b], msg))

func assert_gte(a, b, msg := ""):
	if not (a >= b):
		_record_failure(_fmt("Expected %s >= %s", [a, b], msg))

func assert_lte(a, b, msg := ""):
	if not (a <= b):
		_record_failure(_fmt("Expected %s <= %s", [a, b], msg))

func assert_null(val, msg := ""):
	if val != null:
		_record_failure(_fmt("Expected null, got %s", [val], msg))

func assert_not_null(val, msg := ""):
	if val == null:
		_record_failure(_fmt("Expected non-null value", [], msg))

func assert_has(collection, item, msg := ""):
	if item not in collection:
		_record_failure(_fmt("Expected collection to contain %s", [item], msg))

func assert_almost_eq(a, b, epsilon := 0.001, msg := ""):
	var diff
	if a is Vector2 or a is Vector3:
		diff = (a - b).length()
	else:
		diff = abs(a - b)
	if diff >= epsilon:
		_record_failure(_fmt("Expected %s ~= %s (diff %s, epsilon %s)", [a, b, diff, epsilon], msg))

func assert_typeof(val, type: int, msg := ""):
	if typeof(val) != type:
		_record_failure(_fmt("Expected type %d, got %d", [type, typeof(val)], msg))

func assert_no_new_orphans(msg := ""):
	var current := Performance.get_monitor(Performance.OBJECT_ORPHAN_NODE_COUNT)
	var new_orphans := current - _orphan_count_before
	if new_orphans > 0:
		_record_failure(_fmt("Test leaked %d orphan node(s)", [new_orphans], msg))

# --- Signals ---

func watch_signals(obj: Object):
	var id := obj.get_instance_id()
	_watched_signals[id] = {}
	for s in obj.get_signal_list():
		var sig_name: String = s.name
		_watched_signals[id][sig_name] = []
		var captured_id := id
		var captured_name := sig_name
		obj.connect(sig_name, func():
			_watched_signals[captured_id][captured_name].append([]))

func assert_signal_emitted(obj: Object, signal_name: String, msg := ""):
	var emissions := _get_emissions(obj, signal_name)
	if emissions == null: return
	if emissions.is_empty():
		_record_failure(_fmt("Expected signal '%s' to be emitted", [signal_name], msg))

func assert_signal_not_emitted(obj: Object, signal_name: String, msg := ""):
	var emissions := _get_emissions(obj, signal_name)
	if emissions == null: return
	if not emissions.is_empty():
		_record_failure(_fmt("Expected signal '%s' NOT to be emitted", [signal_name], msg))

func assert_signal_emitted_with(obj: Object, signal_name: String, expected_params: Array, msg := ""):
	var emissions := _get_emissions(obj, signal_name)
	if emissions == null: return
	for emission in emissions:
		if emission == expected_params:
			return
	_record_failure(_fmt("Signal '%s' never emitted with %s", [signal_name, expected_params], msg))

func _get_emissions(obj: Object, signal_name: String):
	var id := obj.get_instance_id()
	if id not in _watched_signals:
		_record_failure("Signal not watched. Call _test.watch_signals(obj) first.")
		return null
	if signal_name not in _watched_signals[id]:
		_record_failure("Unknown signal: " + signal_name)
		return null
	return _watched_signals[id][signal_name]

# --- Lifecycle ---

func autofree(obj: Object) -> Object:
	_autofree_objects.append(obj)
	return obj

func autoqfree(node: Node) -> Node:
	_autoqfree_nodes.append(node)
	return node

func add_child_autofree(node: Node) -> Node:
	_scene_tree.current_scene.add_child(node)
	_autoqfree_nodes.append(node)
	return node

func skip(reason := ""):
	_skipped = true
	_skip_reason = reason

func pending(msg := ""):
	_pending = true
	_pending_message = msg

func fail(msg: String):
	_record_failure(msg)

# --- Async ---

func wait_for_signal(sig: Signal, max_wait: float, msg := "") -> bool:
	var timer := _scene_tree.create_timer(max_wait)
	var result := await _race_signal(sig, timer.timeout)
	if not result and msg.is_empty():
		msg = "Signal not emitted within %ss" % max_wait
	if not result:
		_record_failure(msg)
	return result

func wait_seconds(time: float, _msg := ""):
	await _scene_tree.create_timer(time).timeout

func wait_frames(count: int, _msg := ""):
	for i in count:
		await _scene_tree.process_frame

func _race_signal(wanted: Signal, timeout: Signal) -> bool:
	var done := false
	var timed_out := false
	wanted.connect(func(): done = true, CONNECT_ONE_SHOT)
	timeout.connect(func(): timed_out = true, CONNECT_ONE_SHOT)
	while not done and not timed_out:
		await _scene_tree.process_frame
	return done

# --- Internal ---

func _fmt(template: String, args: Array, msg: String) -> String:
	var m := template % args if args.size() > 0 else template
	if msg: m += " | " + msg
	return m
)GD";

static const char *GDIGNORE_CONTENT = "";

// ============================================================================
// Tool Registration
// ============================================================================

void MCPTestingTools::register_tools(MCPToolRegistry *p_registry) {
	ERR_FAIL_NULL(p_registry);

	// ---- testing/run (was test/run) ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Path to a test file or directory. If a directory, all files matching "
				"'test_*.gd' are collected recursively. Must start with 'res://'. "
				"Examples: 'res://tests/test_inventory.gd', 'res://tests/', 'res://tests/unit/'.");
		props["filter"] = make_prop("string",
				"Optional glob or substring to filter which test methods to run within "
				"matched files. Example: 'test_add*' runs only methods starting with "
				"'test_add'. Default: run all test_* methods.");
		props["timeout"] = make_prop("integer",
				"Maximum time in seconds for the entire test run. If exceeded, the game "
				"is killed and remaining tests are reported as timed out. Default: 30. Max: 120.");
		props["verbose"] = make_prop("boolean",
				"If true, include captured print() output per test method. If false, only "
				"include output for failed/errored tests. Default: false.");
		Array required;
		required.push_back("path");
		p_registry->register_tool(
				"testing/run", "Run Tests",
				"Run GDScript tests to verify code behavior. Write test files in res://tests/ "
				"named test_*.gd with test_*() methods. Each method should test one behavior.\n\n"
				"WORKFLOW:\n"
				"1. Write your code with native file tools, then call editor/scan_filesystem\n"
				"2. Write a test file with native file tools, then call editor/scan_filesystem\n"
				"3. Check for compile errors (testing/check_script)\n"
				"4. Run the tests (testing/run)\n"
				"5. Fix failures and repeat\n\n"
				"TEST FILE FORMAT:\n"
				"  extends RefCounted\n\n"
				"  func test_example():\n"
				"      var result = my_function(2, 3)\n"
				"      _test.assert_eq(result, 5, \"2 + 3 should equal 5\")\n\n"
				"ASSERTIONS: Use _test.assert_eq(), assert_true(), assert_gt(), "
				"assert_almost_eq(), assert_has(), assert_signal_emitted(), etc. "
				"Use _test over GDScript's built-in assert() for better error messages.\n\n"
				"TIPS:\n"
				"- Extend RefCounted for pure logic tests (fastest)\n"
				"- Extend Node if you need scene tree access (signals, process, etc.)\n"
				"- Use before_each()/after_each() for setup/teardown\n"
				"- Keep tests fast -- avoid loading heavy scenes/textures\n"
				"- One assertion concept per test method",
				make_schema(props, required),
				make_annotations(/*readOnly=*/false, /*destructive=*/false,
						/*idempotent=*/true, /*openWorld=*/false),
				callable_mp_static(&MCPTestingTools::handle_run));
	}

	// ---- testing/list (was test/list) ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Directory to scan for test files. Default: 'res://tests/'. "
				"Must start with 'res://'.");
		props["include_methods"] = make_prop("boolean",
				"If true, parse each file and list the test_*() methods. "
				"If false, only list file paths. Default: true.");
		Array required;
		p_registry->register_tool(
				"testing/list", "List Tests",
				"Discover test files and their test methods. Scans for files matching test_*.gd.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false,
						/*idempotent=*/true, /*openWorld=*/false),
				callable_mp_static(&MCPTestingTools::handle_list));
	}

	// ---- testing/check_script (was gdscript/check_errors) ----
	{
		Dictionary props;
		props["path"] = make_prop("string",
				"Path to the GDScript file in res:// format (e.g., res://scripts/player.gd)");
		Array required;
		required.push_back("path");
		p_registry->register_tool(
				"testing/check_script",
				"Check GDScript Errors",
				"Validate a single GDScript file for compile errors and warnings. Returns all "
				"errors and warnings with file path, line number, column, and message. Use this "
				"after modifying a .gd file to check for mistakes before running. Reads the file "
				"fresh from disk each time (not from editor cache). Path must use res:// format.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPTestingTools::handle_check_script));
	}

	// ---- testing/check_all_scripts (was gdscript/check_all) ----
	{
		Dictionary props;
		props["include_warnings"] = make_prop("boolean",
				"Whether to include warnings in the output (default: false, only errors)");
		props["page"] = make_prop("integer",
				"Page number (1-based) for paginated results. Each page shows up to 10 files with issues. Default: 1");
		Array required;
		p_registry->register_tool(
				"testing/check_all_scripts",
				"Check All GDScript Files",
				"Validate every .gd file in the project for compile errors. Returns summary "
				"counts (total, errors, warnings, clean) plus details for up to 10 files with "
				"issues per page (5 errors max per file). Clean files are counted but not listed. "
				"Use 'page' parameter to see more results. "
				"Use testing/check_script on individual files for full error details. "
				"Useful for a project-wide health check after large changes. Optionally includes warnings.",
				make_schema(props, required),
				make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
				callable_mp_static(&MCPTestingTools::handle_check_all_scripts),
				&MCPTestingTools::handle_check_all_scripts_with_progress);
	}
}

// ============================================================================
// Common Helpers
// ============================================================================

MCPDebuggerBridge *MCPTestingTools::_get_bridge() {
	MCPProtocol *protocol = MCPProtocol::get_singleton();
	if (!protocol) {
		return nullptr;
	}
	return protocol->get_debugger_bridge();
}

// ============================================================================
// Tool: testing/run
// ============================================================================

Dictionary MCPTestingTools::handle_run(const Dictionary &p_args) {
	String path = p_args.get("path", "");
	String filter = p_args.get("filter", "");
	int timeout = CLAMP((int)p_args.get("timeout", 30), 1, 120);
	bool verbose = p_args.get("verbose", false);

	// 1. Validate path.
	if (path.is_empty()) {
		return make_tool_error(
				"Missing required parameter: path\n\n"
				"Provide a test file or directory path in res:// format.\n"
				"Examples: 'res://tests/test_inventory.gd', 'res://tests/'");
	}
	if (!path.begins_with("res://")) {
		return make_tool_error(
				"Path must start with 'res://'. Got: " + path + "\n\n"
				"Example: 'res://tests/test_inventory.gd'");
	}
	if (!validate_path(path)) {
		return make_tool_error(vformat(
				"Invalid path: %s\n\n"
				"Paths must start with res:// and cannot contain '..' traversal sequences.",
				path));
	}

	// 2. Collect test files.
	Vector<String> test_files;
	if (path.ends_with(".gd")) {
		// Single file.
		if (!FileAccess::exists(path)) {
			return make_tool_error("File not found: " + path);
		}
		if (!path.get_file().begins_with("test_")) {
			return make_tool_error(
					"Test files must be named 'test_*.gd'. Got: " + path.get_file() +
					"\nRename to: " + path.get_base_dir().path_join("test_" + path.get_file()));
		}
		test_files.push_back(path);
	} else {
		// Directory: collect test_*.gd recursively.
		String dir_path = path.ends_with("/") ? path : path + "/";
		if (!DirAccess::dir_exists_absolute(dir_path)) {
			return make_tool_error(
					"Directory not found: " + dir_path + "\n\n"
					"Create it and add test files named 'test_*.gd'.");
		}
		test_files = _collect_test_files(dir_path);
		if (test_files.is_empty()) {
			return make_tool_error(
					"No test files found in " + dir_path +
					"\nTest files must be named 'test_*.gd'." +
					"\nExample: " + dir_path + "test_example.gd");
		}
	}

	// 3. Compile-check all test files.
	Array compile_results;
	bool any_compile_fail = false;
	for (const String &file : test_files) {
		Dictionary validation = _validate_single_file(file);
		if (!(bool)validation["valid"]) {
			any_compile_fail = true;
		}
		compile_results.push_back(validation);
	}

	// 4. If any compile errors, push events and return immediately.
	MCPDebuggerBridge *bridge = _get_bridge();
	if (!bridge) {
		return make_tool_error("Internal error: debugger bridge not available.");
	}

	if (any_compile_fail) {
		int run_number = bridge->get_current_test_run_number();
		// Push compile error events to the bridge for the status panel.
		for (int i = 0; i < compile_results.size(); i++) {
			Dictionary cr = compile_results[i];
			if (!(bool)cr["valid"]) {
				bridge->push_test_compile_error(cr["path"], cr["errors"], run_number);
			}
		}
		return _build_compile_error_response(compile_results, run_number);
	}

	// 5. Check game not already running.
	if (bridge->is_game_running()) {
		return make_tool_error(
				"Game is already running. Stop it with runtime/stop before running tests.\n"
				"Tests launch a separate game instance for execution.");
	}

	// 6. Write test manifest to user://mcp_test_manifest.json.
	Dictionary manifest;
	Array file_array;
	for (const String &f : test_files) {
		file_array.push_back(f);
	}
	manifest["files"] = file_array;
	manifest["filter"] = filter;
	manifest["timeout_ms"] = timeout * 1000;
	manifest["verbose"] = verbose;
	manifest["method_timeout_ms"] = 5000;

	String manifest_json = JSON::stringify(manifest, "  ");
	{
		Ref<FileAccess> fa = FileAccess::open("user://mcp_test_manifest.json", FileAccess::WRITE);
		if (fa.is_null()) {
			return make_tool_error("Failed to write test manifest to user://mcp_test_manifest.json");
		}
		fa->store_string(manifest_json);
	}

	// 7. Copy runner files into the project.
	_copy_runner_files_to_project();

	// 8. Set launching state and reset test run state.
	bridge->set_game_launching();
	bridge->reset_test_run_state();

	// 9. Create pending request for the test run result.
	// NOTE: create_pending() and wait_for_pending() are public wrappers
	// around the bridge's pending request mechanism, added alongside the
	// test runner bridge integration (reset_test_run_state, etc.).
	PendingRequest *req = bridge->create_pending("test_run");

	// 10. Launch the test runner scene via EditorRunBar (deferred to main thread).
	callable_mp(EditorRunBar::get_singleton(),
			&EditorRunBar::play_custom_scene)
			.bind(String("res://addons/mcp_test/runner.tscn"), Vector<String>())
			.call_deferred();

	// 11. Wait for results (blocks until test_complete or timeout).
	// Add 15s grace for game startup overhead on top of the test timeout.
	Dictionary result = bridge->wait_for_pending(req, (timeout + 15) * 1000);

	// 12. Cleanup runner files from the project.
	_cleanup_runner_files();

	// 13. Build and return the structured response.
	return _build_test_results_response(result, verbose);
}

// ============================================================================
// Tool: testing/list
// ============================================================================

Dictionary MCPTestingTools::handle_list(const Dictionary &p_args) {
	String path = p_args.get("path", "res://tests/");
	bool include_methods = p_args.get("include_methods", true);

	// Validate path.
	if (!path.begins_with("res://")) {
		return make_tool_error(
				"Path must start with 'res://'. Got: " + path);
	}
	if (!validate_path(path)) {
		return make_tool_error(vformat(
				"Invalid path: %s\n\n"
				"Paths must start with res:// and cannot contain '..' traversal sequences.",
				path));
	}

	String dir_path = path.ends_with("/") ? path : path + "/";

	if (!DirAccess::dir_exists_absolute(dir_path)) {
		// If directory doesn't exist, return helpful message (not an error).
		Dictionary structured;
		structured["path"] = dir_path;
		structured["file_count"] = 0;
		structured["total_methods"] = 0;
		structured["files"] = Array();
		return make_tool_result(
				"No test files found in " + dir_path +
				"\nCreate test files named 'test_*.gd' with test_*() methods.",
				structured);
	}

	Vector<String> test_files = _collect_test_files(dir_path);

	if (test_files.is_empty()) {
		Dictionary structured;
		structured["path"] = dir_path;
		structured["file_count"] = 0;
		structured["total_methods"] = 0;
		structured["files"] = Array();
		return make_tool_result(
				"No test files found in " + dir_path +
				"\nCreate test files named 'test_*.gd' with test_*() methods.",
				structured);
	}

	Array files;
	int total_methods = 0;

	for (const String &file_path : test_files) {
		Dictionary file_info;
		file_info["path"] = file_path;

		if (include_methods) {
			// Parse file to extract test_* method names.
			Vector<String> methods = _extract_test_methods(file_path);
			Array method_arr;
			for (const String &m : methods) {
				method_arr.push_back(m);
			}
			file_info["methods"] = method_arr;
			file_info["valid"] = true; // Could also validate here.
			total_methods += methods.size();
		}

		files.push_back(file_info);
	}

	// Build text response.
	String text = "Found " + itos(files.size()) + " test file" +
			(files.size() != 1 ? "s" : "") + " in " + dir_path;
	if (include_methods) {
		text += " (" + itos(total_methods) + " test method" +
				(total_methods != 1 ? "s" : "") + ")";
	}
	text += ":\n";

	for (int i = 0; i < files.size(); i++) {
		Dictionary fi = files[i];
		text += "\n  " + String(fi["path"]);
		if (include_methods) {
			Array methods = fi["methods"];
			text += " (" + itos(methods.size()) + " method" +
					(methods.size() != 1 ? "s" : "") + ")";
			for (int j = 0; j < methods.size(); j++) {
				text += "\n    " + String(methods[j]) + "()";
			}
		}
	}

	Dictionary structured;
	structured["path"] = dir_path;
	structured["file_count"] = files.size();
	structured["total_methods"] = total_methods;
	structured["files"] = files;

	return make_tool_result(text, structured);
}

// ============================================================================
// Tool: testing/check_script
// ============================================================================

Dictionary MCPTestingTools::handle_check_script(const Dictionary &p_args) {
	String path = p_args.get("path", "");

	if (path.is_empty()) {
		return make_tool_error("Missing required parameter: path");
	}
	if (!validate_path(path)) {
		return make_tool_error(vformat(
				"Invalid path: %s\n\n"
				"Paths must start with res:// and cannot contain '..' traversal sequences.",
				path));
	}

	// Must be a .gd file.
	if (path.get_extension().to_lower() != "gd") {
		return make_tool_error(vformat(
				"Not a GDScript file: %s\n\n"
				"testing/check_script only validates .gd files.",
				path));
	}

	// Check file exists.
	if (!FileAccess::exists(path)) {
		return make_tool_error(vformat("File not found: %s", path));
	}

	Dictionary file_result = _validate_single_file(path);

	// Build text output.
	bool valid = file_result["valid"];
	Array errors = file_result["errors"];
	Array warnings = file_result["warnings"];

	String text;
	if (valid && errors.size() == 0 && warnings.size() == 0) {
		text = vformat("%s: No errors or warnings.", path);
	} else {
		text = vformat("Found %d errors and %d warnings in %s:\n",
				errors.size(), warnings.size(), path);

		if (errors.size() > 0) {
			text += "\nErrors:";
			for (int i = 0; i < errors.size(); i++) {
				Dictionary e = errors[i];
				text += vformat("\n  Line %d, Col %d: %s",
						(int)e["line"], (int)e["column"], (String)e["message"]);
			}
		}

		if (warnings.size() > 0) {
			text += "\n\nWarnings:";
			for (int i = 0; i < warnings.size(); i++) {
				Dictionary w = warnings[i];
				text += vformat("\n  Line %d: %s [%s]",
						(int)w["line"], (String)w["message"], (String)w["code_name"]);
			}
		}
	}

	return make_tool_result(text, file_result);
}

// ============================================================================
// Tool: testing/check_all_scripts
// ============================================================================

Dictionary MCPTestingTools::handle_check_all_scripts(const Dictionary &p_args) {
	// Delegates to the progress-aware variant with a null context.
	// When p_ctx is null, progress reporting and cancellation are no-ops.
	return handle_check_all_scripts_with_progress(p_args, nullptr);
}

// ============================================================================
// Tool: testing/check_all_scripts (progress-aware variant)
// ============================================================================

Dictionary MCPTestingTools::handle_check_all_scripts_with_progress(
		const Dictionary &p_args, ProgressContext *p_ctx) {
	bool include_warnings = p_args.get("include_warnings", false);
	int page = CLAMP((int)p_args.get("page", 1), 1, 1000);

	// Find all .gd files in the project.
	Vector<String> gd_files;
	_find_gd_files_recursive("res://", gd_files);

	if (gd_files.size() == 0) {
		Dictionary structured;
		structured["total_files"] = 0;
		structured["files_with_errors"] = 0;
		structured["files_with_warnings"] = 0;
		structured["files_clean"] = 0;
		structured["files"] = Array();
		return make_tool_result("No .gd files found in project.", structured);
	}

	// Sort for deterministic output.
	gd_files.sort();

	int total = gd_files.size();
	static constexpr int PAGE_SIZE = 10;
	int page_start = (page - 1) * PAGE_SIZE; // How many issue-files to skip.

	// First pass: validate all files, collect summary counts and issue-files.
	// We need to validate everything for accurate counts, but only emit
	// details for the requested page.
	Vector<Dictionary> all_issue_results;
	Vector<String> all_issue_paths;
	Vector<int> all_issue_error_counts;
	Vector<int> all_issue_warning_counts;
	int files_with_errors = 0;
	int files_with_warnings = 0;
	int files_clean = 0;

	for (int i = 0; i < total; i++) {
		// Cooperative cancellation check.
		if (p_ctx && p_ctx->is_cancelled()) {
			break;
		}

		// Report progress.
		if (p_ctx) {
			p_ctx->report_progress(i, total, "Checking " + gd_files[i]);
		}

		Dictionary result = _validate_single_file(gd_files[i]);
		Array errors = result["errors"];
		Array warnings = result["warnings"];

		if (errors.size() > 0) {
			files_with_errors++;
			all_issue_results.push_back(result);
			all_issue_paths.push_back(gd_files[i]);
			all_issue_error_counts.push_back(errors.size());
			all_issue_warning_counts.push_back(warnings.size());
		} else if (warnings.size() > 0) {
			files_with_warnings++;
			if (include_warnings) {
				all_issue_results.push_back(result);
				all_issue_paths.push_back(gd_files[i]);
				all_issue_error_counts.push_back(0);
				all_issue_warning_counts.push_back(warnings.size());
			}
		} else {
			files_clean++;
		}
	}

	// Final progress (skip if cancelled -- don't send misleading 100% completion).
	bool was_cancelled = (p_ctx && p_ctx->is_cancelled());
	if (p_ctx && !was_cancelled) {
		p_ctx->report_progress(total, total, "Done");
	}
	int files_checked = files_with_errors + files_with_warnings + files_clean;
	int total_issues = all_issue_results.size();
	int total_pages = (total_issues + PAGE_SIZE - 1) / PAGE_SIZE;
	if (total_pages == 0) {
		total_pages = 1;
	}

	// Clamp page to valid range.
	page = CLAMP(page, 1, total_pages);
	page_start = (page - 1) * PAGE_SIZE;

	// Build text and structured results for the requested page.
	String text;
	Array file_results;
	int page_end = MIN(page_start + PAGE_SIZE, total_issues);

	for (int i = page_start; i < page_end; i++) {
		int err_count = all_issue_error_counts[i];
		int warn_count = all_issue_warning_counts[i];
		Dictionary result = all_issue_results[i];

		if (err_count > 0) {
			text += vformat("=== %s (%d errors) ===\n", all_issue_paths[i], err_count);
			Array errors = result["errors"];
			int err_limit = MIN(errors.size(), 5);
			for (int j = 0; j < err_limit; j++) {
				Dictionary e = errors[j];
				text += vformat("  Line %d, Col %d: %s\n",
						(int)e["line"], (int)e["column"], (String)e["message"]);
			}
			if (errors.size() > err_limit) {
				text += vformat("  ... and %d more errors\n", errors.size() - err_limit);
			}
		} else {
			text += vformat("=== %s (%d warnings) ===\n", all_issue_paths[i], warn_count);
			Array warnings = result["warnings"];
			int warn_limit = MIN(warnings.size(), 5);
			for (int j = 0; j < warn_limit; j++) {
				Dictionary w = warnings[j];
				text += vformat("  Line %d: %s [%s]\n",
						(int)w["line"], (String)w["message"], (String)w["code_name"]);
			}
			if (warnings.size() > warn_limit) {
				text += vformat("  ... and %d more warnings\n", warnings.size() - warn_limit);
			}
		}
		file_results.push_back(result);
	}

	// Pagination info.
	if (total_pages > 1) {
		text += vformat("\nPage %d of %d (%d files with issues total). Use page=%d to see more.\n",
				page, total_pages, total_issues, MIN(page + 1, total_pages));
	}

	text += vformat("\nSummary: %d files checked, %d with errors, %d with warnings, %d clean",
			files_checked, files_with_errors, files_with_warnings, files_clean);
	if (was_cancelled) {
		text += " (Cancelled by client)";
	}

	Dictionary structured;
	structured["total_files"] = total;
	structured["files_checked"] = files_checked;
	structured["files_with_errors"] = files_with_errors;
	structured["files_with_warnings"] = files_with_warnings;
	structured["files_clean"] = files_clean;
	structured["cancelled"] = was_cancelled;
	structured["page"] = page;
	structured["total_pages"] = total_pages;
	structured["files"] = file_results;

	return make_tool_result(text, structured);
}

// ============================================================================
// Internal: Collect test files recursively
// ============================================================================

Vector<String> MCPTestingTools::_collect_test_files(const String &p_dir_path) {
	Vector<String> result;

	Ref<DirAccess> da = DirAccess::open(p_dir_path);
	if (da.is_null()) {
		return result;
	}

	da->list_dir_begin();
	String item = da->get_next();
	while (!item.is_empty()) {
		if (da->current_is_dir()) {
			if (!is_skip_directory(item) && !item.begins_with(".")) {
				Vector<String> sub_files = _collect_test_files(p_dir_path + item + "/");
				result.append_array(sub_files);
			}
		} else {
			if (item.begins_with("test_") && item.get_extension().to_lower() == "gd") {
				result.push_back(p_dir_path + item);
			}
		}
		item = da->get_next();
	}
	da->list_dir_end();

	// Sort for deterministic output.
	result.sort();
	return result;
}

// ============================================================================
// Internal: Validate a single GDScript file (GDScript tools version with
// error_count/warning_count fields -- used by both test and check_script tools)
// ============================================================================

Dictionary MCPTestingTools::_validate_single_file(const String &p_path) {
	Dictionary result;
	result["path"] = p_path;
	result["valid"] = true;
	result["errors"] = Array();
	result["warnings"] = Array();
	result["error_count"] = 0;
	result["warning_count"] = 0;

	// Read the file from disk (fresh read, not cached).
	String source = FileAccess::get_file_as_string(p_path);
	if (FileAccess::get_open_error() != OK) {
		result["valid"] = false;
		Array errors;
		Dictionary err;
		err["line"] = 0;
		err["column"] = 0;
		err["message"] = vformat("Failed to read file: %s", p_path);
		errors.push_back(err);
		result["errors"] = errors;
		result["error_count"] = 1;
		return result;
	}

	// Use GDScriptLanguage::validate() -- this is thread-safe as it creates
	// local parser/analyzer instances.
	GDScriptLanguage *gdscript = GDScriptLanguage::get_singleton();
	if (!gdscript) {
		result["valid"] = false;
		Array errors;
		Dictionary err;
		err["line"] = 0;
		err["column"] = 0;
		err["message"] = "GDScriptLanguage singleton not available.";
		errors.push_back(err);
		result["errors"] = errors;
		result["error_count"] = 1;
		return result;
	}

	List<String> functions;
	List<ScriptLanguage::ScriptError> script_errors;
	List<ScriptLanguage::Warning> script_warnings;
	HashSet<int> safe_lines;

	bool is_valid = gdscript->validate(
			source,
			p_path,
			&functions,
			&script_errors,
			&script_warnings,
			&safe_lines);

	result["valid"] = is_valid;

	// Convert errors to Dictionary array.
	Array errors;
	for (const ScriptLanguage::ScriptError &se : script_errors) {
		Dictionary e;
		e["line"] = se.line;
		e["column"] = se.column;
		e["message"] = se.message;
		errors.push_back(e);
	}
	result["errors"] = errors;
	result["error_count"] = errors.size();

	// Convert warnings to Dictionary array.
	// ScriptLanguage::Warning has: start_line, end_line, code, string_code, message.
	Array warnings;
	for (const ScriptLanguage::Warning &sw : script_warnings) {
		Dictionary w;
		w["line"] = sw.start_line;
		w["end_line"] = sw.end_line;
		w["message"] = sw.message;
		w["code"] = sw.code;
		w["code_name"] = sw.string_code;
		warnings.push_back(w);
	}
	result["warnings"] = warnings;
	result["warning_count"] = warnings.size();

	return result;
}

// ============================================================================
// Internal: Extract test method names from a GDScript file
// ============================================================================

Vector<String> MCPTestingTools::_extract_test_methods(const String &p_path) {
	Vector<String> methods;

	// Read and validate the file to get the function list.
	String source = FileAccess::get_file_as_string(p_path);
	if (FileAccess::get_open_error() != OK) {
		return methods;
	}

	GDScriptLanguage *gdscript = GDScriptLanguage::get_singleton();
	if (!gdscript) {
		return methods;
	}

	List<String> functions;
	List<ScriptLanguage::ScriptError> script_errors;
	List<ScriptLanguage::Warning> script_warnings;
	HashSet<int> safe_lines;

	gdscript->validate(
			source,
			p_path,
			&functions,
			&script_errors,
			&script_warnings,
			&safe_lines);

	// Filter for test_* functions.
	for (const String &func_name : functions) {
		if (func_name.begins_with("test_")) {
			methods.push_back(func_name);
		}
	}

	methods.sort();
	return methods;
}

// ============================================================================
// Internal: Build compile error response
// ============================================================================

Dictionary MCPTestingTools::_build_compile_error_response(const Array &p_compile_results, int p_run_number) {
	// Build the structured response per design doc Section 3.1 (compile error case).
	Array files;
	int compile_error_count = 0;
	String text;

	for (int i = 0; i < p_compile_results.size(); i++) {
		Dictionary cr = p_compile_results[i];
		String file_path = cr["path"];
		bool valid = cr["valid"];
		Array errors = cr["errors"];
		Array warnings = cr["warnings"];

		Dictionary file_info;
		file_info["path"] = file_path;
		file_info["compile_ok"] = valid;
		file_info["tests"] = Array();

		if (!valid) {
			compile_error_count++;
			file_info["errors"] = errors;

			text += "Compile error in " + file_path + " -- tests not executed.\n";
			for (int j = 0; j < errors.size(); j++) {
				Dictionary e = errors[j];
				text += vformat("  Line %d, Col %d: %s\n",
						(int)e["line"], (int)e["column"], (String)e["message"]);
			}
			text += "\n";
		}

		files.push_back(file_info);
	}

	// Build the summary.
	Dictionary summary;
	summary["total"] = 0;
	summary["passed"] = 0;
	summary["failed"] = 0;
	summary["errors"] = 0;
	summary["skipped"] = 0;
	summary["compile_errors"] = compile_error_count;
	summary["duration_ms"] = 0;

	Dictionary structured;
	structured["summary"] = summary;
	structured["files"] = files;

	// Compile errors are returned as tool errors with isError=true.
	Dictionary content_item;
	content_item["type"] = "text";
	content_item["text"] = text.strip_edges();
	Array content;
	content.push_back(content_item);

	Dictionary result;
	result["content"] = content;
	result["isError"] = true;
	result["structuredContent"] = structured;
	return result;
}

// ============================================================================
// Internal: Build test results response
// ============================================================================

Dictionary MCPTestingTools::_build_test_results_response(const Dictionary &p_raw_result, bool p_verbose) {
	// Check for errors from the pending request mechanism (timeout, game crash, etc.).
	if (p_raw_result.has("error") && !(bool)p_raw_result.get("success", true)) {
		String error_msg = p_raw_result.get("error", "Unknown error");

		// If we have partial results, include them.
		if (p_raw_result.has("test_results")) {
			Dictionary partial = p_raw_result["test_results"];
			String text = "Test run failed: " + error_msg + "\n\n";

			if (p_raw_result.get("game_crashed", false)) {
				text += "The game process crashed during test execution.\n";
				text += "Check runtime/get_errors for runtime errors.\n\n";
			}

			// Append whatever partial results we have.
			Dictionary summary;
			summary["total"] = partial.get("total", 0);
			summary["passed"] = partial.get("passed", 0);
			summary["failed"] = partial.get("failed", 0);
			summary["errors"] = partial.get("errors", 0);
			summary["skipped"] = partial.get("skipped", 0);
			summary["duration_ms"] = partial.get("duration_ms", 0);
			summary["incomplete"] = true;

			text += vformat("Partial results: %d/%d passed",
					(int)summary["passed"], (int)summary["total"]);

			Dictionary structured;
			structured["summary"] = summary;
			structured["files"] = partial.get("files", Array());

			Dictionary content_item;
			content_item["type"] = "text";
			content_item["text"] = text;
			Array content;
			content.push_back(content_item);

			Dictionary r;
			r["content"] = content;
			r["isError"] = true;
			r["structuredContent"] = structured;
			return r;
		}

		return make_tool_error("Test run failed: " + error_msg);
	}

	// Successful test run. Parse the results from the bridge.
	int total = p_raw_result.get("total", 0);
	int passed = p_raw_result.get("passed", 0);
	int failed = p_raw_result.get("failed", 0);
	int errors = p_raw_result.get("errors", 0);
	int skipped = p_raw_result.get("skipped", 0);
	int duration_ms = p_raw_result.get("duration_ms", 0);
	Array file_results = p_raw_result.get("files", Array());

	// Build human-readable text.
	String text;

	if (file_results.size() == 1) {
		// Single-file format.
		Dictionary fi = file_results[0];
		String file_path = fi.get("path", "");
		Array tests = fi.get("tests", Array());

		if (failed == 0 && errors == 0) {
			text = vformat("Tests passed: %d/%d in %s (%.2fs)\n",
					passed, total, file_path.get_file(),
					duration_ms / 1000.0);
		} else {
			text = vformat("Tests: %d passed", passed);
			if (failed > 0) {
				text += vformat(", %d FAILED", failed);
			}
			if (errors > 0) {
				text += vformat(", %d ERROR", errors);
			}
			text += vformat(" in %s (%.2fs)\n",
					file_path.get_file(), duration_ms / 1000.0);
		}

		text += "\n";
		for (int i = 0; i < tests.size(); i++) {
			Dictionary t = tests[i];
			String status = t.get("status", "");
			String method = t.get("method", "");
			int t_duration = t.get("duration_ms", 0);

			if (status == "passed") {
				text += vformat("  PASS %s (%.2fs)\n", method, t_duration / 1000.0);
			} else if (status == "failed") {
				text += vformat("  FAIL %s (%.2fs)\n", method, t_duration / 1000.0);
				String msg = t.get("message", "");
				if (!msg.is_empty()) {
					text += "      " + msg + "\n";
				}
				String err_file = t.get("file", "");
				int err_line = t.get("line", 0);
				if (!err_file.is_empty() && err_line > 0) {
					text += vformat("      at %s:%d\n", err_file, err_line);
				}
			} else if (status == "error") {
				text += vformat("  FAIL %s (%.2fs) [ERROR]\n", method, t_duration / 1000.0);
				String msg = t.get("message", "");
				if (!msg.is_empty()) {
					text += "      " + msg + "\n";
				}
				String err_file = t.get("file", "");
				int err_line = t.get("line", 0);
				if (!err_file.is_empty() && err_line > 0) {
					text += vformat("      at %s:%d\n", err_file, err_line);
				}
			} else if (status == "skipped") {
				text += vformat("  SKIP %s\n", method);
				String msg = t.get("message", "");
				if (!msg.is_empty()) {
					text += "      " + msg + "\n";
				}
			} else if (status == "timeout") {
				text += vformat("  TIMEOUT %s (%.2fs)\n", method, t_duration / 1000.0);
				String msg = t.get("message", "");
				if (!msg.is_empty()) {
					text += "      " + msg + "\n";
				}
			}

			// Include output for failed/errored tests, or all if verbose.
			Array output = t.get("output", Array());
			if (output.size() > 0 && (p_verbose || status == "failed" || status == "error")) {
				for (int k = 0; k < output.size(); k++) {
					text += "      > " + String(output[k]) + "\n";
				}
			}
		}
	} else {
		// Multi-file format.
		text = vformat("Test run: %d passed", passed);
		if (failed > 0) {
			text += vformat(", %d FAILED", failed);
		}
		if (errors > 0) {
			text += vformat(", %d ERROR", errors);
		}
		text += vformat(" across %d files (%.2fs)\n",
				file_results.size(), duration_ms / 1000.0);

		for (int i = 0; i < file_results.size(); i++) {
			Dictionary fi = file_results[i];
			String file_path = fi.get("path", "");
			Array tests = fi.get("tests", Array());

			int file_passed = 0;
			int file_failed = 0;
			int file_errors = 0;
			int file_total = tests.size();
			int file_duration = 0;

			for (int j = 0; j < tests.size(); j++) {
				Dictionary t = tests[j];
				String s = t.get("status", "");
				int td = t.get("duration_ms", 0);
				file_duration += td;
				if (s == "passed") {
					file_passed++;
				} else if (s == "failed") {
					file_failed++;
				} else if (s == "error" || s == "timeout") {
					file_errors++;
				}
			}

			text += "\n  " + file_path + ": " + itos(file_passed) + "/" + itos(file_total) + " passed";
			if (file_failed > 0) {
				text += ", " + itos(file_failed) + " FAILED";
			}
			if (file_errors > 0) {
				text += ", " + itos(file_errors) + " ERROR";
			}
			text += vformat(" (%.2fs)", file_duration / 1000.0);

			// Show details for failed/errored tests.
			for (int j = 0; j < tests.size(); j++) {
				Dictionary t = tests[j];
				String status = t.get("status", "");
				if (status == "failed" || status == "error" || status == "timeout") {
					String method = t.get("method", "");
					int t_duration = t.get("duration_ms", 0);
					text += vformat("\n    FAIL %s (%.2fs)", method, t_duration / 1000.0);
					if (status == "error") {
						text += " [ERROR]";
					} else if (status == "timeout") {
						text += " [TIMEOUT]";
					}
					String msg = t.get("message", "");
					if (!msg.is_empty()) {
						text += "\n        " + msg;
					}
					String err_file = t.get("file", "");
					int err_line = t.get("line", 0);
					if (!err_file.is_empty() && err_line > 0) {
						text += vformat("\n        at %s:%d", err_file, err_line);
					}
				}
			}
		}
	}

	// Build structured response.
	Dictionary summary;
	summary["total"] = total;
	summary["passed"] = passed;
	summary["failed"] = failed;
	summary["errors"] = errors;
	summary["skipped"] = skipped;
	summary["duration_ms"] = duration_ms;

	Dictionary structured;
	structured["summary"] = summary;
	structured["files"] = file_results;

	return make_tool_result(text, structured);
}

// ============================================================================
// Internal: Copy runner files to project
// ============================================================================

void MCPTestingTools::_copy_runner_files_to_project() {
	// Create the addons/mcp_test/ directory if it does not exist.
	String addon_dir = "res://addons/mcp_test/";

	{
		Ref<DirAccess> da = DirAccess::open("res://");
		if (da.is_valid()) {
			if (!da->dir_exists("addons")) {
				da->make_dir("addons");
			}
			da = DirAccess::open("res://addons/");
			if (da.is_valid() && !da->dir_exists("mcp_test")) {
				da->make_dir("mcp_test");
			}
		}
	}

	// Write the runner scene file.
	{
		Ref<FileAccess> fa = FileAccess::open(addon_dir + "runner.tscn", FileAccess::WRITE);
		if (fa.is_valid()) {
			fa->store_string(RUNNER_TSCN_CONTENT);
		}
	}

	// Write the runner script.
	{
		Ref<FileAccess> fa = FileAccess::open(addon_dir + "runner.gd", FileAccess::WRITE);
		if (fa.is_valid()) {
			fa->store_string(RUNNER_GD_CONTENT);
		}
	}

	// Write the test context script.
	{
		Ref<FileAccess> fa = FileAccess::open(addon_dir + "test_context.gd", FileAccess::WRITE);
		if (fa.is_valid()) {
			fa->store_string(TEST_CONTEXT_GD_CONTENT);
		}
	}

	// Write .gdignore to prevent the editor from importing these as game assets.
	{
		Ref<FileAccess> fa = FileAccess::open(addon_dir + ".gdignore", FileAccess::WRITE);
		if (fa.is_valid()) {
			fa->store_string(GDIGNORE_CONTENT);
		}
	}
}

// ============================================================================
// Internal: Cleanup runner files from project
// ============================================================================

void MCPTestingTools::_cleanup_runner_files() {
	String addon_dir = "res://addons/mcp_test/";

	Ref<DirAccess> da = DirAccess::open(addon_dir);
	if (da.is_null()) {
		return;
	}

	// Remove the files we created.
	da->remove("runner.tscn");
	da->remove("runner.gd");
	da->remove("test_context.gd");
	da->remove(".gdignore");

	// Try to remove the directory itself (only succeeds if empty).
	Ref<DirAccess> parent = DirAccess::open("res://addons/");
	if (parent.is_valid()) {
		parent->remove("mcp_test");
	}

	// Also try to remove the manifest.
	Ref<DirAccess> user_dir = DirAccess::open("user://");
	if (user_dir.is_valid()) {
		user_dir->remove("mcp_test_manifest.json");
	}
}

// ============================================================================
// Internal: Find all .gd files recursively
// ============================================================================

void MCPTestingTools::_find_gd_files_recursive(const String &p_dir,
		Vector<String> &r_files) {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return;
	}

	da->list_dir_begin();
	String item = da->get_next();
	while (!item.is_empty()) {
		if (da->current_is_dir()) {
			if (!is_skip_directory(item) && !item.begins_with(".")) {
				_find_gd_files_recursive(p_dir + item + "/", r_files);
			}
		} else {
			if (item.get_extension().to_lower() == "gd") {
				r_files.push_back(p_dir + item);
			}
		}
		item = da->get_next();
	}
	da->list_dir_end();
}
