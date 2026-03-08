# Milestone 4 — Two-Project Switch

## Status: COMPLETE
**Started**: 2026-03-08
**Completed**: 2026-03-08

## Objective
Mount Project A then Project B with no stale scene/script/autoload behavior.

## Pass Criteria
- [x] class_name_collision_test passes (Enemy A=Aggressive, B=Passive)
- [x] autoload lifecycle tested
- [x] resource_cache_test passes
- [x] No stale registrations survive between mounts

## Tasks
- [x] Create class_name_collision_test_a (Project A with class_name Enemy = Aggressive behavior)
- [x] Create class_name_collision_test_b (Project B with class_name Enemy = Passive behavior)
- [x] Create resource_cache_test_a and resource_cache_test_b
- [x] Create autoload_reset_test
- [x] Test mount A -> unmount A -> mount B -> verify B's classes
- [x] Verify class_name isolation: Enemy resolves to Aggressive in A, Passive in B
- [x] Verify no stale registrations survive
- [x] Test autoload teardown and rebuild across project switches
- [x] Test 10x A/B toggle cycles

## Key Verification: class_name Isolation

The definitive test for domain isolation is the class_name collision test:

- **Project A** (`class_name_collision_test_a`): defines `class_name Enemy` with Aggressive behavior
- **Project B** (`class_name_collision_test_b`): defines `class_name Enemy` with Passive behavior

After mounting Project A, `Enemy` resolves to the Aggressive implementation. After unmounting A and mounting B, `Enemy` must resolve to the Passive implementation with zero bleed from A.

This works because the unmount sequence:
1. Calls `ScriptServer::global_classes_clear()` to remove A's class_name registrations
2. Calls `GDScriptCache::flush_project_caches()` to clear A's compiled scripts
3. Calls `scan_and_register_global_classes()` on mount to register B's class_name declarations

## Autoload Lifecycle

Autoloads are fully torn down during unmount and rebuilt during mount:
- `AutoloadSessionManager::destroy_autoloads()` uses `memdelete()` for immediate cleanup
- `AutoloadSessionManager::build_autoloads_from_project()` reads new project's autoload configuration
- No stale autoload nodes survive across project switches

## Test Projects Used
- `class_name_collision_test_a` -- Enemy class with Aggressive behavior
- `class_name_collision_test_b` -- Enemy class with Passive behavior
- `resource_cache_test_a` -- Resource caching verification (project A)
- `resource_cache_test_b` -- Resource caching verification (project B)
- `autoload_reset_test` -- Autoload teardown/rebuild verification

## Dependencies
- M3 must be complete (SATISFIED)
