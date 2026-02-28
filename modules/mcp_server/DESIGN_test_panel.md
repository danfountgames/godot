# MCP Test Panel & Enhanced Assertions — Design Document

## 1. Motivation

The MCP server has a solid test runner: `testing/run` compiles-first, launches a runner scene,
reports structured results via the debugger bridge, and displays them in the MCPStatusPanel's
test results section. An LLM can write tests, run them, and iterate on failures.

But **humans can't use any of this without the LLM.**

The test results are buried inside the MCP Status panel (inside the AI main screen tab). There
is no way for a developer to:
- See which test files exist at a glance
- Click a button to run a single test, a file, or all tests
- Get results without switching to the AI tab
- Run tests from the script editor context menu or FileSystem dock

gdUnit4 solves this with a dedicated inspector panel. We want the same UX — but without
requiring an addon install and without the `GdUnitTestSuite` base class. Our tests should stay
minimal-ceremony: any `test_*.gd` file with `test_*()` methods, no inheritance required.

Additionally, the current assertion API (`_test.assert_eq`, `_test.assert_true`, etc.) produces
generic failure messages. gdUnit4's type-specific assertions give much better diagnostics:
"Array missing element: 'sword'" vs "Expected [a, b, c] == [a, b]". We can steal the useful
diagnostics without adopting the fluent API.

### Goals

1. **Test Panel** — dedicated editor dock showing discovered tests with run/filter/navigate controls
2. **Enhanced assertions** — type-specific assertions with diagnostic failure messages
3. **Editor integration** — run tests from FileSystem dock and script editor context menus
4. **Keep minimal ceremony** — no base class, no addon, no project contamination

### Non-goals

- Mocking/spying framework (too complex, overlaps with MCP runtime introspection)
- Fluent assertion API (more tokens for LLM, no benefit for the actual consumer)
- Scene runner with input simulation (MCP's runtime/input/* tools already do this)
- C# support
- CI/CD reporting formats (JUnit XML, HTML reports)

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│  MCPTestPanel (new C++ class)                               │
│  Extends VBoxContainer — added as editor bottom panel       │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  Toolbar: [▶ Run All] [▶ Run File] [↻ Rerun] [■ Stop]   │
│  │           [🔍 Filter...] [⚙ Settings]              │    │
│  └─────────────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  Test Tree (Tree widget)                            │    │
│  │                                                     │    │
│  │  📁 res://tests/                                    │    │
│  │    📄 test_inventory.gd                             │    │
│  │      ✓ test_add_item              12ms              │    │
│  │      ✗ test_remove_item           5ms   "Expected…" │    │
│  │      ✓ test_inventory_full        90ms              │    │
│  │    📄 test_player.gd                                │    │
│  │      ✓ test_take_damage           8ms               │    │
│  │      ○ test_respawn               --    (pending)   │    │
│  └─────────────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  Summary: 4 passed, 1 failed, 1 pending (0.12s)    │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
         │                              ▲
         │  Run tests                   │  Results
         ▼                              │
┌──────────────────────┐     ┌──────────────────────────────┐
│  MCPTestingTools      │     │  MCPDebuggerBridge           │
│  (existing C++)       │     │  MCPTestEventBuffer          │
│                       │     │  (existing ring buffer)      │
│  - Compile check      │     │                              │
│  - Write manifest     │     │  ← mcp:test_method_result    │
│  - Launch runner      │     │  ← mcp:test_complete         │
│  - Build response     │     │                              │
└──────────────────────┘     └──────────────────────────────┘
         │                              ▲
         ▼                              │
┌──────────────────────────────────────────────────────────┐
│  Game process (runner.gd + test_context.gd)              │
│  Embedded in C++ as string literals, copied to project   │
│  before each run, cleaned up after                       │
└──────────────────────────────────────────────────────────┘
```

### Key Reuse

- **Test execution**: reuses the existing `MCPTestingTools::handle_run()` pipeline
  (compile check → manifest → launch runner scene → wait for results)
- **Results delivery**: reuses `MCPTestEventBuffer` + `MCPDebuggerBridge::capture()`
- **Test discovery**: reuses `_collect_test_files()` and `_extract_test_methods()`
- **Script navigation**: reuses the same `EditorInterface::edit_script()` pattern
  from `MCPStatusPanel::_on_test_item_activated()`

### What's New

- `MCPTestPanel` — new C++ UI class (editor/mcp_test_panel.h/.cpp)
- Enhanced `test_context.gd` — additional assertion methods (embedded in mcp_testing_tools.cpp)
- `MCPServerPlugin` registration — adds bottom panel + context menu integrations
- Editor integration — filesystem dock & script editor context menus

---

## 3. Test Panel UI (MCPTestPanel)

### 3.1 Panel Placement

Added as an **editor bottom panel** (like Output, Debugger, Audio) via
`EditorPlugin::add_control_to_bottom_panel()`. This makes it always accessible regardless
of which main screen tab (2D, 3D, Script, AI) is active.

Bottom panel is better than a dock because:
- Docks compete for narrow side panel space — test trees need horizontal room
- Bottom panels match Godot's existing pattern for tool output (Debugger, Output)
- The panel can be hidden when not needed via the bottom panel tab bar

```cpp
// In MCPServerPlugin::_enter_tree() or constructor:
test_panel = memnew(MCPTestPanel);
test_panel->set_protocol(protocol);
test_panel->set_debugger_bridge(debugger_bridge.ptr());
add_control_to_bottom_panel(test_panel, TTR("Tests"));
```

### 3.2 Toolbar

```
┌────────────────────────────────────────────────────────────────────────┐
│ [▶ Run All] [▶ Run Selected] [↻ Rerun Failed] [■ Stop]  │ 🔍 Filter  │
└────────────────────────────────────────────────────────────────────────┘
```

| Button | Action | Shortcut | Notes |
|--------|--------|----------|-------|
| ▶ Run All | Run all discovered test files | Alt+Shift+T | Compile-check first |
| ▶ Run Selected | Run the selected file or method | Alt+T | Context-sensitive |
| ↻ Rerun Failed | Re-run only previously failed tests | — | Only visible after a run with failures |
| ■ Stop | Kill the running test game process | — | Only visible during a run |
| 🔍 Filter | LineEdit to filter test names | — | Live filter on tree (shows matching methods + their parent files) |

### 3.3 Test Tree

A `Tree` widget with 4 columns:

| Column | Width | Content |
|--------|-------|---------|
| 0 (Name) | Expanding | Hierarchical: directory → file → method |
| 1 (Status) | 80px fixed | Icon + "PASS" / "FAIL" / "SKIP" / "—" |
| 2 (Duration) | 70px fixed | "12ms" or "1.20s" |
| 3 (Message) | Expanding | Assertion failure message (failures only) |

**Hierarchy:**

```
📁 res://tests/
  📁 res://tests/unit/
    📄 test_inventory.gd          3/3 passed     0.12s
      ✓ test_add_item             PASS           12ms
      ✓ test_remove_item          PASS           5ms
      ✓ test_inventory_full       PASS           90ms
    📄 test_crafting.gd           1/2, 1 FAIL    0.08s
      ✓ test_craft_item           PASS           50ms
      ✗ test_craft_no_materials   FAIL           30ms    "Expected false, got true"
  📁 res://tests/integration/
    📄 test_save_load.gd          2/2 passed     0.35s
      ✓ test_save                 PASS           200ms
      ✓ test_load                 PASS           150ms
```

**Tree states:**

Before any run:
- All test methods show as "—" (not yet run) in gray
- File nodes show method count: "test_inventory.gd (3 methods)"

During a run:
- Currently executing test shows a pulsing indicator
- Results appear in real-time as `TEST_METHOD_RESULT` events arrive via `MCPTestEventBuffer`

After a run:
- Full results with status icons, durations, failure messages
- Failed tests are expanded by default, passed files can be collapsed

**Node states and icons:**

| State | Icon | Color | When |
|-------|------|-------|------|
| Not run | — | Gray (0.5, 0.5, 0.5) | Before first run |
| Running | ⟳ (animated) | Yellow pulse | Currently executing |
| Passed | ✓ | Green (0.4, 0.9, 0.4) | Test passed |
| Failed | ✗ | Red (0.9, 0.3, 0.3) | Assertion failure |
| Error | ✗ | Orange (0.9, 0.8, 0.3) | Runtime error / crash |
| Skipped | ○ | Gray (0.6, 0.6, 0.6) | `_test.skip()` called |
| Pending | ○ | Gray (0.6, 0.6, 0.6) | `_test.pending()` called |
| Timeout | ⏱ | Orange (0.9, 0.8, 0.3) | Method or run timeout |
| Compile Error | ✗ | Red (0.9, 0.3, 0.3) | Script won't compile |

### 3.4 Summary Bar

Fixed bar at the bottom of the panel:

```
✓ 8 passed  ✗ 1 failed  ⚠ 0 errors  ○ 1 skipped  ⏱ 0.45s  │  Run #3
```

Shows aggregated counts from the current/latest test run. During a run, counts update
in real-time. After completion, the run number links to the history.

### 3.5 Context Menu (Right-Click)

Right-clicking a tree item shows:

| On a file node | On a method node | On a directory node |
|----------------|-------------------|---------------------|
| Run this file | Run this test | Run all in directory |
| Run failed in file | — | Run failed in directory |
| Open in editor | Open in editor (at method line) | — |
| Copy path | Copy path | Copy path |

### 3.6 Double-Click Behavior

- **On a test method**: Opens the script in the ScriptEditor, jumps to the method line.
  If the test failed, jumps to the error line instead (if available).
- **On a file node**: Opens the script at line 1.

Implementation uses `EditorInterface::edit_script()` — same as the existing
`MCPStatusPanel::_on_test_item_activated()`.

### 3.7 Test Discovery

Discovery runs:
1. On panel creation (first time the Tests tab is shown)
2. When `editor/scan_filesystem` completes (filesystem change notification)
3. When the user clicks a "Refresh" button (or presses F5 in the panel)

Discovery process:
1. Scan `res://tests/` recursively for `test_*.gd` files (configurable root)
2. For each file, use `GDScriptLanguage::validate()` to parse and extract function names
3. Filter for `test_*()` methods
4. Build the tree hierarchy

```cpp
void MCPTestPanel::_discover_tests() {
    String test_root = _get_test_root(); // Default: "res://tests/"
    Vector<String> test_files = MCPTestingTools::collect_test_files(test_root);

    // Clear and rebuild tree.
    test_tree->clear();
    TreeItem *root = test_tree->create_item();

    for (const String &file_path : test_files) {
        Vector<String> methods = MCPTestingTools::extract_test_methods(file_path);

        TreeItem *file_item = _get_or_create_directory_chain(root, file_path);
        // ... add method items ...
    }
}
```

**Note**: `_collect_test_files` and `_extract_test_methods` are currently private static
methods in MCPTestingTools. They need to be made accessible (either `public static` or
extracted into a shared utility).

### 3.8 Test Execution from Panel

When the user clicks "Run All" or "Run Selected", the panel:

1. Determines which test files/methods to run
2. Calls the same pipeline as `MCPTestingTools::handle_run()`:
   - Compile-check all files
   - Write manifest to `user://mcp_test_manifest.json`
   - Copy runner files to project
   - Launch via `EditorRunBar::play_custom_scene()`
3. Listens on `MCPTestEventBuffer` for results (same as MCPStatusPanel does)
4. Updates tree items in real-time as results arrive

The key difference from MCP tool calls: the panel does NOT block on a PendingRequest.
Instead, it polls the `MCPTestEventBuffer` every frame in `_process()` / `_notification(NOTIFICATION_INTERNAL_PROCESS)`.

```cpp
void MCPTestPanel::_process_test_events() {
    MCPTestEventBuffer *buffer = debugger_bridge->get_test_event_buffer();
    Vector<MCPTestEvent> events = buffer->read_since(last_event_cursor, 100);

    for (const MCPTestEvent &evt : events) {
        last_event_cursor = evt.seq;
        switch (evt.type) {
            case MCPTestEvent::TEST_METHOD_RESULT:
                _update_method_result(evt);
                break;
            case MCPTestEvent::TEST_RUN_COMPLETE:
                _on_run_complete(evt);
                break;
            // ...
        }
    }
}
```

### 3.9 Running Individual Tests

To run a single test method, the manifest's `filter` field is used:

```json
{
  "files": ["res://tests/test_inventory.gd"],
  "filter": "test_remove_item",
  "timeout_ms": 30000,
  "method_timeout_ms": 5000
}
```

The existing runner.gd already supports filtering:
```gdscript
if not filter.is_empty():
    methods = methods.filter(func(m): return m.match(filter))
```

### 3.10 Rerun Failed

Stores the list of failed file+method pairs from the last run. When "Rerun Failed" is
clicked, creates a manifest with only those files and a filter matching the failed methods.

If failures span multiple files, each file gets its own entry in the manifest with a
combined filter glob: `"test_remove_item|test_craft_no_materials"` — but since the runner's
filter is per-run not per-file, a simpler approach is to just re-run the full files that
had failures and let all methods run again. This is simpler and catches regressions.

---

## 4. Editor Integration

### 4.1 FileSystem Dock Context Menu

When right-clicking a `test_*.gd` file in the FileSystem dock, add:
- **"Run Test"** — runs that single test file

When right-clicking a directory containing test files, add:
- **"Run Tests in Folder"** — runs all `test_*.gd` in that directory recursively

Implementation: `EditorPlugin::_forward_canvas_gui_input()` doesn't apply here.
Instead, we hook into the filesystem dock's popup menu. This requires either:

**Option A**: Use `EditorPlugin::_get_plugin_icon()` + add context menu items via
the filesystem dock's signal `"file_selected"` / `"folder_selected"`.

**Option B**: Add a toolbar button in the ScriptEditor that detects when a test file
is open and offers "Run This Test" — simpler and more discoverable.

**Recommended: Option B** — simpler to implement, no FileSystem dock API hacking needed.

```cpp
// In MCPServerPlugin, connect to script editor tab changes:
void MCPServerPlugin::_on_script_tab_changed(int p_tab) {
    // Check if current script is a test file
    Script *script = EditorInterface::get_singleton()->get_script_editor()->get_current_script();
    if (script && script->get_path().get_file().begins_with("test_")) {
        // Show "Run Test" button in toolbar
        test_run_button->set_visible(true);
    } else {
        test_run_button->set_visible(false);
    }
}
```

### 4.2 Keyboard Shortcuts

Register editor shortcuts via `ED_SHORTCUT`:

| Action | Default Shortcut | Description |
|--------|-----------------|-------------|
| Run All Tests | Alt+Shift+T | Run all discovered tests |
| Run Current Test File | Alt+T | Run the test file open in script editor |
| Rerun Failed | Alt+Shift+R | Re-run previously failed tests |

---

## 5. Enhanced Assertions

### 5.1 What to Add (Inspired by gdUnit4)

The current `test_context.gd` has 16 assertion methods. We add 14 more for better
diagnostics, particularly for collections and strings where generic `assert_eq` gives
poor failure messages.

### 5.2 New Assertions

#### Collection Assertions

```gdscript
## Assert that an array contains all specified items (in any order).
func assert_contains_all(collection: Array, items: Array, msg := "") -> void:
    var missing: Array = []
    for item: Variant in items:
        if item not in collection:
            missing.append(item)
    if not missing.is_empty():
        _record_failure(_fmt("Array missing elements: %s", [missing], msg))

## Assert that an array does NOT contain any of the specified items.
func assert_contains_none(collection: Array, items: Array, msg := "") -> void:
    var found: Array = []
    for item: Variant in items:
        if item in collection:
            found.append(item)
    if not found.is_empty():
        _record_failure(_fmt("Array should not contain: %s", [found], msg))

## Assert array has exactly the expected number of elements.
func assert_array_size(collection: Array, expected_size: int, msg := "") -> void:
    if collection.size() != expected_size:
        _record_failure(_fmt("Expected array size %d, got %d", [expected_size, collection.size()], msg))

## Assert array is empty.
func assert_empty(collection: Variant, msg := "") -> void:
    if collection is Array:
        if not collection.is_empty():
            _record_failure(_fmt("Expected empty array, got %d elements", [collection.size()], msg))
    elif collection is Dictionary:
        if not collection.is_empty():
            _record_failure(_fmt("Expected empty dictionary, got %d entries", [collection.size()], msg))
    elif collection is String:
        if not collection.is_empty():
            _record_failure(_fmt("Expected empty string, got '%s' (length %d)", [collection, collection.length()], msg))
    else:
        _record_failure(_fmt("assert_empty: unsupported type %s", [typeof(collection)], msg))

## Assert array/dict/string is NOT empty.
func assert_not_empty(collection: Variant, msg := "") -> void:
    if collection is Array:
        if collection.is_empty():
            _record_failure(_fmt("Expected non-empty array", [], msg))
    elif collection is Dictionary:
        if collection.is_empty():
            _record_failure(_fmt("Expected non-empty dictionary", [], msg))
    elif collection is String:
        if collection.is_empty():
            _record_failure(_fmt("Expected non-empty string", [], msg))
    else:
        _record_failure(_fmt("assert_not_empty: unsupported type %s", [typeof(collection)], msg))
```

#### Dictionary Assertions

```gdscript
## Assert dictionary contains a specific key.
func assert_has_key(dict: Dictionary, key: Variant, msg := "") -> void:
    if not dict.has(key):
        _record_failure(_fmt("Dictionary missing key: %s (keys: %s)", [key, dict.keys()], msg))

## Assert dictionary contains a specific key with a specific value.
func assert_has_entry(dict: Dictionary, key: Variant, value: Variant, msg := "") -> void:
    if not dict.has(key):
        _record_failure(_fmt("Dictionary missing key: %s", [key], msg))
    elif dict[key] != value:
        _record_failure(_fmt("Key '%s': expected %s, got %s", [key, value, dict[key]], msg))
```

#### String Assertions

```gdscript
## Assert string contains a substring.
func assert_str_contains(text: String, substr: String, msg := "") -> void:
    if text.find(substr) == -1:
        _record_failure(_fmt("String does not contain '%s': '%s'",
            [substr, _truncate(text, 80)], msg))

## Assert string starts with a prefix.
func assert_str_starts_with(text: String, prefix: String, msg := "") -> void:
    if not text.begins_with(prefix):
        _record_failure(_fmt("String does not start with '%s': '%s'",
            [prefix, _truncate(text, 80)], msg))

## Assert string ends with a suffix.
func assert_str_ends_with(text: String, suffix: String, msg := "") -> void:
    if not text.ends_with(suffix):
        _record_failure(_fmt("String does not end with '%s': '%s'",
            [suffix, _truncate(text, 80)], msg))

## Assert string matches a pattern (Godot's String.match() — glob-style).
func assert_str_matches(text: String, pattern: String, msg := "") -> void:
    if not text.match(pattern):
        _record_failure(_fmt("String '%s' does not match pattern '%s'",
            [_truncate(text, 80), pattern], msg))
```

#### Type / Instance Assertions

```gdscript
## Assert value is an instance of a specific class (by class name string).
func assert_is(value: Variant, class_name_str: String, msg := "") -> void:
    if value is Object:
        if not (value as Object).is_class(class_name_str):
            _record_failure(_fmt("Expected instance of '%s', got '%s'",
                [class_name_str, (value as Object).get_class()], msg))
    else:
        _record_failure(_fmt("Expected Object instance of '%s', got non-Object type %d",
            [class_name_str, typeof(value)], msg))

## Assert numeric value is within a range [min_val, max_val] (inclusive).
func assert_in_range(value: Variant, min_val: Variant, max_val: Variant, msg := "") -> void:
    if value < min_val or value > max_val:
        _record_failure(_fmt("Expected %s in range [%s, %s]",
            [value, min_val, max_val], msg))
```

#### Error Expectation

```gdscript
## Internal state for error expectation.
var _expecting_error := false
var _error_received := false

## Call before code that should produce a push_error().
## After the test method completes, if no error was captured, the test fails.
func expect_error() -> void:
    _expecting_error = true
    _error_received = false
    # NOTE: Requires runner integration — the runner must hook into the
    # error output capture to set _error_received = true when a push_error()
    # is detected during the test method execution.
```

### 5.3 Internal Helpers

```gdscript
## Truncate long strings in assertion messages for readability.
func _truncate(text: String, max_len: int) -> String:
    if text.length() <= max_len:
        return text
    return text.substr(0, max_len) + "..."
```

### 5.4 Updated Assertion Reference Table

After enhancement, the full assertion API:

| Method | Description | Category |
|--------|-------------|----------|
| `assert_eq(a, b, msg?)` | `a == b` | Core |
| `assert_ne(a, b, msg?)` | `a != b` | Core |
| `assert_true(val, msg?)` | `val` is truthy | Core |
| `assert_false(val, msg?)` | `val` is falsy | Core |
| `assert_gt(a, b, msg?)` | `a > b` | Core |
| `assert_lt(a, b, msg?)` | `a < b` | Core |
| `assert_gte(a, b, msg?)` | `a >= b` | Core |
| `assert_lte(a, b, msg?)` | `a <= b` | Core |
| `assert_null(val, msg?)` | val is null | Core |
| `assert_not_null(val, msg?)` | val is not null | Core |
| `assert_has(coll, item, msg?)` | `item in coll` | Core |
| `assert_almost_eq(a, b, eps?, msg?)` | `abs(a-b) < eps` (Vector2/3 too) | Core |
| `assert_typeof(val, type, msg?)` | `typeof(val) == type` | Core |
| `assert_no_new_orphans(msg?)` | No leaked nodes | Core |
| `assert_contains_all(arr, items, msg?)` | Array has all items | Collection (NEW) |
| `assert_contains_none(arr, items, msg?)` | Array has none of items | Collection (NEW) |
| `assert_array_size(arr, size, msg?)` | Array length check | Collection (NEW) |
| `assert_empty(coll, msg?)` | Array/Dict/String is empty | Collection (NEW) |
| `assert_not_empty(coll, msg?)` | Array/Dict/String is not empty | Collection (NEW) |
| `assert_has_key(dict, key, msg?)` | Dictionary has key | Dictionary (NEW) |
| `assert_has_entry(dict, key, val, msg?)` | Dictionary key=val | Dictionary (NEW) |
| `assert_str_contains(text, sub, msg?)` | String contains substring | String (NEW) |
| `assert_str_starts_with(text, pre, msg?)` | String begins with | String (NEW) |
| `assert_str_ends_with(text, suf, msg?)` | String ends with | String (NEW) |
| `assert_str_matches(text, pat, msg?)` | Glob pattern match | String (NEW) |
| `assert_is(val, class_name, msg?)` | Instance of class | Type (NEW) |
| `assert_in_range(val, min, max, msg?)` | Value in [min, max] | Numeric (NEW) |
| `expect_error()` | Next push_error() is expected | Error (NEW) |
| `watch_signals(obj)` | Start signal recording | Signal |
| `assert_signal_emitted(obj, sig, msg?)` | Signal was emitted | Signal |
| `assert_signal_not_emitted(obj, sig, msg?)` | Signal was NOT emitted | Signal |
| `assert_signal_emitted_with(obj, sig, params, msg?)` | Signal emitted with params | Signal |
| `skip(reason?)` | Skip this test | Control |
| `pending(msg?)` | Mark as not implemented | Control |
| `fail(msg)` | Immediately fail | Control |
| `autofree(obj)` | Auto-free after test | Lifecycle |
| `autoqfree(node)` | Auto-queue_free after test | Lifecycle |
| `add_child_autofree(node)` | Add to tree + auto-cleanup | Lifecycle |
| `wait_for_signal(sig, max, msg?)` | Await with timeout | Async |
| `wait_seconds(time)` | Await duration | Async |
| `wait_frames(count)` | Await N frames | Async |

Total: 36 assertions/helpers (up from 22).

---

## 6. Implementation Details

### 6.1 New Files

| File | Type | Purpose |
|------|------|---------|
| `editor/mcp_test_panel.h` | C++ header | MCPTestPanel class declaration |
| `editor/mcp_test_panel.cpp` | C++ impl | Panel UI construction, discovery, result display |

### 6.2 Modified Files

| File | Change |
|------|--------|
| `mcp_server_plugin.h` | Add `MCPTestPanel *test_panel` member |
| `mcp_server_plugin.cpp` | Create panel, register as bottom panel, wire up context menu hooks |
| `mcp_testing_tools.h` | Make `_collect_test_files()` and `_extract_test_methods()` public static |
| `mcp_testing_tools.cpp` | Update embedded `TEST_CONTEXT_GD_CONTENT` with new assertions |
| `test_runner/test_context.gd` | Add new assertion methods (standalone copy) |
| `SCsub` | Already compiles `editor/*.cpp`, no change needed |

### 6.3 MCPTestPanel Class Structure

```cpp
// editor/mcp_test_panel.h
class MCPTestPanel : public VBoxContainer {
    GDCLASS(MCPTestPanel, VBoxContainer)

private:
    MCPDebuggerBridge *debugger_bridge = nullptr;

    // -- Toolbar --
    Button *run_all_button = nullptr;
    Button *run_selected_button = nullptr;
    Button *rerun_failed_button = nullptr;
    Button *stop_button = nullptr;
    Button *refresh_button = nullptr;
    LineEdit *filter_edit = nullptr;

    // -- Test Tree --
    Tree *test_tree = nullptr;

    // -- Summary Bar --
    HBoxContainer *summary_bar = nullptr;
    Label *passed_label = nullptr;
    Label *failed_label = nullptr;
    Label *errors_label = nullptr;
    Label *skipped_label = nullptr;
    Label *duration_label = nullptr;
    Label *run_number_label = nullptr;

    // -- State --
    String test_root = "res://tests/";
    bool is_running = false;
    uint64_t last_event_cursor = 0;
    int current_run_number = 0;

    // Discovered tests (cached for re-run and filtering).
    struct TestMethod {
        String name;
        int line = 0;
        TreeItem *tree_item = nullptr;
    };
    struct TestFile {
        String path;
        Vector<TestMethod> methods;
        TreeItem *tree_item = nullptr;
    };
    Vector<TestFile> discovered_tests;

    // Failed tests from last run (for rerun).
    Vector<String> last_failed_files;

    // -- Methods --
    void _build_ui();
    void _discover_tests();
    TreeItem *_get_or_create_dir_item(TreeItem *p_root, const String &p_dir);
    void _update_tree_filter(const String &p_filter);

    void _run_all();
    void _run_selected();
    void _run_files(const Vector<String> &p_files, const String &p_filter = "");
    void _rerun_failed();
    void _stop_tests();

    void _process_test_events();
    void _update_method_result(const MCPTestEvent &p_event);
    void _on_run_complete(const MCPTestEvent &p_event);
    void _update_summary();
    void _reset_tree_status();

    void _on_item_activated();  // Double-click.
    void _on_item_rmb(const Vector2 &p_pos);  // Right-click.
    void _on_filter_changed(const String &p_text);
    void _on_filesystem_changed();

    void _notification(int p_what);

protected:
    static void _bind_methods();

public:
    void set_debugger_bridge(MCPDebuggerBridge *p_bridge) { debugger_bridge = p_bridge; }
    void set_test_root(const String &p_root) { test_root = p_root; }

    MCPTestPanel();
    ~MCPTestPanel();
};
```

### 6.4 Test Execution Flow (Panel-Initiated)

When the user clicks "Run All" in the panel:

```
User clicks [▶ Run All]
       │
       ▼
MCPTestPanel::_run_all()
       │
       ├─ 1. Collect all discovered test file paths
       ├─ 2. _run_files(paths)
       │      │
       │      ├─ Compile-check each file via _validate_single_file()
       │      │    └─ If errors: update tree with COMPILE ERROR, stop
       │      ├─ Write test manifest to user://mcp_test_manifest.json
       │      ├─ Copy runner files to res://addons/mcp_test/
       │      ├─ Increment run number, push TEST_RUN_STARTED event
       │      ├─ _reset_tree_status()  — set all methods to "running" state
       │      ├─ Set is_running = true, update toolbar button visibility
       │      └─ Launch: EditorRunBar::play_custom_scene("res://addons/mcp_test/runner.tscn")
       │
       ▼
[Game process runs tests, sends debugger messages]
       │
       ▼
MCPTestPanel::_process_test_events()  [called every frame via NOTIFICATION_INTERNAL_PROCESS]
       │
       ├─ Reads MCPTestEventBuffer::read_since(cursor)
       ├─ For each TEST_METHOD_RESULT: _update_method_result()
       │    └─ Find matching TreeItem, update icon/color/status/message
       ├─ For each TEST_RUN_COMPLETE: _on_run_complete()
       │    ├─ Set is_running = false
       │    ├─ Update summary bar
       │    ├─ Record failed files for "Rerun Failed"
       │    ├─ Cleanup runner files from project
       │    └─ Update toolbar button visibility
       └─ _update_summary()
```

### 6.5 Coexistence with MCPStatusPanel

Both `MCPTestPanel` and `MCPStatusPanel` read from the same `MCPTestEventBuffer`. This is
fine because `read_since()` is cursor-based — each consumer has its own cursor and gets
all events independently.

The MCPStatusPanel's test results section continues to work exactly as before (it shows
results triggered by MCP tool calls from the LLM). The MCPTestPanel shows results triggered
by the human user clicking buttons in the panel. Both can show the same results from the
same test run.

### 6.6 Preventing Conflicts

Only one test run can happen at a time (the game process is a singleton). The panel must
check `debugger_bridge->is_game_running()` before launching. If the game is already
running (either from an MCP tool call or a manual "Play" press), the panel shows an
error message: "Game is already running. Stop it first."

```cpp
void MCPTestPanel::_run_files(const Vector<String> &p_files, const String &p_filter) {
    if (debugger_bridge->is_game_running()) {
        // Show inline error in summary bar
        _show_error("Cannot run tests — game is already running. Stop it first.");
        return;
    }
    // ...
}
```

### 6.7 Editor Setting for Test Root

Add an editor setting for the test discovery root directory:

```cpp
_EDITOR_DEF("network/mcp_server/test_root", String("res://tests/"));
```

The panel reads this on startup and when settings change. This allows projects to use
a different test directory convention (e.g., `res://test/`, `res://spec/`).

---

## 7. Integration with MCP Tool Calls

When the LLM runs `testing/run` via MCP, the results should also appear in the
MCPTestPanel. This already works because both panels read from MCPTestEventBuffer.

When the human user runs tests from the MCPTestPanel, the results should also be
available via MCP's `testing/list` (to see what tests exist). This already works because
`testing/list` scans the filesystem independently.

The only new integration needed is making `_collect_test_files()` and
`_extract_test_methods()` callable from MCPTestPanel. Currently they're private
static methods in MCPTestingTools. Make them public:

```cpp
// mcp_testing_tools.h
class MCPTestingTools {
public:
    static void register_tools(MCPToolRegistry *p_registry);

    // Exposed for MCPTestPanel to reuse.
    static Vector<String> collect_test_files(const String &p_dir_path);
    static Vector<String> extract_test_methods(const String &p_path);
    static Dictionary validate_single_file(const String &p_path);
    static void copy_runner_files_to_project();
    static void cleanup_runner_files();
    // ...
};
```

---

## 8. Comparison with gdUnit4's Panel

| Feature | gdUnit4 | Our MCPTestPanel |
|---------|---------|------------------|
| **Placement** | Left dock (EditorPlugin::add_control_to_dock) | Bottom panel (like Debugger) |
| **Test discovery** | Scans for `GdUnitTestSuite` subclasses | Scans for `test_*.gd` files |
| **Base class required** | Yes (`extends GdUnitTestSuite`) | No (any `extends RefCounted/Node`) |
| **Run individual test** | Yes (right-click → Run Test) | Yes (right-click or select + button) |
| **Run until failure** | Yes (v6.1.0+) | No (not needed for our use case) |
| **Debug tests** | Yes (with breakpoints) | Yes (breakpoints work via Godot debugger) |
| **Auto-create test** | Yes (from script editor) | No (LLM or human creates tests manually) |
| **Flaky detection** | Yes (retry + mark as flaky) | No (keep simple) |
| **Mocking** | Yes (full mock/spy framework) | No (not in scope) |
| **CI/CD output** | JUnit XML, HTML reports | MCP structured JSON (consumed by LLM) |
| **Setup required** | Install addon + configure | Zero — built into engine |
| **Tree hierarchy** | Flat (files → methods) | Nested (directories → files → methods) |
| **Keyboard shortcuts** | Configurable | Alt+Shift+T (Run All), Alt+T (Run File) |
| **Result history** | No | Yes (via MCPTestEventBuffer history) |
| **Double-click nav** | Yes (opens script) | Yes (opens script at method/error line) |
| **Filter/search** | Via IDE integration (C#) | Built-in LineEdit filter |
| **Compile-first** | No (discovers at runtime) | Yes (validates before launching) |

### Key Advantages Over gdUnit4

1. **Zero setup** — no addon to install, no project.godot changes
2. **No base class** — tests are lighter, LLM generates fewer tokens
3. **Compile-first** — catches syntax errors without wasting a game launch
4. **MCP integration** — both human and LLM can run the same tests
5. **Result history** — navigate previous runs via MCPTestEventBuffer
6. **Bottom panel** — always accessible, doesn't compete with inspector dock

---

## 9. Implementation Plan

### Phase 1: Enhanced Assertions (Small, self-contained)

1. Add new assertion methods to `TEST_CONTEXT_GD_CONTENT` in `mcp_testing_tools.cpp`
2. Mirror changes in `test_runner/test_context.gd` (standalone copy)
3. Update `testing/run` tool description to mention new assertions
4. Update agent prompts to reference new assertion categories

**Estimated effort**: ~2 hours. No C++ architecture changes.

### Phase 2: MCPTestPanel Core (Tree + Discovery + Run All)

1. Create `editor/mcp_test_panel.h` and `editor/mcp_test_panel.cpp`
2. Implement: `_build_ui()`, `_discover_tests()`, `_run_all()`
3. Make MCPTestingTools helper methods public
4. Register panel in `MCPServerPlugin` via `add_control_to_bottom_panel()`
5. Wire up `MCPTestEventBuffer` polling for real-time results
6. Implement double-click navigation

**Estimated effort**: ~8 hours. Core feature, most C++ work.

### Phase 3: Run Selected + Rerun Failed + Context Menu

1. Implement `_run_selected()` with tree selection handling
2. Implement `_rerun_failed()` with failed file tracking
3. Add right-click context menu with run/navigate options
4. Add keyboard shortcuts (ED_SHORTCUT)

**Estimated effort**: ~3 hours. UI polish.

### Phase 4: Editor Integration

1. Add "Run Test" button to ScriptEditor toolbar (when test file is open)
2. Connect filesystem change notifications to auto-refresh discovery
3. Add editor setting for test root directory

**Estimated effort**: ~3 hours. Integration work.

---

## 10. Open Questions

1. **Filter persistence**: Should the filter text persist across editor sessions?
   Recommendation: No — keep it session-only to avoid confusion.

2. **Auto-run on save**: Should tests auto-run when a test file is saved?
   Recommendation: No — this is a preference that some users hate. Add it as
   an opt-in editor setting if requested later.

3. **Test output capture**: Should the panel show captured `print()` output
   for each test? Recommendation: Yes, in a collapsible sub-row under failed
   tests. The infrastructure already exists (ctx._output).

4. **Status panel duplication**: With the new MCPTestPanel, should the test
   results section in MCPStatusPanel be removed? Recommendation: Keep both.
   MCPStatusPanel shows MCP-triggered runs (what the LLM did). MCPTestPanel
   shows human-triggered runs. They serve different audiences looking at
   different tabs.

5. **Test root auto-detection**: Should the panel auto-detect `res://tests/`
   vs `res://test/` vs other conventions? Recommendation: Check for
   `res://tests/` first, fall back to scanning res:// for any `test_*.gd`.
   Make configurable via editor setting.
