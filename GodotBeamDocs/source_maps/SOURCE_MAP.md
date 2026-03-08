# Source Map — Engine Areas for Project Domain System

## Status: FINAL (all milestones complete, 2026-03-08)

Module structure: 10 subsystems, 24 source files.
Engine modifications: 7 files, 183 insertions.

---

## 1. Main Startup / Runtime Orchestration
**Directory**: main/

| File | Key Classes/Functions | Role | Reset Strategy |
|------|----------------------|------|----------------|
| main/main.h | Main class | Static setup/setup2/start/cleanup | Call cleanup(), re-run setup flow |
| main/main.cpp:990 | Main::setup() | Phase 1: core init, ProjectSettings creation, settings load | Re-invoke with new project path |
| main/main.cpp:2951 | Main::setup2() | Phase 2: servers, rendering, physics, scene tree | Servers persist, scene tree recreated |
| main/main.cpp:3883 | Main::start() | Loads main scene, inits autoloads, editor/game mode | Re-invoke for new project |
| main/main.cpp:4370-4437 | Autoload instantiation | Two-pass: register constants then instantiate | Destroy nodes via memdelete(), unregister constants |
| main/main.cpp:4473-4490 | EditorNode creation | **INTERCEPTION POINT** for shell | Replace with DevPlayerShell |
| main/main.cpp:4674-4682 | Main scene load | ResourceLoader::load(local_game_path) | Load different scene |
| main/main.cpp | **--devplayer flag** | **ADDED: Shell mode entry point** | Bypasses editor, launches DevPlayerShell |
| main/main.cpp | **--devplayer-mount flag** | **ADDED: Mount project on startup** | Mounts specified project directory |
| main/main.cpp | **--devplayer-test flag** | **ADDED: Test mode** | Runs automated tests then exits |

## 2. Project Settings
**Directory**: core/config/

**CRITICAL DISCOVERY**: `ProjectSettings::setup()` **CANNOT** be used for project switching because `OS::get_resource_dir()` short-circuits the resource path. Solution: `set_resource_path()` (engine modification) + `load_custom()`.

| File | Key Classes/Functions | Role | Reset Strategy |
|------|----------------------|------|----------------|
| core/config/project_settings.h:67-71 | AutoloadInfo struct | name, path, is_singleton | Clear autoloads HashMap |
| core/config/project_settings.h | **set_resource_path()** | **ADDED: Declaration** | N/A |
| core/config/project_settings.cpp | **set_resource_path()** | **ADDED: Direct resource path binding** | **Use for mount: bypasses OS::get_resource_dir()** |
| core/config/project_settings.cpp | **load_custom()** | **Loads project.godot from specific path** | **Use for mount: pair with set_resource_path()** |
| core/config/project_settings.cpp:198 | setup() | Loads project.godot | **DO NOT USE for project switching** -- OS::get_resource_dir() short-circuits |
| core/config/project_settings.cpp:222-224 | get_autoload_list/add/remove | Autoload management | Clear and rebuild |
| core/config/project_settings.cpp:1426 | refresh_global_class_list() | Reloads global_script_class_cache.cfg | Call after clearing |
| core/config/project_settings.cpp:1439 | get_global_class_list() | Returns Array of global classes | Refresh from new project |
| core/config/project_settings.cpp:1466 | store_global_class_list() | Saves to disk | Write for new project |

### Mount Sequence (Proven Working)
```
ProjectSettings::set_resource_path(project_dir)                    // Bind res:// to project
ProjectSettings::load_custom(project_dir + "/project.godot")       // Load settings
scan_and_register_global_classes()                                 // Register class_name identifiers
// res:// now resolves to mounted project
// Scene loading and GDScript execution work
```

## 3. Resource Loading / Cache
**Directory**: core/io/

**CRITICAL DISCOVERY**: `ResourceCache::clear()` is too aggressive (clears shell resources too). Solution: targeted eviction via `set_path("")` on project-specific resources.

| File | Key Classes/Functions | Role | Reset Strategy |
|------|----------------------|------|----------------|
| core/io/resource_loader.h | ResourceLoader static class | Manages resource loading | clear_thread_load_tasks() |
| core/io/resource_loader.cpp:244 | load() | Load with caching | Use CACHE_MODE_REPLACE |
| core/io/resource_loader.cpp:300 | clear_thread_load_tasks() | Clear in-flight loads | Call during unmount |
| core/io/resource.h | ResourceCache static class | HashMap<String, Resource*> | **Evict via set_path("") -- NOT clear()** |
| core/io/resource.cpp:208 | ResourceCache::clear() | Clear all cached resources | **DO NOT USE** -- clears shell resources |
| core/io/resource.cpp:212-214 | has()/get_ref()/get_cached_resources() | Query cache | Used for verification |
| core/io/resource.cpp | **set_path("")** | Evicts resource from cache | **Use for targeted project resource eviction** |

## 4. Scene Launch / Main Loop
**Directory**: scene/main/

| File | Key Classes/Functions | Role | Reset Strategy |
|------|----------------------|------|----------------|
| scene/main/scene_tree.h | SceneTree : MainLoop | Root node, groups, processing | Clear children, reset groups |
| scene/main/scene_tree.cpp | Constructor, process, physics | Scene lifecycle | Recreate or clear |
| scene/main/node.h/.cpp | Node tree system | All scene objects | **memdelete()** children -- NOT queue_free() |

**CRITICAL DISCOVERY**: `queue_free()` causes stale scripts because deferred deletion keeps references alive past script cache clearing. Use `memdelete()` for immediate synchronous cleanup during unmount.

## 5. GDScript Runtime
**Directory**: modules/gdscript/

**CRITICAL DISCOVERY**: `GDScriptCache::clear()` has two bugs: (1) `cleared` flag prevents re-use, (2) `dependencies` map never cleared. Solution: `flush_project_caches()` (engine modification).

| File | Key Classes/Functions | Role | Reset Strategy |
|------|----------------------|------|----------------|
| modules/gdscript/gdscript_cache.h | GDScriptCache singleton | parser_map, shallow/full/static caches | **flush_project_caches()** -- NOT clear() |
| modules/gdscript/gdscript_cache.h | **flush_project_caches()** | **ADDED: Declaration** | N/A |
| modules/gdscript/gdscript_cache.cpp:117 | remove_script() | Remove from all caches | Call per script |
| modules/gdscript/gdscript_cache.cpp:135 | clear() | Clear ALL caches | **DO NOT USE** -- cleared flag bug + dependencies bug |
| modules/gdscript/gdscript_cache.cpp | **flush_project_caches()** | **ADDED: Clears caches, resets cleared flag, clears dependencies** | **Use during unmount** |
| modules/gdscript/gdscript.h | GDScript class | global_name, member_functions, instances | clear() method |
| modules/gdscript/gdscript.cpp:2096 | GDScriptLanguage::init() | Initialize language, add globals | Re-invoke |
| modules/gdscript/gdscript.cpp:2177 | GDScriptLanguage::finish() | Cleanup, calls GDScriptCache::clear() | Call during unmount |

## 6. Global Script Class Registration
**Directory**: core/object/

**CRITICAL DISCOVERY**: class_name identifiers are not registered on dynamic mount. Must explicitly call `scan_and_register_global_classes()` after mounting.

| File | Key Classes/Functions | Role | Reset Strategy |
|------|----------------------|------|----------------|
| core/object/script_language.h:69 | ScriptServer::global_classes HashMap | class_name registry | global_classes_clear() |
| core/object/script_language.cpp:90 | global_classes_clear() | Clear all global classes | **Call during unmount** |
| core/object/script_language.cpp:91-93 | add/remove_global_class | Register/unregister class_name | Individual clear |
| core/object/script_language.cpp:104 | save_global_classes() | Persist to disk | Save new project's classes |

## 7. Editor Import Pipeline
**Directory**: editor/

| File | Key Classes/Functions | Role | Reset Strategy |
|------|----------------------|------|----------------|
| editor/file_system/editor_file_system.h | EditorFileSystem | File scanning, import tracking | force scan_sources() |
| editor/file_system/editor_file_system.cpp | scan(), scan_sources() | Detect new/modified files | Re-scan new project root |
| editor/import/ | Various importers | Texture, model, audio, font | Re-import for new project |

## 8. Autoload Setup
**Files**: Multiple locations

| File | Key Classes/Functions | Role | Reset Strategy |
|------|----------------------|------|----------------|
| main/main.cpp:4370-4437 | Two-pass autoload setup | Constants first, then instantiate | Reverse: memdelete() then unregister |
| core/config/project_settings.h | autoloads HashMap | Storage of autoload config | Clear and reload |
| scene/main/scene_tree.cpp | root->add_child() | Autoload nodes added to root | Remove from root, memdelete() |

## 9. Editor UI Entry (for bypass)
**Files**: editor/

| File | Key Classes/Functions | Role | Reset Strategy |
|------|----------------------|------|----------------|
| editor/editor_node.h | EditorNode class | Full editor UI | **Do not instantiate** |
| editor/editor_node.cpp:8223+ | Constructor | Creates all editor UI | Replace with DevPlayerShell |
| editor/editor_node.cpp:8377-8465 | Importer registration | Registers all importers | **Keep this** |
| editor/project_manager/ | ProjectManager | Project picker | Replace with DevPlayerShell |

## 10. iOS Platform
**Directory**: platform/ios/

| File | Key Classes/Functions | Role | Engine Modification |
|------|----------------------|------|---------------------|
| platform/ios/detect.py | get_flags() | **Blocks tools=yes by default** | **MODIFIED: Documentation for override** |
| platform/ios/main_ios.mm | apple_embedded_main() | Entry point | **MODIFIED: Auto-inject --devplayer** |
| platform/ios/os_ios.h/mm | OS_IOS | OS implementation | Persist |
| platform/ios/display_server_ios.h/mm | DisplayServerIOS | Screen metrics | Persist |
| platform/ios/godot_view_ios.h/mm | GDTViewIOS | Rendering view | Persist |
| platform/ios/SCsub | Build config | Compiles iOS .mm files | Add new files |

---

## DevPlayer Module Structure (10 Subsystems, 24 Source Files)
**Directory**: modules/devplayer/

| Subsystem | Purpose |
|-----------|---------|
| LaunchController | Orchestrates mount/unmount lifecycle, shell mode, test mode |
| ProjectDomainManager | Project directory binding, validation, mount/unmount state |
| ScriptDomainManager | Script cache management, class_name scanning and registration |
| ProjectSettingsLayerManager | ProjectSettings binding via set_resource_path() + load_custom() |
| AutoloadSessionManager | Autoload instantiation (two-pass) and teardown (memdelete) |
| ResourceDomainManager | Resource cache eviction via set_path("") strategy |
| ImportSessionManager | Import state management for mounted projects |
| DevPlayerShell | Shell UI, command processing, test mode orchestration |
| GitManager | Git operations: clone, fetch, checkout, switch_and_remount |
| SyncServer | WebSocket server on port 6850 for live file sync |

---

## Engine Modification Summary (7 Files, 183 Insertions)

| File | Lines | What Was Added |
|------|-------|----------------|
| core/config/project_settings.h | ~5 | `set_resource_path()` declaration |
| core/config/project_settings.cpp | ~15 | `set_resource_path()` implementation |
| main/main.cpp | ~100 | `--devplayer`, `--devplayer-mount`, `--devplayer-test` flags; shell mode entry |
| modules/gdscript/gdscript_cache.h | ~5 | `flush_project_caches()` declaration |
| modules/gdscript/gdscript_cache.cpp | ~30 | `flush_project_caches()` -- fixes cleared flag + dependencies map bugs |
| platform/ios/main_ios.mm | ~15 | Auto-inject `--devplayer` flag on iOS launch |
| platform/ios/detect.py | ~13 | Documentation comments for tools=yes override |
| **Total** | **~183** | |
