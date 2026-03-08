# Linux Interactive Baseline — Milestone H

## Status

**Linux Interactive Baseline Achieved.**

The shared-core DevPlayer architecture is meaningfully proven on Linux with an interactive shell flow, Git branch remount flow, and basic live sync flow demonstrated. iOS remains unbuilt and unproven.

Date frozen: 2026-03-08

## Build Artifacts

| Artifact | Value |
|----------|-------|
| Binary | `bin/godot.linuxbsd.editor.x86_64` |
| Size | 159,685,608 bytes (152 MB) |
| SHA-256 | `13811a2a25ec55c8d83b9feed1741ace3d23a91b4f51820815bdc2d6c6e14331` |
| Commit | `28f79b3aca` (Build interactive Linux DevPlayer shell with 11-step regression demo) |
| Branch | `GodotBeamDev` |
| Engine version | Godot 4.6.1.stable (custom_build) |

### Build Environment

| Component | Version |
|-----------|---------|
| OS | Linux Mint 22.1, kernel 6.8.0-100-generic x86_64 |
| GCC | 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1) |
| SCons | 4.5.2 |
| Python | 3.x (with `websockets` library for sync tests) |

### Build Command

```bash
scons platform=linuxbsd target=editor module_devplayer_enabled=yes -j$(nproc)
```

Build time: ~13 seconds (incremental), ~10 minutes (full clean build).

## Commit History (DevPlayer-related)

```
28f79b3aca Build interactive Linux DevPlayer shell with 11-step regression demo (Milestone F+G)
285043b551 Add iOS safety guards and document build blockers (Milestone E)
f66ae8b52c Add functional tests for GitManager and SyncServer (Milestone D)
2d5b0458f3 Fix autoload contamination, add real leak metrics, make ImportSessionManager functional (Milestone A+B+C)
e1eec815b7 Add DevPlayer module: dynamic project mounting engine for iOS (Initial)
```

## What Is Proven on Linux

### 1. Shell boot and project selection flow

The DevPlayer shell starts, creates its UI, discovers 9 test projects, and presents them for selection. No project is mounted at startup unless `--devplayer-mount` is specified.

**Command:**
```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer --headless --quit-after 2000
```

**Verified output includes:**
- `Shell UI created`
- `DevPlayer shell ready`
- `Found 9 projects`
- No `PROJECT LAUNCHED` (nothing auto-mounted)

### 2. Mount/unmount flow in a real interactive shell

Projects mount via `LaunchController::launch_project()`, which sequences through all 10 steps (settings capture, path remap, script scanning, resource tracking, import validation, autoload build, scene load). Unmount reverses all steps. The shell hides during project run and shows an overlay button.

**Verified by:** Steps 2-4 of regression demo (3 + 2 + 3 = 8 checks)

### 3. A/B project switching with domain isolation

Mounting project A, unmounting, mounting project B, unmounting, then remounting project A — all with zero residual references and no cross-contamination of `class_name` registrations, autoloads, or ProjectSettings properties.

**Verified by:** Step 7 of regression demo (5 checks), plus Test 6 in domain tests (56 mount/unmount cycles, 227 internal checks)

### 4. Autoload-related regression coverage

Autoloads are built from project settings on mount, destroyed on unmount. Autoload nodes are `memdelete`'d (not `queue_free`'d) to release script refs before cache flush. Project-added `autoload/*` properties are removed on unmount by setting to NIL, which triggers `ProjectSettings::remove_autoload()`.

**Verified by:** Steps 5-6 of regression demo (5 checks), plus Tests 5 and 7 in domain tests

### 5. Branch switch/remount flow

GitManager can list branches, get current branch, get commit info, and checkout a different branch. After checkout, remounting the same path loads different content from the new branch.

**Verified by:** Steps 8-9 of regression demo (6 checks), plus `test_git_manager.sh` (19 checks)

### 6. SyncServer basic write/reload path

SyncServer starts on a configurable port, accepts WebSocket connections, handles the full 5-message protocol (hello, manifest, write_small_file, reload_hint, sync_complete), writes files to disk, and rejects path traversal attacks.

**Verified by:** Step 10 of regression demo (7 checks), plus `test_sync_server.sh` (23 checks)

### 7. Shared core usable in a non-headless workflow

The same `modules/devplayer/` code that passes headless tests also runs in a windowed interactive shell with UI elements (ItemList, LineEdit, Button, RichTextLabel, etc.).

**Command for interactive (windowed) mode:**
```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer
```

## What Is NOT Proven

| Area | Status | Detail |
|------|--------|--------|
| iOS build | Unbuilt | No macOS/Xcode machine available. `scons platform=ios` fails on Linux. |
| iOS runtime behavior | Untested | Safety guards (`APPLE_EMBEDDED_ENABLED`) are present but never exercised on real iOS. |
| iOS import behavior | Untested | Import cache validation logic is platform-independent, but never tested on iOS filesystem. |
| Full editor-backed import session | Not implemented | `ImportSessionManager` is a validator, not an importer. Projects must be pre-imported in the full Godot editor. See `subsystems/import_session_manager.md`. |
| Real mobile UX constraints | Unknown | Shell UI is functional but developer-facing. Not designed for touch input, small screens, or iOS-specific UX patterns. |
| Live sync robustness beyond tested paths | Partially tested | The 5-message protocol is exercised. Concurrent clients, large file transfers, reconnection, and error recovery are not tested. |
| Overall "no leaks" | Not proven | Measured post-unmount counters (res:// resources, autoloads, ProjectSettings autoload entries) return to zero across 56 cycles. This does NOT prove zero leaks overall — native engine allocations, global state side effects, and ResourceCache entries outside `res://` are not measured. |

## Test Evidence

### Test Suite Summary

| Script | Path | Checks | Result |
|--------|------|--------|--------|
| Domain tests | `scripts/run_domain_tests.sh` | 36 | 36/36 pass |
| GitManager | `scripts/test_git_manager.sh` | 19 | 19/19 pass |
| SyncServer | `scripts/test_sync_server.sh` | 23 | 23/23 pass |
| Regression demo | `scripts/regression_demo.sh` | 38 | 38/38 pass |
| In-engine test | `--devplayer-test` (headless) | 227 internal | All pass |

### How to reproduce

**Build:**
```bash
scons platform=linuxbsd target=editor module_devplayer_enabled=yes -j$(nproc)
```

**36/36 domain tests** (mount/unmount lifecycle, class_name isolation, autoload reset, stress cycling, import validation):
```bash
bash scripts/run_domain_tests.sh
```
Expected last line: `RESULTS: 36 passed, 0 failed out of 36 checks`
Exit code: 0

**38/38 regression demo** (11-step end-to-end: shell, mount A/B, A→B→A, git branch switch, SyncServer sync, shutdown):
```bash
bash scripts/regression_demo.sh
```
Expected last line: `STATUS: ALL STEPS PASSED`
Exit code: 0

**19/19 GitManager tests** (GDScript API: branch listing, commit info, checkout):
```bash
bash scripts/test_git_manager.sh
```
Expected last line: `RESULTS: 19 passed, 0 failed out of 19 checks`
Exit code: 0

**23/23 SyncServer tests** (WebSocket protocol, path traversal, disk write):
```bash
bash scripts/test_sync_server.sh
```
Expected last line: `RESULTS: 23 passed, 0 failed out of 23 checks`
Exit code: 0
Prerequisite: `pip3 install websockets`

All four scripts exit 0 on success, non-zero on any failure.

### Raw regression demo output

See `GodotBeamDocs/test_results/regression_demo_38checks.txt` for the full captured output of the 38-check regression demo. See `GodotBeamDocs/REGRESSION_CHECKS.md` for what each of the 38 checks asserts.
