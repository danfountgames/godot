# ProjectDomainManager

## Purpose

ProjectDomainManager is the state tracker for the currently mounted project. It records whether a project is mounted, the absolute path to its directory, and the target scene path. It does not perform heavy lifting itself -- that is delegated to the other subsystem managers -- but it serves as the canonical source of truth for "is a project mounted?" and "where does it live?".

By the time `mount_project()` is called, `ProjectSettingsLayerManager` has already redirected `res://` to the project directory via `ProjectSettings::set_resource_path()`. ProjectDomainManager validates the directory exists and records the mount state.

**Source files:** `modules/devplayer/project_domain_manager.h`, `modules/devplayer/project_domain_manager.cpp`

## Key APIs

| Method | Description |
|--------|-------------|
| `mount_project(path, target_scene) -> Error` | Validates the project directory exists, stores the absolute path and target scene, sets `mounted = true`. Returns `ERR_ALREADY_IN_USE` if already mounted. Resolves `res://` prefixed paths to absolute via `ProjectSettings::globalize_path()`. |
| `unmount_project()` | Clears the project path, target scene, and sets `mounted = false`. No-op if not mounted. |
| `relaunch_target_scene()` | Placeholder method that logs a relaunch request. The actual scene change is orchestrated by LaunchController. |
| `is_project_mounted() -> bool` | Returns `true` if a project is currently mounted. Used extensively by other subsystems as a guard check. |
| `get_mounted_project_path() -> String` | Returns the absolute path to the mounted project directory. Empty string if not mounted. |
| `get_target_scene() -> String` | Returns the `res://` path of the scene that was mounted. Empty string if not mounted or no scene specified. |

## Architecture

ProjectDomainManager is intentionally simple. Its internal state consists of three fields:

- `bool mounted` -- whether a project is currently active.
- `String project_path` -- absolute filesystem path to the project root.
- `String target_scene` -- the `res://` path of the target scene (e.g., `res://main.tscn`).

On `mount_project()`:
1. Rejects if already mounted (`ERR_ALREADY_IN_USE`).
2. Resolves `res://` paths to absolute paths if needed.
3. Verifies the directory exists via `DirAccess::exists()`.
4. Stores the path and scene, sets `mounted = true`.
5. Logs the current `res://` mapping for debugging.

On `unmount_project()`:
1. Clears all three state fields.
2. Logs completion.

The class does not interact with `ProjectSettings` directly during mount. The `res://` redirection has already been performed by `ProjectSettingsLayerManager::load_project_settings()` before `mount_project()` is called.

## Integration

- **Queried by nearly every other subsystem** to check mount state and get the project path.
- **LaunchController** calls `mount_project()` as step 4 of the mount sequence and `unmount_project()` as the final step 7 of unmount.
- **GitManager** reads `get_mounted_project_path()` and `get_target_scene()` to determine whether a branch switch requires an unmount/remount cycle.
- **SyncServer** reads `get_mounted_project_path()` via `_get_project_root()` to determine where to write synced files.
- **DevPlayerShell** checks `is_project_mounted()` to update the UI status display and button states.

## Critical Notes

- `mount_project()` does NOT load `project.godot` or change `resource_path`. That is `ProjectSettingsLayerManager`'s responsibility. By design, ProjectDomainManager only records state.
- The `relaunch_target_scene()` method is a stub. It logs a message but does not perform the actual relaunch. Use `LaunchController::relaunch()` instead.
- Callers should always check `is_project_mounted()` before calling mount or unmount to avoid error conditions.
