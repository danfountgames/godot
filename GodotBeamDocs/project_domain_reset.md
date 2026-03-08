# Project Domain Reset -- Mount/Unmount Sequences

## Status: ALL SEQUENCES PROVEN (2026-03-08)

Both mount and unmount sequences have been validated end-to-end. 226/226 checks pass across 56 cycles (6 named + 50 stress) with 0 leaked references.

---

## Mount Sequence (PROVEN WORKING -- All Milestones Complete)

### Critical Discoveries

1. **ProjectSettings::setup() cannot be used** for project switching because `OS::get_resource_dir()` short-circuits the resource path, ignoring any provided path argument. Solution: `set_resource_path()` (engine modification) + `load_custom()`.

2. **class_name identifiers are not registered on dynamic mount.** Godot's normal startup scans and registers class_name declarations, but dynamic mount via `set_resource_path()` + `load_custom()` does not trigger this scan. Solution: explicit `scan_and_register_global_classes()`.

### Proven Working Mount Sequence

1. Validate project root contains `project.godot`
2. Ensure no project currently mounted, or unmount existing project first
3. **`ProjectSettings::set_resource_path(project_dir)`** -- binds `res://` to the mounted project directory (bypasses `OS::get_resource_dir()` short-circuit)
4. **`ProjectSettings::load_custom(project_dir + "/project.godot")`** -- loads project settings from the mounted project file
5. **`scan_and_register_global_classes()`** -- scans `.gd` files for `class_name` declarations and registers them with `ScriptServer`
6. `res://` remapping now points to mounted project
7. Scene loading works -- `ResourceLoader::load()` resolves paths against new `res://`
8. GDScript execution works -- scripts compile and run in mounted project context
9. Autoloads instantiated via `AutoloadSessionManager::build_autoloads_from_project()`
10. Target scene launched via `LaunchController::launch_project()`

### Mount Performance
- Mount time: **0.005s** for minimal_2d project

---

## Unmount Sequence (PROVEN WORKING -- All Milestones Complete)

### Critical Discoveries

1. **queue_free() causes stale scripts.** Deferred deletion keeps references alive past script cache clearing. Solution: `memdelete()` for immediate synchronous cleanup.

2. **GDScriptCache::clear() has two bugs.** The `cleared` flag prevents re-use, and the `dependencies` map is never cleared. Solution: custom `flush_project_caches()`.

3. **ResourceCache::clear() is too aggressive.** It clears ALL cached resources including shell resources. Solution: targeted eviction via `set_path("")` on project-specific resources.

### Proven Working Unmount Sequence

1. **`LaunchController::stop_project()`** -- stop running scene, `memdelete()` all project scene nodes (immediate, not deferred via queue_free)
2. **`AutoloadSessionManager::destroy_autoloads()`** -- teardown autoload nodes in reverse order using `memdelete()`
3. **`ScriptServer::global_classes_clear()`** -- remove all project class_name entries from the registry
4. **`GDScriptCache::flush_project_caches()`** -- clear all script caches (parser_map, shallow/full/static caches), reset `cleared` flag, clear `dependencies` map
5. **`ResourceDomainManager::purge_project_resources()`** -- evict project resources from ResourceCache via `set_path("")` (preserves shell resources)
6. **`ImportSessionManager::clear_import_state()`** -- clear import tracking state
7. **`ProjectSettingsLayerManager::clear_project_settings()`** -- remove project-specific settings
8. **`ProjectSettings::set_resource_path(shell_dir)`** -- rebind `res://` back to shell directory
9. **`ProjectDomainManager::unmount_project()`** -- mark no project mounted
10. Return control to DevPlayerShell

---

## Full Cycle: Mount A -> Unmount A -> Mount B

This is the proven sequence for project switching (M4):

1. **Mount Project A** (mount sequence above)
2. Run Project A's scene
3. **Stop Project A** (step 1 of unmount)
4. **Unmount Project A** (steps 2-9 of unmount)
5. **Mount Project B** (mount sequence above)
6. Run Project B's scene
7. Verify: Project B's class_name identifiers resolve correctly (e.g., Enemy = Passive, not Aggressive from A)
8. Verify: No stale autoloads from A
9. Verify: No stale resources from A
10. Verify: No stale script caches from A

### class_name Isolation Verification
- Project A defines `class_name Enemy` with Aggressive behavior
- Project B defines `class_name Enemy` with Passive behavior
- After A -> B switch, `Enemy` resolves to Passive (B's definition)
- After B -> A switch, `Enemy` resolves to Aggressive (A's definition)
- Verified across 50 stress cycles with 0 failures

---

## Branch Switch Cycle: Same Project, Different Branch

This is the proven sequence for branch switching (M5):

1. **Mount project from branch X** (mount sequence above)
2. Run project scene
3. **Stop project** (step 1 of unmount)
4. **Unmount project** (steps 2-9 of unmount)
5. **`GitManager::checkout(project_dir, branch_Y)`** -- switch to different branch
6. **Mount project from branch Y** (mount sequence above)
7. Run project scene (now shows branch Y content)

---

## APIs Used (Final Reference)

### Mount APIs
| API | Location | Purpose |
|-----|----------|---------|
| `ProjectSettings::set_resource_path()` | core/config/project_settings.cpp | Bind res:// to project dir (engine modification) |
| `ProjectSettings::load_custom()` | core/config/project_settings.cpp | Load project.godot from specific path |
| `scan_and_register_global_classes()` | modules/devplayer/ | Scan .gd files and register class_name with ScriptServer |

### Unmount APIs
| API | Location | Purpose |
|-----|----------|---------|
| `memdelete()` | core/os/memory.h | Immediate node deletion (NOT queue_free) |
| `ScriptServer::global_classes_clear()` | core/object/script_language.cpp | Remove all class_name registrations |
| `GDScriptCache::flush_project_caches()` | modules/gdscript/gdscript_cache.cpp | Clear all caches, fix cleared flag + dependencies (engine modification) |
| `set_path("")` on resources | core/io/resource.cpp | Evict specific resources from ResourceCache |
| `ProjectSettings::set_resource_path()` | core/config/project_settings.cpp | Rebind res:// back to shell |

### APIs NOT to Use
| API | Reason |
|-----|--------|
| `ProjectSettings::setup()` | OS::get_resource_dir() short-circuits the resource path |
| `GDScriptCache::clear()` | `cleared` flag bug prevents re-use; `dependencies` map never cleared |
| `ResourceCache::clear()` | Too aggressive -- clears shell resources along with project resources |
| `queue_free()` during unmount | Deferred deletion keeps script references alive past cache clearing |

---

## Test Infrastructure

- **8 test projects**: minimal_2d, class_name_collision_test_a/b, autoload_reset_test, resource_cache_test_a/b, branch_switch_test, live_reload_test
- **23 shell tests** in run_domain_tests.sh
- **226/226 automated cycling checks pass** across 56 cycles
- **0 leaked references** after all cycles
