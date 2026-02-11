# Breakpoint Management System -- MCP Tool Design

## 1. Overview and Rationale

The MCP server currently provides rich game lifecycle and inspection tools (`runtime/run_project`, `runtime/get_status`, `runtime/evaluate`, etc.) but has **zero breakpoint support**. An LLM debugging an interactive Godot game cannot:

- Set a breakpoint to pause execution at a specific line
- Know when/why the game has paused at a breakpoint
- Inspect the call stack and local variables at the break site
- Step through code line-by-line (step over, step into, step out)
- Resume execution after inspecting state
- List or manage existing breakpoints

This is the single largest gap in the MCP debugging story. Without breakpoints, an LLM must resort to print-statement debugging or `runtime/evaluate` polling -- vastly inferior to proper interactive debugging.

### Design Principles

1. **Use existing Godot APIs.** Godot already has a complete breakpoint system (`EditorDebuggerNode::set_breakpoint`, `ScriptEditorDebugger` signals, `RemoteDebugger` message protocol). We wrap these APIs, not reinvent them.
2. **Reactive break notification.** When the game pauses at a breakpoint, the MCP bridge must capture the event and make break state queryable. The LLM can discover breaks via `runtime/get_status` (which already reports "paused") or the dedicated `debug/get_break_state` tool.
3. **Thread safety.** All breakpoint state accessible to MCP HTTP threads must be guarded by mutexes or use atomic operations. The pattern follows the existing `PendingRequest` async model.
4. **Editor-main-thread dispatch.** Breakpoint mutation and stepping commands must run on the editor main thread (via `call_deferred`), same as `runtime/run_project` and `runtime/stop`.
5. **1-based line numbers.** The MCP protocol uses 1-based line numbers everywhere, matching Godot's debugger protocol and what humans/LLMs see in editors.

---

## 2. Proposed Tools

### Tool Summary

| Tool Name | Purpose | Game Required? |
|---|---|---|
| `debug/set_breakpoint` | Set or remove a breakpoint at a file:line | No (editor-only) |
| `debug/get_breakpoints` | List all current breakpoints | No (editor-only) |
| `debug/get_break_state` | Get full break context (stack, reason, variables) | Yes (must be paused) |
| `debug/step` | Step into / step over / step out / continue | Yes (must be paused) |

---

## 3. Tool Schemas

### 3.1 `debug/set_breakpoint`

Set, remove, or toggle a breakpoint at a specific file and line. Works whether or not the game is running -- breakpoints persist and are sent to the game when it starts.

**Input Schema:**

```json
{
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "Resource path to the script file (e.g., 'res://scripts/player.gd'). Must start with 'res://'."
    },
    "line": {
      "type": "integer",
      "description": "1-based line number where the breakpoint should be set."
    },
    "enabled": {
      "type": "boolean",
      "description": "true to add/enable the breakpoint, false to remove/disable it. Default: true."
    }
  },
  "required": ["path", "line"]
}
```

**Annotations:**

```json
{
  "title": "Set Breakpoint",
  "idempotentHint": true,
  "destructiveHint": false,
  "readOnlyHint": false,
  "openWorldHint": false
}
```

**Response (success):**

```
Breakpoint set: res://scripts/player.gd:42

structured:
{
  "path": "res://scripts/player.gd",
  "line": 42,
  "enabled": true
}
```

**Response (removed):**

```
Breakpoint removed: res://scripts/player.gd:42

structured:
{
  "path": "res://scripts/player.gd",
  "line": 42,
  "enabled": false
}
```

**Error cases:**
- `path` doesn't start with `res://` → tool error with suggestion
- `line` < 1 → tool error
- Path doesn't end with `.gd`, `.cs`, or `.gdscript` → warning but proceed (future languages)

### 3.2 `debug/get_breakpoints`

List all breakpoints currently set in the editor, including those from closed scripts (persisted in the editor cache).

**Input Schema:**

```json
{
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "Optional: filter to only show breakpoints in this file. If omitted, returns all breakpoints."
    }
  }
}
```

**Annotations:**

```json
{
  "title": "Get Breakpoints",
  "idempotentHint": true,
  "destructiveHint": false,
  "readOnlyHint": true,
  "openWorldHint": false
}
```

**Response:**

```
Breakpoints (3 total):
  res://scripts/player.gd:42
  res://scripts/player.gd:87
  res://scripts/enemy.gd:15

structured:
{
  "breakpoints": [
    {"path": "res://scripts/player.gd", "line": 42, "enabled": true},
    {"path": "res://scripts/player.gd", "line": 87, "enabled": true},
    {"path": "res://scripts/enemy.gd", "line": 15, "enabled": true}
  ],
  "count": 3,
  "filter": null
}
```

### 3.3 `debug/get_break_state`

When the game is paused at a breakpoint (or error), retrieve the complete break context: reason, stack trace, and optionally the local/member variables for a specific stack frame.

**Input Schema:**

```json
{
  "type": "object",
  "properties": {
    "frame": {
      "type": "integer",
      "description": "0-based stack frame index to inspect variables for. Frame 0 is the current (top) frame. Default: 0. Set to -1 to skip variable inspection."
    },
    "max_variables": {
      "type": "integer",
      "description": "Maximum number of variables to return per category (locals, members, globals). Default: 50."
    }
  }
}
```

**Annotations:**

```json
{
  "title": "Get Break State",
  "idempotentHint": true,
  "destructiveHint": false,
  "readOnlyHint": true,
  "openWorldHint": false
}
```

**Response (when paused):**

```
Game paused: Breakpoint
  at res://scripts/player.gd:42 in function _physics_process

Stack trace:
  #0  res://scripts/player.gd:42  _physics_process()
  #1  res://scripts/player.gd:108 handle_movement()
  #2  res://scenes/level.gd:25    _process()

Frame #0 locals (5 variables):
  velocity: Vector2(120, -350)
  delta: 0.0166667
  is_on_floor: true
  direction: 1
  speed: 200.0

structured:
{
  "paused": true,
  "reason": "Breakpoint",
  "can_debug": true,
  "has_stackdump": true,
  "stack": [
    {"frame": 0, "file": "res://scripts/player.gd", "line": 42, "function": "_physics_process"},
    {"frame": 1, "file": "res://scripts/player.gd", "line": 108, "function": "handle_movement"},
    {"frame": 2, "file": "res://scenes/level.gd", "line": 25, "function": "_process"}
  ],
  "inspected_frame": 0,
  "locals": [
    {"name": "velocity", "value": "Vector2(120, -350)", "type": "Vector2"},
    {"name": "delta", "value": "0.0166667", "type": "float"},
    {"name": "is_on_floor", "value": "true", "type": "bool"},
    {"name": "direction", "value": "1", "type": "int"},
    {"name": "speed", "value": "200.0", "type": "float"}
  ],
  "members": [
    {"name": "health", "value": "100", "type": "int"},
    {"name": "position", "value": "Vector2(512, 300)", "type": "Vector2"}
  ],
  "globals": []
}
```

**Response (when not paused):**

```
Game is not paused at a breakpoint.
State: running

structured:
{
  "paused": false,
  "state": "running"
}
```

### 3.4 `debug/step`

Control execution flow when paused at a breakpoint. Supports step into, step over, step out (step to return), and continue.

**Input Schema:**

```json
{
  "type": "object",
  "properties": {
    "action": {
      "type": "string",
      "enum": ["into", "over", "out", "continue", "break"],
      "description": "Stepping action: 'into' = step into function calls, 'over' = step to next line (skip function internals), 'out' = run until current function returns, 'continue' = resume normal execution, 'break' = pause a running game."
    }
  },
  "required": ["action"]
}
```

**Annotations:**

```json
{
  "title": "Step Execution",
  "idempotentHint": false,
  "destructiveHint": false,
  "readOnlyHint": false,
  "openWorldHint": false
}
```

**Response (step/continue):**

```
Stepped over. Now paused at res://scripts/player.gd:43 in _physics_process()

structured:
{
  "action": "over",
  "paused": true,
  "file": "res://scripts/player.gd",
  "line": 43,
  "function": "_physics_process"
}
```

**Response (continue):**

```
Resumed execution.

structured:
{
  "action": "continue",
  "paused": false
}
```

**Response (break):**

```
Break requested. Game will pause at next script line.

structured:
{
  "action": "break",
  "requested": true
}
```

**Error cases:**
- `action` is `into`/`over`/`out`/`continue` but game is not paused → tool error
- `action` is `break` but game is not running → tool error
- Game is not running at all → tool error

---

## 4. Architecture

### 4.1 Component Diagram

```
┌──────────────────────────────────────────────────────────────────────┐
│ MCP HTTP Thread                                                      │
│                                                                      │
│  debug/set_breakpoint ──► MCPBreakpointTools::handle_set_breakpoint  │
│  debug/get_breakpoints ─► MCPBreakpointTools::handle_get_breakpoints │
│  debug/get_break_state ─► MCPBreakpointTools::handle_get_break_state │
│  debug/step ────────────► MCPBreakpointTools::handle_step            │
│       │                                                              │
│       ▼                                                              │
│  MCPDebuggerBridge  (thread-safe cached break state)                 │
│       │                                                              │
│       ▼ (call_deferred for mutations, semaphore wait for queries)    │
│  Editor Main Thread                                                  │
│       │                                                              │
│       ▼                                                              │
│  EditorDebuggerNode::set_breakpoint()  ── breakpoint mgmt           │
│  EditorDebuggerNode::debug_next()      ── stepping                  │
│  EditorDebuggerNode::debug_step()      ── stepping                  │
│  EditorDebuggerNode::debug_continue()  ── continue                  │
│  EditorDebuggerNode::debug_break()     ── break request             │
│       │                                                              │
│       ▼  (debugger protocol messages)                                │
│  ScriptEditorDebugger ◄─── signals ──── running game                 │
│       │                                                              │
│       ▼  ("breaked", "stack_dump", "stack_frame_var" signals)        │
│  MCPDebuggerBridge::_on_breaked()                                    │
│  MCPDebuggerBridge::_on_stack_dump()                                 │
│  MCPDebuggerBridge::_on_stack_frame_vars()                           │
│  MCPDebuggerBridge::_on_stack_frame_var()                            │
│       │                                                              │
│       ▼  (cache state + post semaphore)                              │
│  Cached break state → returned to MCP HTTP thread                    │
└──────────────────────────────────────────────────────────────────────┘
```

### 4.2 Break State Caching

The bridge must cache the break state because multiple signals arrive sequentially (`breaked` → `stack_dump` → `stack_frame_vars` → N × `stack_frame_var`), and MCP tools need an atomic snapshot.

```
MCPDebuggerBridge (new members):
    mutable Mutex break_state_mutex;

    struct BreakState {
        bool paused = false;
        bool can_debug = false;
        String reason;                     // "Breakpoint", "Error: ...", etc.
        bool has_stackdump = false;

        // Stack frames (populated when "stack_dump" signal arrives).
        struct StackFrame {
            int frame_index;
            String file;
            String function;
            int line;
        };
        Vector<StackFrame> stack;

        // Variables for the inspected frame (populated on demand).
        int inspected_frame = -1;
        struct Variable {
            String name;
            String value;        // String representation of the Variant.
            String type_name;    // Variant::get_type_name().
            int category;        // 0=local, 1=member, 2=global.
        };
        Vector<Variable> variables;
        int expected_var_count = 0;

        void clear() {
            paused = false;
            can_debug = false;
            reason = "";
            has_stackdump = false;
            stack.clear();
            inspected_frame = -1;
            variables.clear();
            expected_var_count = 0;
        }
    };
    BreakState cached_break_state;

    // Semaphore for async break state queries.
    // Posted when stack dump + variables have all arrived.
    Semaphore break_state_ready;
```

### 4.3 Signal Flow When Game Hits Breakpoint

```
1. Game hits breakpoint → sends "debug_enter" message
2. ScriptEditorDebugger::_msg_debug_enter()
   → stores ThreadDebugged
   → emits "breaked"(true, can_debug, reason, has_stackdump)
   → auto-sends "get_stack_dump" to game
3. Bridge receives "breaked" signal:
   → Lock break_state_mutex
   → Set cached_break_state.paused = true
   → Set reason, can_debug, has_stackdump
   → Set game_paused SafeFlag  <-- FIX: currently never set!
   → Unlock
4. Game responds with "stack_dump" message
5. ScriptEditorDebugger::_msg_stack_dump() → emits "stack_dump" signal
6. Bridge receives "stack_dump" signal:
   → Lock break_state_mutex
   → Populate cached_break_state.stack
   → If no frame variable request pending: post break_state_ready
   → Unlock
7. If MCP tool requests frame variables:
   → Bridge calls ScriptEditorDebugger::request_stack_dump(frame)
   → Game responds with "stack_frame_vars" then N × "stack_frame_var"
   → Bridge collects all variables, posts break_state_ready
```

### 4.4 Signal Flow When Game Resumes

```
1. Game resumes → sends "debug_exit" message
2. ScriptEditorDebugger::_msg_debug_exit()
   → emits "breaked"(false, false, "", false)
3. Bridge receives "breaked" signal:
   → Lock break_state_mutex
   → cached_break_state.clear()
   → game_paused.clear()
   → Unlock
```

---

## 5. Implementation Plan

### 5.1 New Files

| File | Purpose |
|---|---|
| `modules/mcp_server/tools/mcp_breakpoint_tools.h` | Tool handler class declaration |
| `modules/mcp_server/tools/mcp_breakpoint_tools.cpp` | Tool handler implementations |

### 5.2 Modified Files

| File | Changes |
|---|---|
| `modules/mcp_server/mcp_debugger_bridge.h` | Add `BreakState` struct, signal callbacks, break query methods |
| `modules/mcp_server/mcp_debugger_bridge.cpp` | Implement signal connections, break state caching, query methods |
| `modules/mcp_server/mcp_protocol.cpp` | Register breakpoint tools |
| `modules/mcp_server/mcp_tool_registry.cpp` | Add `debug/step` to long-running tools list (stepping blocks until re-paused) |

### 5.3 Detailed Bridge Changes

#### 5.3.1 New Signal Connections (in `setup_session`)

Connect to `ScriptEditorDebugger` signals that the bridge doesn't currently listen to:

```cpp
void MCPDebuggerBridge::setup_session(int p_idx) {
    // ... existing code ...

    ScriptEditorDebugger *dbg = EditorDebuggerNode::get_singleton()->get_debugger(p_idx);
    if (dbg) {
        dbg->connect("output", callable_mp(this, &MCPDebuggerBridge::_on_output_received));

        // NEW: Breakpoint signals.
        dbg->connect("breaked", callable_mp(this, &MCPDebuggerBridge::_on_breaked));
        dbg->connect("stack_dump", callable_mp(this, &MCPDebuggerBridge::_on_stack_dump));
        dbg->connect("stack_frame_vars", callable_mp(this, &MCPDebuggerBridge::_on_stack_frame_vars));
        dbg->connect("stack_frame_var", callable_mp(this, &MCPDebuggerBridge::_on_stack_frame_var));
    }
}
```

#### 5.3.2 New Signal Handlers

```cpp
// Called when game pauses or resumes.
void MCPDebuggerBridge::_on_breaked(bool p_reallydid, bool p_can_debug,
                                      const String &p_reason, bool p_has_stackdump) {
    MutexLock lock(break_state_mutex);
    if (p_reallydid) {
        cached_break_state.paused = true;
        cached_break_state.can_debug = p_can_debug;
        cached_break_state.reason = p_reason;
        cached_break_state.has_stackdump = p_has_stackdump;
        game_paused.set();  // <-- FIXES the never-set bug!
    } else {
        cached_break_state.clear();
        game_paused.clear();
    }
}

// Called when stack trace arrives from the game.
void MCPDebuggerBridge::_on_stack_dump(const Array &p_stack_dump) {
    MutexLock lock(break_state_mutex);
    cached_break_state.stack.clear();
    for (int i = 0; i < p_stack_dump.size(); i++) {
        Dictionary frame_dict = p_stack_dump[i];
        BreakState::StackFrame sf;
        sf.frame_index = frame_dict.get("frame", i);
        sf.file = frame_dict.get("file", "");
        sf.function = frame_dict.get("function", "");
        sf.line = frame_dict.get("line", 0);
        cached_break_state.stack.push_back(sf);
    }
    // Post semaphore if no variable request is pending.
    if (cached_break_state.inspected_frame < 0) {
        break_state_ready.post();
    }
}

// Called with variable count header.
void MCPDebuggerBridge::_on_stack_frame_vars(int p_num_vars) {
    MutexLock lock(break_state_mutex);
    cached_break_state.expected_var_count = p_num_vars;
    cached_break_state.variables.clear();
    if (p_num_vars == 0) {
        break_state_ready.post();
    }
}

// Called once per variable.
void MCPDebuggerBridge::_on_stack_frame_var(const Array &p_data) {
    MutexLock lock(break_state_mutex);
    // p_data is serialized ScriptStackVariable: [name, type, value, var_type]
    BreakState::Variable var;
    // Parse from DebuggerMarshalls::ScriptStackVariable::deserialize format.
    if (p_data.size() >= 3) {
        var.name = p_data[0];
        var.category = p_data[1];  // 0=local, 1=member, 2=global
        var.value = Variant(p_data[2]).stringify();
        if (p_data.size() >= 4) {
            var.type_name = Variant::get_type_name(Variant::Type((int)p_data[3]));
        }
    }
    cached_break_state.variables.push_back(var);

    // When all variables have arrived, signal ready.
    if (cached_break_state.variables.size() >= cached_break_state.expected_var_count) {
        break_state_ready.post();
    }
}
```

#### 5.3.3 New Query Methods (called from MCP HTTP threads)

```cpp
// Get cached break state snapshot (non-blocking).
Dictionary MCPDebuggerBridge::get_break_state_snapshot() const {
    MutexLock lock(break_state_mutex);
    Dictionary result;
    result["paused"] = cached_break_state.paused;
    if (!cached_break_state.paused) {
        return result;
    }
    result["reason"] = cached_break_state.reason;
    result["can_debug"] = cached_break_state.can_debug;
    result["has_stackdump"] = cached_break_state.has_stackdump;

    Array stack_arr;
    for (const auto &sf : cached_break_state.stack) {
        Dictionary fd;
        fd["frame"] = sf.frame_index;
        fd["file"] = sf.file;
        fd["function"] = sf.function;
        fd["line"] = sf.line;
        stack_arr.push_back(fd);
    }
    result["stack"] = stack_arr;

    // Variables (if previously requested and populated).
    if (cached_break_state.inspected_frame >= 0) {
        result["inspected_frame"] = cached_break_state.inspected_frame;
        Array locals, members, globals;
        for (const auto &v : cached_break_state.variables) {
            Dictionary vd;
            vd["name"] = v.name;
            vd["value"] = v.value;
            vd["type"] = v.type_name;
            if (v.category == 0) locals.push_back(vd);
            else if (v.category == 1) members.push_back(vd);
            else globals.push_back(vd);
        }
        result["locals"] = locals;
        result["members"] = members;
        result["globals"] = globals;
    }

    return result;
}

// Request variables for a specific stack frame (blocks until received).
Dictionary MCPDebuggerBridge::request_frame_variables(int p_frame, int p_timeout_msec) {
    {
        MutexLock lock(break_state_mutex);
        if (!cached_break_state.paused) {
            Dictionary err;
            err["error"] = "Game is not paused";
            return err;
        }
        cached_break_state.inspected_frame = p_frame;
        cached_break_state.variables.clear();
        cached_break_state.expected_var_count = 0;
    }

    // Request on main thread.
    // ScriptEditorDebugger::request_stack_dump sends "get_stack_frame_vars".
    ScriptEditorDebugger *dbg = EditorDebuggerNode::get_singleton()->get_default_debugger();
    if (dbg) {
        callable_mp(dbg, &ScriptEditorDebugger::request_stack_dump)
            .call_deferred(p_frame);
    }

    // Wait for all variables to arrive.
    break_state_ready.wait(p_timeout_msec);

    return get_break_state_snapshot();
}
```

### 5.4 Tool Handler Implementation Sketch

```cpp
// debug/set_breakpoint
Dictionary MCPBreakpointTools::handle_set_breakpoint(const Dictionary &p_args) {
    String path = p_args.get("path", "");
    int line = p_args.get("line", 0);
    bool enabled = p_args.get("enabled", true);

    // Validation.
    if (!path.begins_with("res://")) {
        return make_tool_error("Path must start with 'res://'. Got: " + path);
    }
    if (line < 1) {
        return make_tool_error("Line must be >= 1. Got: " + itos(line));
    }

    // Dispatch to main thread.
    callable_mp(EditorDebuggerNode::get_singleton(),
        &EditorDebuggerNode::set_breakpoint)
        .call_deferred(path, line, enabled);

    // Response.
    String action = enabled ? "set" : "removed";
    String text = "Breakpoint " + action + ": " + path + ":" + itos(line);

    Dictionary structured;
    structured["path"] = path;
    structured["line"] = line;
    structured["enabled"] = enabled;

    return make_tool_result(text, structured);
}

// debug/get_breakpoints
Dictionary MCPBreakpointTools::handle_get_breakpoints(const Dictionary &p_args) {
    String filter_path = p_args.get("path", "");

    // Must run on main thread to access ScriptEditor.
    // Use the PendingRequest pattern or call_deferred + semaphore.
    List<String> bp_strings;
    ScriptEditor::get_singleton()->get_breakpoints(&bp_strings);

    Array breakpoints;
    for (const String &bp : bp_strings) {
        int colon = bp.rfind(":");
        if (colon < 0) continue;
        String path = bp.substr(0, colon);
        int line = bp.substr(colon + 1).to_int();

        if (!filter_path.is_empty() && path != filter_path) continue;

        Dictionary bpd;
        bpd["path"] = path;
        bpd["line"] = line;
        bpd["enabled"] = true;
        breakpoints.push_back(bpd);
    }

    String text = "Breakpoints (" + itos(breakpoints.size()) + " total):";
    for (int i = 0; i < breakpoints.size(); i++) {
        Dictionary bpd = breakpoints[i];
        text += "\n  " + String(bpd["path"]) + ":" + itos((int)bpd["line"]);
    }
    if (breakpoints.is_empty()) {
        text = "No breakpoints set.";
    }

    Dictionary structured;
    structured["breakpoints"] = breakpoints;
    structured["count"] = breakpoints.size();
    structured["filter"] = filter_path.is_empty() ? Variant() : Variant(filter_path);

    return make_tool_result(text, structured);
}

// debug/get_break_state
Dictionary MCPBreakpointTools::handle_get_break_state(const Dictionary &p_args) {
    MCPDebuggerBridge *bridge = _get_bridge();
    if (!bridge) {
        return make_tool_error("Debugger bridge not available");
    }

    int frame = p_args.get("frame", 0);
    int max_vars = p_args.get("max_variables", 50);

    if (!bridge->is_game_paused()) {
        String state = bridge->is_game_running() ? "running" : "stopped";
        Dictionary structured;
        structured["paused"] = false;
        structured["state"] = state;
        return make_tool_result("Game is not paused at a breakpoint.\nState: " + state, structured);
    }

    // If frame >= 0, request variables (blocking).
    Dictionary break_state;
    if (frame >= 0) {
        break_state = bridge->request_frame_variables(frame, 10000);
    } else {
        break_state = bridge->get_break_state_snapshot();
    }

    // Build text response from break_state Dictionary.
    // ... (format stack trace and variables as shown in schema section)

    return make_tool_result(text, break_state);
}

// debug/step
Dictionary MCPBreakpointTools::handle_step(const Dictionary &p_args) {
    MCPDebuggerBridge *bridge = _get_bridge();
    if (!bridge) {
        return make_tool_error("Debugger bridge not available");
    }

    String action = p_args.get("action", "");

    if (action == "break") {
        if (!bridge->is_game_running()) {
            return make_tool_error("Game is not running");
        }
        callable_mp(EditorDebuggerNode::get_singleton(),
            &EditorDebuggerNode::debug_break).call_deferred();

        Dictionary structured;
        structured["action"] = "break";
        structured["requested"] = true;
        return make_tool_result("Break requested. Game will pause at next script line.", structured);
    }

    // All other actions require the game to be paused.
    if (!bridge->is_game_paused()) {
        return make_tool_error("Game is not paused. Use action 'break' to pause, or set a breakpoint.");
    }

    if (action == "continue") {
        callable_mp(EditorDebuggerNode::get_singleton(),
            &EditorDebuggerNode::debug_continue).call_deferred();
    } else if (action == "over") {
        callable_mp(EditorDebuggerNode::get_singleton(),
            &EditorDebuggerNode::debug_next).call_deferred();
    } else if (action == "into") {
        callable_mp(EditorDebuggerNode::get_singleton(),
            &EditorDebuggerNode::debug_step).call_deferred();
    } else if (action == "out") {
        // Godot doesn't have a native "step out" in EditorDebuggerNode.
        // We can simulate via ScriptEditorDebugger directly.
        // For now, use continue (document limitation).
        return make_tool_error("'out' (step out) is not yet supported by Godot's debugger. Use 'continue' or 'over' instead.");
    } else {
        return make_tool_error("Unknown action: '" + action + "'. Valid: into, over, out, continue, break");
    }

    // For step into/over, the game will re-pause at the next line.
    // We should wait briefly for the re-pause event so we can return
    // the new break location. Use a short timeout.
    if (action == "into" || action == "over") {
        // Wait for break_state_ready (the game should re-pause very quickly).
        bridge->wait_for_rebreak(2000);  // 2 second timeout for stepping.

        Dictionary break_state = bridge->get_break_state_snapshot();
        if (break_state.get("paused", false)) {
            String file = "";
            int line = 0;
            String func = "";
            Array stack = break_state.get("stack", Array());
            if (stack.size() > 0) {
                Dictionary top = stack[0];
                file = top.get("file", "");
                line = top.get("line", 0);
                func = top.get("function", "");
            }
            String text = "Stepped " + action + ". Now paused at " + file + ":" + itos(line);
            if (!func.is_empty()) {
                text += " in " + func + "()";
            }

            Dictionary structured;
            structured["action"] = action;
            structured["paused"] = true;
            structured["file"] = file;
            structured["line"] = line;
            structured["function"] = func;
            return make_tool_result(text, structured);
        }
    }

    // Continue: game is now running.
    Dictionary structured;
    structured["action"] = action;
    structured["paused"] = false;
    return make_tool_result("Resumed execution.", structured);
}
```

---

## 6. Threading and Synchronization

### 6.1 Thread Safety Model

| Operation | Thread | Synchronization |
|---|---|---|
| `set_breakpoint` | MCP HTTP → deferred to main | `call_deferred` (fire-and-forget) |
| `get_breakpoints` | MCP HTTP → deferred to main, block | `PendingRequest` pattern (semaphore) |
| `get_break_state` (cached) | MCP HTTP | `break_state_mutex` (lock + copy) |
| `get_break_state` (with vars) | MCP HTTP → deferred to main, block | `break_state_mutex` + `break_state_ready` semaphore |
| `step` / `continue` | MCP HTTP → deferred to main | `call_deferred` + optional wait for re-break |
| Signal handlers | Editor main thread | `break_state_mutex` (write) |

### 6.2 `set_breakpoint` Threading

`EditorDebuggerNode::set_breakpoint()` is safe to call via `call_deferred` because it only touches editor-main-thread data structures. The call is fire-and-forget -- we don't wait for confirmation since the operation is synchronous on the main thread.

### 6.3 `get_breakpoints` Threading

`ScriptEditor::get_breakpoints()` accesses the script editor's internal state, which is only safe on the main thread. This must use the `PendingRequest` semaphore pattern:

1. MCP thread creates a `PendingRequest`
2. `call_deferred` executes `get_breakpoints` on the main thread
3. Main thread stores result, posts semaphore
4. MCP thread wakes up and returns the result

### 6.4 Step + Re-break Wait

When stepping, the game pauses and immediately re-pauses at the next line. The time between `debug_next()` and the next `breaked(true,...)` signal is typically <1ms (single script line execution). We wait up to 2 seconds with the `break_state_ready` semaphore to capture the new pause location, allowing the tool to return the updated break state in a single response.

If the game doesn't re-pause within the timeout (e.g., the step caused a long computation), we return a "stepping..." response and the LLM can poll with `debug/get_break_state`.

---

## 7. Edge Cases and Considerations

### 7.1 Step Out

Godot's debugger does not expose a `debug_step_out()` method on `EditorDebuggerNode`. The DAP implementation doesn't support it either. For initial implementation, we return an error with guidance. A future enhancement could send `"next"` messages in a loop until the stack depth decreases, but this is complex and fragile.

### 7.2 Multiple Threads

Godot 4.x supports multi-threaded script debugging. `ScriptEditorDebugger` tracks `threads_debugged` and `debugging_thread_id`. The MCP bridge initially focuses on the **active debugging thread** (what the editor shows in the debugger UI). Multi-thread debugging support can be added later with a `thread_id` parameter.

### 7.3 Breakpoints in Closed Scripts

`ScriptEditor::get_breakpoints()` returns breakpoints from both open and cached/closed scripts. This is the correct behavior -- breakpoints persist even when a script tab is closed.

### 7.4 Error Breakpoints

When the game hits an error (not a user-set breakpoint), `ScriptEditorDebugger::breaked` is emitted with the error message as the reason. The MCP bridge handles this identically -- `get_break_state` will show the error reason and stack trace.

### 7.5 Breakpoint Keyword

GDScript's `breakpoint` keyword (`OPCODE_BREAKPOINT`) triggers a break even without an MCP-set breakpoint. The bridge handles this transparently since it captures all `breaked` signals.

### 7.6 Race Condition: Set Breakpoint Then Step

If an MCP client sets a breakpoint and immediately issues `debug/step`, there's a brief window where the `call_deferred` for `set_breakpoint` hasn't executed yet. Since both operations go through `call_deferred`, they execute in order on the main thread, so the breakpoint will be set before the step command processes.

### 7.7 Variable Serialization

`ScriptStackVariable::serialize()` has a max size limit (default 1MB). Very large variables (arrays, dictionaries) will be truncated. The `max_variables` parameter limits the count, but individual variable values may still be truncated by Godot's serializer. This is documented in the tool description.

---

## 8. Testing Strategy

### 8.1 Manual Testing with MCP Client

1. Set breakpoint → verify appears in editor gutter
2. Run project → verify game pauses at breakpoint
3. Get break state → verify stack and variables
4. Step over → verify line advances
5. Continue → verify game resumes
6. Remove breakpoint → verify cleared from gutter
7. Test error breakpoint → verify error reason in break state
8. Test breakpoint keyword → verify break detected
9. Test get_breakpoints with filter → verify filtering works
10. Test set_breakpoint before game start → verify sent when game connects

### 8.2 Edge Case Tests

- Set breakpoint on non-existent file → should succeed (editor doesn't validate)
- Set breakpoint on line with no code → should succeed (game ignores if no opcode)
- Step when game is running → should error
- Continue when game is not paused → should error
- Get break state when game is stopped → should report not paused
- Rapid step commands → should work in sequence (call_deferred ordering)

---

## 9. Future Enhancements

1. **Conditional breakpoints.** GDScript supports `if condition:` after the `breakpoint` keyword, but there's no API for conditional breakpoints via the debugger protocol. Would require engine modification.
2. **Step out.** Implement by tracking stack depth and issuing `next` until depth decreases.
3. **Watch expressions.** Re-evaluate expressions at each break using `runtime/evaluate`.
4. **Breakpoint hit count.** Track how many times each breakpoint has been hit.
5. **Multi-thread debugging.** Add `thread_id` parameter to `get_break_state` and `step`.
6. **Logpoints.** Breakpoints that log a message instead of pausing (requires engine support).
