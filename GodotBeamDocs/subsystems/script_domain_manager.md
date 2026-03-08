# ScriptDomainManager

## Purpose

ScriptDomainManager handles GDScript cache management and `class_name` registration for the DevPlayer mount/unmount lifecycle. When a project is mounted, it scans all `.gd` files, parses `class_name` declarations, and registers them with `ScriptServer` so that scripts can reference custom types by name (e.g., `var e = Enemy.new()`). When a project is unmounted, it flushes the GDScript cache and clears all global class registrations to prevent stale scripts from leaking into the next project session.

**Source files:** `modules/devplayer/script_domain_manager.h`, `modules/devplayer/script_domain_manager.cpp`

## Key APIs

| Method | Description |
|--------|-------------|
| `initialize_for_project(path)` | Records the current project path. Called early in the mount sequence (step 5) to prepare the script domain. |
| `scan_and_register_global_classes(project_root)` | Recursively collects all `.gd` files in the project, extracts `class_name` declarations using `ScriptLanguage::get_global_class_name()`, and registers each one with `ScriptServer::add_global_class()`. Must be called BEFORE loading any scenes. |
| `shutdown_project_scripts()` | Calls `clear_script_caches()` and `clear_global_class_registrations()`, then clears the stored project path. Called during unmount. |
| `clear_script_caches()` | Flushes the GDScript cache via `GDScriptCache::flush_project_caches()`. |
| `clear_global_class_registrations()` | Clears all globally registered script classes via `ScriptServer::global_classes_clear()`. |

## Architecture

### Global Class Scanning

The `scan_and_register_global_classes()` method works as follows:

1. **Collect `.gd` files** -- `_collect_gd_files()` recursively walks the project directory using `DirAccess`. It skips hidden directories (those starting with `.`, such as `.godot`, `.git`, `.import`).

2. **Find the GDScript language** -- Iterates `ScriptServer::get_language_count()` to locate the language instance named `"GDScript"`.

3. **Parse each file for `class_name`** -- For each `.gd` file, converts the absolute path to a `res://` path, then calls `gdscript_lang->get_global_class_name(res_path, ...)`. This returns the class name (if declared), the base type, icon path, and whether the class is abstract or a tool script.

4. **Register with ScriptServer** -- If a class name is found, calls `ScriptServer::add_global_class()` with the class name, base type, language name, resource path, and metadata flags.

This scanning must happen before any scene loading because scenes may contain nodes with scripts that reference `class_name` identifiers. If the global classes are not registered first, those references will fail to resolve.

### Cache Flushing Strategy

The cache clearing uses `GDScriptCache::flush_project_caches()` rather than `GDScriptCache::clear()`. This is a deliberate design choice:

- **`GDScriptCache::clear()`** sets an internal `cleared` flag that prevents subsequent cache operations from working. It also does not clear the `dependencies` map. This makes it unsuitable for repeated dynamic project switching.
- **`GDScriptCache::flush_project_caches()`** is designed for the dynamic switching use case. It clears cached scripts and dependencies without setting any blocking flags, allowing the cache to be repopulated cleanly on the next mount.

## Integration

- **LaunchController** calls `initialize_for_project()` at step 5 and `scan_and_register_global_classes()` at step 5b during mount. Calls `shutdown_project_scripts()` at step 3 during unmount.
- Depends on the **GDScript module** (`modules/gdscript/gdscript_cache.h`) for cache operations and on **ScriptServer** for global class registration.

## Critical Notes

- DO NOT replace `flush_project_caches()` with `clear()`. The `clear()` method sets a `cleared` flag that permanently disables subsequent cache operations, breaking all future project mounts.
- Global class scanning MUST happen before scene loading. If the order is reversed, any script using `class_name` identifiers will fail to compile at load time.
- The scanner skips hidden directories (`.godot`, `.git`, etc.) but processes all other directories recursively. Projects with deeply nested directory structures may see measurable scan times.
- When switching between two projects that define the same `class_name` with different implementations, the old registration is overwritten by the new one. The `clear_global_class_registrations()` call during unmount ensures no stale registrations persist.
