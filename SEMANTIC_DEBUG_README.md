# Semantic Debug System — GDScript Reference

Engine singleton: **`Debug`** (class `DebugSemanticRegistry`)
Console toggle: **backtick `` ` ``** (mobile: three-finger swipe down)
Branch: `feature/debug-console` on top of `feature/mcp-server`
All APIs are no-ops in release builds. No `#ifdef` needed in GDScript.

---

## 1. CVars — Persistent Tuning Variables

```gdscript
# Register with type inferred from default value.
Debug.register_cvar("player.speed", 300.0, "Walk speed", {"min": 50, "max": 1000, "category": "player"})

# Read (returns Variant).
var spd = Debug.get_cvar("player.speed")

# Typed reads (return default if CVar doesn't exist — safe in release builds).
var spd2  = Debug.cvar_float("player.speed", 300.0)
var godm  = Debug.cvar_bool("god_mode", false)
var diff  = Debug.cvar_int("difficulty", 1)
var name  = Debug.cvar_string("player.name", "Hero")

# Write (auto-clamps to min/max, coerces type).
Debug.set_cvar("player.speed", 500.0)
```

### CVar Options Dictionary

| Key | Type | Description |
|-----|------|-------------|
| `category` | String | Grouping label for list/manifest output |
| `min` | Variant | Minimum value (clamped on set) |
| `max` | Variant | Maximum value (clamped on set) |
| `flags` | int | Bitfield: `Debug.CVAR_ARCHIVE`, `CVAR_READONLY`, `CVAR_CHEAT`, `CVAR_HIDDEN` |

Console: type CVar name to read, `name value` to write.

---

## 2. Commands — Console-Callable Functions

```gdscript
# Handler receives PackedStringArray, returns String (displayed in console).
func _teleport(args: PackedStringArray) -> String:
    var x = float(args[0]) if args.size() > 0 else 0.0
    var y = float(args[1]) if args.size() > 1 else 0.0
    player.global_position = Vector2(x, y)
    return "Teleported to %s, %s" % [x, y]

Debug.register_command("teleport", _teleport, "Teleport player", {"x": "float", "y": "float"})

# Optional: tab-complete for arguments.
Debug.set_command_completion("teleport", func(partial: String) -> PackedStringArray:
    return ["0,0", "100,100", "spawn"]
)
```

Console: `teleport 100 200`

---

## 3. Queries — Live-Readable Values

```gdscript
# Register a callable that returns a value. Called each frame when watched.
Debug.register_query("player.health", func(): return player.health, "Current HP")
Debug.register_query("fps", func(): return Engine.get_frames_per_second(), "Framerate")

# Read from code.
var hp = Debug.evaluate_query("player.health")
```

Console: `query.player.health` — read once. `watch query.player.health` — pin to overlay.

---

## 4. Actions — Named Operations with Parameters

```gdscript
# Handler receives Dictionary params, returns Dictionary result.
func _heal(params: Dictionary) -> Dictionary:
    var amount = int(params.get("amount", 50))
    player.health = min(player.health + amount, player.max_health)
    return {"new_health": player.health}

Debug.register_action("heal_player", _heal, "Heal the player", {"amount": "int"})
```

Console: `action.heal_player amount=50`
MCP: the `invoke_action` tool calls this directly.

---

## 5. Events — Signal Monitoring

```gdscript
# Pass a signal reference. The system auto-connects and logs when it fires.
Debug.register_event("player_died", player.died, "Player death")
Debug.register_event("enemy_spawned", spawner.enemy_spawned, "Enemy spawned")

# Query recent events (newest first).
var recent: Array = Debug.get_recent_events(10)
# Each entry: {"name": String, "args": Array, "frame": int, "timestamp_msec": int}
```

Console: `list events` — shows registered events. Events auto-log to console when they fire.

---

## 6. Interactables — Semantic Hints for MCP

```gdscript
# Optional. Helps MCP agents discover what nodes exist and what they can do.
Debug.register_interactable("boss", $Boss, "world_3d", "Level boss",
    ["heal_player", "kill_boss"], "enemies")
```

Types: `"ui"`, `"world_2d"`, `"world_3d"`, `"logic"`.
Not required for the `node` command — that works on any node via path.

---

## 7. auto_expose — One-Line Bulk Registration

```gdscript
# Designed for singletons and unique nodes.
# Scans @export properties → CVars (live-bound, read/write through).
# Scans debug_*() methods → Commands.
class_name GameManager extends Node

@export var difficulty: int = 1        # → CVar "GameManager.difficulty"
@export var music_volume: float = 0.8  # → CVar "GameManager.music_volume"
@export var god_mode: bool = false     # → CVar "GameManager.god_mode"

func _ready():
    Debug.auto_expose(self)            # Tag defaults to class_name

func debug_reset_level(args: PackedStringArray) -> String:
    get_tree().reload_current_scene()  # → Command "GameManager.reset_level"
    return "Level reset."

func debug_spawn_enemy(args: PackedStringArray) -> String:
    var count = int(args[0]) if args.size() > 0 else 1
    for i in count:
        _spawn_enemy()
    return "Spawned %d enemies" % count
```

Auto-cleanup: when the Node exits the tree, all its CVars and Commands are unregistered.

**Tag collision**: if two objects share the same tag, the old one is evicted with a warning.
For multiple instances, pass a unique tag:
```gdscript
Debug.auto_expose(self, "enemy_%d" % get_index())
```

---

## 8. Logging

```gdscript
Debug.log("Player entered zone 3")          # Info  (light blue in console)
Debug.log_warning("Low health: %d" % hp)     # Warning (yellow)
Debug.log_error("Failed to load save file")  # Error (red)
```

Also prints to Godot's standard output/error channels.

---

## 9. Scene Tree Navigation — cd, ls, pwd

Browse the scene tree like a filesystem. The prompt shows your current location.

```
~> ls
  /root (1 children):
    Level [Node2D] / (4)

~> cd Level
~/Level> ls
  Player [CharacterBody2D] / (3)
  Enemies [Node2D] / (5)
  UI [CanvasLayer] / (8)
  Camera [Camera2D]

~/Level> cd Enemies
~/Level/Enemies> ls
  Goblin1 [Enemy]
  Goblin2 [Enemy]
  Goblin3 [Enemy]
  Boss [BossEnemy] / (2)
  Spawner [EnemySpawner]

~/Level/Enemies> Goblin3.health            # ← bare child name, no "node" prefix needed
  /root/Level/Enemies/Goblin3.health = 100
~/Level/Enemies> Goblin3.health 1
  /root/Level/Enemies/Goblin3.health = 1
~/Level/Enemies> Boss:take_damage 999
  Called take_damage() on /root/Level/Enemies/Boss

~/Level/Enemies> cd ..
~/Level> Player.position
  /root/Level/Player.position = (200, 300)

~/Level> cd
~>
```

### Commands

| Command | Description |
|---------|-------------|
| `cd [path]` | Change directory. No args = go to `/root`. `..` = parent. |
| `ls [path]` | List children. No args = current directory. |
| `pwd` | Print current working directory. |

All paths (cd, ls, node, bare names) resolve relative to cwd. Absolute paths (`/root/...`) always work.

---

## 10. The `node` Command — Direct Node I/O

`node` gives explicit access to any node. But most of the time, you'll just `cd` there and use bare child names (see above). The `node` prefix is for absolute paths, groups, and one-off access.

```
node /root/Level/Player                    # Inspect (class, properties, children)
node /root/Level/Player.health             # Read property
node /root/Level/Player.health 100         # Write property
node /root/Level/Player.position 200,300   # Write Vector2
node /root/Level/Player.modulate #ff0000   # Write Color (hex)
node /root/Level/Player:take_damage 50     # Call method
node @enemies                              # List all nodes in group
node @enemies.health 999                   # Set on all in group
node @enemies:queue_free                   # Call on all in group
```

### Implicit shortcuts (no `node` prefix needed)

| Input | Resolves to |
|-------|-------------|
| `Player.health` | child "Player" property "health" (if cwd has child "Player") |
| `../Boss:die` | relative path method call |
| `@enemies.health 999` | group targeting (always works, `@` is unambiguous) |
| `./Sprite.visible false` | explicit relative |

### Delimiters

| Delimiter | Meaning |
|-----------|---------|
| `.` after last `/` | Property access (read or write) |
| `:` | Method call |
| `@` prefix | Group targeting (`SceneTree.get_nodes_in_group()`) |

Property writes auto-detect the target type: bool, int, float, Vector2 (`x,y`), Vector3 (`x,y,z`), Color (`#hex` or `r,g,b,a`), string fallback.

---

## 11. UI Page Semantics — Navigation Graph

Annotate your game's screen/page structure so the debug console and MCP agents can understand UI navigation flow.

### Registration

```gdscript
# Register each screen/page with its Control node.
func _ready():
    Debug.register_ui_page("main_menu", $MainMenu, "Title screen with Play/Settings/Quit", {
        "children": ["settings", "credits"],
    })
    Debug.register_ui_page("settings", $SettingsPanel, "Game settings", {
        "parent": "main_menu",
        "back": "BackButton",            # Node name or path for "go back"
        "children": ["settings.audio", "settings.video", "settings.controls"],
        "enter_actions": ["SettingsBtn"],  # What button navigates here
    })
    Debug.register_ui_page("settings.audio", $SettingsPanel/AudioTab, "Audio settings", {
        "parent": "settings",
        "back": "settings",
    })
    Debug.register_ui_page("game_hud", $HUD, "In-game HUD overlay")
    Debug.register_ui_page("pause_menu", $PauseMenu, "Pause menu", {
        "parent": "game_hud",
        "back": "ResumeBtn",
    })
```

### Options Dictionary

| Key | Type | Description |
|-----|------|-------------|
| `parent` | String | Parent page name (builds hierarchy) |
| `children` | Array[String] | Sub-pages reachable from here |
| `back` | String | How to go back (button name, page name, or action) |
| `enter_actions` | Array[String] | Buttons/actions that navigate to this page |
| `metadata` | Dictionary | Game-specific extra data |

Auto-cleanup: pages are unregistered when their Control exits the tree.

### Console Commands

```
~> ui pages                              # Show page tree with active markers
=== UI Pages ===
main_menu [ACTIVE] — Title screen with Play/Settings/Quit
├─ settings — Game settings
│  ├─ settings.audio — Audio settings
│  ├─ settings.video — Video settings
│  └─ settings.controls — Control settings
└─ credits — Credits screen
game_hud — In-game HUD overlay
├─ pause_menu — Pause menu

~> ui where                              # Show current location + navigation options
Current: main_menu
  (Title screen with Play/Settings/Quit)
  Navigate to: settings, credits

~> ui go settings                        # Navigate to a page (hides siblings, shows target)
Navigated to: settings

~> ui go settings.audio                  # Navigate to nested page
Navigated to: settings.audio

~> ui detect                             # Auto-detect page-like structures (no registration needed)
=== Auto-Detected UI Structure ===
  [TabContainer] /root/UI/Settings — 3 tabs:
   → 0: Audio
     1: Video
     2: Controls
  [Page Stack] /root/UI/Screens — 3 pages (1 visible, 2 hidden):
    MainMenu [Control] [ACTIVE]
    GameScreen [Control]
    Credits [Control]
```

### GDScript API

```gdscript
Debug.register_ui_page(name, node, description, options)
Debug.unregister_ui_page(name)
Debug.has_ui_page(name) -> bool
Debug.get_ui_page_list() -> PackedStringArray
Debug.get_active_ui_page() -> String           # Currently visible page
Debug.get_ui_page_info(name) -> Dictionary     # Full info with live visibility
Debug.get_ui_navigation_graph() -> Dictionary  # Entire graph (for MCP)
```

### MCP Integration

The manifest (`Debug.get_manifest()`) includes:
- `ui_pages` — full navigation graph with visibility state
- `active_ui_page` — name of the currently active page

MCP agents can call `get_ui_navigation_graph()` to understand where they are in the game's UI and what pages are reachable.

---

## 12. The `ui` Command — Control Interaction + Page Navigation

### Control Interaction

```
ui /root/UI/PlayBtn press                # Press a button
ui /root/UI/GodMode toggle               # Toggle a checkbox
ui /root/UI/Volume 0.8                   # Set slider value
ui /root/UI/Difficulty 2                 # Select option by index
ui /root/UI/Name text Hello              # Set text on LineEdit
ui /root/UI/Tabs tab 2                   # Switch tab
ui /root/UI/Menu select 3               # Activate menu item
```

### Page Navigation

```
ui pages                                 # Show page hierarchy
ui where                                 # Current page + breadcrumb
ui go <page>                             # Navigate to named page
ui detect                                # Auto-detect page patterns
```

### Filter Keywords

```
ui buttons                               # List only buttons in cwd
ui sliders                               # List only sliders
ui toggles                               # List only checkboxes
```

---

## 13. Built-in Console Commands

| Command | Description |
|---------|-------------|
| **Navigation** | |
| `cd [path]` | Change working directory in scene tree |
| `ls [path]` | List children of node |
| `pwd` | Print working directory |
| `node target` | Explicit node I/O (see section 10) |
| **UI** | |
| `ui <path> [action]` | Interact with UI controls semantically |
| `ui pages` | Show registered UI page hierarchy |
| `ui where` | Show current active page and breadcrumb |
| `ui go <page>` | Navigate to a named UI page |
| `ui detect` | Auto-detect page-like structures |
| **Debug** | |
| `help [name]` | Show help for a command, CVar, action, or query |
| `clear` | Clear console output |
| `list [category]` | List items (actions, queries, events, cvars, commands, interactables, pages, all) |
| `watch query.name` | Pin a query to the on-screen overlay |
| `unwatch [query.name]` | Remove from overlay (no args = clear all) |
| **Time control** | |
| `pause` | Suspend the game (engine-level, console stays active) |
| `resume` | Resume the game |
| `step [N]` | Advance N frames then re-suspend |
| `timescale [value]` | Get/set Engine time scale (0.0–100.0) |
| **Utility** | |
| `echo text` | Print text to console |
| `exec path` | Execute commands from a text file |
| `screenshot [path]` | Save screenshot (default: `user://screenshot_TIMESTAMP.png`) |

---

## 14. Manifest — Full Introspection

```gdscript
var manifest: Dictionary = Debug.get_manifest()
# Returns: {"actions": {...}, "queries": {...}, "events": {...},
#           "interactables": {...}, "cvars": {...}, "commands": {...},
#           "ui_pages": {...}, "active_ui_page": "..."}
```

Used by the MCP server to discover all registered debug capabilities.

---

## 15. Complete Example — Game Singleton

```gdscript
class_name GameDebug extends Node

@export var god_mode: bool = false
@export var draw_hitboxes: bool = false
@export_range(0.1, 3.0) var speed_multiplier: float = 1.0

var player: CharacterBody2D

func _ready():
    player = get_tree().get_first_node_in_group("player")

    # Bulk: all @export → CVars, all debug_*() → Commands.
    Debug.auto_expose(self)

    # Manual registrations for things auto_expose can't infer.
    Debug.register_query("player.pos", func(): return player.global_position, "Player position")
    Debug.register_query("player.health", func(): return player.health, "Player HP")
    Debug.register_query("enemy.count", func(): return get_tree().get_nodes_in_group("enemies").size())
    Debug.register_event("player_died", player.died, "Player death")
    Debug.register_action("give_item", _give_item, "Give item", {"item": "string", "count": "int"})

func _give_item(params: Dictionary) -> Dictionary:
    var item = params.get("item", "sword")
    var count = int(params.get("count", 1))
    player.inventory.add(item, count)
    return {"given": item, "count": count}

func debug_kill_all(args: PackedStringArray) -> String:
    var enemies = get_tree().get_nodes_in_group("enemies")
    for e in enemies:
        e.queue_free()
    return "Killed %d enemies" % enemies.size()

func debug_tp(args: PackedStringArray) -> String:
    if args.size() < 2:
        return "Usage: tp <x> <y>"
    player.global_position = Vector2(float(args[0]), float(args[1]))
    return "Teleported to %s, %s" % [args[0], args[1]]

func _process(_delta):
    # Typed getters — zero-cost in release builds (return the default).
    if Debug.cvar_bool("god_mode"):
        player.health = player.max_health
```

### UI page registration example

```gdscript
# In your UI manager or main scene:
func _ready():
    Debug.register_ui_page("main_menu", $UI/MainMenu, "Title screen", {
        "children": ["settings", "credits"],
    })
    Debug.register_ui_page("settings", $UI/Settings, "Settings page", {
        "parent": "main_menu",
        "back": "BackBtn",
        "enter_actions": ["SettingsBtn"],
    })
    Debug.register_ui_page("game", $UI/GameHUD, "In-game HUD")
    Debug.register_ui_page("pause", $UI/PauseMenu, "Pause menu", {
        "parent": "game",
    })
```

### Console session

```
~> help
~> list cvars
~> GameDebug.god_mode true
~> GameDebug.speed_multiplier 2.5
~> watch query.player.health
~> watch query.enemy.count
~> GameDebug.kill_all
~> @enemies                             # list all enemies
~> @enemies.health 999                  # god-mode all enemies
~> cd Level/Enemies
~/Level/Enemies> ls
~/Level/Enemies> Boss.health
~/Level/Enemies> Boss.health 1
~/Level/Enemies> Boss:queue_free
~/Level/Enemies> cd
~> action.give_item item=sword count=5
~> ui pages                               # show page hierarchy
~> ui where                               # where am I?
~> ui go settings                         # navigate to settings
~> ui detect                              # auto-detect page structures
~> pause
~> step 10
~> resume
~> screenshot
```

---

## 16. Release Build Behavior

Every `Debug.*` call compiles and runs in release — they're just no-ops:
- `register_*` / `unregister_*` → do nothing
- `get_cvar` → returns `Variant()` (null)
- `cvar_bool("x", true)` → returns `true` (the default param)
- `cvar_float("x", 300.0)` → returns `300.0`
- `log` / `log_warning` / `log_error` → do nothing
- `get_manifest` → returns `{}`

**No `#ifdef` in GDScript. No performance cost.** The typed CVar getters are designed so
the default parameter IS your production value.

---

## 17. File Inventory

| File | Role |
|------|------|
| `scene/debugger/debug_semantic_registry.h` | Registry singleton (Actions, Queries, Events, Interactables, CVars, Commands) |
| `scene/debugger/debug_semantic_registry.cpp` | Implementation + release stubs |
| `scene/debugger/debug_console.h` | In-game console (input, history, output ring buffer) |
| `scene/debugger/debug_console.cpp` | Console logic, all built-in commands including `node` |
| `scene/debugger/debug_console_renderer.h/cpp` | RenderingServer-based overlay drawing |
| `scene/debugger/debug_console_autocomplete.h/cpp` | Prefix/substring completion engine |
| `scene/register_scene_types.cpp` | Registers singleton as `Debug`, wires up lifecycle |
| `main/main.cpp` | Creates DebugConsole, calls `poll()` from `Main::iteration()` |
| `scene/main/viewport.cpp` | Input hook — root viewport routes input to console first |
