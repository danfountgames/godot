# iOS Handoff Checklist

This document is for a developer with access to a macOS machine and Xcode who will attempt the first real iOS build and test of GodotBeam DevPlayer.

## Start Here

1. Read `WALKTHROUGH.md` first — it covers build, shell, mount, git, sync on Linux
2. Build on Linux and run `bash scripts/regression_demo.sh` to confirm 38/38 baseline
3. Then attempt the iOS build on macOS using the commands in this document
4. Follow the 5-phase test plan at the bottom, reporting results for each phase

The Linux build is the known-good reference. If something fails on iOS, compare against Linux behavior to isolate platform-specific issues.

## Prerequisites

- macOS with Xcode installed (iOS SDK 14.0+)
- Python 3 and SCons (`pip3 install scons`)
- The GodotBeam repository checked out on the `GodotBeamDev` branch
- Commit `1e25311c5e` or later (Milestone H)

## Engine Patches (7 files outside modules/devplayer/)

These are the exact modifications to core Godot engine files. All are guarded with `#ifdef MODULE_DEVPLAYER_ENABLED` (except the ProjectSettings change which is always compiled).

### 1. `core/config/project_settings.h` — 1 line added

```cpp
void set_resource_path(const String &p_path);
```

Added public setter for `resource_path`. Needed for `ProjectSettingsLayerManager` to dynamically remap `res://`.

### 2. `core/config/project_settings.cpp` — 5 lines added

```cpp
void ProjectSettings::set_resource_path(const String &p_path) {
    resource_path = p_path;
    if (!resource_path.is_empty() && resource_path[resource_path.length() - 1] == '/') {
        resource_path = resource_path.substr(0, resource_path.length() - 1);
    }
}
```

### 3. `modules/gdscript/gdscript_cache.h` — 5 lines added

```cpp
static void flush_project_caches();
```

Custom cache flush method that clears all caches including `dependencies` map (which `clear()` misses) and resets the `cleared` flag (which `clear()` sets permanently).

### 4. `modules/gdscript/gdscript_cache.cpp` — 52 lines added

Implementation of `flush_project_caches()`. Clears parser refs, abandoned parsers, shallow/full/static caches, and dependency map. Resets `cleared = false`.

### 5. `main/main.cpp` — 98 lines added

- 4 static variables: `devplayer_mode`, `devplayer_test_mode`, `devplayer_mount_path`, `devplayer_mount_scene`
- CLI flag parsing for `--devplayer`, `--devplayer-mount`, `--devplayer-scene`, `--devplayer-test`
- Conditional block to create `DevPlayerShell` instead of `EditorNode` when `devplayer_mode` is true
- 3 editor flow guards: `if (editor && !devplayer_mode)` to skip editor embed subwindows, editor scene loading, and SSL certificate loading

### 6. `platform/ios/detect.py` — 8 lines added (comment only)

Documentation explaining that `target=editor` can be passed on the command line to override iOS's default of not supporting editor builds.

### 7. `platform/ios/main_ios.mm` — 18 lines added

Auto-injects `--devplayer` into argv if not already present, so iOS builds always boot into the DevPlayer shell.

## iOS Build Command

```bash
# ARM64 (real device)
scons platform=ios target=editor arch=arm64 module_devplayer_enabled=yes -j$(nproc)

# x86_64 (simulator on Intel Mac)
scons platform=ios target=editor arch=x86_64 simulator=yes module_devplayer_enabled=yes -j$(nproc)

# arm64 (simulator on Apple Silicon Mac)
scons platform=ios target=editor arch=arm64 simulator=yes module_devplayer_enabled=yes -j$(nproc)
```

## Linux Build Command (for comparison)

```bash
scons platform=linuxbsd target=editor module_devplayer_enabled=yes -j$(nproc)
```

This is the command that produces the known-good Linux binary.

## Expected Outcomes

### Category 1: Fixes Already Applied (in code, tested on Linux only)

These problems were identified by code review and fixed with compile guards. The fixes compile and work on Linux. They have **not** been tested on actual iOS.

| ID | Problem | Fix in code | File | Tested on iOS? |
|----|---------|------------|------|---------------|
| R1 | `GitManager::_run_git()` calls `fork()` which crashes on iOS | `#ifdef APPLE_EMBEDDED_ENABLED` returns `ERR_UNAVAILABLE` immediately | `git_manager.cpp` | **No** |
| R2 | GitManager default `repos_base_path` is inside read-only `.app` bundle | Defaults to `OS::get_user_data_dir().path_join("repos")` on `APPLE_EMBEDDED_ENABLED` | `git_manager.cpp` | **No** |
| R4 | DevPlayerShell default project path is inside read-only `.app` bundle | Defaults to `get_user_data_dir().path_join("projects/minimal_2d")` on `APPLE_EMBEDDED_ENABLED` | `dev_player_shell.cpp` | **No** |

### Category 2: Hypotheses (untested predictions based on code review)

| Hypothesis | Basis | Risk |
|-----------|-------|------|
| The module will compile on iOS | All DevPlayer code uses Godot platform abstractions (`DirAccess`, `FileAccess`, `OS`, `TCPServer`, `WebSocketPeer`), no raw platform headers | Low — but iOS SDK headers may have surprises |
| `EditorSettings::create()` works on iOS | It's a pure C++ singleton with no platform-specific code | Low |
| UI widgets (Label, LineEdit, Button, ItemList) render on iOS | These are Godot's standard scene/gui widgets, used by the editor on macOS | Low — but touch vs mouse input may need work |
| `WebSocketPeer`/`TCPServer` work on iOS | Godot's networking uses platform-abstracted sockets | Medium — may need network entitlement in Info.plist |
| `ResourceLoader::load()` works within the iOS sandbox | File paths within the app bundle and Documents directory should be accessible via `DirAccess`/`FileAccess` | Low |

### Category 3: Untested Assumptions (things that might not work at all)

| Assumption | Why it might fail | Impact |
|-----------|------------------|--------|
| SyncServer is reachable from host Mac | Binds to `127.0.0.1` — unreachable over WiFi. USB requires `iproxy` forwarding. | Workaround: `iproxy 6850 6850`. May need `0.0.0.0` bind option for WiFi. |
| TCP server doesn't need entitlement | iOS may require `com.apple.developer.networking.multicast` or local network permission | May need Info.plist entries |
| Touch input works with the shell UI | Shell UI was built for mouse/keyboard. No touch-specific adaptations. | Buttons should work via Godot's touch→mouse emulation. ItemList scrolling may need work. |
| `memdelete()` in unmount doesn't trigger iOS memory warnings | Rapid allocation/deallocation of scene trees may behave differently under iOS memory pressure | May need memory profiling on device |
| Bundled test projects load from the app bundle | `res://` remapping to an app bundle resource path hasn't been tested | May need a different path strategy for bundled content |

### Compilation

**Hypothesis:** The build should succeed (see Category 2 above).

**If it fails:** Capture the full error output. Most likely causes:
- iOS SDK header differences (unlikely — we use Godot abstractions, not platform headers)
- Linker errors from WebSocket dependencies (check that the `websocket` module is enabled)
- Missing `TOOLS_ENABLED` guards (check that `target=editor` is specified)

## Platform-Specific Files Touched

| File | iOS-specific change |
|------|-------------------|
| `platform/ios/main_ios.mm` | Auto-inject `--devplayer` flag |
| `platform/ios/detect.py` | Comment about `target=editor` override |
| `modules/devplayer/git_manager.cpp` | `#ifdef APPLE_EMBEDDED_ENABLED` guard in `_run_git()` and constructor |
| `modules/devplayer/dev_player_shell.cpp` | `#ifdef APPLE_EMBEDDED_ENABLED` for default project path |

## What to Test First on iOS

### Phase 1: Does it boot?

1. Build the binary (or static library)
2. Create an Xcode project, link the binary, set up code signing
3. Run on simulator
4. **Expected:** The DevPlayer shell window appears with "No project mounted"
5. **Check:** Does `EditorSettings::create()` work? Do LineEdit/Label widgets render?

### Phase 2: Can it mount a bundled project?

1. Bundle `test_projects/minimal_2d/` as app resources
2. Mount it via the shell UI or by setting a launch argument `--devplayer-mount`
3. **Expected:** "Minimal 2D project loaded" appears in output
4. **Check:** Does `res://` remap work within the app sandbox? Does scene loading work?

### Phase 3: Does SyncServer work over USB?

1. Mount a project on the device
2. Start SyncServer in the shell UI (port 6850)
3. On the Mac, run `iproxy 6850 6850` (from `usbmuxd` / `libimobiledevice`)
4. From the Mac, run the Python sync client against `ws://127.0.0.1:6850`
5. **Expected:** File written to the device's app sandbox, reload_hint acknowledged

### Phase 4: Does mount/unmount cycle cleanly?

1. Mount project A, unmount, mount project B, unmount, remount project A
2. **Expected:** No crashes, no stale state, measured counters return to zero
3. **Check:** Does `memdelete` work correctly on iOS? Any memory warnings?

### Phase 5: Full regression

1. Adapt `scripts/regression_demo.sh` for iOS (replace headless engine invocations with on-device test mode)
2. Run `--devplayer-test` on the device
3. **Expected:** All automated test cycles pass (stress test may need longer timeouts on device)

## Xcode Project Setup Notes

The standard Godot iOS export uses `EditorExportPlatformIOS` to generate an Xcode project. Since DevPlayer runs as the editor itself (not as an export template), you may need a manual Xcode project:

1. Create a new iOS App project in Xcode
2. Replace the default main with the Godot `main_ios.mm` entry point
3. Link against the SCons-built static library (`libgodot.ios.editor.arm64.a` or similar)
4. Add all required frameworks (listed in `platform/ios/detect.py` under `configure()`)
5. Set `Info.plist` entries for local network access if using SyncServer
6. Bundle test projects as app resources (copy `test_projects/` into the app bundle)
7. The `--devplayer` flag is auto-injected by `main_ios.mm`, no manual argument needed

## File Inventory

All DevPlayer module files:

```
modules/devplayer/
├── autoload_session_manager.cpp
├── autoload_session_manager.h
├── config.py
├── dev_player_shell.cpp
├── dev_player_shell.h
├── devplayer_debug.cpp
├── devplayer_debug.h
├── git_manager.cpp
├── git_manager.h
├── import_session_manager.cpp
├── import_session_manager.h
├── launch_controller.cpp
├── launch_controller.h
├── project_domain_manager.cpp
├── project_domain_manager.h
├── project_settings_layer_manager.cpp
├── project_settings_layer_manager.h
├── register_types.cpp
├── register_types.h
├── resource_domain_manager.cpp
├── resource_domain_manager.h
├── SCsub
├── script_domain_manager.cpp
├── script_domain_manager.h
├── sync_server.cpp
└── sync_server.h
```

All test projects:

```
test_projects/
├── autoload_reset_test/
├── branch_switch_test/
├── class_name_collision_test_a/
├── class_name_collision_test_b/
├── import_test/
├── live_reload_test/
├── minimal_2d/
├── resource_cache_test_a/
└── resource_cache_test_b/
```

All test scripts:

```
scripts/
├── regression_demo.sh     (38 checks — 11-step end-to-end)
├── run_domain_tests.sh    (36 checks — core lifecycle)
├── test_git_manager.sh    (19 checks — Git API)
└── test_sync_server.sh    (23 checks — WebSocket protocol)
```
