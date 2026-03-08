# LaunchController

## Purpose

LaunchController is the top-level orchestrator for the DevPlayer mount/unmount lifecycle. It coordinates all other subsystem managers in a strict sequence to mount an external Godot project into the running engine at runtime, and to cleanly tear it down when the project is stopped or switched.

It is a singleton (`LaunchController::get_singleton()`) and exposes its methods to GDScript via `ClassDB::bind_method`.

**Source files:** `modules/devplayer/launch_controller.h`, `modules/devplayer/launch_controller.cpp`

## Key APIs

| Method | Description |
|--------|-------------|
| `launch_project(path, target_scene) -> Error` | Executes the full 10-step mount sequence to load and run a project. Returns `OK` on success. If `target_scene` is empty, reads `application/run/main_scene` from the project's settings. |
| `stop_project()` | Executes the 7-step unmount sequence, removing the scene from the SceneTree and tearing down all subsystems in reverse order. |
| `relaunch()` | Convenience method that captures the current project path and scene, calls `stop_project()`, then calls `launch_project()` with the same arguments. Used for hot-reload. |
| `set_managers(...)` | Injects all subsystem manager pointers. Must be called before `launch_project()`. |

## Architecture

### Mount Sequence (10 Steps)

The `launch_project()` method executes these steps in strict order:

1. **Capture base settings** -- `ProjectSettingsLayerManager::load_base_settings()` snapshots all current ProjectSettings values and the original `resource_path` so they can be restored on unmount.

2. **Load project settings** -- `ProjectSettingsLayerManager::load_project_settings(path)` calls `ProjectSettings::set_resource_path()` and `load_custom()` to redirect `res://` to the mounted project and load its `project.godot`.

3. **Determine target scene** -- If no explicit scene was passed, reads `application/run/main_scene` from the freshly loaded project settings. Fails if no scene can be determined.

4. **Mount project domain** -- `ProjectDomainManager::mount_project(path, scene)` records the mount state and validates the directory exists.

5. **Initialize script domain** -- `ScriptDomainManager::initialize_for_project(path)` sets the current project path for script operations.

5b. **Scan and register global classes** -- `ScriptDomainManager::scan_and_register_global_classes(path)` recursively finds all `.gd` files, parses `class_name` declarations, and registers them with `ScriptServer::add_global_class()`. This MUST happen before loading any scenes so that `class_name` identifiers resolve correctly.

6. **Begin resource tracking** -- `ResourceDomainManager::begin_tracking(path)` prepares to track all `res://` resources loaded during the session for later eviction.

7. **Bind and scan imports** -- `ImportSessionManager::bind_project_root(path)` and `scan_filesystem()` trigger `EditorFileSystem::scan()` if running with TOOLS_ENABLED, ensuring `.import` files are processed.

8. **Build autoloads** -- `AutoloadSessionManager::build_autoloads_from_project()` reads autoload entries from ProjectSettings and instantiates them into the SceneTree root.

9. **Commit effective settings** -- `ProjectSettingsLayerManager::commit_effective_settings()` finalizes the settings layer (currently a no-op placeholder for future version-bump notifications).

10. **Load and instantiate the target scene** -- Uses `ResourceLoader::load()` with `CACHE_MODE_REPLACE_DEEP` to force a fresh load from disk (critical for project switching where `res://main.tscn` might collide). Instantiates the scene and adds it to `SceneTree::get_root()`. Updates `DevPlayerDebug` metrics.

If step 10 fails (scene load or instantiation), a full rollback is performed: autoloads are destroyed, scripts shut down, resources purged, import state cleared, settings restored, and the project domain unmounted.

### Unmount Sequence (7 Steps)

The `stop_project()` method tears down in reverse dependency order:

1. **Remove and free the mounted scene** -- The scene node is removed from its parent and freed with `memdelete()` (not `queue_free()`). This is intentional: synchronous deletion ensures script instances release their references to GDScript resources before the cache is cleared. Using `queue_free()` would leave stale references and prevent proper cache eviction.

2. **Destroy autoloads** -- `AutoloadSessionManager::destroy_autoloads()`.

3. **Shutdown scripts** -- `ScriptDomainManager::shutdown_project_scripts()` flushes the GDScript cache and clears global class registrations.

4. **Purge resources** -- `ResourceDomainManager::purge_project_resources()` evicts all `res://` entries from `ResourceCache`.

5. **Clear import state** -- `ImportSessionManager::clear_import_state()`.

6. **Restore base settings** -- `ProjectSettingsLayerManager::clear_project_settings()` restores the original `resource_path` and all backed-up settings.

7. **Unmount project domain** -- `ProjectDomainManager::unmount_project()`.

Both mount and unmount durations are recorded via `DevPlayerDebug` for performance monitoring.

## Integration

- **Holds references to all six subsystem managers** plus `DevPlayerDebug`, injected via `set_managers()`.
- **Called by `SyncServer`** for tier-2 reloads (script changes trigger `relaunch()`).
- **Called by `GitManager`** during `switch_and_remount()` for branch-switch workflows.
- **Called by `DevPlayerShell`** when the user presses Mount/Unmount buttons, and during automated test cycles.

## Critical Notes

- The mount sequence must not be interrupted. If any step fails, the remaining steps are skipped and earlier steps are rolled back.
- `CACHE_MODE_REPLACE_DEEP` is essential in step 10. Without it, switching between projects that share the same `res://` scene paths would silently reuse stale cached resources from the previous project.
- `memdelete()` in step 1 of unmount is deliberate. Changing this to `queue_free()` will break cache eviction and cause stale GDScript references on remount.
- A project must be explicitly stopped before a new one can be mounted. Calling `launch_project()` while mounted returns `ERR_ALREADY_IN_USE`.
