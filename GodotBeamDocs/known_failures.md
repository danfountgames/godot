# Known Failures

## Status: ALL RESOLVED (2026-03-08)

All failures discovered during development have been resolved. 226/226 checks pass across 56 cycles with 0 leaked references.

| Date | Test/Area | Failure | Root Cause | Resolution |
|------|-----------|---------|------------|------------|
| 2026-03-08 | M1 Mount | ProjectSettings::setup() ignores new project path | OS::get_resource_dir() short-circuits the resource path argument, returning the original binary location | Added set_resource_path() to ProjectSettings as engine modification; pair with load_custom() |
| 2026-03-08 | M2 Unmount | Stale script references survive unmount, corrupt next mount | queue_free() uses deferred deletion (end of frame), keeping script references alive past GDScriptCache clearing | Replaced queue_free() with memdelete() for immediate synchronous cleanup |
| 2026-03-08 | M3 Unmount | Second mount after unmount fails -- scripts won't load | GDScriptCache::clear() sets `cleared` flag to true and never resets it; subsequent cache operations short-circuit | Implemented flush_project_caches() which resets `cleared = false` after clearing |
| 2026-03-08 | M3 Unmount | Stale script dependencies from Project A appear in Project B | GDScriptCache::clear() never clears the `dependencies` map; stale entries persist across mounts | flush_project_caches() explicitly clears the `dependencies` map |
| 2026-03-08 | M3 Unmount | Shell UI resources (textures, themes) destroyed during unmount | ResourceCache::clear() removes ALL cached resources regardless of origin (shell or project) | Replaced with targeted eviction: iterate project resources and call set_path("") on each |
| 2026-03-08 | M4 Switch | class_name identifiers from mounted project not resolvable | Dynamic mount via set_resource_path() + load_custom() does not trigger class_name scanning/registration | Added scan_and_register_global_classes() to mount sequence |

## Failure Patterns Worth Noting

### Deferred vs Immediate Deletion
The queue_free() failure is a fundamental pattern to watch for: any resource cleanup that uses deferred deletion (MessageQueue, call_deferred, etc.) during unmount will race with cache clearing. The rule is: **always use immediate deletion (memdelete) during unmount sequences**.

### Stock API Bugs in Remount Context
Godot's stock cleanup APIs (GDScriptCache::clear(), ResourceCache::clear()) were designed for shutdown, not for remount. They work fine when the engine is about to exit, but fail when the engine needs to continue running with a new project. The pattern: **verify that every cleanup API properly resets state for re-use, not just for shutdown**.

### Dynamic Mount Missing Auto-Registration
Godot's normal startup path performs many implicit registrations (class_name scanning, autoload setup, etc.) that are not triggered by dynamic mount. The pattern: **any implicit registration that happens during Godot startup must be explicitly performed during dynamic mount**.
