# Milestone 6 — Live Sync

## Status: COMPLETE
**Started**: 2026-03-08
**Completed**: 2026-03-08

## Objective
Live host discovery or manual IP. File updates arrive on device. Reload tiers triggered.

## Pass Criteria
- [x] live_reload_test passes for .gd and .tscn

## Tasks
- [x] Implement SyncServer (WebSocket transport on device, port 6850)
- [x] Implement sync protocol (hello, manifest, write_small_file, reload_hint)
- [x] Build desktop sync agent in Rust (dev-sync-agent/)
- [x] Implement reload tier classification
- [x] Implement file sync and reload
- [x] Create live_reload_test project
- [x] Test .gd file sync and reload
- [x] Test .tscn file sync and reload

## SyncServer Architecture

### Device Side: SyncServer (C++ / Godot Module)
- WebSocket server running on **port 6850**
- Part of the devplayer module (modules/devplayer/)
- Receives file updates from the desktop sync agent
- Classifies changes and triggers appropriate reload tier

### Desktop Side: dev-sync-agent (Rust)
- Located in `dev-sync-agent/` directory
- Watches project files for changes using filesystem events
- Connects to the device's SyncServer via WebSocket
- Sends file diffs/updates to the device

### Sync Protocol
| Message | Direction | Purpose |
|---------|-----------|---------|
| `hello` | Agent -> Device | Establish connection, exchange version info |
| `manifest` | Agent -> Device | Send current file manifest for delta detection |
| `write_small_file` | Agent -> Device | Push a changed file to the device |
| `reload_hint` | Agent -> Device | Signal that a reload should be triggered |

### Reload Tiers
- **Tier 1**: Lightweight content reload (e.g., .gd script hot-reload)
- **Tier 2**: Project session relaunch (unmount + remount, uses M3/M4 pipeline)
- **Tier 3**: Runtime-unsafe changes (requires full restart)

## Test Projects Used
- `live_reload_test` -- Project with scripts and scenes for sync verification

## Dependencies
- M4 must be complete for Tier 2 reload (SATISFIED)
