# ProjectSettingsLayerManager

## Purpose

ProjectSettingsLayerManager saves and restores the engine's `ProjectSettings` during the mount/unmount lifecycle. When the DevPlayer shell starts, the engine has its own `project.godot` and its own `resource_path`. When a user project is mounted, the shell's settings must be temporarily replaced with the project's settings (including redirecting `res://` to the project directory). When the project is unmounted, the original shell settings must be restored so the engine returns to its baseline state.

This subsystem acts as a "settings layer" -- capturing the base layer, overlaying the project layer, and peeling it off on unmount.

**Source files:** `modules/devplayer/project_settings_layer_manager.h`, `modules/devplayer/project_settings_layer_manager.cpp`

## Key APIs

| Method | Description |
|--------|-------------|
| `load_base_settings()` | Snapshots all current ProjectSettings properties and the current `resource_path` into a backup HashMap. Called once at the start of each mount sequence before any settings are modified. |
| `load_project_settings(path) -> Error` | Redirects `res://` to the project directory via `ProjectSettings::set_resource_path()`, then loads the project's `project.godot` via `ProjectSettings::load_custom()`. Restores the original resource path on failure. |
| `apply_session_overrides(overrides)` | Applies a Dictionary of key-value overrides to ProjectSettings. Allows runtime configuration injection (e.g., forced display settings). |
| `commit_effective_settings()` | Finalizes the settings for the current session. Currently a no-op placeholder for future notifications to settings-dependent subsystems. |
| `clear_project_settings()` | Restores all backed-up base settings and the original `resource_path`. Called during unmount to return the engine to its shell state. |

## Architecture

### Settings Backup

The `load_base_settings()` method iterates `ProjectSettings::get_property_list()` and stores every property's current value into a `HashMap<StringName, Variant> base_settings_backup`. It also saves the current `ProjectSettings::get_resource_path()` separately as `original_resource_path`. Properties whose names start with `_` or are empty are skipped.

### Project Settings Loading

The `load_project_settings()` method performs two critical operations:

1. **`ProjectSettings::set_resource_path(abs_path)`** -- Directly sets the resource path so that `res://` resolves to the mounted project's directory. This bypasses `ProjectSettings::setup()`, which would use `OS::get_resource_dir()` and re-discover the engine's own project path instead of the mounted one.

2. **`ProjectSettings::load_custom(project_file)`** -- Loads the `project.godot` file from the specified path. This merges the project's settings into the current ProjectSettings instance, overwriting any keys that conflict with the shell's settings.

If `load_custom()` fails, the original resource path is restored immediately to avoid leaving the engine in a broken state.

### Settings Restoration

The `clear_project_settings()` method restores state in two steps:

1. Restores `original_resource_path` via `set_resource_path()`, so `res://` points back to the engine shell's directory.
2. Iterates the `base_settings_backup` HashMap and calls `ps->set(key, value)` for each saved property, overwriting the project's settings with the original shell values.

## Integration

- **LaunchController** calls `load_base_settings()` as step 1 and `load_project_settings()` as step 2 during mount. Calls `commit_effective_settings()` at step 9. Calls `clear_project_settings()` at step 6 during unmount.
- **ProjectDomainManager** relies on the resource path already being set by this manager before `mount_project()` is called.
- The `res://` redirection established by `load_project_settings()` affects all subsequent `ResourceLoader::load()` calls, making them resolve against the mounted project's directory.

## Critical Notes

- `set_resource_path()` is used instead of `ProjectSettings::setup()` because `setup()` would use the OS-level resource directory, which points to the engine binary's own project -- not the mounted project. This distinction is critical.
- `load_custom()` does not clear existing settings before loading. It overlays the project's settings on top of the engine's. The `clear_project_settings()` method must restore ALL backed-up properties to undo this overlay, not just the ones the project changed.
- The `apply_session_overrides()` method is available for runtime injection but is not currently called by LaunchController. It is intended for future use cases such as forcing resolution or display mode settings for specific devices.
- `commit_effective_settings()` is currently a no-op. Future implementations may emit signals or bump a version counter to notify subsystems that depend on settings changes.
- `base_captured` must be `true` before `clear_project_settings()` can execute. If `load_base_settings()` was never called, the restore will fail with an error and refuse to proceed.
