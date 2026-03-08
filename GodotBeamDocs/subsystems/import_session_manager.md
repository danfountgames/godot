# ImportSessionManager

## Purpose

ImportSessionManager handles resource reimporting for mounted projects. Godot uses an import pipeline (managed by `EditorFileSystem`) to process assets like textures, audio, and 3D models into engine-optimized formats stored in `.import/`. When a project is mounted in the DevPlayer, the import system must be pointed at the project's directory and triggered to scan for assets that need importing or re-importing.

This subsystem is only functional when the engine is compiled with `TOOLS_ENABLED`. In template (export) builds, the scan and import methods are no-ops that log informational messages.

**Source files:** `modules/devplayer/import_session_manager.h`, `modules/devplayer/import_session_manager.cpp`

## Key APIs

| Method | Description |
|--------|-------------|
| `bind_project_root(path)` | Records the project root path and marks the manager as bound. Must be called before `scan_filesystem()` or `import_pending_assets()`. |
| `scan_filesystem()` | Triggers `EditorFileSystem::scan()` to perform a full filesystem scan of the mounted project. Only works with `TOOLS_ENABLED`. |
| `import_pending_assets()` | Triggers `EditorFileSystem::scan_changes()` to detect and import assets that have changed since the last scan. Only works with `TOOLS_ENABLED`. |
| `clear_import_state()` | Clears the project root and resets the bound state. Called during unmount. |

## Architecture

### Binding and Scanning

The import workflow follows a simple bind-then-scan pattern:

1. **`bind_project_root(path)`** -- Stores the project root path and sets `bound = true`. This is a prerequisite for all other operations.

2. **`scan_filesystem()`** -- If `TOOLS_ENABLED` is defined and `EditorFileSystem::get_singleton()` returns a valid instance, calls `efs->scan()`. This triggers a full directory walk that discovers all importable assets and processes any that are new or have changed.

3. **`import_pending_assets()`** -- Similar to `scan_filesystem()`, but calls `efs->scan_changes()` instead, which is a lighter-weight scan that only processes changed files.

### Template Build Behavior

When compiled without `TOOLS_ENABLED` (i.e., export template builds), the `#ifdef` guards cause `scan_filesystem()` and `import_pending_assets()` to skip their `EditorFileSystem` calls entirely. They log messages indicating that the functionality is unavailable. This means DevPlayer running as an export template will rely on pre-imported assets and will not be able to process new imports at runtime.

### State Management

The manager's state is minimal: a `String project_root` and a `bool bound` flag. The `clear_import_state()` method resets both to their defaults, preparing the manager for a new mount session.

## Integration

- **LaunchController** calls `bind_project_root()` and `scan_filesystem()` at step 7 during mount. Calls `clear_import_state()` at step 5 during unmount.
- Depends on **EditorFileSystem** (from `editor/file_system/editor_file_system.h`) for the actual import pipeline. This dependency only exists in editor/tools builds.
- The `import_pending_assets()` method is available for use by other subsystems (e.g., SyncServer could call it after receiving new files) but is not currently called during the standard mount sequence.

## Critical Notes

- The `scan_filesystem()` call may be asynchronous depending on Godot's internal implementation of `EditorFileSystem::scan()`. The LaunchController does not wait for it to complete before proceeding to step 8.
- In export template builds (no `TOOLS_ENABLED`), projects must have their `.import/` directory pre-populated. Any assets that have not been imported will fail to load.
- The manager does not track which assets were imported during a session. The `clear_import_state()` method only resets the binding, not any import artifacts on disk.
- Calling `scan_filesystem()` or `import_pending_assets()` without first calling `bind_project_root()` will trigger an error.
