# GodotBeam iOS Build Guide

## Build Requirements

iOS builds require **macOS with Xcode** installed. Cross-compilation from Linux is only possible with `OSXCROSS_IOS` (not tested).

### Prerequisites

- macOS (any recent version)
- Xcode with iOS SDK (14.0+)
- Python 3 and SCons
- The Godot build system's standard dependencies

### Build Command

```bash
scons platform=ios target=editor arch=arm64 module_devplayer_enabled=yes -j$(nproc)
```

### Why `target=editor`?

DevPlayer requires `TOOLS_ENABLED` to access `EditorFileSystem` APIs, the GDScript cache flush methods, and the `--devplayer` CLI flag parsing (guarded by `#ifdef TOOLS_ENABLED` in `main/main.cpp`). The iOS platform's `detect.py` does not list "editor" in its `supported` list, but this only blocks "library" builds, not executable builds. Passing `target=editor` on the command line overrides the default.

## Current Status (as of 2026-03-08)

**Hypothesis: the module should compile on iOS.** All DevPlayer code uses platform-abstracted Godot APIs (`DirAccess`, `FileAccess`, `OS`, `TCPServer`, `WebSocketPeer`, etc.), so no platform-specific compile errors are predicted. This is an untested hypothesis.

**The module has NOT been built or tested on iOS.** The development machine is Linux. The entire analysis below is based on code review, not empirical testing. Until the module is actually compiled, linked, and run on an iOS device (or simulator), iOS support is unproven.

## iOS Build Attempt: Blocker Analysis

### Host Machine

```
Linux Mint 22.1 (x86_64)
No Xcode, no OSXCROSS_IOS
scons platform=ios → "Invalid target platform"
```

### Predicted Blockers (untested — based on code review only)

#### RUNTIME: Predicted to compile but fail at runtime without the applied fixes

**R1. GitManager — `fork()` is prohibited on iOS (FIXED)**

`OS::execute("git", ...)` uses `fork()` / `popen()` internally on Unix platforms. iOS prohibits `fork()` — calling it will crash the process or return EPERM. There is also no `git` binary on iOS.

**Fix applied:** `_run_git()` now has an `#ifdef APPLE_EMBEDDED_ENABLED` guard that returns `ERR_UNAVAILABLE` immediately, preventing the crash.

**R2. GitManager default `repos_base_path` is read-only on iOS (FIXED)**

The default path was `get_executable_path().get_base_dir().path_join("repos")`, which is inside the code-signed `.app` bundle (read-only).

**Fix applied:** On `APPLE_EMBEDDED_ENABLED`, defaults to `OS::get_user_data_dir().path_join("repos")` (Documents directory).

**R3. SyncServer binds to `127.0.0.1` — limited connectivity**

The SyncServer binds to localhost only. On a real iOS device, the host Mac cannot connect directly. This is intentional for security — the design assumes USB tunnel (`iproxy`) forwarding. But it means the SyncServer won't work over WiFi without changing the bind address.

**Workaround:** Use `iproxy 6850 6850` to forward the port over USB, or add a runtime option to bind to `0.0.0.0` when explicitly requested.

**R4. DevPlayerShell default project path is invalid on iOS (FIXED)**

The default path pointed to `test_projects/minimal_2d` relative to the engine root, which is inside the read-only `.app` bundle.

**Fix applied:** On `APPLE_EMBEDDED_ENABLED`, defaults to `get_user_data_dir().path_join("projects/minimal_2d")`.

#### LIMITATION: Will work but with reduced functionality

**L1. GitManager is entirely non-functional on iOS**

All git operations (clone, fetch, checkout, branch listing, switch_and_remount) return `ERR_UNAVAILABLE`. Projects must be delivered via SyncServer or pre-bundled in the app.

**L2. Filesystem writes are restricted to the app sandbox**

`SyncServer::_write_file_to_project()` and `ImportSessionManager::_scan_dir_recursive()` use `DirAccess`/`FileAccess` which work within the sandbox (Documents, Library, tmp) but not for arbitrary filesystem paths.

**L3. Automated test mode requires bundled test projects**

`--devplayer-test` expects `test_projects/` at the engine root path. On iOS, these would need to be bundled into the `.app` as resources at build time.

**L4. SyncServer port may require iOS network entitlements**

Listening on TCP port 6850 may require `com.apple.developer.networking.multicast` entitlement and `Info.plist` local network access declaration.

## Xcode Project Setup

After building the static library with SCons, you need an Xcode project to:
1. Link the Godot binary into an iOS app bundle
2. Set the `Info.plist` with required entitlements
3. Bundle test projects as app resources (for `--devplayer-test`)
4. Configure code signing
5. Set the app to boot with `--devplayer` flag (done automatically by `main_ios.mm`)

The Godot export template system normally handles this via `EditorExportPlatformIOS`, but since DevPlayer runs as the editor itself (not as an export template), a manual Xcode project may be needed.

## Next Steps

1. **Get access to a macOS machine** with Xcode installed
2. **Run the actual build:** `scons platform=ios target=editor arch=arm64 module_devplayer_enabled=yes -j$(nproc)`
3. **Capture any compiler/linker errors** (none predicted, but iOS SDK headers may have surprises)
4. **Create an Xcode project** to package the binary into an .ipa
5. **Test on iOS Simulator** first (`scons platform=ios target=editor arch=x86_64 simulator=yes`)
6. **Test on real device** with USB-connected iproxy for SyncServer connectivity
7. **Exercise the SyncServer protocol** from the host Mac to verify file sync works
