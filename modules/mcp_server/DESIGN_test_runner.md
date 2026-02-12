# GDScript Test Runner -- MCP Tool Design

## 1. Overview and Rationale

The MCP server provides a comprehensive toolkit for an LLM to write, validate, run, and debug Godot projects. However, there is a critical gap in the **verification loop**. Today an LLM can:

- Write GDScript files (using native file tools + `editor/scan_filesystem`)
- Check them for compile errors (`gdscript/check_errors`)
- Run the project or a scene (`runtime/run_project`, `runtime/run_scene`)
- Evaluate expressions in a running game (`runtime/evaluate`)

But it **cannot programmatically verify behavior**. Running the project is heavyweight and requires visual inspection. `runtime/evaluate` only supports simple expressions (no control flow, no assertions, no multi-step logic). The LLM is effectively coding blind -- it can check syntax but never prove correctness.

A test runner closes this gap by enabling the **write → check → test → fix** loop that is the foundation of effective agentic coding. The LLM writes code, writes a test, runs the test, reads the results, and iterates -- all without human intervention.

### Design Principles

1. **Minimal ceremony.** A test file is just a GDScript file with `test_*()` methods. No base class to extend, no registration, no scene file required. The runner handles everything.
2. **Compile-first.** Before executing, validate the test file (and its dependencies) and return compiler errors immediately. Don't waste a game launch on code that won't parse.
3. **Structured results.** Return per-method pass/fail/error/skip with messages, timings, and any captured output. The LLM needs machine-readable results, not just a print log.
4. **Isolation.** Each test file runs in a fresh scene tree. Tests cannot leak state to each other. A crash in one test method does not prevent others from reporting.
5. **Reuse existing infrastructure.** The test runner launches via the same debugger bridge used by `runtime/run_scene`. Output and errors flow through the existing ring buffers. Results return via the existing custom message / `PendingRequest` pattern.
6. **Security.** Test files must live under `res://` and pass the same path validation as all other MCP file operations. The test runner scene is built into the module, not user-editable.

---

## 2. Proposed Tools

### Tool Summary

| Tool Name | Purpose | Game Required? |
|---|---|---|
| `test/run` | Compile-check then execute test file(s), return structured results | No (launches game internally) |
| `test/list` | Discover test files in project | No (editor-only filesystem scan) |

---

## 3. Tool Schemas

### 3.1 `test/run`

Run one or more GDScript test files. Each file is compiled first -- if compilation fails, compiler errors are returned immediately without launching the game. If compilation succeeds, a lightweight test runner scene executes all `test_*()` methods and reports results.

**Input Schema:**

```json
{
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "Path to a test file or directory. If a directory, all files matching 'test_*.gd' are collected recursively. Must start with 'res://'. Examples: 'res://tests/test_inventory.gd', 'res://tests/', 'res://tests/unit/'."
    },
    "filter": {
      "type": "string",
      "description": "Optional glob or substring to filter which test methods to run within matched files. Example: 'test_add*' runs only methods starting with 'test_add'. Default: run all test_* methods."
    },
    "timeout": {
      "type": "integer",
      "description": "Maximum time in seconds for the entire test run. If exceeded, the game is killed and remaining tests are reported as timed out. Default: 30. Max: 120."
    },
    "verbose": {
      "type": "boolean",
      "description": "If true, include captured print() output per test method. If false, only include output for failed/errored tests. Default: false."
    }
  },
  "required": ["path"]
}
```

**Annotations:**

```json
{
  "title": "Run Tests",
  "idempotentHint": true,
  "destructiveHint": false,
  "readOnlyHint": false,
  "openWorldHint": false
}
```

**Response (all pass):**

```
Tests passed: 3/3 in test_inventory.gd (0.12s)

  ✓ test_add_item (0.02s)
  ✓ test_remove_item (0.01s)
  ✓ test_inventory_full (0.09s)

structured:
{
  "summary": {
    "total": 3,
    "passed": 3,
    "failed": 0,
    "errors": 0,
    "skipped": 0,
    "duration_ms": 120
  },
  "files": [
    {
      "path": "res://tests/test_inventory.gd",
      "compile_ok": true,
      "tests": [
        {
          "method": "test_add_item",
          "status": "passed",
          "duration_ms": 20,
          "output": []
        },
        {
          "method": "test_remove_item",
          "status": "passed",
          "duration_ms": 10,
          "output": []
        },
        {
          "method": "test_inventory_full",
          "status": "passed",
          "duration_ms": 90,
          "output": []
        }
      ]
    }
  ]
}
```

**Response (with failures):**

```
Tests: 2 passed, 1 FAILED, 1 ERROR in test_inventory.gd (0.15s)

  ✓ test_add_item (0.02s)
  ✗ test_remove_item (0.01s)
      Assertion failed: Item 'sword' should be removed
      at res://tests/test_inventory.gd:18
  ✗ test_stack_overflow (0.05s) [ERROR]
      Stack overflow in recursive call
      at res://scripts/inventory.gd:42
  ✓ test_inventory_full (0.07s)

structured:
{
  "summary": {
    "total": 4,
    "passed": 2,
    "failed": 1,
    "errors": 1,
    "skipped": 0,
    "duration_ms": 150
  },
  "files": [
    {
      "path": "res://tests/test_inventory.gd",
      "compile_ok": true,
      "tests": [
        {
          "method": "test_add_item",
          "status": "passed",
          "duration_ms": 20,
          "output": []
        },
        {
          "method": "test_remove_item",
          "status": "failed",
          "duration_ms": 10,
          "message": "Assertion failed: Item 'sword' should be removed",
          "file": "res://tests/test_inventory.gd",
          "line": 18,
          "output": []
        },
        {
          "method": "test_stack_overflow",
          "status": "error",
          "duration_ms": 50,
          "message": "Stack overflow in recursive call",
          "file": "res://scripts/inventory.gd",
          "line": 42,
          "output": []
        },
        {
          "method": "test_inventory_full",
          "status": "passed",
          "duration_ms": 70,
          "output": []
        }
      ]
    }
  ]
}
```

**Response (compile error):**

```
Compile error in res://tests/test_inventory.gd — tests not executed.

  Line 5, Col 2: Expected ":" after "if" condition
  Line 12, Col 0: Unexpected "end of file"

structured:
{
  "summary": {
    "total": 0,
    "passed": 0,
    "failed": 0,
    "errors": 0,
    "skipped": 0,
    "compile_errors": 1,
    "duration_ms": 0
  },
  "files": [
    {
      "path": "res://tests/test_inventory.gd",
      "compile_ok": false,
      "errors": [
        {"line": 5, "column": 2, "message": "Expected \":\" after \"if\" condition"},
        {"line": 12, "column": 0, "message": "Unexpected \"end of file\""}
      ],
      "tests": []
    }
  ]
}
```

**Response (multiple files, directory run):**

```
Test run: 8 passed, 1 FAILED across 3 files (0.45s)

  res://tests/test_inventory.gd: 3/3 passed (0.12s)
  res://tests/test_player.gd: 4/4 passed (0.18s)
  res://tests/test_crafting.gd: 1/2 passed, 1 FAILED (0.15s)
    ✗ test_craft_without_materials (0.03s)
        Assertion failed: Should not craft without materials
        at res://tests/test_crafting.gd:24

structured:
{
  "summary": {
    "total": 9,
    "passed": 8,
    "failed": 1,
    "errors": 0,
    "skipped": 0,
    "duration_ms": 450
  },
  "files": [ ... ]
}
```

**Error cases:**
- `path` doesn't start with `res://` → tool error with suggestion
- `path` doesn't end with `.gd` and isn't a directory → tool error
- File/directory doesn't exist → tool error
- No `test_*.gd` files found in directory → tool error with hint
- No `test_*()` methods found in file → tool error with hint
- Game already running → tool error: "Stop the running game first, or use a separate test scene"
- Timeout exceeded → partial results with remaining tests marked `"status": "timeout"`

### 3.2 `test/list`

Discover test files in the project. Scans for files matching `test_*.gd` and lists the test methods in each.

**Input Schema:**

```json
{
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "Directory to scan for test files. Default: 'res://tests/'. Must start with 'res://'."
    },
    "include_methods": {
      "type": "boolean",
      "description": "If true, parse each file and list the test_*() methods. If false, only list file paths. Default: true."
    }
  }
}
```

**Annotations:**

```json
{
  "title": "List Tests",
  "idempotentHint": true,
  "destructiveHint": false,
  "readOnlyHint": true,
  "openWorldHint": false
}
```

**Response:**

```
Found 3 test files in res://tests/ (9 test methods):

  res://tests/test_inventory.gd (3 methods)
    test_add_item()
    test_remove_item()
    test_inventory_full()

  res://tests/test_player.gd (4 methods)
    test_take_damage()
    test_heal()
    test_death()
    test_respawn()

  res://tests/test_crafting.gd (2 methods)
    test_craft_item()
    test_craft_without_materials()

structured:
{
  "path": "res://tests/",
  "file_count": 3,
  "total_methods": 9,
  "files": [
    {
      "path": "res://tests/test_inventory.gd",
      "methods": ["test_add_item", "test_remove_item", "test_inventory_full"],
      "valid": true
    },
    {
      "path": "res://tests/test_player.gd",
      "methods": ["test_take_damage", "test_heal", "test_death", "test_respawn"],
      "valid": true
    },
    {
      "path": "res://tests/test_crafting.gd",
      "methods": ["test_craft_item", "test_craft_without_materials"],
      "valid": true
    }
  ]
}
```

---

## 4. Test File Convention

### 4.1 Minimal Test File

A test file is any `.gd` file whose name starts with `test_`. No special base class is required. The test runner instantiates the script and calls each `test_*()` method.

```gdscript
# res://tests/test_inventory.gd
extends RefCounted

func test_add_item():
    var inv = Inventory.new()
    inv.add_item("sword")
    assert(inv.has_item("sword"), "Should have sword after adding")

func test_remove_item():
    var inv = Inventory.new()
    inv.add_item("sword")
    inv.remove_item("sword")
    assert(!inv.has_item("sword"), "Should not have sword after removing")

func test_inventory_full():
    var inv = Inventory.new()
    for i in inv.max_capacity:
        inv.add_item("item_%d" % i)
    assert(inv.is_full(), "Should be full at max capacity")
```

### 4.2 Base Classes

Test scripts can extend:
- **`RefCounted`** (default) — for pure logic tests. Lightest weight. No scene tree needed.
- **`Node`** — for tests that need to be in the scene tree (signals, process callbacks, etc.). The runner adds the node to the tree and removes it after the test.

The runner detects the base class and handles instantiation accordingly.

### 4.3 Setup and Teardown

Optional lifecycle methods, called if present:

```gdscript
func before_all():
    # Called once before any test method in this file.
    pass

func before_each():
    # Called before each test_*() method.
    pass

func after_each():
    # Called after each test_*() method (even if it failed/errored).
    pass

func after_all():
    # Called once after all test methods in this file.
    pass
```

### 4.4 Assertion Helpers

The built-in `assert()` works but produces terse messages. The test runner catches assertion failures and reports them. For richer assertions, the runner injects a helper object accessible via the `_test` property:

```gdscript
extends RefCounted

func test_with_helpers():
    var result = calculate_damage(10, 5)
    _test.assert_eq(result, 50, "10 attack * 5 multiplier should be 50")
    _test.assert_gt(result, 0, "Damage should be positive")
    _test.assert_true(is_critical(result), "50 damage should be critical")

func test_approximate():
    var pos = move_towards(Vector2.ZERO, Vector2(1, 0), 0.5)
    _test.assert_almost_eq(pos, Vector2(0.5, 0), 0.01, "Should move halfway")

func test_signal_emitted():
    var obj = HealthComponent.new()
    _test.watch_signals(obj)
    obj.take_damage(100)
    _test.assert_signal_emitted(obj, "died")

func test_expected_error():
    # Mark that this test expects a push_error / assertion failure.
    _test.expect_error()
    some_function_that_should_fail()
```

**Available `_test` methods:**

| Method | Description |
|---|---|
| `assert_eq(a, b, msg?)` | `a == b` |
| `assert_ne(a, b, msg?)` | `a != b` |
| `assert_true(val, msg?)` | `val` is truthy |
| `assert_false(val, msg?)` | `val` is falsy |
| `assert_gt(a, b, msg?)` | `a > b` |
| `assert_lt(a, b, msg?)` | `a < b` |
| `assert_gte(a, b, msg?)` | `a >= b` |
| `assert_lte(a, b, msg?)` | `a <= b` |
| `assert_null(val, msg?)` | `val` is null |
| `assert_not_null(val, msg?)` | `val` is not null |
| `assert_has(collection, item, msg?)` | `item in collection` |
| `assert_almost_eq(a, b, eps, msg?)` | `abs(a - b) < eps` (supports Vector2/3) |
| `assert_typeof(val, type, msg?)` | `typeof(val) == type` |
| `assert_no_new_orphans(msg?)` | No new orphaned nodes since test started |
| `watch_signals(obj)` | Start recording signals on `obj` |
| `assert_signal_emitted(obj, signal, msg?)` | Signal was emitted since `watch_signals()` |
| `assert_signal_not_emitted(obj, signal, msg?)` | Signal was NOT emitted |
| `assert_signal_emitted_with(obj, signal, params, msg?)` | Signal was emitted with specific parameters |
| `skip(reason?)` | Skip this test method |
| `pending(msg?)` | Mark test as not yet implemented (distinct from skip) |
| `fail(msg)` | Immediately fail the test |

**Lifecycle helpers** (inspired by [GUT](https://github.com/bitwes/Gut)):

| Method | Description |
|---|---|
| `autofree(obj)` | `free()` the object automatically after the test method ends |
| `autoqfree(obj)` | `queue_free()` the object automatically after the test method ends |
| `add_child_autofree(node)` | Add to scene tree + auto `queue_free()` after test |

**Async helpers** (avoids raw `await` with no timeout):

| Method | Description |
|---|---|
| `wait_for_signal(sig, max_wait, msg?)` | Await a signal with timeout. Returns `true` if signal fired. |
| `wait_seconds(time, msg?)` | Await a duration. |
| `wait_frames(count, msg?)` | Await N process frames. |

The `_test` object is a `RefCounted`-based GDScript class (`res://addons/mcp_test/test_context.gd`) bundled with the module and injected at runtime. This is **not** exposed to the project normally -- it only exists during test execution.

### 4.5 Async Tests

Tests that need to wait for signals or frames use the `_test` async helpers, which have built-in timeouts so tests can't hang silently:

```gdscript
extends Node

func test_timer_fires():
    var timer = Timer.new()
    _test.add_child_autofree(timer)  # auto-cleaned after test
    timer.wait_time = 0.1
    timer.one_shot = true
    timer.start()
    var fired = await _test.wait_for_signal(timer.timeout, 2.0)
    _test.assert_true(fired, "Timer should complete within 2s")

func test_tween_completes():
    var sprite = Sprite2D.new()
    _test.add_child_autofree(sprite)
    var tween = create_tween()
    tween.tween_property(sprite, "modulate:a", 0.0, 0.1)
    await _test.wait_for_signal(tween.finished, 2.0)
    _test.assert_almost_eq(sprite.modulate.a, 0.0, 0.01)
```

The runner detects that a test method returns a coroutine (via GDScript's awaitable detection) and waits for it to complete, subject to the per-method timeout (default 5 seconds). The `_test` wait helpers provide a second layer of timeout within the test itself, with better error context.

### 4.6 Directory Structure Convention

```
res://tests/
├── test_inventory.gd          # Unit tests for inventory system
├── test_player.gd             # Unit tests for player logic
├── test_crafting.gd           # Unit tests for crafting
├── integration/
│   ├── test_shop_flow.gd      # Integration: shop purchase flow
│   └── test_save_load.gd      # Integration: save/load cycle
└── helpers/
    └── mock_database.gd       # Shared test helpers (not run as tests)
```

Files in subdirectories are discovered recursively. Files that don't match `test_*.gd` are ignored (so helper/mock files are safe).

---

## 5. Architecture

### 5.1 Component Diagram

```
┌────────────────────────────────────────────────────────────────────────────┐
│ MCP HTTP Thread                                                            │
│                                                                            │
│  test/run ───────► MCPTestTools::handle_run                                │
│       │                                                                    │
│       ▼  Phase 1: Compile Check (no game needed)                           │
│  Collect test files (glob res://tests/test_*.gd)                           │
│       │                                                                    │
│       ▼                                                                    │
│  For each file: GDScriptLanguage::validate()                               │
│       │                                                                    │
│       ├── compile error? → return immediately with errors (no game launch) │
│       │                                                                    │
│       ▼  Phase 2: Execute (game launch)                                    │
│  Write test manifest to user://mcp_test_manifest.json                      │
│       │                                                                    │
│       ▼                                                                    │
│  Launch built-in test runner scene via runtime/run_scene mechanism            │
│  MCPDebuggerBridge::set_game_launching()                                   │
│       │                                                                    │
│       ▼  (call_deferred)                                                   │
│  Editor Main Thread                                                        │
│       │                                                                    │
│       ▼                                                                    │
│  EditorRunBar::play_custom_scene("res://addons/mcp_test/runner.tscn")      │
│       │                                                                    │
│       ▼                                                                    │
│  ┌──────────────────────────────────────────────────┐                      │
│  │ Game Process (test runner scene)                  │                      │
│  │                                                   │                      │
│  │  1. Read manifest from user://                    │                      │
│  │  2. For each test file:                           │                      │
│  │     a. Load script                                │                      │
│  │     b. Instantiate (RefCounted or Node)           │                      │
│  │     c. Inject _test context                       │                      │
│  │     d. Call before_all()                           │                      │
│  │     e. For each test_*() method:                  │                      │
│  │        - Call before_each()                        │                      │
│  │        - Call test method (catch errors)           │                      │
│  │        - Call after_each()                         │                      │
│  │        - Send result via debugger message          │                      │
│  │     f. Call after_all()                            │                      │
│  │  3. Send final summary via debugger message       │                      │
│  │  4. Quit                                          │                      │
│  └──────────────────────────────────────────────────┘                      │
│       │                                                                    │
│       ▼  (debugger protocol: "mcp:test_result", "mcp:test_complete")       │
│  MCPDebuggerBridge::capture()                                              │
│       │                                                                    │
│       ▼                                                                    │
│  Accumulate results → _complete_pending("test_run", results)               │
│       │                                                                    │
│       ▼  (semaphore post)                                                  │
│  MCP HTTP Thread wakes → returns structured results to client              │
└────────────────────────────────────────────────────────────────────────────┘
```

### 5.2 Test Manifest

The MCP tool communicates the test plan to the runner scene via a JSON manifest written to `user://mcp_test_manifest.json`. This avoids passing complex data through command-line arguments.

```json
{
  "files": [
    "res://tests/test_inventory.gd",
    "res://tests/test_player.gd"
  ],
  "filter": "test_add*",
  "timeout_ms": 30000,
  "verbose": false,
  "method_timeout_ms": 5000
}
```

### 5.3 Debugger Messages

The test runner communicates results back to the editor via custom debugger messages, following the same `mcp:*` prefix convention used by existing tools.

**Per-method result:**

```
Message: "mcp:test_method_result"
Data: [
  "res://tests/test_inventory.gd",   // file path
  "test_add_item",                    // method name
  "passed",                           // status: "passed", "failed", "error", "skipped", "timeout"
  "",                                 // message (empty for passed)
  "",                                 // error file (empty for passed)
  0,                                  // error line (0 for passed)
  20,                                 // duration_ms
  []                                  // captured output lines
]
```

**Test run complete:**

```
Message: "mcp:test_complete"
Data: [
  total,            // int: total test count
  passed,           // int: passed count
  failed,           // int: failed count
  errors,           // int: error count
  skipped,          // int: skipped count
  duration_ms       // int: total duration
]
```

### 5.4 Bridge Integration

The bridge accumulates `test_method_result` messages into a results array and completes the pending request when `test_complete` arrives (or on game exit / timeout).

```
MCPDebuggerBridge (new members):
    struct TestRunState {
        Array results;          // Accumulated per-method results
        bool complete = false;
    };
    TestRunState test_run_state;
```

---

## 6. Implementation Plan

### 6.1 New Files

| File | Purpose | Est. Lines |
|---|---|---|
| `modules/mcp_server/tools/mcp_test_tools.h` | Tool handler class declaration | ~40 |
| `modules/mcp_server/tools/mcp_test_tools.cpp` | Tool handlers: `test/run`, `test/list` | ~350 |
| `modules/mcp_server/test_runner/runner.tscn` | Minimal scene: single Node with runner script | ~10 |
| `modules/mcp_server/test_runner/runner.gd` | Game-side test runner: loads manifest, executes tests, reports results | ~300 |
| `modules/mcp_server/test_runner/test_context.gd` | `_test` assertion helper object | ~200 |

### 6.2 Modified Files

| File | Changes |
|---|---|
| `modules/mcp_server/mcp_debugger_bridge.h` | Add `TestRunState`, `test_method_result` / `test_complete` message handling |
| `modules/mcp_server/mcp_debugger_bridge.cpp` | Handle `mcp:test_method_result` and `mcp:test_complete` in `capture()`, accumulate results, complete pending request |
| `modules/mcp_server/mcp_protocol.cpp` | Register test tools: `MCPTestTools::register_tools(&tool_registry)` |
| `modules/mcp_server/mcp_tool_registry.cpp` | Add `test/run` to long-running tools list |
| `modules/mcp_server/SCsub` | Include new source files in build |

### 6.3 Tool Handler: `test/run`

```cpp
Dictionary MCPTestTools::handle_run(const Dictionary &p_args) {
    String path = p_args.get("path", "");
    String filter = p_args.get("filter", "");
    int timeout = CLAMP((int)p_args.get("timeout", 30), 1, 120);
    bool verbose = p_args.get("verbose", false);

    // 1. Validate path.
    if (!path.begins_with("res://")) {
        return make_tool_error("Path must start with 'res://'. Got: " + path);
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
                "Test files must be named 'test_*.gd'. Got: " + path.get_file()
                + "\nRename to: " + path.get_base_dir().path_join("test_" + path.get_file()));
        }
        test_files.push_back(path);
    } else {
        // Directory: glob for test_*.gd recursively.
        String dir_path = path.ends_with("/") ? path : path + "/";
        test_files = _collect_test_files(dir_path);
        if (test_files.is_empty()) {
            return make_tool_error(
                "No test files found in " + dir_path
                + "\nTest files must be named 'test_*.gd'."
                + "\nExample: " + dir_path + "test_example.gd");
        }
    }

    // 3. Compile-check all test files.
    Array compile_errors;
    bool any_compile_fail = false;
    for (const String &file : test_files) {
        Dictionary validation = _validate_single_file(file);
        if (!(bool)validation["valid"]) {
            any_compile_fail = true;
        }
        compile_errors.push_back(validation);
    }

    if (any_compile_fail) {
        // Return compile errors without launching game.
        return _build_compile_error_response(compile_errors);
    }

    // 4. Check game not already running.
    MCPDebuggerBridge *bridge = _get_bridge();
    if (bridge && bridge->is_game_running()) {
        return make_tool_error(
            "Game is already running. Stop it with runtime/stop before running tests."
            "\nTests launch a separate game instance for execution.");
    }

    // 5. Write test manifest.
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
    Ref<FileAccess> f = FileAccess::open("user://mcp_test_manifest.json", FileAccess::WRITE);
    f->store_string(manifest_json);
    f->close();

    // 6. Launch test runner scene.
    bridge->set_game_launching();
    bridge->reset_test_run_state();

    PendingRequest *req = bridge->create_pending("test_run");

    callable_mp(EditorRunBar::get_singleton(),
        &EditorRunBar::play_custom_scene)
        .call_deferred("res://addons/mcp_test/runner.tscn", Vector<String>());

    // 7. Wait for results (blocks until test_complete or timeout).
    Dictionary result = bridge->wait_for_pending(req, (timeout + 15) * 1000);
    // +15s grace for game startup overhead on top of test timeout.

    // 8. Build response.
    return _build_test_results_response(result, verbose);
}
```

### 6.4 Tool Handler: `test/list`

```cpp
Dictionary MCPTestTools::handle_list(const Dictionary &p_args) {
    String path = p_args.get("path", "res://tests/");
    bool include_methods = p_args.get("include_methods", true);

    if (!path.begins_with("res://")) {
        return make_tool_error("Path must start with 'res://'. Got: " + path);
    }

    String dir_path = path.ends_with("/") ? path : path + "/";
    Vector<String> test_files = _collect_test_files(dir_path);

    if (test_files.is_empty()) {
        return make_tool_result(
            "No test files found in " + dir_path
            + "\nCreate test files named 'test_*.gd' with test_*() methods.",
            Dictionary());
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
            file_info["valid"] = true;  // Could also validate here.
            total_methods += methods.size();
        }

        files.push_back(file_info);
    }

    // Build text response.
    String text = "Found " + itos(files.size()) + " test file"
        + (files.size() != 1 ? "s" : "") + " in " + dir_path;
    if (include_methods) {
        text += " (" + itos(total_methods) + " test method"
            + (total_methods != 1 ? "s" : "") + ")";
    }
    text += ":\n";

    for (int i = 0; i < files.size(); i++) {
        Dictionary fi = files[i];
        text += "\n  " + String(fi["path"]);
        if (include_methods) {
            Array methods = fi["methods"];
            text += " (" + itos(methods.size()) + " method"
                + (methods.size() != 1 ? "s" : "") + ")";
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
```

### 6.5 Game-Side Test Runner (runner.gd)

```gdscript
# res://addons/mcp_test/runner.gd
extends Node

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

    for file_path in files:
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
    if instance.has_method("set") or "script" in instance:
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

        # Handle await (coroutine).
        if result is Signal or (result != null and result has_method("is_valid")):
            # Wait with timeout.
            var timer := get_tree().create_timer(method_timeout / 1000.0)
            var completed := await _race(result, timer.timeout)
            if not completed:
                status = "timeout"
                message = "Test method timed out after %dms" % method_timeout

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
        elif status == "timeout":
            _errors += 1
        else:
            _passed += 1

        # after_each
        if instance.has_method("after_each"):
            instance.after_each()

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

func _race(coroutine, timeout_signal: Signal) -> bool:
    # Returns true if coroutine finished first, false if timeout.
    var finished := false
    var timed_out := false

    coroutine.connect(func(): finished = true)
    timeout_signal.connect(func(): timed_out = true)

    while not finished and not timed_out:
        await get_tree().process_frame

    return finished
```

### 6.6 Test Context (test_context.gd)

```gdscript
# res://addons/mcp_test/test_context.gd
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
var _output := []
var _watched_signals := {}  # obj_id -> { signal_name -> [[params], ...] }
var _autofree_objects := []  # Objects to free() after test
var _autoqfree_nodes := []   # Nodes to queue_free() after test
var _orphan_count_before := 0
var _scene_tree: SceneTree   # Set by runner for add_child_autofree

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
    # Walk up past internal frames to find the test method call site.
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
        _record_failure(_fmt("Expected %s ≈ %s (diff %s, epsilon %s)", [a, b, diff, epsilon], msg))

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
        # Capture parameters on emission.
        obj.connect(sig_name, func(...args):
            _watched_signals[id][sig_name].append(args))

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
    wanted.connect(func(..._a): done = true, CONNECT_ONE_SHOT)
    timeout.connect(func(): timed_out = true, CONNECT_ONE_SHOT)
    while not done and not timed_out:
        await _scene_tree.process_frame
    return done

# --- Internal ---

func _fmt(template: String, args: Array, msg: String) -> String:
    var m := template % args if args.size() > 0 else template
    if msg: m += " | " + msg
    return m
```

---

## 7. Threading and Synchronization

### 7.1 Thread Safety Model

| Operation | Thread | Synchronization |
|---|---|---|
| Collect test files | MCP HTTP | Direct filesystem access (thread-safe) |
| `GDScriptLanguage::validate()` | MCP HTTP | Thread-safe (creates local parser) |
| Write manifest | MCP HTTP | `FileAccess::open()` (thread-safe) |
| Launch runner scene | MCP HTTP → deferred to main | `call_deferred` |
| Receive `test_method_result` | Editor main thread | `request_mutex` guarding `test_run_state` |
| Receive `test_complete` | Editor main thread | `_complete_pending("test_run", ...)` posts semaphore |
| Wait for results | MCP HTTP | Blocks on `PendingRequest` semaphore (existing pattern) |

### 7.2 Timeout Handling

Three timeout layers:

1. **Per-method timeout** (default 5s) — enforced game-side by the runner script. If a test method's coroutine doesn't complete in time, the runner moves on and reports `"timeout"`.

2. **Total test run timeout** (default 30s, max 120s) — enforced game-side. The runner checks elapsed time before starting each test method. If exceeded, remaining tests are skipped with `"timeout"` status.

3. **Bridge-side timeout** (test timeout + 15s grace) — enforced by the `PendingRequest` wait. If the game crashes or hangs and never sends `test_complete`, the bridge times out and returns whatever partial results accumulated. The +15s accounts for game startup time.

### 7.3 Game Crash Handling

If the game process crashes mid-test:
1. `_on_session_stopped()` fires on the editor main thread
2. Bridge detects active `test_run` pending request
3. Completes it with partial results + a `"game_crashed": true` flag
4. MCP tool returns accumulated results with a clear error message

---

## 8. Edge Cases and Considerations

### 8.1 Test File That Loads Heavy Resources

A test file might `preload()` or `load()` large scenes/textures. This is expected -- the game process handles loading the same as a normal game run. The timeout prevents infinite hangs. The LLM should be guided to write lightweight unit tests that don't depend on heavy assets.

### 8.2 Tests That Modify Project Files

A test might call `FileAccess.open("res://...", FileAccess.WRITE)`. This is dangerous but not preventable without sandboxing the game process (which Godot doesn't support). The tool description should warn against this, and the `_test` context does not provide file-writing helpers. This is a documentation concern, not an enforcement concern.

### 8.3 Tests That Never Return

An `await` on a signal that never fires, or an infinite loop. Handled by the per-method timeout (5s default). The runner catches the timeout and moves to the next test.

### 8.4 GDScript `assert()` vs. `_test` Assertions

GDScript's built-in `assert()` calls `push_error()` and can be configured to abort or continue. In debug builds (which the test runner uses), failed `assert()` triggers a debugger break. The test runner should configure `ProjectSettings` at startup to make `assert()` non-fatal, or catch the error via the debugger protocol. The `_test` assertions are always non-fatal and accumulate results cleanly. The tool description should recommend `_test` assertions over `assert()`.

### 8.5 Autoloads

The project's autoloads will be active during test execution (since we launch via `run_scene` with the project). This is intentional -- tests may depend on singletons. If isolation from autoloads is needed, a future `isolated` parameter could launch a minimal project.

### 8.6 No `test_*()` Methods Found

If a file matches `test_*.gd` but contains no `test_*()` methods, the tool returns a clear error with a hint showing the expected format.

### 8.7 Concurrent Test Runs

Only one test run can execute at a time (since it uses the game process). If the game is already running (tests or otherwise), the tool returns an error directing the user to stop the game first.

### 8.8 Relationship to GUT and gdUnit4

Projects may already use [GUT](https://github.com/bitwes/Gut) or [gdUnit4](https://github.com/MikeSchulze/gdUnit4). Our test runner is **independent and intentionally simpler** — it's designed for LLM-driven test-and-fix loops, not as a replacement for full test frameworks. Key differences:

- No mocking/doubling system (LLMs can write simple inline mocks)
- No editor inspector integration beyond the MCP panel
- No `extends GutTest` or `extends GdUnitTestSuite` — just plain GDScript

If a user has existing GUT tests, the LLM can run them via `runtime/run_scene` pointing at GUT's own runner scene. Our runner only discovers `test_*.gd` files — it won't accidentally pick up GUT test files since those extend `GutTest`, not `RefCounted`/`Node`, and our runner will just skip them gracefully if instantiation fails.

The assertion API (`assert_eq`, `assert_true`, `watch_signals`, etc.) intentionally matches GUT's naming so LLMs don't have to learn a different vocabulary. The `autofree`/`autoqfree`, `wait_for_signal`, and `assert_no_new_orphans` patterns are also borrowed from GUT because they solve real problems.

### 8.9 Runner Scene Packaging

The test runner scene (`runner.tscn`) and its scripts (`runner.gd`, `test_context.gd`) are bundled with the MCP module. They need to be accessible at `res://addons/mcp_test/` in the user's project. Two options:

**Option A: Copy on demand.** The `test/run` tool copies the runner files from the module's data directory into the project's `res://addons/mcp_test/` before launching. Adds a `.gdignore` to prevent the editor from importing them as game assets. Cleaned up after the test run.

**Option B: Custom scene from memory.** Build the runner scene programmatically in C++ and launch it without a `.tscn` file. More complex but avoids file pollution.

**Recommended: Option A** for simplicity and debuggability. The files are small and temporary.

---

## 9. LLM Guidance (Tool Description)

The tool description embedded in the MCP `tools/list` response should guide the LLM toward effective test writing:

```
Run GDScript tests to verify code behavior. Write test files in res://tests/
named test_*.gd with test_*() methods. Each method should test one behavior.

WORKFLOW:
1. Write your code with native file tools, then call editor/scan_filesystem
2. Write a test file with native file tools, then call editor/scan_filesystem
3. Check for compile errors (gdscript/check_errors)
4. Run the tests (test/run)
5. Fix failures and repeat

TEST FILE FORMAT:
  extends RefCounted

  func test_example():
      var result = my_function(2, 3)
      _test.assert_eq(result, 5, "2 + 3 should equal 5")

ASSERTIONS: Use _test.assert_eq(), assert_true(), assert_gt(),
assert_almost_eq(), assert_has(), assert_signal_emitted(), etc.
Use _test over GDScript's built-in assert() for better error messages.

TIPS:
- Extend RefCounted for pure logic tests (fastest)
- Extend Node if you need scene tree access (signals, process, etc.)
- Use before_each()/after_each() for setup/teardown
- Keep tests fast — avoid loading heavy scenes/textures
- One assertion concept per test method
```

---

## 10. Testing Strategy

### 10.1 Manual Testing

1. Create a test project with known-good and known-bad tests
2. `test/list` → verify discovery of test files and methods
3. `test/run` on passing tests → verify all-pass response
4. `test/run` on failing tests → verify failure messages with line numbers
5. `test/run` on file with syntax error → verify compile error returned without game launch
6. `test/run` on directory → verify multi-file execution
7. `test/run` with filter → verify only matching methods run
8. `test/run` with timeout → verify hanging test is killed and reported
9. `test/run` while game running → verify error message
10. Test with `extends Node` → verify scene tree lifecycle
11. Test with `await` → verify async test completion
12. Test with `before_each`/`after_each` → verify lifecycle hooks

### 10.2 Python Integration Tests

Extend the existing `tests/` Python test suite:

```python
@pytest.mark.p1
def test_run_passing_tests(client, project):
    result = client.call_tool("test/run", {"path": "res://tests/test_basic.gd"})
    structured = get_structured(result)
    assert structured["summary"]["passed"] == structured["summary"]["total"]
    assert structured["summary"]["failed"] == 0

@pytest.mark.p1
def test_run_failing_test(client, project):
    result = client.call_tool("test/run", {"path": "res://tests/test_failures.gd"})
    structured = get_structured(result)
    assert structured["summary"]["failed"] > 0
    # Verify failure has file/line info.
    for f in structured["files"]:
        for t in f["tests"]:
            if t["status"] == "failed":
                assert "line" in t
                assert t["line"] > 0

@pytest.mark.p1
def test_run_compile_error(client, project):
    # Write a bad test file using native file I/O + scan.
    # (In practice, the LLM writes via its native tools and calls editor/scan_filesystem)
    result = client.call_tool("test/run", {"path": "res://tests/test_broken.gd"})
    structured = get_structured(result)
    assert structured["files"][0]["compile_ok"] == False
    assert len(structured["files"][0]["errors"]) > 0

@pytest.mark.p1
def test_list_tests(client, project):
    result = client.call_tool("test/list", {"path": "res://tests/"})
    structured = get_structured(result)
    assert structured["file_count"] > 0
    assert structured["total_methods"] > 0

@pytest.mark.p2
def test_run_with_filter(client, project):
    result = client.call_tool("test/run", {
        "path": "res://tests/test_basic.gd",
        "filter": "test_add*"
    })
    structured = get_structured(result)
    for f in structured["files"]:
        for t in f["tests"]:
            assert t["method"].startswith("test_add")
```

---

## 11. Future Enhancements

1. **Code coverage.** Track which lines of the code-under-test were executed during the test run. Would require GDScript instrumentation or debugger-based line tracking.
2. **Parameterized tests.** Allow test methods to declare parameter sets (e.g., `test_damage.params = [[10, 5, 50], [0, 5, 0]]`).
3. **Snapshot testing.** Compare complex output (scene trees, dictionaries) against saved snapshots.
4. **Parallel execution.** Run test files in separate game instances concurrently (leveraging the existing multi-instance MCP support).
5. **Test generation.** A `test/generate` tool that reads a GDScript file and generates a skeleton test file with one `test_*()` method per public function.
6. **Isolation mode.** Launch tests without autoloads for pure unit testing.
7. **Watch mode.** Re-run tests automatically when source files change (useful for interactive development sessions).
8. **Performance benchmarks.** `bench_*()` methods that run multiple iterations and report timing statistics.
9. **Mocking/doubling.** Inspired by GUT's `double()` and `stub()` — auto-generate empty implementations of a class for dependency injection. Deferred because LLMs can write simple inline mocks.
10. **`simulate(obj, frames, delta)`.** Call `_process`/`_physics_process` N times without running real frames. Useful for deterministic game logic testing.
11. **JUnit XML export.** Write results to a JUnit XML file for CI integration (both GUT and gdUnit4 support this).

---

## 12. MCP Status Panel Integration

Test results should appear in the MCP bottom panel so the developer can see what the LLM is testing, watch results arrive in real time, and review history across runs -- even without reading the MCP tool response JSON.

### 12.1 Design Goals

1. **Live results.** Each test method result appears in the panel as it completes, not only after the full run finishes. This gives immediate feedback during long test runs.
2. **Glanceable summary.** A compact summary bar shows pass/fail/error counts with color-coded indicators. You should be able to glance at it the same way you glance at a CI badge.
3. **Drill-down.** Click a file row to expand and see individual test results. Click a failed test to see the assertion message and source location.
4. **History.** The panel retains the last N test runs so you can see whether things are improving or regressing across iterations.
5. **Non-intrusive.** The test results area is collapsed by default and only expands when a test run starts. It doesn't steal focus or interfere with the request log.

### 12.2 Layout

The test results section is inserted between the **Debugger Bridge panel** and the **Request Log** as a collapsible region. When no tests have been run, it shows a single-line placeholder. When a test run is active or has completed, it expands.

```
┌──────────────────────────────────────────────────────────────────────────┐
│ MCP Status Panel (existing)                                              │
│                                                                          │
│ ┌─────────────┐  ┌─────────────┐  ┌──────────────────────────────────┐  │
│ │ Server      │  │ Debugger    │  │ Connected Clients                │  │
│ │ ● Running   │  │ ● Game: Run │  │ [Tree: sessions]                 │  │
│ │ :6009       │  │ Heartbeat OK│  │                                  │  │
│ └─────────────┘  └─────────────┘  └──────────────────────────────────┘  │
│                                                                          │
│ ── Test Results ─── [Run #3] ── 8 passed  1 failed  0 errors ── 0.45s ──│
│ ┌────────────────────────────────────────────────────────────────────┐   │
│ │ ▶ res://tests/test_inventory.gd          3/3 passed       0.12s  │   │
│ │ ▶ res://tests/test_player.gd             4/4 passed       0.18s  │   │
│ │ ▼ res://tests/test_crafting.gd           1/2  1 FAILED    0.15s  │   │
│ │   │  ✓ test_craft_item                                    0.12s  │   │
│ │   │  ✗ test_craft_without_materials                       0.03s  │   │
│ │   │     Assertion failed: Should not craft without materials      │   │
│ │   │     at res://tests/test_crafting.gd:24                       │   │
│ └────────────────────────────────────────────────────────────────────┘   │
│ ◀ Run #1  Run #2  [Run #3] ▶                          [Clear History]   │
│                                                                          │
│ ─────────────────────── [separator] ─────────────────────────────────────│
│ Filter: [________] [All Methods ▾] [All Status ▾]  ☑ Auto-scroll  Clear │
│ ┌─ Request Log ──────────────────────────────────────────────────────┐   │
│ │ Time     Method                  Client   Status  Duration        │   │
│ │ ...                                                               │   │
│ └────────────────────────────────────────────────────────────────────┘   │
│ ┌─ Tool Summary ─────────────────────────────────────────────────────┐   │
│ │ ...                                                               │   │
│ └────────────────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────────┘
```

### 12.3 Components

#### 12.3.1 Summary Header Bar

A single `HBoxContainer` row that is always visible (even when collapsed):

```
── Test Results ─── [Run #3] ── 8 passed  1 failed  0 errors ── 0.45s ──
```

| Element | Control | Behavior |
|---|---|---|
| "Test Results" label | `Label` | Static, dimmed `Color(0.7, 0.7, 0.7)` when no runs yet |
| Run badge | `Label` | Shows current run number `[Run #N]`, highlighted during active run |
| Pass count | `Label` | Green `Color(0.4, 0.9, 0.4)`, e.g., "8 passed" |
| Fail count | `Label` | Red `Color(0.9, 0.3, 0.3)`, e.g., "1 failed" — **hidden when 0** |
| Error count | `Label` | Yellow `Color(0.9, 0.8, 0.3)`, e.g., "2 errors" — **hidden when 0** |
| Skip count | `Label` | Gray `Color(0.6, 0.6, 0.6)`, e.g., "1 skipped" — **hidden when 0** |
| Duration | `Label` | Dimmed, e.g., "0.45s" |
| Collapse toggle | `Button` | `▼` / `▶` to expand/collapse the results tree |

During an active test run, the summary updates in real time as `mcp:test_method_result` messages arrive. The run badge pulses with a subtle yellow background `Color(0.3, 0.3, 0.15, 0.3)` while running.

When no tests have ever been run:
```
── Test Results ─── No test runs yet ──────────────────────────────────────
```

#### 12.3.2 Results Tree

A `Tree` control with expandable file-level rows and per-method children:

**Columns:**

| # | Title | Width | Expand | Content |
|---|---|---|---|---|
| 0 | Name | 300px | true | File path or method name (indented) |
| 1 | Result | 120px | false | "3/3 passed", "1/2  1 FAILED", or status icon + text |
| 2 | Duration | 70px | false | "0.12s", "23ms" |
| 3 | Message | remainder | true | Assertion message (for failed/errored tests only) |

**File rows (collapsed):**

```
▶ res://tests/test_inventory.gd          3/3 passed       0.12s
```

- Background: default (all passed) or subtle red tint `Color(0.15, 0.08, 0.08, 0.3)` (any failures)
- Result text: green if all pass, red count if failures

**File rows (expanded):**

```
▼ res://tests/test_crafting.gd           1/2  1 FAILED    0.15s
  │  ✓ test_craft_item                                    0.12s
  │  ✗ test_craft_without_materials                       0.03s    Assertion failed: Should not...
  │     at res://tests/test_crafting.gd:24
```

**Method row colors:**

| Status | Icon | Color |
|---|---|---|
| passed | `✓` | Green `Color(0.4, 0.9, 0.4)` |
| failed | `✗` | Red `Color(0.9, 0.3, 0.3)` |
| error | `✗` | Yellow `Color(0.9, 0.8, 0.3)` |
| skipped | `○` | Gray `Color(0.6, 0.6, 0.6)` |
| timeout | `⏱` | Yellow `Color(0.9, 0.8, 0.3)` |
| running | `◌` (spinner) | White, animated dot cycle |

**Compile error rows:**

If a file failed compilation, it shows as a special row:

```
▼ res://tests/test_broken.gd             COMPILE ERROR
  │  Line 5, Col 2: Expected ":" after "if" condition
  │  Line 12, Col 0: Unexpected "end of file"
```

All-red text `Color(0.9, 0.3, 0.3)` with no method children.

**Interaction:**
- Click file row → toggle expand/collapse (same as request log JSON expand)
- Double-click method row → open file in Script Editor at that line (if error/failure has line info)
- Right-click method → "Copy assertion message", "Re-run this file"

#### 12.3.3 Run History Navigation

Below the tree, a small `HBoxContainer` with run selector:

```
◀ Run #1  Run #2  [Run #3] ▶                          [Clear History]
```

| Element | Control | Behavior |
|---|---|---|
| `◀` / `▶` | `Button` | Navigate to prev/next run |
| Run labels | `Button` (toggle style) | Click to view that run's results. Current run is highlighted |
| Clear History | `Button` | Clears all stored runs, resets to placeholder |

**Maximum history:** 10 runs stored. Oldest runs are evicted when the 11th arrives. Each run stores the full structured result dictionary.

Run buttons show a tiny color indicator:
- Green dot: all passed
- Red dot: any failures
- Yellow dot: compile errors or runtime errors
- Gray dot: no tests found / empty run

### 12.4 Data Flow

```
┌─ Game Process ───────────────────────────────────────────┐
│  runner.gd sends:                                        │
│    mcp:test_method_result  (per method, as they finish)  │
│    mcp:test_complete       (when all done)               │
└──────────────────────┬───────────────────────────────────┘
                       │ debugger protocol
                       ▼
┌─ MCPDebuggerBridge (editor main thread) ─────────────────┐
│  capture() handles:                                      │
│    "test_method_result" → append to test_run_state        │
│                         → push to MCPTestEventBuffer      │
│    "test_complete"      → finalize test_run_state         │
│                         → push summary event              │
│                         → _complete_pending("test_run")   │
└──────────────────────┬───────────────────────────────────┘
                       │ read by UI (every frame, fast path)
                       ▼
┌─ MCPStatusPanel (editor main thread) ────────────────────┐
│  _update_test_results():                                 │
│    Read new events from MCPTestEventBuffer                │
│    Update summary bar counts                             │
│    Add/update tree rows for each method result           │
│    When "complete" event arrives:                         │
│      Stop pulsing animation                              │
│      Store run in history ring buffer                    │
└──────────────────────────────────────────────────────────┘
```

### 12.5 MCPTestEventBuffer

A lightweight ring buffer (similar to `MCPEventBuffer`) that holds test events for the UI to consume. Separate from the request event buffer so test results aren't mixed into the HTTP log.

```cpp
struct MCPTestEvent {
    enum Type {
        TEST_RUN_STARTED,     // Test run began (with file list)
        TEST_METHOD_RESULT,   // Individual method result
        TEST_FILE_COMPILE_ERROR,  // File failed compilation
        TEST_RUN_COMPLETE,    // All tests finished
    };

    uint64_t seq = 0;
    Type type = TEST_METHOD_RESULT;
    uint64_t timestamp_usec = 0;

    // Run identification.
    int run_number = 0;

    // For TEST_METHOD_RESULT:
    String file_path;
    String method_name;
    String status;          // "passed", "failed", "error", "skipped", "timeout"
    String message;         // Assertion message or error description
    String error_file;      // File where error occurred (may differ from test file)
    int error_line = 0;
    int duration_ms = 0;

    // For TEST_FILE_COMPILE_ERROR:
    // file_path set above, message contains formatted error list.
    Array compile_errors;   // [{line, column, message}, ...]

    // For TEST_RUN_COMPLETE:
    int total = 0;
    int passed = 0;
    int failed = 0;
    int errors = 0;
    int skipped = 0;
    int total_duration_ms = 0;
};

class MCPTestEventBuffer {
public:
    static const int CAPACITY = 500;  // Enough for ~50 test runs of ~10 methods each

    void push(const MCPTestEvent &p_event);
    Vector<MCPTestEvent> read_since(uint64_t p_cursor, int p_limit = 100) const;
    uint64_t latest_seq() const;
    void clear();

private:
    Vector<MCPTestEvent> events;
    uint64_t next_seq = 1;
    int write_pos = 0;
    int count = 0;
    mutable Mutex mutex;
};
```

### 12.6 Panel State

```cpp
// Added to MCPStatusPanel:

// -- Test Results --
HBoxContainer *test_summary_bar = nullptr;
Label *test_run_label = nullptr;
Label *test_passed_label = nullptr;
Label *test_failed_label = nullptr;
Label *test_errors_label = nullptr;
Label *test_skipped_label = nullptr;
Label *test_duration_label = nullptr;
Button *test_collapse_button = nullptr;

Tree *test_results_tree = nullptr;

HBoxContainer *test_history_bar = nullptr;
Button *test_prev_button = nullptr;
Button *test_next_button = nullptr;
Button *test_clear_history_button = nullptr;
Vector<Button *> test_run_buttons;

// State.
uint64_t last_test_event_cursor = 0;
int current_run_number = 0;
int viewing_run_number = 0;  // Which run is displayed (may differ from current during history nav)
bool test_section_expanded = true;

struct TestRunHistory {
    int run_number;
    int total, passed, failed, errors, skipped;
    int duration_ms;
    Array file_results;  // Full structured results for drill-down.
};

Vector<TestRunHistory> test_history;  // Last 10 runs.
static const int MAX_TEST_HISTORY = 10;
```

### 12.7 Update Logic

Test results use the **fast update path** (every frame), same as the request log:

```cpp
void MCPStatusPanel::_update_test_results() {
    MCPTestEventBuffer *buffer = /* from bridge or protocol */;
    if (!buffer) return;

    Vector<MCPTestEvent> new_events = buffer->read_since(last_test_event_cursor, 100);
    if (new_events.is_empty()) return;

    for (const MCPTestEvent &evt : new_events) {
        last_test_event_cursor = evt.seq;

        switch (evt.type) {
            case MCPTestEvent::TEST_RUN_STARTED: {
                // New run starting.
                current_run_number = evt.run_number;
                viewing_run_number = current_run_number;
                _clear_test_tree();
                _set_test_summary_running(evt.run_number);
                if (!test_section_expanded) {
                    _expand_test_section();
                }
            } break;

            case MCPTestEvent::TEST_METHOD_RESULT: {
                if (evt.run_number != viewing_run_number) break;
                _add_or_update_test_method_row(evt);
                _update_test_summary_counts();
            } break;

            case MCPTestEvent::TEST_FILE_COMPILE_ERROR: {
                if (evt.run_number != viewing_run_number) break;
                _add_compile_error_row(evt);
                _update_test_summary_counts();
            } break;

            case MCPTestEvent::TEST_RUN_COMPLETE: {
                _finalize_test_run(evt);
                _store_in_history(evt);
                _rebuild_history_bar();
            } break;
        }
    }
}
```

### 12.8 Interaction: Open in Script Editor

Double-clicking a failed test method opens the file at the error line:

```cpp
void MCPStatusPanel::_on_test_item_activated() {
    TreeItem *selected = test_results_tree->get_selected();
    if (!selected) return;

    Dictionary meta = selected->get_metadata(0);
    String file = meta.get("error_file", "");
    int line = meta.get("error_line", 0);

    if (file.is_empty() || line <= 0) {
        // Fallback to the test file itself.
        file = meta.get("file_path", "");
        line = 1;
    }

    if (!file.is_empty()) {
        // Open in script editor.
        Ref<Script> script = ResourceLoader::load(file);
        if (script.is_valid()) {
            EditorInterface::get_singleton()->edit_script(script, line);
        }
    }
}
```

### 12.9 Live Progress Animation

While tests are running, the active method row shows a simple spinner:

```
  │  ◌ test_craft_without_materials                       ...
```

The spinner cycles through `◌ ○ ◎ ●` (or simpler: just a yellow dot that blinks) using the fast update loop. When the result arrives, it's replaced with `✓` / `✗`.

The summary bar's run badge also pulses:

```cpp
// During active run (called every frame):
if (is_test_running) {
    float pulse = 0.5f + 0.5f * Math::sin(OS::get_ticks_usec() / 300000.0f);
    test_run_label->add_theme_color_override("font_color",
        Color(0.9, 0.8, 0.3).lerp(Color(1, 1, 1), pulse));
}
```

### 12.10 Compact Mode

When the panel height is limited, the test section compresses gracefully:

- **Summary bar always visible** (single line, ~20px)
- **Tree collapses** to `0` height when toggled with `▼`/`▶`
- **History bar** hides when collapsed
- **Tree shows max 8 visible rows** before scrolling (set via `set_custom_minimum_size`)

This ensures the test results area doesn't squeeze out the request log below it.

### 12.11 Modified Files for Panel Integration

| File | Changes |
|---|---|
| `editor/mcp_status_panel.h` | Add test result controls, state, `TestRunHistory`, update methods |
| `editor/mcp_status_panel.cpp` | Build test section UI, `_update_test_results()`, history navigation, script editor integration |
| `editor/mcp_status_data.h` | Add `MCPTestEvent` struct and `MCPTestEventBuffer` class |
| `editor/mcp_status_data.cpp` | Implement `MCPTestEventBuffer` (same pattern as `MCPEventBuffer`) |
| `mcp_debugger_bridge.h` | Add `MCPTestEventBuffer` member, `push_test_event()` method |
| `mcp_debugger_bridge.cpp` | In `capture()`: push `MCPTestEvent` when handling `mcp:test_method_result` and `mcp:test_complete` |
