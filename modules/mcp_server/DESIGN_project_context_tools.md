# MCP Project Context Tools — Design Document

## 1. Motivation

AI agents waste enormous amounts of tokens and time discovering project structure. The current workflow looks like this:

1. `project/get_overview` → returns file tree + autoloads (no architecture)
2. Agent reads 5-10 .gd files one by one to understand what exists
3. Agent reads 3-5 .tscn files to understand scene instancing
4. Agent still doesn't know the signal wiring map
5. Agent starts writing code, guesses at method names it saw 8 calls ago

This costs thousands of tokens just to orient. Worse, the agent's "mental model" of the project decays as the conversation grows — it forgets which file had which method, confuses signal names, and can't trace call chains without re-reading files.

SDL-MCP (https://github.com/GlitterKill/sdl-mcp) solves this for general codebases with symbol cards, graph slices, and token-budgeted context retrieval. But their approach is over-engineered for GDScript game projects:
- GDScript files are typically 50-300 lines (no need for graduated code access ladders)
- Godot projects have .tscn scene files as the real architecture (not just code)
- Signal wiring is a first-class concept that generic symbol indexing misses
- Version deltas and policy gating are enterprise concerns, not game dev concerns

**Our approach**: Three targeted tools that give agents structured, Godot-native project context in minimal tokens. No indexing pipeline, no database, no external process — we parse the project live using APIs already available inside the editor.

**Advantage over SDL-MCP**: Zero setup, zero indexing step, scene-aware (understands .tscn instancing, signal wiring, groups), and Godot-native (knows about @exports, class_name, autoloads).

## 2. Architecture Overview

```
LLM (Claude, etc.)
  │
  │  MCP tools/call
  ▼
┌──────────────────────────────────────────────────┐
│  MCPProjectContextTools (new tool class)          │
│                                                   │
│  project/get_architecture    ← full project map   │
│  project/get_script_summary  ← skeleton of a .gd  │
│  project/search_symbols      ← find things fast    │
│                                                   │
│  Reads from:                                      │
│  ├─ FileAccess (res://)       — .gd source files  │
│  ├─ FileAccess (res://)       — .tscn scene files │
│  ├─ GDScriptLanguage::validate()  — symbol lists  │
│  ├─ ProjectSettings           — autoloads, config │
│  └─ EditorFileSystem          — file index        │
└──────────────────────────────────────────────────┘
```

No new singletons, no persistent state, no indexing cache. Each tool call scans the project fresh. For typical Godot projects (10-100 scripts), this takes <100ms — well within MCP response budgets.

## 3. Tools

### 3.1 `project/get_architecture`

**Purpose**: Single-call structured overview of the entire project's architecture — scenes, scripts, signal wiring, groups, autoloads, and how they connect. Replaces the "read 15 files to understand the project" pattern.

**Parameters**:
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `focus` | string | no | Limit to a subtree: scene path (e.g., "res://scenes/player.tscn") or script path. Default: entire project. |
| `include_methods` | boolean | no | Include method signatures in script summaries (default true) |
| `include_signals` | boolean | no | Include signal definitions and wiring (default true) |
| `max_depth` | integer | no | Scene instancing depth limit (default 4, max 8) |

**What it gathers** (in order):

1. **Autoloads**: Name → script path → class_name → public signals + key methods
2. **Scene graph**: Main scene → instanced subscenes (recursive), with node types and attached scripts
3. **Script summaries**: For each .gd file: class_name, extends, @exports, signals, method signatures (no bodies)
4. **Signal wiring map**: All connections — both editor-wired (.tscn `[connection]` entries) and code-wired (`.connect()` calls in .gd files)
5. **Groups**: Group name → which scenes/scripts use them
6. **Input actions**: Action name → mapped keys (from project/get_input_map data)

**Return Format** (text):
```
# Project Architecture — MyGame

## Autoloads
  Events (res://autoloads/events.gd)
    signals: game_over(), score_changed(new_score: int), level_loaded(level_name: String)
  SaveManager (res://autoloads/save_manager.gd)
    methods: save_game() -> bool, load_game() -> bool, has_save() -> bool

## Scene Graph
  main.tscn (Node2D)
  ├── Player (CharacterBody2D)       ← res://scenes/player.tscn
  │   ├── Sprite2D
  │   ├── CollisionShape2D
  │   └── HitBox (Area2D)            ← res://scenes/hitbox.tscn
  ├── EnemySpawner (Node2D)          [script: res://scripts/enemy_spawner.gd]
  │   └── Timer
  └── UI (CanvasLayer)               ← res://scenes/hud.tscn
      ├── HealthBar (TextureProgressBar)
      └── ScoreLabel (Label)

## Scripts
  player.gd (Player extends CharacterBody2D)
    @exports: speed: float = 200.0, jump_force: float = -400.0, max_health: int = 100
    signals: died(), health_changed(new_health: int)
    methods: take_damage(amount: int) -> void, heal(amount: int) -> void

  enemy_spawner.gd (EnemySpawner extends Node2D)
    @exports: enemy_scene: PackedScene, spawn_rate: float = 2.0
    signals: wave_complete(wave_num: int)
    methods: spawn_enemy(pos: Vector2) -> Node2D, start_wave(num: int) -> void

  hud.gd (extends CanvasLayer)
    methods: update_health(value: int) -> void, update_score(value: int) -> void

## Signal Wiring
  player.died → main._on_player_died                    [editor-wired in main.tscn]
  player.health_changed → hud.update_health             [editor-wired in main.tscn]
  enemy_spawner.wave_complete → hud._on_wave_complete    [code-wired in main.gd:42]
  Events.game_over → main._on_game_over                 [code-wired in main.gd:15]

## Groups
  enemies: enemy.tscn, boss.tscn
  interactable: chest.tscn, door.tscn
  save_target: player.tscn, world.tscn

## Input Actions
  move_left: A, Left
  move_right: D, Right
  jump: Space
  attack: Mouse Left
```

**Structured content**: JSON with arrays for each section, enabling programmatic access.

**Size management**:
- For projects with >30 scripts: omit method signatures by default, show only class_name/extends/@exports/signals per script. Append: "Use `project/get_script_summary` for full API of specific scripts."
- For projects with >50 scripts: omit script section entirely, keep scene graph + wiring + groups. Append guidance.
- Hard cap: 32KB text output.

### 3.2 `project/get_script_summary`

**Purpose**: Skeleton view of one or more .gd files — the API surface without implementation bodies. SDL-MCP calls this a "symbol card." This is what agents need before writing code that interacts with a script.

**Parameters**:
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `path` | string | yes | Script path (res://...) or glob pattern (res://scripts/*.gd) |
| `include_private` | boolean | no | Include methods starting with _ (default false — shows only public API) |
| `include_comments` | boolean | no | Include doc comments (## lines above members) (default true) |

**What it extracts** (per script):

1. `class_name` and `extends`
2. Enums (name + values)
3. Constants (`const NAME: Type = value`)
4. Signals (`signal name(param: Type)`)
5. @export variables (name, type, default, @export annotation variant)
6. Non-export member variables (name, type, default) — only if typed or initialized
7. Method signatures (name, params with types, return type, qualifiers like static)
8. Inner classes (`class InnerName:`)
9. Doc comments (## lines preceding any member)

**Return Format** (text):
```
# res://scripts/player.gd

class_name Player extends CharacterBody2D

## Movement
enum State { IDLE, RUN, JUMP, FALL, ATTACK }

const MAX_COYOTE_TIME: float = 0.15

signal died()
signal health_changed(new_health: int)

@export var speed: float = 200.0
@export var jump_force: float = -400.0
@export_range(1, 200) var max_health: int = 100

var health: int = 100
var current_state: State = State.IDLE

## Takes damage and emits health_changed. Emits died if health reaches 0.
func take_damage(amount: int) -> void
func heal(amount: int) -> void
func _physics_process(delta: float) -> void
func _state_idle(delta: float) -> void
func _state_run(delta: float) -> void
func _state_jump(delta: float) -> void
```

**Glob support**: When `path` is a glob (e.g., `res://scripts/*.gd`), returns summaries for all matching files concatenated, with clear file separators.

**Size management**:
- Single file: always full detail (GDScript files are small).
- Glob returning >10 files: show only class_name/extends/signals/@exports per file. Append: "Use `project/get_script_summary` with specific path for full details."
- Hard cap: 32KB text output.

### 3.3 `project/search_symbols`

**Purpose**: Find functions, signals, classes, variables, or constants across all project scripts without reading every file. The "where is the thing I need?" tool.

**Parameters**:
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `query` | string | yes | Search terms (e.g., "damage", "spawn enemy", "score") |
| `kind` | string | no | Filter: "function", "signal", "class", "variable", "constant", "export", or empty for all |
| `limit` | integer | no | Max results (default 20, max 50) |

**Matching strategy** (keyword/substring scoring, same pattern as doc tools):
- **+10**: Exact symbol name match
- **+8**: Symbol name contains all query terms
- **+5**: Doc comment above symbol contains query terms
- **+3**: Parameter names contain query terms
- **+1**: File path contains query terms
- All terms must match somewhere (AND logic)

**Return Format** (text):
```
# Symbol Search Results for "damage"

## Functions
  Player.take_damage(amount: int) -> void          res://scripts/player.gd:45
  Enemy.take_damage(amount: int) -> void            res://scripts/enemy.gd:32
  Boss.take_damage(amount: int, type: String) -> void  res://scripts/boss.gd:78
  DamageNumbers.show(value: int, pos: Vector2)      res://scripts/damage_numbers.gd:12

## Signals
  Player.damage_taken(amount: int, source: Node)    res://scripts/player.gd:8
  Events.damage_dealt(target: Node, amount: int)     res://autoloads/events.gd:5

## Variables
  @export var damage_multiplier: float = 1.0         res://scripts/weapon.gd:15
  const BASE_DAMAGE: int = 10                        res://scripts/weapon.gd:7
```

**Structured content**: Array of `{name, kind, class_name, signature, file, line, score}`.

## 4. Implementation Details

### 4.1 File Structure

```
modules/mcp_server/tools/mcp_project_context_tools.h    — Class declaration
modules/mcp_server/tools/mcp_project_context_tools.cpp  — Implementation
```

### 4.2 Registration

In `mcp_protocol.cpp`:
```cpp
#include "tools/mcp_project_context_tools.h"
// ...
MCPProjectContextTools::register_tools(&tool_registry);
```

### 4.3 GDScript Parsing Strategy

We need to extract symbols from .gd files without full AST access. Two approaches available:

**Approach A: GDScriptLanguage::validate() + regex parsing (recommended)**
```cpp
// Step 1: Get function names via validate()
GDScriptLanguage *gdscript = GDScriptLanguage::get_singleton();
List<String> functions;
List<ScriptLanguage::ScriptError> errors;
gdscript->validate(source, path, &functions, &errors, nullptr, nullptr);

// Step 2: Parse source with regex for everything else
// - class_name, extends: first lines
// - signals: /^signal\s+(\w+)\((.*?)\)/
// - @exports: /^@export\S*\s+var\s+(\w+)\s*:\s*(\w+)/
// - constants: /^const\s+(\w+)\s*:\s*(\w+)\s*=\s*(.*)/
// - enums: /^enum\s+(\w+)\s*\{/
// - method signatures: /^(static\s+)?func\s+(\w+)\((.*?)\)(\s*->\s*\w+)?/
// - doc comments: /^##\s*(.*)/
```

This is fast, robust enough for GDScript's relatively simple syntax, and doesn't require accessing internal parser APIs that might change between versions.

**Approach B: GDScriptParser (deeper but fragile)**
```cpp
GDScriptParser parser;
parser.parse(source, path, false);
// Walk the AST for ClassNode, FunctionNode, SignalNode, etc.
```

More accurate but couples us to parser internals. Recommendation: start with Approach A, move to B only if regex parsing proves insufficient.

### 4.4 Scene File Parsing

Reuse the text-based .tscn parsing pattern already established in `mcp_analysis_tools.cpp`:

```cpp
// Parse .tscn for:
// 1. ext_resource references (scripts, subscenes)
//    [ext_resource type="Script" path="res://player.gd" id="1_abc"]
//    [ext_resource type="PackedScene" path="res://hitbox.tscn" id="2_def"]
//
// 2. Node hierarchy
//    [node name="Player" type="CharacterBody2D" parent="."]
//    [node name="HitBox" parent="Player" instance=ExtResource("2_def")]
//
// 3. Signal connections
//    [connection signal="died" from="Player" to="." method="_on_player_died"]
//
// 4. Group assignments
//    [node name="Enemy" type="..." groups=["enemies"]]
```

Build a lightweight in-memory scene graph:

```cpp
struct SceneNode {
    String name;
    String type;
    String parent_path;
    String script_path;         // resolved from ext_resource
    String instance_scene_path; // resolved from ext_resource
    Vector<String> groups;
};

struct SceneConnection {
    String signal_name;
    String from_node;
    String to_node;
    String method_name;
};
```

### 4.5 Code-Wired Signal Detection

For `project/get_architecture`, we also need to find `.connect()` calls in .gd files:

```cpp
// Pattern: <signal_source>.<signal_name>.connect(<target_callable>)
// Regex: /(\w[\w.]*?)\.(\w+)\.connect\((\w[\w.]*)\)/
//
// Examples:
//   Events.game_over.connect(_on_game_over)
//   $Player.died.connect(_on_player_died)
//   enemy.health_changed.connect(hud.update_health)
//
// Also detect: connect("signal_name", callable) — legacy syntax
// Pattern: /\.connect\("(\w+)",\s*(.*?)\)/
```

This is heuristic — regex won't catch every edge case (computed signal names, lambda callables). But it covers 90%+ of real Godot code. The remaining 10% would need AST parsing.

### 4.6 Project Index Building

All three tools need to scan the project. To avoid redundant work when multiple tools are called in sequence, build a shared index:

```cpp
struct ScriptInfo {
    String path;
    String class_name;
    String extends;
    Vector<String> signals;       // "signal_name(param: Type, ...)"
    Vector<String> exports;       // "@export var name: Type = default"
    Vector<String> methods;       // "func name(params) -> ReturnType"
    Vector<String> constants;     // "const NAME: Type = value"
    Vector<String> enums;         // "enum Name { A, B, C }"
    Vector<String> variables;     // "var name: Type = default"
    Vector<String> doc_comments;  // "## comment" mapped to following member
    Vector<String> connect_calls; // "source.signal.connect(target)"
    int line_count;
};

struct ProjectIndex {
    HashMap<String, ScriptInfo> scripts;     // path → info
    HashMap<String, SceneNode> scene_nodes;  // scene_path:node_path → node
    Vector<SceneConnection> connections;      // all editor-wired connections
    HashMap<String, Vector<String>> groups;  // group_name → [scene_paths]
    HashMap<String, String> autoloads;       // name → script_path

    void build();  // Scans entire project
    bool is_stale() const;  // Check if files changed since last build
};
```

**Caching**: The index is rebuilt on every `project/get_architecture` call (no persistent cache). For typical projects this takes <100ms. If profiling shows this is a problem for large projects (>200 scripts), add a timestamp-based staleness check and reuse the last index within a 5-second window.

### 4.7 Thread Safety

All file reads use `FileAccess::get_file_as_string()` which is thread-safe for reading. `GDScriptLanguage::validate()` acquires the GDScript mutex internally. `ProjectSettings` reads are safe from the MCP poll thread. No main-thread dispatch needed — these are all read-only operations.

### 4.8 Interaction with Existing Tools

- **`project/get_overview`**: Remains as-is. It's the "what files exist" tool. `project/get_architecture` is the "how things connect" tool. They complement each other.
- **`analysis/signal_flow`**: Overlap in signal wiring detection. `get_architecture` can reuse `_parse_tscn_connections()` from `mcp_analysis_tools.cpp`. The analysis tool returns diagnostic data (orphan signals, missing connections); the architecture tool returns structural data (what's wired to what).
- **`analysis/dependencies`**: Overlap in autoload/reference detection. Same reuse opportunity.
- **`analysis/complexity`**: No overlap — complexity scoring is a quality metric, not a context tool.

### 4.9 Output Size Estimation

Typical project sizes and expected output:

| Project Size | Scripts | Scenes | get_architecture | get_script_summary (one) | search_symbols |
|---|---|---|---|---|---|
| Small (jam game) | 5-10 | 3-5 | ~2KB | ~0.5KB | ~1KB |
| Medium (indie) | 20-50 | 10-20 | ~8KB | ~1KB | ~2KB |
| Large (complex) | 50-150 | 30-60 | ~15KB (summary mode) | ~2KB | ~3KB |
| Very large | 150+ | 60+ | ~25KB (minimal mode) | ~2KB | ~3KB |

All well within the 32KB cap. The token cost of one `get_architecture` call replaces 10-20 file reads — a net savings of 5,000-15,000 tokens.

## 5. Tool Descriptions (for LLM Discovery)

### project/get_architecture
```
Get the complete architecture of this Godot project in one call.

Returns: scene instancing graph, script summaries (class_name, @exports, signals,
method signatures), signal wiring map (both editor-wired and code-wired), groups,
autoloads, and input actions.

CALL THIS when you need to understand how the project is structured before planning
or building features. Replaces reading 10+ files individually.

Use 'focus' to limit to a specific scene subtree (e.g., focus="res://scenes/player.tscn").
```

### project/get_script_summary
```
Get the API surface of a GDScript file without implementation bodies.

Returns: class_name, extends, enums, constants, signals, @exports, method signatures,
and doc comments. No function bodies — just the interface.

Use this before writing code that interacts with a script you haven't read yet.
Supports glob patterns (e.g., path="res://scripts/*.gd") for multiple files.
```

### project/search_symbols
```
Search for functions, signals, classes, variables, or constants across all project scripts.

Use this when you need to find WHERE something is defined or used.
Examples:
- "damage" → finds take_damage(), damage_multiplier, damage_dealt signal
- "spawn" → finds spawn_enemy(), spawn_wave(), EnemySpawner class
- "score" → finds update_score(), score_changed signal, high_score variable

Use 'kind' to filter: "function", "signal", "class", "variable", "constant", "export".
```

## 6. Prompt Integration

These tools should be woven into the agent prompts to ensure they get used.

### system_prompt.txt — add to tool catalog
```
**PROJECT**: project/get_overview, project/get_architecture, project/get_input_map, project/get_script_summary, project/search_symbols
```

### system_prompt.txt — update Essential Workflows
```
**Understand the project before writing code:**
  1. project/get_overview — file tree, autoloads, main scene
  2. project/get_architecture — scene graph, scripts, signal wiring, groups
  3. project/get_script_summary path="res://relevant_script.gd" — API surface of specific scripts
  4. doc/get_class — Godot class reference for unfamiliar engine types
```

### agent_planner.txt — update Planning Workflow step 1-2
```
1. project/get_overview — file tree and autoloads
2. project/get_architecture — full scene graph, script summaries, signal wiring
   This replaces reading individual files for orientation. Only read specific files
   when you need to understand implementation details inside a function body.
3. Read specific scripts ONLY where you need implementation detail (not just the API).
```

### agent_builder.txt — update Workflow step 2
```
2. project/get_architecture — understand current scene graph and wiring.
   Then project/get_script_summary for scripts you'll modify — understand their API.
   Only read full file contents when you need to see implementation bodies.
```

### agent_game_player.txt — update Startup
```
1. project/get_overview + project/get_architecture — full project context in two calls
```

## 7. Comparison with Alternatives

| Feature | SDL-MCP | Our Approach | Raw File Reading |
|---|---|---|---|
| Setup required | npm install + init + index | None (built into editor) | None |
| Token cost for orientation | Low (symbol cards) | Low (architecture + summaries) | Very high (read N files) |
| Scene awareness | None (code-only) | Full (.tscn parsing, instancing, wiring) | Manual |
| Signal wiring | None | Automatic (editor + code wiring) | Grep for .connect() |
| @export detection | None (not GDScript-aware) | Native (@export variants) | Grep |
| Group detection | None | Native (from .tscn + add_to_group) | Grep |
| Incremental indexing | Yes (SQLite) | No (full scan each call) | N/A |
| Cross-language | 12 languages | GDScript only | N/A |
| Graduated code access | getSkeleton → getHotPath → needWindow | get_script_summary (sufficient) | Read whole file |
| Version deltas | Yes (blast radius, PR risk) | No (git diff is fine) | N/A |

## 8. Implementation Plan

### Phase 1: Core Architecture Tool
1. Create `mcp_project_context_tools.h/.cpp`
2. Implement `ProjectIndex::build()` — scan all .gd and .tscn files
3. Implement `handle_get_architecture()` — format the index as text + structured
4. Wire into `mcp_protocol.cpp` registration
5. Build, test with a sample project
6. Update system_prompt.txt tool catalog

### Phase 2: Script Summary
7. Implement GDScript regex parser for symbol extraction
8. Implement `handle_get_script_summary()` — single file + glob support
9. Test with various script styles (simple, complex, inner classes)

### Phase 3: Symbol Search
10. Implement search scoring (same pattern as doc tools)
11. Implement `handle_search_symbols()` — cross-project search
12. Test with real queries

### Phase 4: Prompt Integration + Polish
13. Update all agent prompts (system, planner, builder, game_player)
14. Test with real LLM sessions — does the agent actually use the tools?
15. Tune output format based on real usage patterns
16. Add `focus` parameter support for get_architecture

## 9. Open Questions

1. **Should `get_architecture` include .tres resource files?** Resource subclasses (WeaponStats, EnemyConfig) are part of the architecture. But listing them all might bloat the output. Recommendation: include only resources that are referenced by @export properties, with their class_name and key properties.

2. **Should `get_script_summary` show function body line counts?** E.g., `func take_damage(amount: int) -> void  [12 lines]`. This helps the agent gauge complexity without reading the body. Low cost, probably worth it.

3. **Should we detect preload/load paths?** E.g., `var scene = preload("res://enemy.tscn")` creates a dependency that isn't visible in @exports. Including these in the architecture would give a more complete picture but requires regex scanning of all code.

4. **Should `search_symbols` also search .tscn node names?** An agent might search for "Player" meaning the node in the scene tree, not a GDScript symbol. Including scene nodes in search results would be more helpful but mixes two different concepts.

5. **Should we provide a `project/get_call_graph` tool?** "Who calls `Player.take_damage()`?" requires scanning all .gd files for call sites. This is the most expensive operation but also the most useful for tracing bugs and understanding flow. Could be Phase 5, or folded into `search_symbols` with a `kind="call_site"` filter.

6. **Cache invalidation**: If the agent edits a file and then calls `get_architecture`, should we detect the change and rebuild? Current design always rebuilds. `editor/scan_filesystem` sets a dirty flag we could check, but for <100ms rebuilds this seems unnecessary.
