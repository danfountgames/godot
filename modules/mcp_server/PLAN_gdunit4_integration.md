# PLAN: GdUnit4 Deep Integration

**Branch:** Recommend **`mcp-server`** (not `build`)
**Priority:** P1
**Effort:** Medium (3-4 days)
**Dependencies:** GdUnit4 addon installed in user's project

---

## Why `mcp-server` Branch, Not `build`

GdUnit4 is a **GDScript addon** (`addons/gdUnit4/`), not a C++ module.
The integration is about the MCP server discovering, running, and reporting
GdUnit4 tests — it doesn't change the Godot build itself. The test runner
infrastructure is already in `mcp_testing_tools.cpp` (the existing `testing/run`
tool). GdUnit4 integration extends that infrastructure.

If GdUnit4 were compiled as a C/C++ module, `build` would be the right branch.
But since it's GDScript installed per-project, `mcp-server` is correct.

---

## Current State

The MCP server already has a custom test framework in `mcp_testing_tools.cpp`:
- `testing/run` — Launches a runner scene, executes `test_*.gd` files with `test_*()` methods
- `testing/list` — Lists available test files and their methods
- `MCPTestContext` — Assertion library (assert_eq, assert_true, watch_signals, etc.)
- `MCPTestRunner` — GDScript runner that loads a manifest and executes tests

This is a **lightweight built-in framework**. GdUnit4 is much richer:
- Parameterized tests (`@GdUnitTestSuite`)
- Fuzzing / random data
- Scene runners with physics simulation
- Mocking and spying
- HTML report generation
- JUnit XML output
- VS Code integration
- CI/CD GitHub Action

---

## Integration Strategy: Dual-Mode Testing

**Don't replace the built-in runner. Add GdUnit4 as a second backend.**

```
testing/run              → existing MCPTestRunner (works in any project)
testing/list             → existing discovery

testing/gdunit/run       → NEW: run GdUnit4 tests (requires addon)
testing/gdunit/list      → NEW: discover GdUnit4 test suites
testing/gdunit/report    → NEW: get last test run results (HTML/JUnit)
```

The LLM uses the built-in runner for quick ad-hoc tests.
It uses GdUnit4 for the project's real test suite.

---

## New Tools (3 tools)

### Tool 1: `testing/gdunit/discover`

**Purpose:** Detect if GdUnit4 is installed, list all test suites and methods.

**Parameters:**
- `path` (string, optional, default "res://test"): Directory to scan

**Implementation:**
```cpp
// 1. Check if addons/gdUnit4/ exists in the project (via FileAccess).
//    If not, return error with install instructions.
// 2. Scan path recursively for files matching:
//    - Class extends GdUnitTestSuite (grep first line / class_name)
//    - OR files in test/ with test_*.gd naming convention
// 3. For each test file:
//    a. Parse function names starting with test_
//    b. Parse @GdUnitParameterizedTest annotations if present
//    c. Extract class_name
// 4. Return structured list.
```

**Returns:**
```json
{
  "gdunit4_installed": true,
  "gdunit4_version": "4.4.0",
  "suites": [
    {
      "path": "res://test/test_player.gd",
      "class_name": "TestPlayer",
      "extends": "GdUnitTestSuite",
      "tests": [
        {"name": "test_player_movement", "parameterized": false},
        {"name": "test_player_takes_damage", "parameterized": false},
        {"name": "test_player_health_boundaries", "parameterized": true, "data_points": 5}
      ]
    }
  ],
  "total_suites": 12,
  "total_tests": 47
}
```

**LLM description:**
> Discover GdUnit4 test suites in the project. Returns whether GdUnit4 is installed,
> its version, and a list of all test suites with their test methods. If GdUnit4 is
> not installed, returns instructions for installing it. Use testing/list for the
> built-in MCP test runner instead.

---

### Tool 2: `testing/gdunit/run`

**Purpose:** Execute GdUnit4 tests and return structured results.

**Parameters:**
- `path` (string, optional): Specific test file or directory (default: "res://test")
- `filter` (string, optional): Filter test names by pattern (e.g., "test_player*")
- `verbose` (boolean, optional, default false): Include stdout per test
- `timeout` (integer, optional, default 120): Max seconds for entire run

**Implementation:**

GdUnit4 provides a command-line runner. The approach:

```cpp
// GdUnit4's CLI runner is invoked by running a special scene.
// The MCP server needs to:
//
// 1. Verify GdUnit4 is installed (check addons/gdUnit4/).
//
// 2. Write a GdUnit4 run configuration file:
//    user://mcp_gdunit_config.json with:
//    {
//      "test_paths": ["res://test/test_player.gd"],
//      "filter": "test_player*",
//      "report_format": "json"  // or "junit_xml"
//    }
//
// 3. Launch the project with GdUnit4's runner scene:
//    EditorRunBar::play_custom_scene("res://addons/gdUnit4/src/core/GdUnitRunner.tscn")
//    (or however GdUnit4 exposes its runner)
//
// 4. GdUnit4 sends results to stdout.
//    Capture via the existing output ring buffer.
//    Parse structured results from output.
//
// 5. OR: Use the MCP bridge to communicate results.
//    Inject a small wrapper script that:
//    a. Instantiates GdUnit4's runner programmatically
//    b. Hooks into its result signals
//    c. Sends results via EngineDebugger.send_message("mcp:gdunit_result", ...)
//
// Option 5 is more reliable than stdout parsing.

// ALTERNATIVE APPROACH: GdUnit4 command-line mode
// GdUnit4 supports: --gdUnit4 --test_root=res://test --report_dir=user://
// We can run the project with these args and parse the JUnit XML report.
// This is the simplest approach.
```

**Recommended implementation:**

```cpp
Dictionary MCPGdUnitTools::handle_run(const Dictionary &p_args) {
    // 1. Check GdUnit4 installed
    if (!_is_gdunit_installed()) {
        return make_tool_error(
            "GdUnit4 not found in project.\n\n"
            "Install via AssetLib: search 'GdUnit4' and enable the plugin.\n"
            "Or: git clone https://github.com/MikeSchulze/gdUnit4 into addons/");
    }

    // 2. Build arguments for Godot CLI
    String test_path = p_args.get("path", "res://test");
    String filter = p_args.get("filter", "");
    int timeout = (int)p_args.get("timeout", 120);

    // 3. Create a runner manifest
    Dictionary manifest;
    manifest["test_root"] = test_path;
    manifest["filter"] = filter;
    manifest["report_path"] = "user://mcp_gdunit_report.xml";
    _write_manifest(manifest);

    // 4. Launch via MCP's existing runner pattern
    //    Copy a thin wrapper scene that:
    //    - Loads GdUnit4 runner
    //    - Feeds it the manifest
    //    - Reports results via mcp: debugger messages
    //    - Exits when done
    _copy_gdunit_bridge_files();
    EditorRunBar::get_singleton()->play_custom_scene(
        "res://addons/mcp_test/gdunit_bridge.tscn");

    // 5. Wait for results (same pattern as testing/run)
    MCPDebuggerBridge *bridge = _get_bridge();
    PendingRequest *req = bridge->create_pending("gdunit_complete");
    Dictionary result = bridge->wait_for_pending(req, timeout * 1000);

    // 6. Cleanup
    _cleanup_gdunit_bridge_files();

    // 7. Format results
    return _format_gdunit_results(result);
}
```

**Returns:**
```json
{
  "status": "completed",
  "summary": {
    "total": 47,
    "passed": 44,
    "failed": 2,
    "errors": 1,
    "skipped": 0,
    "duration_ms": 3400
  },
  "failures": [
    {
      "suite": "TestPlayer",
      "test": "test_player_takes_damage",
      "file": "res://test/test_player.gd",
      "line": 34,
      "message": "Expected 75 but was 100",
      "assertion": "assert_int(player.health).is_equal(75)",
      "output": "...",
      "duration_ms": 45
    }
  ],
  "report_path": "user://mcp_gdunit_report.xml"
}
```

**LLM description:**
> Run GdUnit4 tests in the project. Requires GdUnit4 addon installed. Returns
> structured results with pass/fail counts, failure details with file/line/message,
> and JUnit XML report path. Supports filtering by path or pattern. For quick
> ad-hoc tests without GdUnit4, use the built-in testing/run instead.

**Annotations:** `readOnly=true, destructive=false, idempotent=true`
**Progress handler:** Yes (long-running, SSE streaming per-test results)

---

### Tool 3: `testing/gdunit/report`

**Purpose:** Retrieve the last GdUnit4 test report.

**Parameters:**
- `format` (string, optional, default "summary"): "summary", "junit_xml", "full"

**Implementation:**
```cpp
// Read the JUnit XML report from user://mcp_gdunit_report.xml
// Parse and return in requested format.
// "summary" → just pass/fail counts
// "junit_xml" → raw XML string
// "full" → parsed JSON with all test details
```

**LLM description:**
> Retrieve the results from the last GdUnit4 test run. Available in summary,
> full JSON, or raw JUnit XML format. The JUnit XML can be used for CI
> integration. Use testing/gdunit/run first.

---

## GdUnit4 Bridge Scene

We need a thin GDScript wrapper that bridges GdUnit4's runner with the MCP
debugger message protocol:

```gdscript
# addons/mcp_test/gdunit_bridge.gd
# Temporary wrapper — injected by MCP, cleaned up after test run.
extends Node

func _ready():
    var manifest = _load_manifest()
    var runner = _create_gdunit_runner(manifest)
    runner.connect("test_completed", _on_test_completed)
    runner.connect("suite_completed", _on_suite_completed)
    runner.connect("run_completed", _on_run_completed)
    add_child(runner)
    runner.start()

func _on_test_completed(result: Dictionary):
    EngineDebugger.send_message("mcp:gdunit_test_result", [
        result.suite, result.test, result.status,
        result.message, result.file, result.line,
        result.duration_ms, result.output
    ])

func _on_run_completed(summary: Dictionary):
    EngineDebugger.send_message("mcp:gdunit_complete", [
        summary.total, summary.passed, summary.failed,
        summary.errors, summary.skipped, summary.duration_ms
    ])
    # Write JUnit XML report
    _write_junit_report(summary)
    get_tree().quit()
```

---

## Detection & Fallback

```cpp
bool MCPGdUnitTools::_is_gdunit_installed() {
    // Check for the addon directory
    Ref<DirAccess> da = DirAccess::open("res://addons/gdUnit4");
    if (da.is_valid()) {
        // Verify key files exist
        return FileAccess::exists("res://addons/gdUnit4/plugin.cfg");
    }
    return false;
}

String MCPGdUnitTools::_get_gdunit_version() {
    // Parse plugin.cfg for version
    Ref<ConfigFile> cfg;
    cfg.instantiate();
    if (cfg->load("res://addons/gdUnit4/plugin.cfg") == OK) {
        return cfg->get_value("plugin", "version", "unknown");
    }
    return "unknown";
}
```

---

## Testing

```python
class TestGdUnitDiscovery:
    def test_not_installed(self, client):
        # Test project doesn't have GdUnit4
        result = client.call_tool("testing/gdunit/discover")
        assert result["gdunit4_installed"] == False

class TestGdUnitRun:
    @pytest.mark.skipif(not gdunit_available(), reason="GdUnit4 not installed")
    def test_run_passing_suite(self, client, running_game):
        result = client.call_tool("testing/gdunit/run", {"path": "res://test"})
        assert result["summary"]["total"] > 0
```

---

## File Changes Summary

| File | Change |
|------|--------|
| `tools/mcp_gdunit_tools.h` | **NEW** — class MCPGdUnitTools |
| `tools/mcp_gdunit_tools.cpp` | **NEW** — 3 tools + GdUnit4 bridge |
| `mcp_protocol.cpp` | Add `MCPGdUnitTools::register_tools(&tool_registry);` |
| `mcp_debugger_bridge.h` | Handle `mcp:gdunit_*` messages |
| `mcp_debugger_bridge.cpp` | Parse GdUnit4 result messages |
| `tests/test_gdunit_tools.py` | **NEW** — GdUnit4 integration tests |
| `README.md` | Update tool count, add GdUnit4 section |

---

## Future: GdUnit4 Test Generation

Once GdUnit4 integration is working, the LLM can:
1. Read project scripts via native file tools
2. Generate GdUnit4 test suites
3. Write them to `res://test/`
4. Run them via `testing/gdunit/run`
5. Read failures, fix code, re-run

This is a natural extension but doesn't need MCP tools — the LLM does it with
file read/write + the run tool.
