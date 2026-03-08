# GodotBeam Dev Player - Master Execution Tracker

## Status: ALL MILESTONES COMPLETE
**Started**: 2026-03-07
**Completed**: 2026-03-08
**Final Result**: 226/226 checks pass across 56 cycles, 0 leaked references

---

## Milestone Status

| Milestone | Description | Status | Started | Completed | Notes |
|-----------|------------|--------|---------|-----------|-------|
| M0 | Build Viability | COMPLETE | 2026-03-07 | 2026-03-08 | Engine compiles with devplayer module. Linux: 4min full, 12s incremental |
| M1 | Minimal Project Mount | COMPLETE | 2026-03-08 | 2026-03-08 | Full mount pipeline with class_name scanning. Mount time: 0.005s |
| M2 | Return to Shell | COMPLETE | 2026-03-08 | 2026-03-08 | stop_project() with memdelete (not queue_free) for clean unmount |
| M3 | Project Domain Unmount | COMPLETE | 2026-03-08 | 2026-03-08 | GDScriptCache::flush_project_caches() fixes two bugs; ResourceCache eviction via set_path("") |
| M4 | Two-Project Switch | COMPLETE | 2026-03-08 | 2026-03-08 | class_name isolation verified (Enemy A=Aggressive, B=Passive), autoload lifecycle tested |
| M5 | Branch Switching | COMPLETE | 2026-03-08 | 2026-03-08 | GitManager subsystem (clone, fetch, checkout, switch_and_remount) |
| M6 | Live Sync | COMPLETE | 2026-03-08 | 2026-03-08 | SyncServer (WebSocket on port 6850) + Rust desktop sync agent (dev-sync-agent/) |
| M7 | Stress Stability | COMPLETE | 2026-03-08 | 2026-03-08 | 226/226 checks pass across 56 cycles (6 named + 50 stress), 0 leaked references |

---

## Critical Discoveries

### 1. ProjectSettings::setup() Cannot Be Used for Remounting
**Problem**: `ProjectSettings::setup()` cannot be used for project switching because `OS::get_resource_dir()` short-circuits the resource path, ignoring the provided path argument.

**Solution**: Added `set_resource_path()` to `ProjectSettings` -- a minimal engine modification that allows direct resource path binding without going through the OS layer. Pair with `load_custom()` for settings loading.

### 2. GDScriptCache::clear() Has Two Bugs
**Problem**: The stock `GDScriptCache::clear()` has two bugs that prevent clean remounting:
- The `cleared` flag is set on first call and never reset, preventing re-use of the cache after clearing
- The `dependencies` map is never cleared, leaving stale dependency references across mounts

**Solution**: Implemented `GDScriptCache::flush_project_caches()` as a new engine modification that properly clears all caches, resets the `cleared` flag, and clears the `dependencies` map.

### 3. queue_free() Causes Stale Scripts
**Problem**: Using `queue_free()` for scene teardown during unmount causes stale script references to persist. Deferred deletion keeps references alive past the point where the script cache is cleared, resulting in dangling script objects on the next mount.

**Solution**: Use `memdelete()` for immediate cleanup during unmount instead of `queue_free()`.

### 4. class_name Identifiers Not Registered on Dynamic Mount
**Problem**: When dynamically mounting a project, `class_name` identifiers declared in GDScript files are not automatically registered with ScriptServer, meaning they cannot be used for type resolution in the mounted project.

**Solution**: Implemented `scan_and_register_global_classes()` which scans all `.gd` files in the mounted project, parses class_name declarations, and registers them with `ScriptServer`.

---

## Module Structure (10 Subsystems, 24 Source Files)

| Subsystem | Status | Role |
|-----------|--------|------|
| LaunchController | COMPLETE | Orchestrates mount/unmount lifecycle, shell mode |
| ProjectDomainManager | COMPLETE | Manages project directory binding and validation |
| ScriptDomainManager | COMPLETE | Script cache management, class_name scanning and registration |
| ProjectSettingsLayerManager | COMPLETE | ProjectSettings binding via set_resource_path() + load_custom() |
| AutoloadSessionManager | COMPLETE | Autoload instantiation, teardown in correct order |
| ResourceDomainManager | COMPLETE | Resource cache eviction via set_path("") strategy |
| ImportSessionManager | COMPLETE | Import state management for mounted projects |
| DevPlayerShell | COMPLETE | Shell UI, command processing, test mode |
| GitManager | COMPLETE | Git operations: clone, fetch, checkout, switch_and_remount |
| SyncServer | COMPLETE | WebSocket server on port 6850 for live sync |

---

## Engine Modifications (7 Files, 183 Insertions)

| File | Modification |
|------|-------------|
| core/config/project_settings.h | Added `set_resource_path()` declaration |
| core/config/project_settings.cpp | Added `set_resource_path()` implementation |
| main/main.cpp | Added `--devplayer`, `--devplayer-mount`, `--devplayer-test` flags; shell mode entry point |
| modules/gdscript/gdscript_cache.h | Added `flush_project_caches()` declaration |
| modules/gdscript/gdscript_cache.cpp | Added `flush_project_caches()` -- fixes `cleared` flag + `dependencies` map bugs |
| platform/ios/main_ios.mm | Auto-inject `--devplayer` flag on iOS |
| platform/ios/detect.py | Documentation for tools=yes override |

---

## Test Infrastructure

- **8 test projects**: minimal_2d, class_name_collision_test_a, class_name_collision_test_b, autoload_reset_test, resource_cache_test_a, resource_cache_test_b, branch_switch_test, live_reload_test
- **23 shell tests** in run_domain_tests.sh
- **226 automated cycling checks** (all pass)
- **4 build/test scripts**
- **56 total test cycles**: 6 named test cycles + 50 stress cycles

---

## Validation Metrics (Final)

| Metric | Value |
|--------|-------|
| Mount success rate | 100% |
| Mount time (minimal_2d) | 0.005s |
| Build time (Linux full) | 4 minutes |
| Build time (Linux incremental) | 12 seconds |
| Unmount success rate | 100% |
| Relaunch success rate | 100% |
| Branch switch success rate | 100% |
| Live reload success rate | 100% |
| Stress test cycles | 56 (6 named + 50 stress) |
| Automated checks passed | 226/226 |
| Leaked references after unmount | 0 |
| Stale autoload count after unmount | 0 |
| Stale resource count after unmount | 0 |
| Stale script class count after unmount | 0 |

---

## Risks -- All Resolved

| Risk | Status | Resolution |
|------|--------|------------|
| A - iOS tools=yes viability | RESOLVED | iOS tools=yes confirmed viable via command-line override of get_flags() defaults |
| B - GDScript domain unload | RESOLVED | flush_project_caches() fixes two bugs in GDScriptCache::clear() (cleared flag + dependencies map) |
| C - Autoload teardown/rebuild | RESOLVED | AutoloadSessionManager handles ordered teardown with memdelete(), rebuild on next mount |
| D - ProjectSettings rebinding | RESOLVED | set_resource_path() + load_custom() bypasses OS::get_resource_dir() short-circuit |
| E - Resource cache invalidation | RESOLVED | ResourceCache eviction via set_path("") strategy -- targeted eviction without clearing shell resources |

---

## Decision Log

| Date | Decision | Rationale |
|------|----------|-----------|
| 2026-03-07 | Start with source exploration | Must understand codebase before modifying |
| 2026-03-08 | Build with tools=yes via command-line override | iOS platform get_flags() defaults to tools=no, but command-line tools=yes overrides it successfully |
| 2026-03-08 | Use devplayer_mode bool to guard editor code paths | All editor-specific code paths guarded by devplayer_mode; EditorNode bypassed, EditorSettings not initialized (intentional) |
| 2026-03-08 | Add set_resource_path() to ProjectSettings | ProjectSettings::setup() can't be used for project switching because OS::get_resource_dir() short-circuits. Minimal engine modification to add direct path binding |
| 2026-03-08 | Use load_custom() instead of setup() for project.godot | load_custom() loads settings from a specific file without the OS path override issues of setup() |
| 2026-03-08 | Use memdelete() instead of queue_free() for unmount | queue_free() uses deferred deletion which keeps script references alive past cache clearing, causing stale scripts on remount |
| 2026-03-08 | Implement flush_project_caches() instead of using GDScriptCache::clear() | clear() has two bugs: cleared flag prevents re-use, dependencies map never cleared. Custom method fixes both |
| 2026-03-08 | Use set_path("") for ResourceCache eviction | Targeted eviction avoids clearing shell resources while fully purging project resources |
| 2026-03-08 | Implement scan_and_register_global_classes() | Dynamic mount does not auto-register class_name identifiers; explicit scanning required |
| 2026-03-08 | WebSocket on port 6850 for SyncServer | Standard port for dev-sync protocol, avoids conflicts with common development ports |
| 2026-03-08 | Rust for desktop sync agent | Performance and reliability for file watching and WebSocket communication |
