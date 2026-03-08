# Milestone 3 — Project Domain Unmount

## Status: COMPLETE
**Started**: 2026-03-08
**Completed**: 2026-03-08

## Objective
Autoload teardown, script cache clear, resource cache purge, project settings removal.

## Pass Criteria
- [x] Debug counters show reset after exit
- [x] All caches fully cleared between mounts
- [x] No leaked references after unmount

## Tasks
- [x] Implement AutoloadSessionManager.destroy_autoloads()
- [x] Implement ScriptDomainManager.shutdown_project_scripts()
- [x] Implement ScriptDomainManager.clear_script_caches() via flush_project_caches()
- [x] Implement ScriptDomainManager.clear_global_class_registrations()
- [x] Implement ResourceDomainManager.purge_project_resources() via set_path("") eviction
- [x] Implement ImportSessionManager.clear_import_state()
- [x] Implement ProjectSettingsLayerManager.clear_project_settings()
- [x] Implement ProjectDomainManager.unmount_project()
- [x] Add debug instrumentation/counters
- [x] Create autoload_reset_test project
- [x] Test debug counters show clean state
- [x] Verify 0 leaked references after full unmount

## Critical Discovery: GDScriptCache::clear() Has Two Bugs

**Problem**: The stock `GDScriptCache::clear()` method in the Godot engine has two bugs that prevent clean remounting of projects:

### Bug 1: `cleared` Flag Prevents Re-use
When `GDScriptCache::clear()` is called, it sets an internal `cleared` boolean flag to `true`. This flag is never reset. On subsequent calls, the cache checks this flag and short-circuits, meaning the cache cannot be used again after the first clear. This makes mount-unmount-remount cycles impossible with the stock API.

### Bug 2: `dependencies` Map Never Cleared
`GDScriptCache::clear()` clears the `parser_map`, `shallow_gdscript_cache`, `full_gdscript_cache`, and `static_gdscript_cache` maps, but it **never clears the `dependencies` map**. This map tracks script-to-script dependencies (preloads, extends). Stale entries from Project A's scripts remain when Project B is mounted, causing incorrect dependency resolution and potential crashes.

**Solution**: Implemented `GDScriptCache::flush_project_caches()` as a new engine modification that:
1. Clears all four script cache maps (same as `clear()`)
2. Clears the `dependencies` map
3. Resets the `cleared` flag to `false`

## Critical Discovery: ResourceCache Eviction Strategy

**Problem**: `ResourceCache::clear()` is too aggressive -- it clears ALL cached resources including shell resources needed for DevPlayerShell UI.

**Solution**: Use targeted eviction via `set_path("")` on project-specific resources. By setting a resource's path to empty string, it is evicted from the ResourceCache without affecting other cached resources. ResourceDomainManager iterates over cached resources, identifies those belonging to the mounted project (path starts with the project's `res://` prefix), and evicts them individually.

## Full Unmount Sequence (Proven Working)
1. `LaunchController::stop_project()` -- stop scene, memdelete nodes (M2)
2. `AutoloadSessionManager::destroy_autoloads()` -- teardown autoload nodes in reverse order
3. `ScriptServer::global_classes_clear()` -- remove all project class_name entries
4. `GDScriptCache::flush_project_caches()` -- clear all script caches, reset cleared flag, clear dependencies
5. `ResourceDomainManager::purge_project_resources()` -- evict project resources via set_path("")
6. `ImportSessionManager::clear_import_state()` -- clear import tracking
7. `ProjectSettingsLayerManager::clear_project_settings()` -- remove project settings
8. `ProjectSettings::set_resource_path(shell_dir)` -- rebind res:// back to shell
9. `ProjectDomainManager::unmount_project()` -- mark no project mounted

## Engine Modifications Introduced in M3

| File | Change |
|------|--------|
| modules/gdscript/gdscript_cache.h | Added `flush_project_caches()` declaration |
| modules/gdscript/gdscript_cache.cpp | Added `flush_project_caches()` -- clears all caches, resets `cleared` flag, clears `dependencies` map |

## APIs Used for Domain Reset (All Resolved)

| API | Location | Purpose | Status |
|-----|----------|---------|--------|
| `GDScriptCache::flush_project_caches()` | modules/gdscript/gdscript_cache.cpp | Clear all script caches, fix cleared flag + dependencies bugs | WORKING (custom) |
| `ScriptServer::global_classes_clear()` | core/object/script_language.cpp:90 | Remove all global class registrations | WORKING (stock) |
| `ResourceCache` eviction via `set_path("")` | core/io/resource.cpp | Evict specific resources without clearing shell | WORKING (stock API, custom strategy) |
| `set_resource_path()` | core/config/project_settings.cpp | Rebind res:// path | WORKING (custom) |

## Dependencies
- M2 must be complete (SATISFIED)
