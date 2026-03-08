# GodotBeam DevPlayer Architecture

## System Overview

GodotBeam DevPlayer is a fork of the Godot Engine that replaces the standard editor UI with a developer shell for dynamically running Godot projects at runtime. Instead of shipping the full Godot editor, DevPlayer provides a lightweight shell that can **mount**, **run**, and **unmount** arbitrary Godot projects without recompiling or restarting the engine.

**Platform status:**
- **Linux:** Proven. The interactive shell, mount/unmount lifecycle, Git integration, and SyncServer protocol are all built and tested on Linux. The 11-step regression demo passes all 38 checks. The automated domain test suite passes all 36 checks across 56 mount/unmount cycles.
- **iOS:** Not yet proven. The code compiles with safety guards (`APPLE_EMBEDDED_ENABLED`) but has not been built or tested on iOS hardware. See `IOS_BUILD.md` for the detailed blocker analysis.

The core concept is **project domain isolation**: the engine boots once with its own minimal `project.godot`, then dynamically redirects `res://` to point at a target project's directory, loads that project's settings/scripts/scenes/autoloads, runs the project, and later tears everything down cleanly so a different project can be mounted in its place.

DevPlayer is built as a Godot module (`modules/devplayer/`) consisting of 10 singleton subsystems that together handle the full lifecycle of mounting, running, and unmounting guest projects. All module code lives in `modules/devplayer/` and is shared across platforms — the same C++ code runs on Linux and iOS.

### Shared Core vs Platform Shell

The DevPlayer architecture separates **shared core** (platform-independent subsystems) from **platform shell** (platform-specific entry points):

- **Shared core** (`modules/devplayer/`): All 10 singletons, the mount/unmount lifecycle, Git integration, SyncServer, import validation, and the DevPlayerShell UI. This code is identical on all platforms.
- **Platform entry points**: `main/main.cpp` (CLI flags, shell creation) and `platform/ios/main_ios.mm` (auto-inject `--devplayer` flag). These are thin integration layers.

Linux is the proving ground for the shared core. Any bug found and fixed on Linux is fixed for all platforms.

## Component Diagram

```
+------------------------------------------------------------------+
|                         main/main.cpp                            |
|  (CLI flags: --devplayer, --devplayer-mount, --devplayer-test)   |
+----------------------------+-------------------------------------+
                             |
                             v
+------------------------------------------------------------------+
|                      DevPlayerShell                              |
|  (Interactive shell: project list, git, sync, log, test runner)  |
+----------------------------+-------------------------------------+
                             |
                             v
+------------------------------------------------------------------+
|                     LaunchController                             |
|  (Orchestrator: sequences mount and unmount across all managers) |
+--------+-------+-------+-------+-------+-------+----------------+
         |       |       |       |       |       |
         v       v       v       v       v       v
+--------+-+ +---+----+ +--+---+ +--+---+ +--+--+ +-----+------+
| Project  | |Settings| |Script| |Resrc | |Import| |Autoload    |
| Domain   | |Layer   | |Domain| |Domain| |Sess. | |Session     |
| Manager  | |Manager | |Mgr   | |Mgr   | |Mgr   | |Manager     |
+----------+ +--------+ +------+ +------+ +------+ +------------+

+------------------------------------------------------------------+
|                      DevPlayerDebug                              |
|  (Metrics/telemetry: mount duration, resource counts, leaks)     |
+------------------------------------------------------------------+

+------------------------------------------------------------------+
|                        GitManager                                |
|  (Git operations: clone, fetch, checkout, switch_and_remount)    |
+------------------------------------------------------------------+

+------------------------------------------------------------------+
|                        SyncServer                                |
|  (WebSocket server: file sync from host, tiered reload hints)    |
+------------------------------------------------------------------+
```

### The 10 Singletons (initialization order)

| #  | Singleton                      | Responsibility |
|----|--------------------------------|----------------|
| 1  | `DevPlayerDebug`               | Measured post-unmount counters: mount duration, resource counts, residual references |
| 2  | `ProjectDomainManager`         | Tracks mount state: project path, target scene, mounted flag |
| 3  | `ScriptDomainManager`          | GDScript cache lifecycle, global class (`class_name`) scanning and registration |
| 4  | `AutoloadSessionManager`       | Builds and destroys autoload nodes from project settings |
| 5  | `ProjectSettingsLayerManager`  | Captures/restores base settings, loads mounted project's `project.godot`, remaps `res://` |
| 6  | `ResourceDomainManager`        | Tracks and purges `res://` resources from `ResourceCache` on unmount |
| 7  | `ImportSessionManager`         | Validates import readiness: scans for `.import` metadata, reports missing assets |
| 8  | `LaunchController`             | Orchestrates mount/unmount sequences across all managers |
| 9  | `GitManager`                   | Git CLI wrapper (clone, fetch, checkout) with orchestrated branch-switch-and-remount |
| 10 | `SyncServer`                   | WebSocket server (port 6850) for receiving file pushes and reload hints from a host |

After construction, `LaunchController` receives references to managers 1-7 via `set_managers()`, and `GitManager` receives a reference to `LaunchController`.

## Mount Sequence

When `LaunchController::launch_project(path, target_scene)` is called, the following steps execute synchronously:

```
Step  1: ProjectSettingsLayerManager::load_base_settings()
         - Snapshots all current ProjectSettings properties
         - Saves original resource_path for later restoration

Step  2: ProjectSettingsLayerManager::load_project_settings(path)
         - Calls ProjectSettings::set_resource_path(abs_path) to remap res://
         - Loads project.godot via ProjectSettings::load_custom()
         - Reads application/run/main_scene

Step  3: Resolve target scene
         - If target_scene argument is non-empty, use it
         - Otherwise, read from ProjectSettings "application/run/main_scene"
         - If still empty, roll back settings and fail

Step  4: ProjectDomainManager::mount_project(path, scene_path)
         - Validates directory exists
         - Sets mounted=true, stores path and scene

Step  5: ScriptDomainManager::initialize_for_project(path)
         - Records current_project_path

Step 5b: ScriptDomainManager::scan_and_register_global_classes(path)
         - Recursively collects all .gd files (skipping . directories)
         - Parses each for class_name declarations
         - Calls ScriptServer::add_global_class() for each
         - MUST happen before scene loading so class_name references resolve

Step  6: ResourceDomainManager::begin_tracking(path)
         - Clears tracked resource list, records project root

Step  7: ImportSessionManager::bind_project_root(path)
         ImportSessionManager::scan_filesystem()
         - Binds to project root
         - Scans for importable assets (png, wav, ttf, etc.)
         - Checks each for .import metadata sidecar file
         - Reports missing imports (project must be pre-imported in editor)

Step  8: AutoloadSessionManager::build_autoloads_from_project()
         - Reads autoload list from ProjectSettings
         - Loads each as PackedScene or Script
         - Instantiates nodes, adds to SceneTree root
         - Registers singletons as named global constants in ScriptServer

Step  9: ProjectSettingsLayerManager::commit_effective_settings()
         - Currently a no-op; reserved for future settings-change notifications

Step 10: ResourceLoader::load(scene_path, CACHE_MODE_REPLACE_DEEP)
         - Loads the target scene fresh from disk (never from cache)
         - Instantiates the scene
         - Adds instantiated node to SceneTree root

         DevPlayerDebug metrics updated: mount duration, resource/autoload counts
```

If any step fails, all previously completed steps are rolled back in reverse order.

## Unmount Sequence

When `LaunchController::stop_project()` is called, the following steps execute synchronously:

```
Step 1: Remove and free mounted scene node
        - parent->remove_child(mounted_scene_node)
        - memdelete(mounted_scene_node)  [NOT queue_free -- see design decisions]
        - Pointer set to nullptr

Step 2: AutoloadSessionManager::destroy_autoloads()
        - Removes named global constants from ScriptServer languages
          (only those we actually registered, tracked in registered_global_names)
        - Removes each autoload node from its parent
        - Calls memdelete() on each autoload node (NOT queue_free -- same reason
          as mounted scene: script refs must be released before cache flush)
        - Clears the active_autoloads and registered_global_names lists

Step 3: ScriptDomainManager::shutdown_project_scripts()
        - Calls GDScriptCache::flush_project_caches() [NOT clear() -- see design decisions]
        - Calls ScriptServer::global_classes_clear()
        - Resets current_project_path

Step 4: ResourceDomainManager::purge_project_resources()
        - Iterates all entries in ResourceCache
        - Collects every resource with a res:// path
        - Calls set_path("") on each to evict from ResourceCache [see design decisions]
        - Drops all references so resources can be freed

Step 5: ImportSessionManager::clear_import_state()
        - Clears project_root, resets bound flag

Step 6: ProjectSettingsLayerManager::clear_project_settings()
        - Restores original resource_path (res:// maps back to engine root)
        - Removes properties ADDED by the mounted project (not in base backup)
          - Setting to NIL triggers ProjectSettings::remove_autoload() for
            "autoload/*" keys, preventing autoload contamination across mounts
        - Restores all base settings from backup snapshot
        - Resets base_captured flag for fresh capture on next mount

Step 7: ProjectDomainManager::unmount_project()
        - Clears project_path, target_scene
        - Sets mounted=false

        Post-unmount leak check: measures remaining res:// resources in
        ResourceCache, remaining autoloads, and remaining ProjectSettings
        autoload entries. All should be 0 after clean unmount.

        DevPlayerDebug metrics updated: unmount duration, leak counts
```

## Engine Modifications

Seven core Godot engine files were modified to support DevPlayer's dynamic project mounting:

### 1. `main/main.cpp`
- Added CLI flags: `--devplayer`, `--devplayer-mount <path>`, `--devplayer-scene <scene>`, `--devplayer-test`
- `--devplayer` sets `devplayer_mode=true` and `editor=true` (editor subsystems are needed at runtime)
- `--devplayer-test` implies `--devplayer` and enables automated test mode
- In the editor startup path, when `devplayer_mode` is true, the standard editor UI is bypassed entirely; a `DevPlayerShell` control is created and added to the scene tree instead
- Auto-mount and auto-test are triggered after shell creation
- Several editor-specific paths are guarded with `&& !devplayer_mode` to avoid loading editor chrome, SSL certificates, and session restoration

### 2. `core/config/project_settings.h`
- Added `void set_resource_path(const String &p_path)` as a public method
- This was previously private/non-existent; DevPlayer needs it to dynamically remap `res://` to a mounted project directory

### 3. `core/config/project_settings.cpp`
- Implemented `set_resource_path()`: sets `resource_path` and strips trailing slash
- This allows `ProjectSettingsLayerManager` to point `res://` at any directory at runtime

### 4. `modules/gdscript/gdscript_cache.h`
- Added `static void flush_project_caches()` declaration
- Designed for repeated dynamic project switching (unlike `clear()` which is one-shot)

### 5. `modules/gdscript/gdscript_cache.cpp`
- Implemented `flush_project_caches()`:
  - Clears `parser_inverse_dependencies`, `abandoned_parser_map`, `parser_map`
  - Clears `shallow_gdscript_cache`, `full_gdscript_cache`, `static_gdscript_cache`
  - Clears `dependencies` (which `clear()` misses)
  - Resets `cleared=false` so the method can be called again on the next mount cycle

### 6. `platform/ios/main_ios.mm`
- When `MODULE_DEVPLAYER_ENABLED` is defined, automatically injects `--devplayer` into the argument list if not already present
- This ensures iOS builds always boot into the DevPlayer shell without requiring manual argument passing

### 7. `platform/ios/detect.py`
- Added a comment block explaining that while iOS does not list "editor" in its `supported` list by default, the GodotBeam fork allows building with `target=editor` on iOS
- The `supported` list only blocks "library" builds, not editor builds, so passing `target=editor` on the command line overrides the default and enables `TOOLS_ENABLED`

## Data Flow

### res:// Path Remapping

```
UNMOUNTED STATE:
  res:// -> /path/to/GodotBeam/         (engine root, contains shell's project.godot)

MOUNT:
  ProjectSettingsLayerManager::load_project_settings(path)
    -> ProjectSettings::set_resource_path("/path/to/target/project")
  res:// -> /path/to/target/project/    (mounted project directory)

UNMOUNT:
  ProjectSettingsLayerManager::clear_project_settings()
    -> ProjectSettings::set_resource_path(original_resource_path)
  res:// -> /path/to/GodotBeam/         (restored to engine root)
```

### Script Loading Flow

```
1. ScriptDomainManager::scan_and_register_global_classes(project_root)
   - Walks filesystem collecting .gd files
   - Converts absolute paths to res:// paths
   - Calls ScriptLanguage::get_global_class_name() to parse class_name
   - Registers via ScriptServer::add_global_class()

2. When scenes are loaded, GDScript resources resolve via res://
   - Since resource_path now points to the mounted project, res://enemy.gd
     loads from /path/to/project/enemy.gd

3. On unmount:
   - GDScriptCache::flush_project_caches() clears all cached parser results
   - ScriptServer::global_classes_clear() removes all class_name registrations
   - Next mount can register entirely different classes with the same names
```

### Scene Instantiation Flow

```
1. ResourceLoader::load(scene_path, CACHE_MODE_REPLACE_DEEP)
   - Loads .tscn from disk, ignoring any cached version
   - Sub-resources (scripts, textures) also loaded fresh

2. packed_scene->instantiate()
   - Creates live node tree from the PackedScene

3. SceneTree::get_root()->add_child(mounted_scene_node)
   - Node enters the tree, _ready() callbacks fire
   - Project GDScript code begins executing
```

## Critical Design Decisions

### Why `memdelete` instead of `queue_free` for the mounted scene

The mounted scene node is freed with `memdelete()` (immediate deletion) rather than `queue_free()` (deferred deletion). This is intentional:

- `queue_free()` defers deletion to the end of the current frame
- During that deferral, the node still holds references to GDScript resources and other `res://` resources
- Steps 3-4 of unmount (script cache flush and resource purge) need those references to already be released
- If references are still held, cache eviction fails silently, and stale scripts from project A persist into the next mount of project B
- `memdelete()` ensures all script instances are released **before** the cache and ResourceCache are cleared

### Why `flush_project_caches` instead of `clear`

`GDScriptCache::clear()` has two problems for DevPlayer's use case:

1. It sets an internal `cleared` flag that **prevents all subsequent calls from working**. Since DevPlayer mounts and unmounts projects repeatedly in a single engine session, `clear()` would only work once.
2. It does not clear the `dependencies` map, which can retain stale dependency edges from a previous project.

`flush_project_caches()` was added to solve both problems: it clears all caches including `dependencies`, and resets `cleared=false` so it can be called again on the next domain switch.

### Why `set_path("")` for ResourceCache eviction

Resources are evicted from `ResourceCache` by calling `resource->set_path("")`. This works because Godot's `Resource::set_path()` implementation calls `ResourceCache::resources.erase(path_cache)` when the path changes. Setting the path to empty effectively removes the resource from the cache's HashMap lookup table.

This approach is preferred over trying to call `ResourceCache::clear()` or similar bulk operations because:
- It only evicts resources belonging to the mounted project (those with `res://` paths)
- It leaves engine-internal resources untouched
- After eviction, the references are dropped so the resources can be garbage collected

### Why `CACHE_MODE_REPLACE_DEEP`

When loading the target scene, `ResourceLoader::load()` is called with `CACHE_MODE_REPLACE_DEEP`. This ensures:

- The scene is always loaded fresh from disk, never from cache
- All sub-resources (scripts, textures, materials) are also loaded fresh
- This is critical for project switching: `res://main.tscn` from project A must not be reused when mounting project B, even though both resolve to the same `res://` path
- Without this flag, the second mount would silently reuse the first project's cached scene

## Interactive Shell (DevPlayerShell)

The `DevPlayerShell` is a `Control` node that provides the interactive user interface for the DevPlayer. It is created in `main/main.cpp` and added directly to the SceneTree root.

### Shell Sections

| Section | UI Elements | Purpose |
|---------|------------|---------|
| Status  | Title, status label, project path, metrics | Shows mount state, resource counts, and measured post-unmount counters |
| Projects | ItemList, Mount/Refresh/Unmount/Relaunch buttons, custom path LineEdit | Discovers projects in `test_projects/`, mounts by selection or custom path |
| Git | Repo path, branch name, Switch & Remount, List Branches | Exercises GitManager: switch branch and remount in one operation |
| Sync | Port input, Start/Stop buttons, status label | Controls the SyncServer WebSocket server |
| Log | RichTextLabel | Scrollable log of all shell operations, mirrors to stdout |
| Overlay | "Back to Shell (F12)" button | Visible only when a project is running and shell is hidden |

### Input Handling

- **F12**: Toggles shell visibility when a project is mounted. Uses `unhandled_key_input` (C++ virtual override) so it works regardless of which control has focus.
- **Double-click**: On project list item, mounts immediately.
- **Shell/project coexistence**: The shell UI and mounted project scene coexist in the same SceneTree. The shell hides itself when a project launches and shows an overlay button to return.

### Engine Root Capture

The shell captures the engine's `resource_path` in its constructor, before any project mount changes `res://`. This preserved path is used to locate `test_projects/` for the project discovery scan. Without this, mounting a project would change `res://` and break project discovery.

## Leak Measurement

`DevPlayerDebug` measures post-unmount counters — **not** a proof of zero leaks overall. After each unmount:

- **Residual res:// resources**: Counts remaining `res://` entries in `ResourceCache`. Should be 0 after clean unmount. Measured, not inferred.
- **Residual autoloads**: Counts remaining autoload nodes. Should be 0 after clean unmount.
- **Residual project settings**: Counts remaining `autoload/*` keys in ProjectSettings. Should be 0 after clean unmount.

These counters are verified to reach zero across 56 mount/unmount cycles (36-check test suite). This demonstrates that the unmount sequence is thorough, but it does not prove the absence of all possible memory leaks (e.g., native engine allocations, global state side effects).

## Editor Coupling Dependencies

The following Godot editor subsystems are **required** by DevPlayer at runtime:

| Dependency | Why Needed | Impact |
|-----------|-----------|--------|
| `EditorSettings` | Many UI widgets (`LineEdit`, `TextEdit`) call `EDITOR_GET()` for caret settings when `TOOLS_ENABLED` is defined. Without EditorSettings, these crash. | Created at shell startup via `EditorSettings::create()` |
| `GDScriptCache` | `flush_project_caches()` is our custom method. Only available in editor/tools builds. | Requires `target=editor` build |
| `ScriptServer::add_global_class` / `global_classes_clear` | Needed for `class_name` registration/deregistration. Available in all builds. | No special requirement |
| `ResourceCache` | Needed for resource tracking and eviction. Available in all builds. | No special requirement |

The following Godot editor subsystem is **not usable** by DevPlayer:

| Blocked Dependency | Why Blocked | Workaround |
|-------------------|------------|------------|
| `EditorFileSystem` | 9+ hard dependencies on `EditorNode` and its singletons (`EditorPaths`, `ProjectSettingsEditor`, `ScriptEditor`, `EditorResourcePreview`, `EditorHelp`, `DisplayServer`, `EditorProgress`). Cannot be instantiated outside the full editor. | `ImportSessionManager` validates import readiness by scanning for `.import` metadata files instead. Projects must be pre-imported in the full Godot editor. |

## Milestone Status

| Milestone | Description | Linux | iOS |
|-----------|------------|-------|-----|
| A | Autoload contamination fix: project-added properties cleaned on unmount | **Proven** — tested in domain test suite, zero autoload leaks across 56 cycles | Untested |
| B | Real leak instrumentation: measured post-unmount counters | **Proven** — counters verified at zero across all test cycles | Untested |
| C | ImportSessionManager: lightweight import cache validator | **Proven** — scans importable files, detects missing `.import` metadata, warns without blocking | Untested |
| D | GitManager + SyncServer functional tests | **Proven** — 19/19 GitManager checks, 23/23 SyncServer checks | Untested (GitManager disabled on iOS) |
| E | iOS safety guards: `APPLE_EMBEDDED_ENABLED` compile guards | **Proven** — compiles on Linux, guards present in code | Untested on actual iOS |
| F | Interactive Linux shell: project list, git, sync, log, F12 toggle | **Proven** — 38/38 regression demo checks, interactive UI operational | Untested |
| G | 11-step regression demo: automated end-to-end lifecycle test | **Proven** — shell launch, mount A, unmount, mount B, A→B→A, git branch switch, sync write | N/A (Linux-only test infrastructure) |

## Test Infrastructure

| Script | Checks | What It Tests |
|--------|--------|---------------|
| `scripts/run_domain_tests.sh` | 36 | Mount/unmount lifecycle, class_name isolation, autoload reset, resource cache, stress cycling, import validation |
| `scripts/test_git_manager.sh` | 19 | GitManager GDScript API: branch listing, commit info, checkout, switch |
| `scripts/test_sync_server.sh` | 23 | SyncServer WebSocket protocol: hello, manifest, write, reload_hint, sync_complete, path traversal rejection |
| `scripts/regression_demo.sh` | 38 | 11-step end-to-end: shell launch, mount A/B, A→B→A isolation, git branch switch, SyncServer live sync, clean shutdown |
| `--devplayer-test` (in-engine) | 36 | Same as `run_domain_tests.sh` but running inside the engine's automated test mode |
