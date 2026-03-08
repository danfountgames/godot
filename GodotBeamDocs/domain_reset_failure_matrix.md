# Domain Reset Failure Matrix

## Status: ALL RESOLVED (2026-03-08)

All domain reset subsystems have been tested and verified working across 226/226 checks in 56 cycles with 0 leaked references.

| Subsystem | Expected Reset Behavior | API Used | Actual Observed | Failure Mode Found | Fix Applied |
|-----------|------------------------|----------|-----------------|-------------------|-------------|
| ProjectSettings | Clear mounted project settings, revert to shell | `set_resource_path(shell_dir)` + clear settings | Works correctly | None | `set_resource_path()` rebinds res:// back to shell |
| GDScript Cache | All compiled scripts from project cleared | `GDScriptCache::flush_project_caches()` | Works correctly after fix | **Two bugs in stock clear()**: (1) `cleared` flag prevents re-use, (2) `dependencies` map never cleared | Custom `flush_project_caches()` resets `cleared` flag and clears `dependencies` map |
| class_name Registry | All project class_name entries removed | `ScriptServer::global_classes_clear()` | Works correctly | class_name identifiers not auto-registered on dynamic mount | Added `scan_and_register_global_classes()` on mount |
| Autoloads | All project autoload nodes destroyed | `AutoloadSessionManager::destroy_autoloads()` via `memdelete()` | Works correctly after fix | **queue_free() causes stale scripts**: deferred deletion keeps refs alive past cache clearing | Use `memdelete()` for immediate synchronous cleanup |
| Resource Cache | All project resources released | `ResourceDomainManager::purge_project_resources()` via `set_path("")` | Works correctly | **ResourceCache::clear() too aggressive**: clears shell resources too | Targeted eviction via `set_path("")` on project-specific resources only |
| Editor Filesystem | Import state cleared for project | `ImportSessionManager::clear_import_state()` | Works correctly | None | Standard clear |
| Preloaded Resources | All preload references released | Cleared as part of script cache + resource cache flush | Works correctly | None | Covered by flush_project_caches() + resource eviction |
| Script Globals | Global variables in scripts reset | Part of `flush_project_caches()` | Works correctly | None | Cleared as part of script cache flush |

## Critical Bugs Found and Fixed

### Bug 1: GDScriptCache::clear() -- `cleared` Flag (Risk B)
- **Location**: modules/gdscript/gdscript_cache.cpp
- **Symptom**: Second mount after first unmount fails to load scripts
- **Root cause**: `clear()` sets `cleared = true` and never resets it. Subsequent cache operations check this flag and short-circuit.
- **Fix**: `flush_project_caches()` resets `cleared = false` after clearing

### Bug 2: GDScriptCache::clear() -- `dependencies` Map (Risk B)
- **Location**: modules/gdscript/gdscript_cache.cpp
- **Symptom**: Script dependency resolution picks up stale entries from previous project
- **Root cause**: `clear()` clears parser_map, shallow/full/static caches but never clears the `dependencies` map
- **Fix**: `flush_project_caches()` explicitly clears `dependencies` map

### Bug 3: queue_free() Stale Scripts (Risk C)
- **Location**: Scene teardown during unmount
- **Symptom**: Stale script references survive unmount, interfere with next mount
- **Root cause**: `queue_free()` defers deletion to end of frame via MessageQueue. Script cache is cleared in the same frame. Nodes still hold script references when cache is cleared.
- **Fix**: Use `memdelete()` for immediate synchronous deletion

### Bug 4: class_name Not Registered on Dynamic Mount
- **Location**: Script domain initialization during mount
- **Symptom**: class_name identifiers from mounted project not resolvable
- **Root cause**: Godot's normal startup scans and registers class_name declarations, but dynamic mount via set_resource_path() + load_custom() does not trigger this scan
- **Fix**: Explicit `scan_and_register_global_classes()` called during mount

### Bug 5: ResourceCache::clear() Too Aggressive (Risk E)
- **Location**: Resource domain cleanup during unmount
- **Symptom**: Shell UI resources (DevPlayerShell textures, themes) also cleared
- **Root cause**: `ResourceCache::clear()` removes ALL cached resources regardless of origin
- **Fix**: Targeted eviction via `set_path("")` on resources whose path matches the mounted project prefix

## All Risks Resolved

| Risk | Description | Resolution |
|------|-------------|------------|
| A | iOS tools=yes viability | tools=yes works via command-line override of get_flags() defaults |
| B | GDScript domain unload | flush_project_caches() fixes both bugs in GDScriptCache::clear() |
| C | Autoload teardown/rebuild | memdelete() for immediate cleanup; AutoloadSessionManager handles ordered teardown |
| D | ProjectSettings rebinding | set_resource_path() + load_custom() bypasses OS::get_resource_dir() short-circuit |
| E | Resource cache invalidation | Targeted eviction via set_path("") preserves shell resources |
