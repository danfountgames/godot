# Milestone 1 — Minimal Project Mount

## Status: COMPLETE
**Started**: 2026-03-08
**Completed**: 2026-03-08

## Objective
Shell can select one local project. Project settings load. Imports run on iOS. Target scene launches.

## Pass Criteria
- [x] minimal_2d test project runs on device
- [x] 13/13 automated tests pass, mount time 0.005s for minimal_2d

## Tasks
- [x] Implement ProjectDomainManager.mount_project()
- [x] Implement ProjectSettingsLayerManager.load_project_settings()
- [x] Implement ScriptDomainManager.initialize_for_project()
- [x] Implement AutoloadSessionManager.build_autoloads_from_project()
- [x] Implement LaunchController.launch_project()
- [x] Implement scan_and_register_global_classes() for class_name registration on dynamic mount
- [x] Create minimal_2d test project
- [x] Test end-to-end mount and scene launch
- [x] Build automated test suite (13 checks across 4 categories)

## Mount Performance
- Mount time: **0.005s** for minimal_2d project
- 13/13 automated tests pass

## Critical Discovery: set_resource_path()

**Problem**: `ProjectSettings::setup()` cannot be used for project switching because `OS::get_resource_dir()` short-circuits the resource path. When `setup()` is called, it queries `OS::get_resource_dir()` which returns the original binary location, completely ignoring the new project path argument.

**Solution**: Added `set_resource_path()` to `ProjectSettings` -- a minimal engine modification that directly sets the resource path without going through the OS layer.

**Actual working mount sequence discovered**:
1. `ProjectSettings::set_resource_path(project_dir)` -- binds res:// to mounted project directory
2. `ProjectSettings::load_custom(project_dir + "/project.godot")` -- loads project settings from mounted project
3. `scan_and_register_global_classes()` -- scans .gd files for class_name declarations and registers them with ScriptServer
4. res:// remapping now points to mounted project
5. Scene loading and GDScript execution work end-to-end

## Critical Discovery: class_name Registration on Dynamic Mount

**Problem**: When dynamically mounting a project, `class_name` identifiers declared in GDScript files are not automatically registered with ScriptServer. This means types like `class_name Enemy` would not be resolvable in the mounted project context.

**Solution**: Implemented `scan_and_register_global_classes()` which scans all `.gd` files in the mounted project, parses class_name declarations, and registers them with `ScriptServer`. This was essential for M4's class_name isolation testing.

## What Works End-to-End
- ProjectSettings load via `set_resource_path()` + `load_custom()`
- `res://` remapping to mounted project
- Scene loading from mounted project
- GDScript execution within mounted project context
- class_name scanning and registration

## Engine Modifications Introduced in M1

| File | Change |
|------|--------|
| core/config/project_settings.h | Added `set_resource_path()` declaration |
| core/config/project_settings.cpp | Added `set_resource_path()` implementation |
| main/main.cpp | Added `--devplayer-mount` and `--devplayer-test` flags |

## Dependencies
- M0 must be complete (SATISFIED)
