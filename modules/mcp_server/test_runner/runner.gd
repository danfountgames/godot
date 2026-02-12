# res://addons/mcp_test/runner.gd
# Game-side test runner for the MCP server module.
# Loads a manifest from user://mcp_test_manifest.json, executes all test files
# and methods, and reports structured results back to the editor via
# EngineDebugger custom messages.
extends Node

var _results: Array = []
var _total := 0
var _passed := 0
var _failed := 0
var _errors := 0
var _skipped := 0
var _start_time := 0
var _timeout_ms := 30000
var _verbose := false


func _ready():
	_start_time = Time.get_ticks_msec()

	var manifest := _load_manifest()
	if manifest.is_empty():
		_send_complete()
		await get_tree().create_timer(0.1).timeout
		get_tree().quit()
		return

	var files: Array = manifest.get("files", [])
	var filter: String = manifest.get("filter", "")
	var method_timeout: int = manifest.get("method_timeout_ms", 5000)
	_timeout_ms = manifest.get("timeout_ms", 30000)
	_verbose = manifest.get("verbose", false)

	for file_path in files:
		# Check total timeout before starting each file.
		var elapsed := Time.get_ticks_msec() - _start_time
		if elapsed >= _timeout_ms:
			# Report remaining files as timed out.
			_send_method_result(file_path, "<timeout>", "timeout",
				"Total test run timeout exceeded (%dms)" % _timeout_ms,
				"", 0, 0, [])
			_errors += 1
			_total += 1
			continue

		await _run_test_file(file_path, filter, method_timeout)

	_send_complete()
	# Small delay to ensure debugger messages are flushed before quit.
	await get_tree().create_timer(0.1).timeout
	get_tree().quit()


func _load_manifest() -> Dictionary:
	var path := "user://mcp_test_manifest.json"
	if not FileAccess.file_exists(path):
		push_error("MCP test manifest not found: " + path)
		return {}
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		push_error("MCP test manifest could not be opened: " + path)
		return {}
	var text := file.get_as_text()
	file.close()
	var json := JSON.new()
	if json.parse(text) != OK:
		push_error("Failed to parse test manifest: " + json.get_error_message())
		return {}
	if json.data is Dictionary:
		return json.data
	push_error("Test manifest root is not a Dictionary")
	return {}


func _run_test_file(file_path: String, filter: String, method_timeout: int):
	# Load the script.
	var script = load(file_path)
	if script == null or not (script is GDScript):
		_send_method_result(file_path, "<load>", "error",
			"Failed to load script: " + file_path, file_path, 0, 0, [])
		_errors += 1
		_total += 1
		return

	# Get test methods.
	var methods := _get_test_methods(script)
	if not filter.is_empty():
		methods = methods.filter(func(m: String): return m.match(filter))

	if methods.is_empty():
		# No test methods found (or all filtered out) -- nothing to report.
		return

	# Instantiate the test object.
	var instance = script.new()
	if instance == null:
		_send_method_result(file_path, "<instantiate>", "error",
			"Failed to instantiate script: " + file_path, file_path, 0, 0, [])
		_errors += 1
		_total += 1
		return

	var is_node := instance is Node
	if is_node:
		add_child(instance)

	# Inject test context.
	var ctx := TestContext.new()
	ctx._scene_tree = get_tree()
	instance.set("_test", ctx)

	# before_all
	if instance.has_method("before_all"):
		var result = instance.call("before_all")
		if result is Signal:
			await result

	for method_name in methods:
		# Check total timeout before each method.
		var elapsed := Time.get_ticks_msec() - _start_time
		if elapsed >= _timeout_ms:
			_send_method_result(file_path, method_name, "timeout",
				"Total test run timeout exceeded (%dms)" % _timeout_ms,
				"", 0, 0, [])
			_errors += 1
			_total += 1
			continue

		_total += 1
		ctx._reset()

		# before_each
		if instance.has_method("before_each"):
			var before_result = instance.call("before_each")
			if before_result is Signal:
				await before_result

		var method_start := Time.get_ticks_msec()
		var status := "passed"
		var message := ""
		var error_file := ""
		var error_line := 0

		# Execute test method with error catching.
		var call_result = instance.call(method_name)

		# Handle coroutine: if the method returned a signal (from await),
		# wait for it with a per-method timeout.
		if call_result is Signal:
			var timer := get_tree().create_timer(method_timeout / 1000.0)
			var completed := await _race(call_result, timer.timeout)
			if not completed:
				status = "timeout"
				message = "Test method timed out after %dms" % method_timeout

		var duration := Time.get_ticks_msec() - method_start

		# Check context state for test outcomes.
		if ctx._pending:
			status = "pending"
			message = ctx._pending_message
			_skipped += 1  # Pending counts as skipped in summary.
		elif ctx._skipped:
			status = "skipped"
			message = ctx._skip_reason
			_skipped += 1
		elif ctx._failed and status != "timeout":
			status = "failed"
			message = ctx._failure_message
			error_file = ctx._failure_file
			error_line = ctx._failure_line
			_failed += 1
		elif status == "timeout":
			_errors += 1
		else:
			_passed += 1

		# after_each (always runs, even on failure)
		if instance.has_method("after_each"):
			var after_result = instance.call("after_each")
			if after_result is Signal:
				await after_result

		# Cleanup autofree/autoqfree objects from context.
		ctx._cleanup()

		# Collect output -- only include for failed/errored tests unless verbose.
		var output: Array = []
		if _verbose or status in ["failed", "error", "timeout"]:
			output = ctx._output.duplicate()

		_send_method_result(file_path, method_name, status, message,
			error_file, error_line, duration, output)

	# after_all
	if instance.has_method("after_all"):
		var result = instance.call("after_all")
		if result is Signal:
			await result

	# Cleanup the test instance.
	if is_node:
		remove_child(instance)
		instance.queue_free()
		await get_tree().process_frame


func _get_test_methods(script: GDScript) -> Array:
	var methods: Array = []
	for m in script.get_script_method_list():
		var method_name: String = m.name
		if method_name.begins_with("test_"):
			methods.append(method_name)
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


func _race(coroutine_signal: Signal, timeout_signal: Signal) -> bool:
	# Returns true if the coroutine signal fired first, false if timeout.
	var finished := false
	var timed_out := false

	coroutine_signal.connect(func(): finished = true, CONNECT_ONE_SHOT)
	timeout_signal.connect(func(): timed_out = true, CONNECT_ONE_SHOT)

	while not finished and not timed_out:
		await get_tree().process_frame

	return finished
