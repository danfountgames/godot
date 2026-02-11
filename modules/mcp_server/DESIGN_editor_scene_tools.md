# MCP Editor & Scene Tools — Design Document (v2)

## 1. The Two Modes: Editor vs Runtime

The MCP server operates in **two distinct modes** depending on whether a game is running.

```
┌──────────────────────────────────────────────────────────────────────────┐
│                           MCP SERVER                                     │
│                                                                          │
│  ┌──────────────────────────────┐   ┌─────────────────────────────────┐  │
│  │     EDITOR MODE              │   │     RUNTIME / DEBUG MODE        │  │
│  │     (always available)       │   │     (game must be running)      │  │
│  │                              │   │                                 │  │
│  │  Works on: the .tscn being   │   │  Works on: the live game        │  │
│  │  edited in the editor        │   │  process via debugger bridge    │  │
│  │                              │   │                                 │  │
│  │  Data source:                │   │  Data source:                   │  │
│  │  EditorInterface,            │   │  MCPDebuggerBridge,             │  │
│  │  SceneTreeDock,              │   │  EditorDebuggerSession,         │  │
│  │  EditorUndoRedoManager       │   │  expression evaluation          │  │
│  │                              │   │                                 │  │
│  │  Undo: YES (Ctrl+Z works)   │   │  Undo: NO (live mutations)      │  │
│  │                              │   │                                 │  │
│  │  ┌────────────────────────┐  │   │  ┌───────────────────────────┐  │  │
│  │  │ scene/* (edits)        │  │   │  │ runtime/* (runtime)         │  │  │
│  │  │                        │  │   │  │                           │  │  │
│  │  │ browse_tree ⚡2-tier   │  │   │  │ run_project, run_scene    │  │  │
│  │  │ set_property           │  │   │  │ stop, get_status          │  │  │
│  │  │ add_node               │  │   │  │ browse_scene_tree ⚡2-tier│  │  │
│  │  │ remove_node            │  │   │  │ get_node_properties       │  │  │
│  │  │ rename_node            │  │   │  │ set_node_property         │  │  │
│  │  │ move_node              │  │   │  │ search_scene_tree         │  │  │
│  │  │ duplicate_node         │  │   │  │ evaluate                  │  │  │
│  │  │ instance_scene         │  │   │  │ get_screenshot            │  │  │
│  │  │ connect_signal         │  │   │  │ send_input, send_key, ... │  │  │
│  │  │ disconnect_signal      │  │   │  │ ui_* (11 tools)           │  │  │
│  │  │ attach_script          │  │   │  │ get/emit_signal           │  │  │
│  │  │ save                   │  │   │  │ get_session_summary       │  │  │
│  │  └────────────────────────┘  │   │  │                           │  │  │
│  │                              │   │  │ set/get_breakpoints       │  │  │
│  │  ┌────────────────────────┐  │   │  │ get_break_state, step     │  │  │
│  │  │ editor/* (navigation)  │  │   │  └───────────────────────────┘  │  │
│  │  │                        │  │   │                                 │  │
│  │  │ focus_node             │  │   │  ┌───────────────────────────┐  │  │
│  │  │ focus_script           │  │   │  │ testing/* (quality)       │  │  │
│  │  │ switch_tab             │  │   │  │                           │  │  │
│  │  │ open_scene             │  │   │  │ run                       │  │  │
│  │  │ get_open_scenes        │  │   │  │ list                      │  │  │
│  │  │ read_file              │  │   │  │ check_script              │  │  │
│  │  │ write_file             │  │   │  │ check_all_scripts         │  │  │
│  │  │ list_files             │  │   │  └───────────────────────────┘  │  │
│  │  │ reimport               │  │   │                                 │  │
│  │  │ scan_filesystem        │  │   │                                 │  │
│  │  │ get_uid, resolve_uid   │  │   │                                 │  │
│  │  └────────────────────────┘  │   │                                 │  │
│  │                              │   │                                 │  │
│  │  ┌────────────────────────┐  │   │                                 │  │
│  │  │ project/* (metadata)   │  │   │                                 │  │
│  │  │ get_info, get_input_map│  │   │                                 │  │
│  │  │                        │  │   │                                 │  │
│  │  │ doc/* (documentation)  │  │   │                                 │  │
│  │  │ search_classes (5 total│  │   │                                 │  │
│  │  └────────────────────────┘  │   │                                 │  │
│  └──────────────────────────────┘   └─────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────────┘
```

### Why Two Modes?

| Aspect | Editor Mode (`scene/*`, `editor/*`) | Runtime Mode (`runtime/*`) |
|--------|-------------------------------------|--------------------------|
| **Target** | The scene file open in the editor | The live game process |
| **When available** | Always (editor is running) | Only when game is running |
| **Data path** | Direct C++ API calls on editor objects | Debugger bridge messages → game process → response |
| **Latency** | <1ms (direct memory access) | 10-100ms (IPC round-trip) |
| **Undo support** | Full (EditorUndoRedoManager) | None (mutations are live) |
| **Persistence** | Changes saved to .tscn on Ctrl+S | Changes lost when game stops |
| **Scene state** | As authored (default values, no _ready) | As running (_ready called, scripts active, physics ticking) |
| **Node paths** | As in .tscn hierarchy | May differ (runtime reparenting, dynamic nodes) |

### When to Use Which

**Editor mode** for:
- Building or modifying scenes (adding nodes, setting up properties)
- Designing UI layouts, wiring signals
- Restructuring the scene tree (rename, move, reparent)
- Any change that should be **saved to disk**

**Runtime/debug mode** for:
- Testing behavior of a running game
- Inspecting runtime state (positions after physics, animation states)
- Debugging issues (checking property values during gameplay)
- Injecting input, evaluating expressions

**Code editing**: The LLM uses its **native file editing tools** (read/write) to modify GDScript files. The MCP server does NOT provide code-writing tools — that's what the LLM already does well. The `editor/read_file` and `editor/write_file` tools exist for project file I/O only.

## 2. Naming Convention (Revised)

| Prefix | Purpose | Role |
|--------|---------|------|
| **`editor/*`** | **Moving around the editor UI** — navigation, focus, file I/O | Navigation & project files |
| **`scene/*`** | **Scene tree edits** — all mutations on the authored scene | Scene manipulation |
| **`runtime/*`** | **Runtime lifecycle & inspection** — running/stopping games, live state | Runtime/debug |
| **`testing/*`** | **Quality assurance** — running tests, validating scripts | Testing & validation |
| **`doc/*`** | **Documentation** — class/method search and lookup | Reference |
| **`project/*`** | **Project metadata** — project info, input map | Metadata |

### Changes from v1

| Old | New | Reason |
|-----|-----|--------|
| `test/run` | `testing/run` | Clearer prefix |
| `test/list` | `testing/list` | Clearer prefix |
| `gdscript/check_errors` | `testing/check_script` | Validation belongs with testing/QA |
| `gdscript/check_all` | `testing/check_all_scripts` | Validation belongs with testing/QA |
| `scene/get_tree` + `scene/get_node` | `scene/browse_tree` | **Merged into 2-tier coarse/detailed tool** |
| `scene/get_signals` | `scene/browse_tree` (signals category) | Signals are a detail view of a node |
| (none) | `scene/instance_scene` | New: instance a .tscn as child |
| (none) | `scene/save` | New: programmatic save |

### Prefix deleted: `gdscript/*`

The `gdscript/` prefix only had 2 validation tools (`check_errors`, `check_all`). These are quality assurance tools — they check code correctness, not write code. They belong under `testing/*` alongside the test runner.

## 3. The Two-Tier Pattern ⚡

### Philosophy

The **coarse/detailed** pattern saves context tokens by giving the LLM a compact overview first, then letting it drill into specific nodes only when needed. This mirrors what a human does: glance at the tree structure, then click on a specific node to see its properties.

### Where it applies

| Tool | Coarse (no path) | Detailed (with path) | Notes |
|------|-------------------|---------------------|-------|
| `scene/browse_tree` | Full tree, names+types only | Single node: all properties, signals, groups, script exports | **NEW** — editor-side |
| `runtime/browse_scene_tree` | Full tree, names+types only | Subtree + properties for target | Already built |
| `testing/check_all_scripts` | Per-file pass/fail summary | (use `testing/check_script` for detail) | Already built as 2-tier |

### Where it does NOT apply

| Tool | Why not |
|------|---------|
| `runtime/get_status` | Already minimal (~5 lines: state, scene, uptime, frame count) |
| `editor/get_open_scenes` | Already compact (just a list of paths) |
| `runtime/get_session_summary` | Already auto-summarizes (depth-2 tree, last 20 output lines, last 5 errors) |
| `project/get_info` | Fixed-size output |

## 4. Tool Inventory

### 4.1 Scene Introspection: `scene/browse_tree` (2-tier)

A single unified tool that handles both the coarse overview AND detailed drill-down of the edited scene. This replaces the separate `scene/get_tree` + `scene/get_node` + `scene/get_signals` from v1.

**Parameters**:
| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `node_path` | string | no | "" | Empty = coarse overview of full tree. Set = detailed view of that node. |
| `max_depth` | integer | no | 3 | Max depth for tree traversal (coarse mode only, -1 for unlimited) |
| `categories` | string | no | "all" | Filter for detailed mode: "properties", "transform", "signals", "groups", "script", "all" |
| `include_defaults` | boolean | no | false | In detailed mode, include properties that match class defaults |

#### Coarse mode (no `node_path`):

```
# Scene: res://scenes/main.tscn (13 nodes)

Main (Node3D) [res://scripts/main.gd]
├── Player (CharacterBody3D) [res://scripts/player.gd]
│   ├── CollisionShape3D
│   ├── PlayerModel (MeshInstance3D)
│   └── Camera3D
├── World (Node3D)
│   ├── Ground (StaticBody3D)
│   │   ├── MeshInstance3D
│   │   └── CollisionShape3D
│   └── SpawnPoints (Node3D)
│       ├── SpawnPoint1 (Marker3D)
│       └── SpawnPoint2 (Marker3D)
└── UI (CanvasLayer)
    └── HUD (Control) [res://scripts/hud.gd]
        ├── HealthBar (ProgressBar)
        └── ScoreLabel (Label)
```

Compact: just name, type, script path. No properties. Total node count in header. Enough for the LLM to understand the structure and pick a node to drill into.

#### Detailed mode (`node_path="Player"`):

```
# Node: Player (CharacterBody3D)
# Path: Player
# Script: res://scripts/player.gd
# Children: 3 (CollisionShape3D, PlayerModel, Camera3D)

## Properties (non-default)
- motion_mode: Grounded (1)
- up_direction: (0, 1, 0)
- floor_max_angle: 0.785398

## Transform
- position: (0, 1, 0)
- rotation: (0, 0, 0)
- scale: (1, 1, 1)

## Script Exports
- speed: 5.0  (float)
- jump_velocity: 4.5  (float)
- health: 100  (int)

## Signals (2 custom, 2 connected)
- health_changed(new_health: int) → UI/HUD._on_player_health_changed
- died() → Main._on_player_died

## Groups
- player, damageable
```

Everything about that one node. Properties filtered to non-default values (unless `include_defaults=true`). Signals show custom signals first, then connected built-in ones.

**Implementation**:
- Coarse: `EditorInterface::get_singleton()->get_edited_scene_root()` → recursive walk with `get_child_count()`/`get_child()`
- Detailed: `scene_root->get_node(NodePath(path))` → `Object::get_property_list()` + `ClassDB::class_get_default_property_value()` for filtering + `Object::get_signal_list()` + `Object::get_signal_connection_list()`

**Contrast with `runtime/browse_scene_tree`**: The runtime version shows the live game's tree (dynamic nodes, runtime values after `_ready()`). `scene/browse_tree` shows the authored tree exactly as in the .tscn.

---

### 4.2 Scene Mutation (undoable)

All mutation tools go through `EditorUndoRedoManager` so that **every change is undoable with Ctrl+Z**. This is critical — an LLM making a mistake shouldn't be catastrophic.

#### `scene/set_property`

Set a property on a node in the edited scene.

**Parameters**:
| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `node_path` | string | yes | — | Path to the node |
| `property` | string | yes | — | Property name (e.g., "position", "visible", "modulate") |
| `value` | variant | yes | — | New value (type must match or be coercible) |

**Value encoding**: JSON → Variant coercion:
- `[1, 2, 3]` → Vector3
- `[1, 0, 0, 1]` → Color (or `"#ff0000"` string)
- `true`/`false` → bool
- `42` → int, `3.14` → float
- `"text"` → String
- `{"x": 1, "y": 2}` → Vector2 (named component syntax)

**Output**:
```
Set Player.position: (0, 1, 0) → (5, 1, 0)
(Undo available: Ctrl+Z)
```

**Contrast with runtime**: `runtime/set_node_property` changes the live game's state (lost when game stops). `scene/set_property` changes the authored scene (persisted on save).

---

#### `scene/add_node`

Add a new node to the edited scene.

**Parameters**:
| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `parent_path` | string | yes | — | Path to parent node (empty string = scene root) |
| `type` | string | yes | — | Node class name (e.g., "Node3D", "RigidBody3D", "Label") |
| `name` | string | no | auto | Node name (auto-generated from type if omitted) |
| `properties` | object | no | {} | Initial property values to set |
| `index` | integer | no | -1 | Position among siblings (-1 = end) |

**Output**:
```
Added SpotLight3D "KeyLight" as child of World/Lighting
Path: World/Lighting/KeyLight
Properties set: light_energy=2.0, light_color=(1, 0.9, 0.8)
(Undo available: Ctrl+Z)
```

---

#### `scene/remove_node`

Remove a node (and its children) from the edited scene.

**Parameters**:
| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `node_path` | string | yes | — | Path to the node to remove |

**Safety**: Cannot remove the scene root. Warns if node has signal connections that will break.

**Output**:
```
Removed "TempDebugMesh" (MeshInstance3D) and 0 children from World
Warning: 1 signal connection was broken:
  - Player.hit → TempDebugMesh._on_hit (disconnected)
(Undo available: Ctrl+Z)
```

---

#### `scene/rename_node`

Rename a node, with automatic reference updates.

**Parameters**:
| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `node_path` | string | yes | — | Path to the node |
| `new_name` | string | yes | — | New name for the node |
| `update_references` | boolean | no | true | Update NodePath references, signal connections, animation tracks |

**What gets updated** (when `update_references=true`):
1. NodePath properties on other nodes (e.g., `^"../OldName"` → `^"../NewName"`)
2. Signal connections with NodePath-based callables
3. AnimationPlayer tracks referencing the old path
4. Unique name marker (`%NodeName` syntax) if applicable

**Output**:
```
Renamed "EnemySpawner" → "BossSpawner"
Updated 3 references:
  - Main.spawner_path: ^"World/EnemySpawner" → ^"World/BossSpawner"
  - AnimationPlayer track: "World/EnemySpawner:visible" → "World/BossSpawner:visible"
  - Signal: timer.timeout → World/EnemySpawner._on_timer → World/BossSpawner._on_timer
(Undo available: Ctrl+Z)

Note: Script references using $EnemySpawner or get_node("EnemySpawner") must be
updated manually in your GDScript files.
```

**Important limitation**: Updates NodePath properties and connections in the scene, but cannot update `$OldName` / `get_node("OldName")` in GDScript files. The tool warns the LLM, which can then use its native file editing to update scripts.

**Implementation**: Uses `SceneTreeDock::fill_path_renames()` and `SceneTreeDock::perform_node_renames()` — exactly what the editor does when you rename via the GUI.

---

#### `scene/move_node`

Reparent a node to a different parent.

**Parameters**:
| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `node_path` | string | yes | — | Path to the node to move |
| `new_parent_path` | string | yes | — | Path to the new parent |
| `index` | integer | no | -1 | Position among new siblings (-1 = end) |
| `keep_global_transform` | boolean | no | true | Preserve global position/rotation (for Node2D/Node3D) |
| `update_references` | boolean | no | true | Update NodePath references pointing to this node |

**Output**:
```
Moved "Weapon" from Player/Hands to Player/Back
Old path: Player/Hands/Weapon → New path: Player/Back/Weapon
Global transform preserved: position (2.5, 1.2, 0.1)
Updated 2 references
(Undo available: Ctrl+Z)
```

---

#### `scene/duplicate_node`

Duplicate a node and its children.

**Parameters**:
| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `node_path` | string | yes | — | Path to the node to duplicate |
| `new_name` | string | no | auto | Name for the duplicate (default: "NodeName2") |
| `parent_path` | string | no | same | Parent for the duplicate (default: same parent as original) |

**Output**:
```
Duplicated "Enemy1" (CharacterBody3D + 5 children) → "Enemy2"
Path: World/Enemies/Enemy2
(Undo available: Ctrl+Z)
```

---

#### `scene/instance_scene`

Instance a .tscn file as a child node. This is the "Instance Child Scene" operation — one of the most common actions in Godot scene building.

**Parameters**:
| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `parent_path` | string | yes | — | Path to parent node |
| `scene_path` | string | yes | — | Path to .tscn file (res:// format) |
| `name` | string | no | auto | Instance name (default: scene's root node name) |
| `index` | integer | no | -1 | Position among siblings (-1 = end) |

**Output**:
```
Instanced res://prefabs/enemy.tscn as "Enemy" under World/Enemies
Path: World/Enemies/Enemy
(Undo available: Ctrl+Z)
```

**Implementation**: Load `PackedScene` via `ResourceLoader::load()`, then instance it and add as child with proper owner chain.

---

#### `scene/connect_signal`

Connect a signal between two nodes in the edited scene.

**Parameters**:
| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `source_path` | string | yes | — | Node that emits the signal |
| `signal_name` | string | yes | — | Signal name (e.g., "pressed", "body_entered") |
| `target_path` | string | yes | — | Node that receives the callback |
| `method_name` | string | yes | — | Method name on target (e.g., "_on_button_pressed") |

**Output**:
```
Connected: UI/StartButton.pressed → Main._on_start_pressed
(Undo available: Ctrl+Z)

Hint: Make sure Main's script (res://scripts/main.gd) has:
  func _on_start_pressed():
      pass
```

---

#### `scene/disconnect_signal`

Disconnect a signal connection.

**Parameters**:
| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `source_path` | string | yes | — | Node that emits the signal |
| `signal_name` | string | yes | — | Signal name |
| `target_path` | string | yes | — | Node that receives the callback |
| `method_name` | string | yes | — | Method name on target |

---

#### `scene/attach_script`

Attach or detach a script on a node.

**Parameters**:
| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `node_path` | string | yes | — | Path to the node |
| `script_path` | string | no | "" | Path to .gd script (empty = detach current script) |

**Output**:
```
Attached res://scripts/enemy_ai.gd to World/Enemies/Enemy1
Previous script: (none)
(Undo available: Ctrl+Z)
```

---

#### `scene/save`

Save the currently edited scene to disk. Useful after a batch of mutations.

**Parameters**: None (saves the active scene).

**Output**:
```
Saved: res://scenes/main.tscn
```

**Implementation**: `EditorInterface::get_singleton()->save_scene()`.

---

### 4.3 Editor Navigation & Focus

These tools control the editor UI itself — drawing the user's attention to changes, switching context, and navigating between scenes. The `editor/*` prefix means "moving around" — no scene data is modified.

#### `editor/focus_node`

Select a node in the scene tree and center the viewport on it.

**Parameters**:
| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `node_path` | string | yes | — | Path to the node |
| `center_view` | boolean | no | true | Center the 2D/3D viewport on the node |

**Output**:
```
Selected and focused on: World/Player (CharacterBody3D)
Editor viewport centered on position (5, 1, 0)
```

**Why this matters**: After the LLM adds a node or makes a change, it focuses the editor on that node. The user immediately sees what changed without hunting through the tree.

**Implementation**:
```cpp
EditorInterface::get_singleton()->get_selection()->clear();
EditorInterface::get_singleton()->get_selection()->add_node(node);
EditorInterface::get_singleton()->edit_node(node);  // Opens Inspector
```

---

#### `editor/focus_script`

Open a script in the script editor and jump to a specific line.

**Parameters**:
| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `script_path` | string | yes | — | Path to the .gd script |
| `line` | integer | no | 1 | Line number to scroll to |
| `column` | integer | no | 0 | Column position |
| `grab_focus` | boolean | no | true | Switch to the Script editor tab |

**Output**:
```
Opened res://scripts/player.gd at line 42
Editor switched to Script tab
```

**Implementation**: `EditorInterface::get_singleton()->edit_script(script, line, col, grab_focus)` — already exists.

---

#### `editor/switch_tab`

Switch the main editor screen (2D, 3D, Script, AssetLib).

**Parameters**:
| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `tab` | string | yes | — | "2d", "3d", "script", or "assetlib" |

**Implementation**: `EditorInterface::get_singleton()->set_main_screen_editor(tab_name)`.

---

#### `editor/open_scene`

Open a scene file in the editor.

**Parameters**:
| Name | Type | Required | Default | Description |
|------|------|----------|---------|-------------|
| `path` | string | yes | — | Path to .tscn file in res:// format |

**Implementation**: `EditorInterface::get_singleton()->open_scene_from_path(path)`.

---

#### `editor/get_open_scenes`

List all currently open scene tabs.

**Parameters**: None.

**Output**:
```
# Open Scenes (3)
1. res://scenes/main.tscn (active)
2. res://scenes/player.tscn
3. res://scenes/level_01.tscn
```

**Implementation**: `EditorInterface::get_singleton()->get_open_scenes()`.

---

### 4.4 Existing `editor/*` tools (already built)

These stay under `editor/*` as they are project file I/O, not scene mutations:

| Tool | Description |
|------|-------------|
| `editor/read_file` | Read a project file |
| `editor/write_file` | Write a project file |
| `editor/list_files` | List files in a directory |
| `editor/reimport` | Reimport a resource |
| `editor/scan_filesystem` | Scan for new/changed files |
| `editor/get_uid` | Get UID for a resource path |
| `editor/resolve_uid` | Resolve UID to a resource path |

---

## 5. Implementation Architecture

### 5.1 New Files

```
modules/mcp_server/tools/mcp_scene_tools.h      — scene/* tools (browse_tree + all mutations)
modules/mcp_server/tools/mcp_scene_tools.cpp
modules/mcp_server/tools/mcp_editor_nav_tools.h  — editor/focus_*, switch_tab, open_scene, get_open_scenes
modules/mcp_server/tools/mcp_editor_nav_tools.cpp
```

### 5.2 Main Thread Constraint

**Critical**: All editor API calls must happen on the **main thread**. The MCP server's HTTP poll thread cannot directly call `EditorInterface`, `SceneTreeDock`, or UI methods.

Solution: dispatch-and-wait pattern (same as `runtime/run_project`):
1. MCP thread creates a result + semaphore
2. `call_deferred()` dispatches actual work to the main thread
3. Main thread does the work, stores result, posts the semaphore
4. MCP thread wakes up and returns the result

We should create a shared helper since many tools will use it:

```cpp
// Helper: run work on main thread, block until complete.
static Dictionary _run_on_main_thread(const Callable &p_work);
```

### 5.3 Value Coercion

The `scene/set_property` tool needs JSON → Variant coercion:

```cpp
Variant _coerce_value(const Variant &p_json_value, Variant::Type p_target_type);
```

Handles: Vector2/3/4, Color (array or "#hex"), Rect2, Transform2D/3D, bool, int, float, String, NodePath, and falls through to Godot's built-in Variant conversion for other types.

### 5.4 Property Filtering

For `scene/browse_tree` detailed mode:
- **Default properties**: Skip values matching `ClassDB::class_get_default_property_value()` (unless `include_defaults=true`)
- **Overridden properties**: Always show (these are what the user explicitly set)
- **Script exports**: Always show (`@export` variables from attached scripts)
- **Editor-only properties**: Skip `_editor_*` prefix
- Filter by `PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_STORAGE` flags

### 5.5 Rename Refactoring Plan

The `gdscript/*` and `test/*` prefixes need renaming in existing code:

| Current registration | New registration |
|---------------------|-----------------|
| `"gdscript/check_errors"` | `"testing/check_script"` |
| `"gdscript/check_all"` | `"testing/check_all_scripts"` |
| `"test/run"` | `"testing/run"` |
| `"test/list"` | `"testing/list"` |

Files to modify:
- `tools/mcp_gdscript_tools.cpp` → rename tool registrations (or rename entire file to `mcp_testing_tools.cpp` and merge with test tools)
- `tools/mcp_test_tools.cpp` → rename tool registrations (or merge into above)
- Consider merging into single `mcp_testing_tools.h/.cpp` since all 4 tools are QA-related

## 6. Implementation Priority

### Phase 1: Scene Introspection (high value, low risk)
1. `scene/browse_tree` — The 2-tier coarse/detailed tool (this is the most important single tool)
2. `editor/get_open_scenes` — List open scenes

### Phase 2: Editor Navigation (low effort, high UX)
3. `editor/focus_node` — Select & center on node
4. `editor/focus_script` — Open script at line
5. `editor/switch_tab` — Switch 2D/3D/Script
6. `editor/open_scene` — Open scene file

### Phase 3: Scene Mutation Basics (medium effort, transformative)
7. `scene/set_property` — Set node property (with undo)
8. `scene/add_node` — Add node to scene (with undo)
9. `scene/remove_node` — Remove node (with undo)
10. `scene/instance_scene` — Instance a .tscn (with undo)
11. `scene/save` — Save current scene

### Phase 4: Advanced Mutation (higher effort, very useful)
12. `scene/rename_node` — Rename with reference updates
13. `scene/move_node` — Reparent with reference updates
14. `scene/duplicate_node` — Clone node subtree

### Phase 5: Signal Wiring (medium effort)
15. `scene/connect_signal` — Wire signals
16. `scene/disconnect_signal` — Remove connections
17. `scene/attach_script` — Attach/detach scripts

### Phase 0: Rename Existing Tools (do first, minimal effort)
- Rename `test/*` → `testing/*`
- Rename `gdscript/*` → `testing/*`
- Consolidate into single `mcp_testing_tools.h/.cpp`

## 7. Complete Tool Inventory (after all phases)

| Prefix | Count | Tools |
|--------|-------|-------|
| `scene/*` | 13 | browse_tree, set_property, add_node, remove_node, rename_node, move_node, duplicate_node, instance_scene, connect_signal, disconnect_signal, attach_script, save |
| `editor/*` | 12 | focus_node, focus_script, switch_tab, open_scene, get_open_scenes, read_file, write_file, list_files, reimport, scan_filesystem, get_uid, resolve_uid |
| `runtime/*` | 39+ | (all existing runtime/debug tools unchanged) |
| `testing/*` | 4 | run, list, check_script, check_all_scripts |
| `doc/*` | 5 | search_classes, get_class, search_methods, get_method, get_property |
| `project/*` | 2 | get_info, get_input_map |
| **Total** | **75** | |

## 8. Example Workflow

```
1. scene/browse_tree              → Coarse: understand scene structure
2. scene/browse_tree path=Player  → Detailed: inspect Player node
3. scene/add_node                 → Add new nodes
4. scene/set_property             → Configure properties
5. scene/connect_signal           → Wire up signals
6. (LLM native file edit)        → Write the GDScript code
7. testing/check_script           → Verify no compile errors
8. scene/save                     → Save the scene
9. editor/focus_node              → Show the user what changed
10. runtime/run_scene               → Test it
11. runtime/browse_scene_tree       → Inspect runtime state
12. runtime/get_screenshot          → See the visual result
13. testing/run                   → Run automated tests
```

Editor mode for **building**. Runtime mode for **testing**. Documentation mode for **reference**. The three pillars of the MCP toolset.

## 9. Open Questions

1. **Should `scene/browse_tree` work on non-active scenes?** Could accept a `scene_path` parameter to read any .tscn without opening it. Requires loading PackedScene temporarily. Lower priority but useful for multi-scene workflows.

2. **Should we expose `scene/create_resource`?** Creating materials, shapes, styles without writing .tres files. Different kind of tool — resource creation rather than scene tree manipulation. Could be Phase 6.

3. **How do we handle the main thread constraint for reads?** For safety, dispatch to main thread via `call_deferred` for all scene operations. The 1-2ms latency is negligible compared to LLM response times.

4. **Should `editor/focus_node` also open the Inspector?** Yes — `edit_node()` automatically shows the Inspector, matching what happens when you click a node in the scene tree panel.

5. **Should we merge mcp_test_tools.cpp and mcp_gdscript_tools.cpp?** They'd both be `testing/*` prefix. Merging reduces file count and makes the testing domain cohesive. Recommendation: yes, create `mcp_testing_tools.h/.cpp`.
