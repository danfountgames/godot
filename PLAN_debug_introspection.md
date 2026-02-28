# Implementation Plan: Debug Introspection System

**Replaces**: DebugSemanticRegistry, semantic contexter agent, debug_compat addon, console/* MCP tools
**Goal**: Runtime-only MCP tools + console commands that use the GDScript parser and live scene tree directly — no game-side registration code needed.

---

## Architecture Overview

The old system required games to call `Debug.register_action()`, `Debug.register_query()`, etc. The new system reads what already exists:

1. **Class info** → extracted from GDScript's own DocData (already parsed by the engine)
2. **Live tree** → walked via scene tree at runtime
3. **Properties/methods** → accessed via Object::get/set/call + ClassDB introspection
4. **Public/private** → underscore convention (`_foo` = private, `foo` = public)

### New Components

| Component | Location | Purpose |
|-----------|----------|---------|
| `DebugIntrospector` | `scene/debugger/debug_introspector.h/.cpp` | Core service: class info extraction, tree walking, glob matching, where filtering |
| `MCPIntrospectionTools` | `modules/mcp_server/tools/mcp_introspection_tools.h/.cpp` | 5 MCP tools: describe_class, browse_tree, get, set, call |
| Console rewrite | `scene/debugger/debug_console.cpp` | `$Path.prop`, `$Path.method()`, `$Path/*.prop` syntax |
| Autocomplete rewrite | `scene/debugger/debug_console_autocomplete.cpp` | Tree path + property/method completion |

### What Gets Removed

| File | Action |
|------|--------|
| `scene/debugger/debug_semantic_registry.h` | DELETE |
| `scene/debugger/debug_semantic_registry.cpp` | DELETE |
| `modules/mcp_server/tools/mcp_console_tools.h` | DELETE |
| `modules/mcp_server/tools/mcp_console_tools.cpp` | DELETE |
| `modules/mcp_server/prompts/agent_semantic_contexter.txt` | DELETE |
| `SEMANTIC_DEBUG_README.md` | DELETE |
| `misc/dist/addons/debug_compat/` (3 files) | DELETE entire directory |
| `modules/mcp_server/PLAN_semantic_actions.md` | DELETE |
| `modules/mcp_server/PLAN_debug_console.md` | DELETE |

---

## Phase 1: Removal (clean slate)

### 1.1 Delete files

```
rm scene/debugger/debug_semantic_registry.h
rm scene/debugger/debug_semantic_registry.cpp
rm modules/mcp_server/tools/mcp_console_tools.h
rm modules/mcp_server/tools/mcp_console_tools.cpp
rm modules/mcp_server/prompts/agent_semantic_contexter.txt
rm SEMANTIC_DEBUG_README.md
rm modules/mcp_server/PLAN_semantic_actions.md
rm modules/mcp_server/PLAN_debug_console.md
rm -rf misc/dist/addons/debug_compat/
```

### 1.2 Clean register_scene_types.cpp

**File**: `scene/register_scene_types.cpp`

- **Line 46**: Remove `#include "scene/debugger/debug_semantic_registry.h"`
- **Lines 1425-1427**: Remove cleanup block:
  ```cpp
  if (DebugSemanticRegistry::get_singleton()) {
      memdelete(DebugSemanticRegistry::get_singleton());
  }
  ```
- **Line 1490**: Remove `GDREGISTER_CLASS(DebugSemanticRegistry);`
- **Line 1493**: Remove `Engine::get_singleton()->add_singleton(Engine::Singleton("Debug", memnew(DebugSemanticRegistry)));`

### 1.3 Clean scene_debugger.cpp

**File**: `scene/debugger/scene_debugger.cpp`

- **Line 52**: Remove `#include "scene/debugger/debug_semantic_registry.h"`
- **Lines 5719-5876**: Remove entire "Native Debug singleton handlers" block:
  - `debug_get_manifest` handler (5723-5737)
  - `debug_query` handler (5739-5756)
  - `debug_batch_query` handler (5758-5779)
  - `debug_invoke_action` handler (5781-5806)
  - `debug_get_cvar` handler (5808-5825)
  - `debug_set_cvar` handler (5827-5855)
  - `debug_get_events` handler (5857-5876)

### 1.4 Clean mcp_protocol.cpp

**File**: `modules/mcp_server/mcp_protocol.cpp`

- Remove `#include "tools/mcp_console_tools.h"`
- Remove `MCPConsoleTools::register_tools(&tool_registry);` from constructor
- **Line 1287**: Remove/rewrite the `game_events` resource description that references DebugSemanticRegistry

### 1.5 Clean mcp_debugger_bridge.cpp

**File**: `modules/mcp_server/mcp_debugger_bridge.cpp`

- **Line 317**: Remove/rewrite the `event_fired` handler block that references DebugSemanticRegistry event callbacks. This entire `if (sub_msg == "event_fired")` block can be removed since events were a registry concept.

### 1.6 Clean debug_console.cpp (registry references only — keep everything else)

**File**: `scene/debugger/debug_console.cpp`

Remove all `DebugSemanticRegistry::get_singleton()` calls at these lines, replacing with stubs that return empty/error until Phase 4 rewrites them:
- **Line 137**: poll() sync — remove registry log sync
- **Line 725**: _update_watches() — stub watch evaluation
- **Lines 742-835**: _update_discovery_items() — stub discovery pills
- **Lines 1126-1206**: _execute_command_string() — stub registry command dispatch (actions, queries, commands, cvars)
- **Line 1248**: _cmd_help() — stub registry help listing
- **Line 1761**: _cmd_list() — stub registry listing
- **Line 1840**: _cmd_watch() — stub watch validation
- **Lines 2877, 2982, 3108**: UI page navigation — remove (UI pages were a registry concept)

### 1.7 Clean debug_console_autocomplete.cpp

**File**: `scene/debugger/debug_console_autocomplete.cpp`

- **Lines 90-130**: Remove registry iteration in `_rebuild_from_registry()` — only keep built-in command list
- Rename function to `_rebuild()` since there's no registry

### 1.8 Clean test games (remove Debug.* calls)

**Files in `/home/dan/Code/GodotPatch/godot-agent-test/`**:

- `neon-breakout/scripts/main.gd` (lines 287-381): Remove all `Debug.register_*` and `Debug.auto_expose` calls
- `neon-breakout/scripts/ball.gd` (lines 34-61): Remove all Debug calls
- `neon-breakout/scripts/paddle.gd` (lines 21-41): Remove all Debug calls
- Other test games: search for `Debug.` calls and remove

### 1.9 Build verification

After all removals, the project must compile. Temporarily stub any removed functions that are still called from kept code (console, viewport, etc.).

---

## Phase 2: DebugIntrospector — the core service

### 2.1 New file: `scene/debugger/debug_introspector.h`

```cpp
#pragma once

#ifdef DEBUG_ENABLED

#include "core/object/object.h"
#include "core/string/ustring.h"
#include "core/variant/dictionary.h"
#include "core/variant/array.h"

class DebugIntrospector : public Object {
    GDCLASS(DebugIntrospector, Object);

    static DebugIntrospector *singleton;

public:
    static DebugIntrospector *get_singleton() { return singleton; }

    // --- Class Description ---
    // Returns parsed class info from GDScript DocData + ClassDB.
    // include_private: if false, omits underscore-prefixed members.
    // Returns: { name, inherits, description, properties: [...], methods: [...],
    //            signals: [...], constants: [...], enums: [...], private_count: int }
    Dictionary describe_class(const String &p_class_or_path, bool p_include_private = false);

    // --- Tree Browsing ---
    // Returns hierarchical tree from p_path down to p_depth levels.
    // Smart summarization: children_count, type, has_script, groups.
    // For large child lists: first N children + "... and M more".
    Dictionary browse_tree(const String &p_path = "/root", int p_depth = 2,
            int p_child_limit = 20, int p_child_offset = 0);

    // --- Property Access ---
    // Glob-aware get. Path can contain * wildcards.
    // Supports: single property, multiple properties, array indexing,
    // range stats, value grouping, where filtering.
    Dictionary get_property(const String &p_path, const String &p_property,
            const Dictionary &p_options = Dictionary());
    // Options: { include_private, index, range, group_by_value, where, properties (array) }

    // --- Property Mutation ---
    // Glob-aware set. Applies to all matching nodes.
    Dictionary set_property(const String &p_path, const String &p_property,
            const Variant &p_value, const Dictionary &p_options = Dictionary());
    // Options: { where }

    // --- Method Invocation ---
    // Glob-aware call. Invokes method on all matching nodes.
    Dictionary call_method(const String &p_path, const String &p_method,
            const Array &p_args = Array(), const Dictionary &p_options = Dictionary());
    // Options: { where }

    // --- Glob Resolution ---
    // Resolves a glob path pattern to a list of matching node paths.
    Array resolve_glob(const String &p_pattern);

    // --- Where Filtering ---
    // Filters a list of nodes by a property condition.
    // condition: "hp > 1", "visible == true", "_current_hp <= 2"
    Array filter_nodes(const Array &p_nodes, const String &p_condition);

    // --- Utility ---
    // Get property list for a node, split public/private.
    Dictionary get_node_properties_split(Node *p_node, bool p_include_private = false);

    // Get method list for a node, split public/private.
    Dictionary get_node_methods_split(Node *p_node, bool p_include_private = false);

    // Summarize an array/collection: count, type distribution, range for numerics.
    Dictionary summarize_collection(const Variant &p_collection);

    DebugIntrospector();
    ~DebugIntrospector();

protected:
    static void _bind_methods();

private:
    // --- Internal Helpers ---

    // Walk the scene tree matching a glob pattern.
    // Supports: /root/Main/BrickGrid/* , /root/Main/*/Sprite, etc.
    void _glob_walk(Node *p_node, const PackedStringArray &p_segments,
            int p_segment_index, const String &p_current_path,
            Vector<Node *> &r_matches);

    // Extract class info from a GDScript's DocData.
    Dictionary _extract_gdscript_info(const Ref<Script> &p_script, bool p_include_private);

    // Extract class info from ClassDB for native types.
    Dictionary _extract_native_info(const StringName &p_class, bool p_include_private);

    // Check if a name is considered "private" (starts with _).
    bool _is_private(const String &p_name) const;

    // Parse a where condition into operator/value/property.
    struct WhereCondition {
        String property;
        String op; // ==, !=, <, >, <=, >=
        Variant value;
    };
    WhereCondition _parse_where(const String &p_condition);
    bool _evaluate_where(Node *p_node, const WhereCondition &p_cond);

    // Summarize property values across multiple nodes.
    // Returns: { unique_values: { val: count }, range: { min, max, mean }, total: N }
    Dictionary _summarize_values(const Vector<Node *> &p_nodes, const String &p_property);
};

#endif // DEBUG_ENABLED
```

### 2.2 New file: `scene/debugger/debug_introspector.cpp`

Key implementation details:

**describe_class()**:
1. If path ends in `.gd`: load the script, get its `DocData::ClassDoc` via `get_documentation()`
2. If it's a native class name: use ClassDB to get method_list, property_list, signal_list
3. Split members into public/private by underscore convention
4. If `!include_private`: omit private members, include `private_count` field
5. Format: compact text with types, defaults, and doc comments

**browse_tree()**:
1. Navigate to `p_path` node in scene tree
2. For each child up to `p_depth`:
   - `name`, `type`, `class` (native), `script` (if has GDScript)
   - `children_count` (total), `children_shown` (up to limit)
   - For shown children: recurse
   - `groups` (array of group names)
3. Pagination: `p_child_offset` and `p_child_limit` for large child arrays

**resolve_glob()**:
1. Split path by `/`
2. Walk tree segment by segment
3. `*` matches any single child name
4. `**` matches any depth (optional, useful extension)
5. Return all matching absolute paths

**get_property()** with options:
```
options = {
    "include_private": false,     // Show underscore members
    "index": 3,                   // Array index access
    "range": true,                // Return min/max/mean for numerics
    "group_by_value": true,       // Group matching nodes by value
    "where": "hp > 1",           // Filter before reading
    "properties": ["hp", "pos"]  // Read multiple properties
}
```

**set_property()** / **call_method()** with where:
```
options = {
    "where": "_current_hp > 0"   // Only affect nodes matching condition
}
```

### 2.3 Registration

**File**: `scene/register_scene_types.cpp`

Replace the old Debug singleton registration with:
```cpp
#include "scene/debugger/debug_introspector.h"

// In register_scene_singletons():
GDREGISTER_CLASS(DebugIntrospector);
Engine::get_singleton()->add_singleton(
    Engine::Singleton("DebugIntrospector", memnew(DebugIntrospector)));

// In unregister_scene_types():
if (DebugIntrospector::get_singleton()) {
    memdelete(DebugIntrospector::get_singleton());
}
```

### 2.4 Game-side message handlers

**File**: `scene/debugger/scene_debugger.cpp`

Replace the 7 removed semantic handlers (Phase 1.3) with new handlers:

```cpp
// --- debug_describe_class ---
if (p_msg == "debug_describe_class") {
    // p_data[0] = class_or_path, p_data[1] = include_private
    DebugIntrospector *di = DebugIntrospector::get_singleton();
    // ... call di->describe_class() and send result ...
}

// --- debug_browse_tree ---
if (p_msg == "debug_browse_tree") {
    // p_data[0] = path, p_data[1] = depth, p_data[2] = child_limit, p_data[3] = child_offset
    DebugIntrospector *di = DebugIntrospector::get_singleton();
    // ... call di->browse_tree() and send result ...
}

// --- debug_get_property ---
if (p_msg == "debug_get_property") {
    // p_data[0] = path, p_data[1] = property, p_data[2] = options_json
    DebugIntrospector *di = DebugIntrospector::get_singleton();
    // ... call di->get_property() and send result ...
}

// --- debug_set_property ---
if (p_msg == "debug_set_property") {
    // p_data[0] = path, p_data[1] = property, p_data[2] = value_json, p_data[3] = options_json
    DebugIntrospector *di = DebugIntrospector::get_singleton();
    // ... call di->set_property() and send result ...
}

// --- debug_call_method ---
if (p_msg == "debug_call_method") {
    // p_data[0] = path, p_data[1] = method, p_data[2] = args_json, p_data[3] = options_json
    DebugIntrospector *di = DebugIntrospector::get_singleton();
    // ... call di->call_method() and send result ...
}
```

---

## Phase 3: MCP Tools

### 3.1 New file: `modules/mcp_server/tools/mcp_introspection_tools.h`

```cpp
#pragma once

#include "core/variant/dictionary.h"

class MCPToolRegistry;

class MCPIntrospectionTools {
public:
    static void register_tools(MCPToolRegistry *p_registry);

    // Tool handlers
    static Dictionary handle_describe_class(const Dictionary &p_args);
    static Dictionary handle_browse_tree(const Dictionary &p_args);
    static Dictionary handle_get(const Dictionary &p_args);
    static Dictionary handle_set(const Dictionary &p_args);
    static Dictionary handle_call(const Dictionary &p_args);

private:
    static class MCPDebuggerBridge *_get_bridge();
    static Dictionary _require_game_running();
};
```

### 3.2 Tool Definitions

**Tool 1: `debug/describe_class`**
```json
{
    "name": "debug/describe_class",
    "description": "Get the public API of a GDScript class or native Godot class. Returns properties (with types, defaults, docs), methods (with signatures), signals, constants, and enums. Private members (underscore-prefixed) are hidden by default.",
    "input_schema": {
        "type": "object",
        "properties": {
            "class": {
                "type": "string",
                "description": "Class name (e.g. 'CharacterBody2D') or script path (e.g. 'res://scripts/player.gd')"
            },
            "include_private": {
                "type": "boolean",
                "description": "Include underscore-prefixed members. Default: false",
                "default": false
            }
        },
        "required": ["class"]
    },
    "annotations": { "readOnlyHint": true }
}
```

**Example output**:
```
Board (extends Node2D)
  The Tetris board: 10 columns x 22 rows (top 2 hidden).

Constants:
  COLS: int = 10
  ROWS: int = 22
  VISIBLE_ROWS: int = 20

Properties:
  @export cell_size: int = 30
  grid: Array = []

Methods:
  clear_grid() -> void
  is_valid_position(cells: Array, offset_col: int, offset_row: int) -> bool
  lock_piece(cells: Array, offset_col: int, offset_row: int, color: Color) -> void
  fill_row(row: int, color: Color = WHITE) -> void
  fill_rows(rows: Array, color: Color = WHITE) -> void
  clear_lines() -> int

[6 private members hidden — use include_private: true]
```

**Tool 2: `debug/browse_tree`**
```json
{
    "name": "debug/browse_tree",
    "description": "Browse the live scene tree of the running game. Shows node hierarchy with types, scripts, group memberships, and child counts. Use path to focus on a subtree. Paginate large child lists with offset/limit.",
    "input_schema": {
        "type": "object",
        "properties": {
            "path": {
                "type": "string",
                "description": "Scene tree path to browse from. Default: '/root'",
                "default": "/root"
            },
            "depth": {
                "type": "integer",
                "description": "How many levels deep to expand. Default: 2",
                "default": 2
            },
            "child_limit": {
                "type": "integer",
                "description": "Max children to show per node. Default: 20",
                "default": 20
            },
            "child_offset": {
                "type": "integer",
                "description": "Skip first N children (for pagination). Default: 0",
                "default": 0
            }
        }
    },
    "annotations": { "readOnlyHint": true }
}
```

**Example output**:
```
/root/Main (Node2D) [script: res://scripts/main.gd]
  ├── Paddle (Node2D) [script: res://scripts/paddle.gd] [groups: player]
  │   └── CollisionArea (Area2D)
  │       ├── Sprite (ColorRect)
  │       └── Shape (CollisionShape2D)
  ├── Ball (Area2D) [script: res://scripts/ball.gd]
  │   └── Trail (Line2D)
  ├── BrickGrid (Node2D) [script: res://scripts/brick_grid.gd]
  │   ├── Brick_0_0 (Area2D) [script: res://scripts/brick.gd]  ... +143 more children
  ├── Walls (StaticBody2D) [3 children]
  └── HUD (CanvasLayer) [4 children]
```

**Tool 3: `debug/get`**
```json
{
    "name": "debug/get",
    "description": "Read properties from nodes in the running game. Supports glob patterns to read from multiple nodes at once. For arrays of nodes, returns smart summaries (value distribution, ranges).",
    "input_schema": {
        "type": "object",
        "properties": {
            "path": {
                "type": "string",
                "description": "Node path. Use * for glob: '/root/Main/BrickGrid/*' matches all children"
            },
            "property": {
                "type": "string",
                "description": "Property name to read. Use comma-separated for multiple: 'hp,position'"
            },
            "include_private": {
                "type": "boolean",
                "description": "Allow reading underscore-prefixed properties. Default: false",
                "default": false
            },
            "where": {
                "type": "string",
                "description": "Filter nodes: 'hp > 1', 'visible == true'. Applied before reading."
            },
            "group_by_value": {
                "type": "boolean",
                "description": "Group results by value (for large node sets). Shows count per unique value.",
                "default": false
            },
            "summarize": {
                "type": "boolean",
                "description": "Return range stats (min/max/mean) for numeric properties across matched nodes.",
                "default": false
            }
        },
        "required": ["path", "property"]
    },
    "annotations": { "readOnlyHint": true }
}
```

**Example outputs**:

Single node:
```
> debug/get { path: "/root/Main/Ball", property: "velocity" }
/root/Main/Ball.velocity = Vector2(250.3, -380.0)
```

Glob with summary:
```
> debug/get { path: "/root/Main/BrickGrid/*", property: "hit_points", summarize: true }
144 nodes matched /root/Main/BrickGrid/*
hit_points: range [1, 3], mean 1.8
  1: 72 nodes
  2: 48 nodes
  3: 24 nodes
```

Glob with where:
```
> debug/get { path: "/root/Main/BrickGrid/*", property: "hit_points,position", where: "hit_points > 1" }
72 nodes matched (of 144 total)
  Brick_0_3.hit_points = 2, .position = Vector2(108, 42)
  Brick_0_4.hit_points = 3, .position = Vector2(144, 42)
  ... [showing first 20, 52 more]
```

**Tool 4: `debug/set`**
```json
{
    "name": "debug/set",
    "description": "Set a property on one or more nodes. Supports glob patterns for batch updates. Use 'where' to filter which nodes are affected.",
    "input_schema": {
        "type": "object",
        "properties": {
            "path": {
                "type": "string",
                "description": "Node path (supports * glob)"
            },
            "property": {
                "type": "string",
                "description": "Property name to set"
            },
            "value": {
                "description": "New value (JSON-compatible)"
            },
            "where": {
                "type": "string",
                "description": "Filter: only set on nodes matching condition"
            }
        },
        "required": ["path", "property", "value"]
    },
    "annotations": { "readOnlyHint": false, "destructiveHint": false }
}
```

**Example**:
```
> debug/set { path: "/root/Main/BrickGrid/*", property: "hit_points", value: 1, where: "hit_points > 1" }
Set hit_points = 1 on 72 nodes (of 144 matched by glob)
```

**Tool 5: `debug/call`**
```json
{
    "name": "debug/call",
    "description": "Call a method on one or more nodes. Supports glob patterns for batch invocation. Use 'where' to filter which nodes are called.",
    "input_schema": {
        "type": "object",
        "properties": {
            "path": {
                "type": "string",
                "description": "Node path (supports * glob)"
            },
            "method": {
                "type": "string",
                "description": "Method name to call"
            },
            "args": {
                "type": "array",
                "description": "Arguments to pass to the method"
            },
            "where": {
                "type": "string",
                "description": "Filter: only call on nodes matching condition"
            }
        },
        "required": ["path", "method"]
    },
    "annotations": { "readOnlyHint": false, "destructiveHint": false }
}
```

**Example**:
```
> debug/call { path: "/root/Main/BrickGrid/*", method: "hit", where: "hit_points == 1" }
Called hit() on 72 nodes (of 144 matched by glob)
```

### 3.3 Registration

**File**: `modules/mcp_server/mcp_protocol.cpp`

In constructor, replace:
```cpp
MCPConsoleTools::register_tools(&tool_registry);
```
with:
```cpp
MCPIntrospectionTools::register_tools(&tool_registry);
```

Add include:
```cpp
#include "tools/mcp_introspection_tools.h"
```

### 3.4 Handler Implementation Pattern

All 5 tool handlers follow this pattern:
1. Call `_require_game_running()` to check game is alive
2. Get bridge via `_get_bridge()`
3. Send `debug_*` command through bridge (editor→game process)
4. Wait for response
5. Format as MCP tool result text

---

## Phase 4: Console Rewrite

### 4.1 New command syntax

The console already has `cd`, `ls`, `pwd`, `node` for tree navigation. We add property/method access:

| Syntax | Meaning | Example |
|--------|---------|---------|
| `$Path.property` | Read property | `$Ball.velocity` |
| `$Path.property = value` | Set property | `$Ball.velocity.x = 500` |
| `$Path.method()` | Call method | `$Board.clear_grid()` |
| `$Path.method(arg1, arg2)` | Call with args | `$Board.fill_row(21, "white")` |
| `$Path/*.property` | Glob read | `$BrickGrid/*.hit_points` |
| `$Path/*.property = value` | Glob set | `$BrickGrid/*.hit_points = 1` |
| `$Path/*.method()` | Glob call | `$BrickGrid/*.hit()` |
| `... where cond` | Filter | `$BrickGrid/*.hit_points where hit_points > 1` |
| `describe ClassName` | Class info | `describe Board` |
| `tree [path]` | Browse tree | `tree /root/Main` |

Relative paths work with `cd`:
```
> cd /root/Main
/root/Main> $Ball.velocity
Vector2(250.3, -380.0)
/root/Main> $BrickGrid/*.hit_points where hit_points > 1
72 matches: range [2, 3], mean 2.3
```

### 4.2 Console command dispatch rewrite

**File**: `scene/debugger/debug_console.cpp` — `_execute_command_string()`

Replace the dispatch chain (lines 1055-1240) with:

```
1. Built-in commands: help, clear, watch, unwatch, pause, resume, step, step!, timescale, echo, exec, screenshot (KEEP these)
2. `describe <class>` → DebugIntrospector::describe_class()
3. `tree [path]` → DebugIntrospector::browse_tree()
4. `$Path...` expressions → parse and dispatch to get/set/call
5. Node navigation: cd, ls, pwd, node (KEEP these)
6. Unknown command error
```

The key new parser handles `$Path.property`, `$Path.property = value`, `$Path.method()`, and `where` clauses.

### 4.3 Autocomplete rewrite

**File**: `scene/debugger/debug_console_autocomplete.cpp`

Replace `_rebuild_from_registry()` with `_rebuild()`:

1. Built-in commands (help, clear, cd, ls, pwd, etc.)
2. `describe` + class names from project scripts
3. `tree` + current children as completions
4. When input starts with `$`:
   - Complete node paths from live tree
   - After `.`: complete property/method names from ClassDB + script
   - After ` where `: complete property names

### 4.4 Watch system update

Watches currently evaluate registry queries. Rewrite to evaluate `$Path.property` expressions:

```
> watch $Ball.velocity
> watch $BrickGrid/*.hit_points --summarize
```

The watch evaluator calls `DebugIntrospector::get_property()` each poll cycle.

---

## Phase 5: Agent Prompts & Documentation

### 5.1 Update agent_game_player.txt

Remove all references to:
- `console/get_manifest`, `console/query`, `console/invoke`, `console/get_cvar`, `console/set_cvar`, `console/get_events`, `console/batch_query`
- `Debug.` singleton calls
- `godot-semantic-contexter` agent delegation
- Semantic context, manifest, CVars, registered actions/queries/events

Replace with documentation for:
- `debug/describe_class` — understand a class's API
- `debug/browse_tree` — explore what's in the running game
- `debug/get` — read properties (single or glob)
- `debug/set` — modify properties
- `debug/call` — invoke methods
- Glob patterns and where clauses
- The discovery workflow: browse_tree → describe_class → get/set/call

### 5.2 Update agent_planner.txt

Remove:
- "semantic debug coverage model" section
- References to `auto_expose`, `godot-semantic-contexter`
- Debug registration planning

Replace with:
- Note that debug introspection is automatic — no game code needed
- Planning should focus on good naming conventions (public API = no underscore)

### 5.3 Update agent_builder.txt

- **Line 74**: Remove reference to semantic-contexter wrapping factory methods
- No Debug.register_* calls needed anymore

### 5.4 Update system_prompt.txt

- **Line 53**: Remove semantic context reference
- **Line 58**: Remove godot-semantic-contexter agent description
- Update tool list to include debug/* tools

### 5.5 Remove semantic contexter from agent list

The semantic contexter agent is no longer needed. Remove its prompt file (Phase 1) and remove it from any agent routing/selection logic in the MCP server.

---

## Phase 6: Build System & Testing

### 6.1 Build file updates

**File**: `scene/debugger/SCsub`
- Add `debug_introspector.cpp` to the build

**File**: `modules/mcp_server/tools/SCsub` (or equivalent)
- Remove `mcp_console_tools.cpp`
- Add `mcp_introspection_tools.cpp`

### 6.2 Test plan

1. **Compile test**: Full engine build with no errors
2. **describe_class tests**:
   - `describe_class("Board")` returns correct public API
   - `describe_class("Board", include_private=true)` includes `_init_grid`, `_draw_bevel_cell`
   - `describe_class("CharacterBody2D")` works for native classes
3. **browse_tree tests**:
   - Browse from `/root` shows game structure
   - Pagination works for large child lists (BrickGrid with 144 children)
   - Depth limiting works
4. **get tests**:
   - Single property: `get("/root/Main/Ball", "velocity")`
   - Glob: `get("/root/Main/BrickGrid/*", "hit_points")`
   - Glob + summarize: returns range stats
   - Glob + where: filters correctly
   - Glob + group_by_value: groups by value with counts
   - Private blocking: `get("/root/Main/Ball", "_active")` fails without `include_private`
5. **set tests**:
   - Single: `set("/root/Main/Ball", "velocity", Vector2(0, 0))`
   - Glob + where: `set("/root/Main/BrickGrid/*", "hit_points", 1, where="hit_points > 1")`
6. **call tests**:
   - Single: `call("/root/Main/Board", "clear_grid")`
   - Glob: `call("/root/Main/BrickGrid/*", "hit", where="hit_points == 1")`
7. **Console tests**:
   - `$Ball.velocity` prints velocity
   - `$BrickGrid/*.hit_points` prints summary
   - `describe Board` prints class info
   - `tree` prints tree
   - Autocomplete works for paths and properties
8. **All 5 test games**: Run each, use tools to inspect and manipulate

---

## Dependency Order

```
Phase 1 (removal) ──────────────────── builds clean
     │
Phase 2 (DebugIntrospector) ────────── core service works
     │
     ├── Phase 3 (MCP tools) ────────── LLM access works
     │
     └── Phase 4 (console rewrite) ──── human console works
              │
Phase 5 (prompts/docs) ────────────── agents updated
     │
Phase 6 (testing) ─────────────────── verified
```

Phases 3 and 4 can be done in parallel after Phase 2.

---

## File Count Summary

| Action | Count |
|--------|-------|
| Files deleted | 11 |
| Files modified (engine) | ~12 |
| Files modified (test games) | ~5 |
| Files modified (prompts) | 4 |
| New files created | 4 (introspector h/cpp, MCP tools h/cpp) |

---

## Key Design Decisions

1. **Public by default**: Members without underscore prefix are public. `_foo` is private. `include_private: true` reveals all. Response includes `private_count` as a hint.

2. **Glob patterns**: `*` matches any single child. No `**` initially (add if needed). Globs resolve at query time against the live tree.

3. **Where clauses**: Simple property comparisons only: `prop op value`. Operators: `==`, `!=`, `<`, `>`, `<=`, `>=`. No complex expressions.

4. **Smart summarization**: When a glob matches many nodes, return aggregate stats instead of listing all values:
   - Numeric: `range [min, max], mean M`
   - Discrete: `value1: N1 nodes, value2: N2 nodes`
   - First N explicit results + "and M more"

5. **No game-side code**: Zero `Debug.*` calls in game scripts. Everything is extracted automatically from the engine's own introspection capabilities.

6. **Console syntax**: `$Path.property` mirrors GDScript's `$Path` node access. Natural for Godot developers.

7. **Backwards compatibility**: The `console/execute` tool (pure console command execution) should be preserved or reimplemented since it's useful independently of the semantic system.
