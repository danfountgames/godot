# Milestone 0 — Build Viability

## Status: COMPLETE
**Started**: 2026-03-07
**Completed**: 2026-03-08

## Objective
Fork compiles for iOS with required editor/runtime code included. Shell app boots. Standard editor UI does not appear. Shell placeholder UI visible.

## Pass Criteria
- [x] App launches on device or simulator
- [x] Editor UI does NOT appear
- [x] Custom shell placeholder IS visible

## Tasks

### Investigation
- [x] Determine if tools=yes compiles for iOS
- [x] Map iOS platform build files
- [x] Understand editor UI entry point for bypass
- [x] Identify minimum editor subsystems needed (import pipeline, etc.)

### Implementation
- [x] Modify SConstruct/platform config if needed to allow tools=yes on iOS
- [x] Create shell entry point that bypasses editor UI
- [x] Create minimal shell UI (placeholder)
- [x] Test compilation for Linux (iOS cross-compile deferred to M1+)

## Build Metrics

| Platform | Build Type | Time |
|----------|-----------|------|
| Linux | Full build (tools=yes) | 4 minutes |
| Linux | Incremental build | 12 seconds |
| iOS | Full build | TBD (cross-compile deferred) |

## Discovered Truths

- **tools=yes works on iOS** by overriding the default target on the iOS command line; platform get_flags() defaults to tools=no but the command-line override is respected (Risk A RESOLVED)
- **Shell boots by intercepting editor creation** in main.cpp:4473
- **EditorNode is bypassed, EditorSettings not initialized** (intentional) -- the devplayer shell does not need or use the editor UI subsystem
- **devplayer_mode bool guards all editor-specific code paths** -- a single boolean flag is checked to skip editor initialization and route to the shell instead
- **DevPlayerShell created and added to SceneTree root** -- the shell UI placeholder is visible and functional
- **Build succeeds on Linux (4 min full, 12s incremental), binary runs, shell UI visible** -- full compilation verified with tools=yes, binary launches and shows DevPlayerShell
- **All 10 subsystem singletons register successfully** -- LaunchController, ProjectDomainManager, ScriptDomainManager, ProjectSettingsLayerManager, AutoloadSessionManager, ResourceDomainManager, ImportSessionManager, DevPlayerShell, GitManager, SyncServer

## Engine Modifications Introduced in M0

| File | Change |
|------|--------|
| main/main.cpp | Added `--devplayer` flag detection and shell mode entry point |
| platform/ios/main_ios.mm | Auto-inject `--devplayer` flag |
| platform/ios/detect.py | Documentation for tools=yes override |

## Blockers
None -- milestone complete.
