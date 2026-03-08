# ImportSessionManager

## Purpose

ImportSessionManager validates the import readiness of mounted projects. When a Godot project uses importable assets (textures, audio, 3D models, fonts), those assets must pass through Godot's import pipeline to be converted into engine-optimized formats. The import pipeline is normally driven by `EditorFileSystem`, which is only available inside the full Godot editor.

**DevPlayer runs without the full editor**, so ImportSessionManager cannot perform actual imports. Instead, it scans the project filesystem to verify that importable assets have been pre-imported (i.e., have corresponding `.import` metadata files) and reports any gaps.

**Source files:** `modules/devplayer/import_session_manager.h`, `modules/devplayer/import_session_manager.cpp`

## EditorFileSystem Architectural Blocker

`EditorFileSystem` (from `editor/file_system/editor_file_system.h`) **cannot be instantiated outside of EditorNode**. It has at least 9 hard dependencies on editor singletons:

| Dependency | Used In | Impact |
|---|---|---|
| `EditorNode::get_editor_data()` | `_first_scan_filesystem()`, `_update_script_classes()`, `_register_global_class_script()` | Null dereference crash |
| `EditorNode::get_singleton()` | `_first_scan_filesystem()`, `_update_scan_actions()`, `_reimport_group()`, `_copy_file()` | Null dereference crash |
| `EditorPaths::get_singleton()` | `_scan_filesystem()`, `_save_filesystem_cache()` | Null dereference crash |
| `EditorProgress` / `EditorProgressBG` | Nearly every major operation | Calls EditorNode static methods |
| `ProjectSettingsEditor::get_singleton()` | `_first_scan_filesystem()` | Null dereference crash |
| `ScriptEditor::get_singleton()` | `_update_script_documentation()`, `_should_reload_script()` | Null dereference crash |
| `EditorResourcePreview::get_singleton()` | `_scan_fs_changes()`, `_reimport_file()` | Null dereference crash |
| `EditorHelp` (static methods) | `_update_script_documentation()` | Crash |
| `DisplayServer::get_singleton()` | `reimport_files()` | Potential crash in headless mode |

EditorFileSystem is also a `Node` subclass that must be added to the SceneTree to function (it relies on `NOTIFICATION_PROCESS` to consume threaded scan results).

**Conclusion:** A lightweight reimplementation would be needed to run the full import pipeline outside the editor. This is tracked as a future enhancement. For now, projects must be pre-imported by opening them in the Godot editor at least once.

## Current Implementation: Import Cache Validation

Instead of attempting actual imports, ImportSessionManager performs useful validation:

### What It Does

1. **Checks for `.godot/imported/` directory** — This is Godot 4.x's import cache directory. If missing, the project has never been opened in the editor.

2. **Scans for importable files** — Recursively walks the project directory looking for files with importable extensions (png, jpg, wav, ogg, mp3, gltf, glb, fbx, ttf, otf, svg, etc.).

3. **Checks for `.import` metadata** — For each importable file, checks if a corresponding `.import` sidecar file exists. This file tells ResourceLoader where to find the imported/cached version.

4. **Reports gaps** — Logs warnings for any importable assets missing their `.import` metadata, listing up to 50 specific files.

### Scan Results

After `scan_filesystem()`, the following are available:

| Accessor | Description |
|---|---|
| `get_total_importable_files()` | Count of files with importable extensions |
| `get_files_with_import_metadata()` | Count of files that have `.import` sidecar files |
| `get_files_missing_import_metadata()` | Count of files missing `.import` metadata |
| `has_import_cache()` | Whether `.godot/imported/` directory exists |
| `get_missing_imports()` | Vector of relative paths to files missing imports |

## Key APIs

| Method | Description |
|--------|-------------|
| `bind_project_root(path)` | Records the project root path. Must be called before `scan_filesystem()`. |
| `scan_filesystem()` | Scans project directory for importable assets and validates import metadata. |
| `import_pending_assets()` | Reports missing imports as warnings. Cannot perform actual imports without EditorFileSystem. |
| `clear_import_state()` | Clears the project root, bound flag, and all scan results. Called during unmount. |

## Importable Extensions

The scan checks for these common file extensions:

- **Images:** png, jpg, jpeg, bmp, svg, webp, tga, hdr, exr
- **Audio:** wav, ogg, mp3
- **3D Models:** gltf, glb, fbx, obj, dae, blend
- **Fonts:** ttf, otf, woff, woff2
- **Translations:** csv, po

This is a subset of what Godot supports (plugins/GDExtensions can add more), but covers the vast majority of real-world project assets.

## Integration

- **LaunchController** calls `bind_project_root()` and `scan_filesystem()` at step 7 during mount. Calls `clear_import_state()` at step 5 during unmount.
- Projects with missing imports will still mount and launch — the warnings are informational. Assets that were never imported will fail to load at runtime (ResourceLoader will return null).

## Implications for DevPlayer Users

- **Pre-imported projects work fully.** If a project has been opened in the Godot editor, all assets have `.import` files and `.godot/imported/` cache. DevPlayer loads them via ResourceLoader normally.
- **Scripts-only projects work fully.** Projects that only use .gd scripts, .tscn scenes, and .tres resources don't need the import pipeline at all.
- **New/unimported assets won't load.** If a PNG texture is added to a project but never imported, DevPlayer cannot import it. The scan will warn about this, but loading the texture will fail.

## Future Work

If DevPlayer needs to support projects with unimported assets, the options are:

1. **Build a lightweight `DevPlayerFileSystem`** — Reuse the core scanning logic from EditorFileSystem (`_scan_new_dir`, `_process_file_system`) and call `ResourceFormatImporter` directly for reimports. Skip all UI concerns (EditorProgress, icons, confirmation dialogs) and all editor concerns (plugin init, script docs, ScriptEditor).

2. **Null-guard EditorFileSystem** — Patch `editor_file_system.cpp` with `if (EditorNode::get_singleton())` guards on all ~15 editor-dependent code paths. Fragile and upstream-hostile.

3. **Initialize a minimal EditorNode** — EditorNode's constructor is ~1000 lines and creates the entire editor UI. Impractical.

Option 1 is the recommended approach if this becomes a priority.
