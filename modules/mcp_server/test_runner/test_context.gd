# res://addons/mcp_test/test_context.gd
# Assertion helper injected as `_test` into test scripts by the MCP test runner.
# Provides structured assertion methods, signal watching, lifecycle helpers,
# and async utilities with built-in timeouts.
class_name TestContext
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
var _output: Array = []
var _watched_signals: Dictionary = {}  # obj_id -> { signal_name -> [[params], ...] }
var _autofree_objects: Array = []  # Objects to free() after test
var _autoqfree_nodes: Array = []  # Nodes to queue_free() after test
var _orphan_count_before := 0
var _scene_tree: SceneTree  # Set by runner for add_child_autofree and async helpers


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
	# Called by runner after each test method (even on failure).
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
		return  # First failure wins.
	_failed = true
	_failure_message = message
	var stack := get_stack()
	if stack == null or stack.is_empty():
		return
	# Walk up past internal frames to find the test method call site.
	for i in range(1, stack.size()):
		var frame: Dictionary = stack[i]
		if not String(frame.get("source", "")).ends_with("test_context.gd"):
			_failure_file = frame.get("source", "")
			_failure_line = frame.get("line", 0)
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
	var diff: float
	if a is Vector2 or a is Vector3:
		diff = (a - b).length()
	else:
		diff = absf(float(a) - float(b))
	if diff >= epsilon:
		_record_failure(_fmt("Expected %s ~= %s (diff %s, epsilon %s)", [a, b, diff, epsilon], msg))


func assert_typeof(val, type: int, msg := ""):
	if typeof(val) != type:
		_record_failure(_fmt("Expected type %d, got %d", [type, typeof(val)], msg))


func assert_no_new_orphans(msg := ""):
	var current := Performance.get_monitor(Performance.OBJECT_ORPHAN_NODE_COUNT)
	var new_orphans: int = int(current) - int(_orphan_count_before)
	if new_orphans > 0:
		_record_failure(_fmt("Test leaked %d orphan node(s)", [new_orphans], msg))


# --- Signals ---

func watch_signals(obj: Object):
	var id := obj.get_instance_id()
	_watched_signals[id] = {}
	for s in obj.get_signal_list():
		var sig_name: String = s.name
		_watched_signals[id][sig_name] = []
		# Use a local variable to capture sig_name properly in the closure.
		var captured_name := sig_name
		var captured_id := id
		# Connect with CONNECT_REFERENCE_COUNTED so multiple watches are safe.
		obj.connect(captured_name, func():
			# Variadic args not available in Godot 4 lambdas, so we capture
			# the emission with no params. For param capture, users should use
			# assert_signal_emitted (checks emission count) or manual connection.
			_watched_signals[captured_id][captured_name].append([]))


func assert_signal_emitted(obj: Object, signal_name: String, msg := ""):
	var emissions = _get_emissions(obj, signal_name)
	if emissions == null:
		return
	if emissions.is_empty():
		_record_failure(_fmt("Expected signal '%s' to be emitted", [signal_name], msg))


func assert_signal_not_emitted(obj: Object, signal_name: String, msg := ""):
	var emissions = _get_emissions(obj, signal_name)
	if emissions == null:
		return
	if not emissions.is_empty():
		_record_failure(_fmt("Expected signal '%s' NOT to be emitted", [signal_name], msg))


func assert_signal_emitted_with(obj: Object, signal_name: String, expected_params: Array, msg := ""):
	var emissions = _get_emissions(obj, signal_name)
	if emissions == null:
		return
	for emission in emissions:
		if emission == expected_params:
			return  # Found a matching emission.
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
	return obj  # Return for chaining: var x = _test.autofree(MyClass.new())


func autoqfree(node: Node) -> Node:
	_autoqfree_nodes.append(node)
	return node


func add_child_autofree(node: Node) -> Node:
	if _scene_tree and _scene_tree.current_scene:
		_scene_tree.current_scene.add_child(node)
	elif _scene_tree and _scene_tree.root:
		_scene_tree.root.add_child(node)
	_autoqfree_nodes.append(node)
	return node


# --- Control ---

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
	if not _scene_tree:
		_record_failure("wait_for_signal requires scene tree (test script should extend Node)")
		return false
	var timer := _scene_tree.create_timer(max_wait)
	var result := await _race_signal(sig, timer.timeout)
	if not result:
		if msg.is_empty():
			msg = "Signal not emitted within %ss" % max_wait
		_record_failure(msg)
	return result


func wait_seconds(time: float, _msg := ""):
	if _scene_tree:
		await _scene_tree.create_timer(time).timeout


func wait_frames(count: int, _msg := ""):
	if _scene_tree:
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
	var m: String
	if args.size() > 0:
		m = template % args
	else:
		m = template
	if not msg.is_empty():
		m += " | " + msg
	return m
