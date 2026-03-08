# SyncServer

## Purpose

SyncServer is a WebSocket-based sync server that enables an external dev-sync-agent to push file changes into the DevPlayer at runtime. It listens for TCP connections, upgrades them to WebSocket, and processes a JSON protocol for file synchronization and reload orchestration. This enables a workflow where a developer edits files on a desktop machine and changes are automatically synced to a DevPlayer running on a mobile device or remote host.

**Source files:** `modules/devplayer/sync_server.h`, `modules/devplayer/sync_server.cpp`

## Key APIs

| Method | Description |
|--------|-------------|
| `start_server(port) -> Error` | Starts the TCP/WebSocket server on the given port (default: 6850). Returns `ERR_ALREADY_IN_USE` if already running. |
| `stop_server()` | Closes all connected peers with code 1001 ("Server shutting down"), drops pending peers, and stops the TCP listener. |
| `is_running() -> bool` | Returns whether the server is currently listening. |
| `get_connected_client_count() -> int` | Returns the number of fully connected WebSocket clients. |
| `get_port() -> int` | Returns the server port number. |
| `poll()` | Must be called every frame. Accepts new connections, completes WebSocket handshakes, and processes incoming messages. |

### Signals

| Signal | Description |
|--------|-------------|
| `sync_file_received(path: String)` | Emitted when a file has been successfully written to the mounted project directory. |
| `reload_requested(tier: int, changed_paths: PackedStringArray)` | Emitted when a reload hint is received from the sync agent. |
| `client_connected(peer_id: int)` | Emitted when a WebSocket client completes the handshake. |
| `client_disconnected(peer_id: int)` | Emitted when a connected client disconnects. |

## Protocol Messages

The sync protocol uses JSON messages with a `type` field for dispatch. All messages are text (not binary).

### Client-to-Server Messages

| Type | Fields | Description |
|------|--------|-------------|
| `hello` | (none required) | Initial handshake from the sync agent. Server responds with `hello_ack`. |
| `manifest` | `files: { "rel/path": "sha256hex", ... }` | File hash manifest. Server compares against local state and responds with `manifest_ack` listing files that need syncing. Currently accepts all offered files. |
| `write_small_file` | `path: String`, `content: String` (base64) | Writes a single file to the mounted project directory. Content is base64-encoded. Server decodes and writes to disk. Responds with `write_ack`. |
| `reload_hint` | `tier: int`, `changed_paths: Array<String>` | Tells the server what kind of reload is needed after file sync. See Reload Tiers below. Server responds with `reload_ack`. |
| `sync_complete` | `files_synced: int` | Signals that a batch of file writes is complete. Server responds with `sync_complete_ack`. |

### Server-to-Client Responses

| Type | Key Fields | Description |
|------|------------|-------------|
| `hello_ack` | `engine`, `device`, `project_mounted`, `project_root` | Handshake response with device info and mount state. |
| `manifest_ack` | `status`, `needs_update: PackedStringArray` | Lists which files the server wants to receive. |
| `write_ack` | `path`, `status`, `bytes` | Confirms a file was written successfully. |
| `reload_ack` | `tier`, `status` | Confirms a reload hint was processed. |
| `sync_complete_ack` | `status` | Confirms the sync batch is complete. |

## Reload Tiers

The sync protocol defines three reload tiers that determine how the DevPlayer responds to file changes:

| Tier | Enum | Trigger | Action |
|------|------|---------|--------|
| 1 | `RELOAD_TIER_CONTENT` | Textures, sounds, or other non-script assets changed | Signal emitted; shell handles hot-reload. No automatic action by SyncServer. |
| 2 | `RELOAD_TIER_SESSION` | GDScript files changed | Calls `LaunchController::relaunch()` to stop and remount the project, picking up the new scripts. |
| 3 | `RELOAD_TIER_FULL` | `project.godot` changed | Logs a warning that a full engine restart is required. Cannot be automated from within the running process. |

## Architecture

### Connection Lifecycle

1. **TCP Accept** -- `_poll_new_connections()` accepts incoming TCP connections from the `TCPServer` and wraps each in a `WebSocketPeer`. The connection is placed in `pending_peers` with a timestamp.

2. **Handshake** -- `_poll_pending_peers()` polls each pending peer. When the WebSocket handshake completes (`STATE_OPEN`), the peer is promoted to `connected_peers` and the `client_connected` signal is emitted. Peers that exceed `HANDSHAKE_TIMEOUT_MSEC` (5 seconds) are dropped.

3. **Message Processing** -- `_poll_connected_peers()` polls each connected peer, reads available text packets, and dispatches them to `_process_message()`, which parses the JSON and routes to the appropriate handler based on the `type` field.

4. **Disconnection** -- If a connected peer's state is no longer `STATE_OPEN`, it is removed from `connected_peers` and the `client_disconnected` signal is emitted.

### File Writing

The `_handle_write_small_file()` method decodes base64 content using `CryptoCore::b64_decode()`, then calls `_write_file_to_project()` which:
1. Gets the project root from `ProjectDomainManager`.
2. Ensures the parent directory exists via `DirAccess::make_dir_recursive_absolute()`.
3. Opens the file for writing and stores the decoded bytes.

### Manifest Handling

The current manifest implementation is simplistic: it requests ALL files offered by the sync agent regardless of local state. The comment in the code notes that a proper implementation would compare SHA-256 hashes, but the current approach treats the sync agent as authoritative.

## Integration

- **LaunchController** is called for tier-2 reloads (`relaunch()`) to stop and remount the project after script changes.
- **ProjectDomainManager** is queried via `_get_project_root()` to determine where to write files and to report mount state in the `hello_ack`.
- Depends on the **WebSocket module** (`modules/websocket/websocket_peer.h`) for the WebSocket protocol implementation.
- Uses **CryptoCore** for base64 decoding of file content.

## Critical Notes

- `poll()` must be called every frame for the server to function. If it is not called, connections will not be accepted, handshakes will not complete, and messages will not be processed.
- The manifest currently accepts all offered files without hash comparison. This means every sync cycle will transfer all files, not just changed ones. This is a known simplification.
- Tier-3 (full restart) reloads cannot be automated. The server logs a warning but does not restart the engine. The user must restart manually.
- The server listens on all interfaces (`IPAddress("*")`), which means it is accessible from any network interface. There is no authentication. In production, consider binding to localhost or adding authentication.
- The `HANDSHAKE_TIMEOUT_MSEC` is set to 5 seconds. Slow network connections may fail to complete the WebSocket handshake within this window.
- File writes go directly to disk without any sandboxing or path validation beyond directory creation. A malicious sync agent could write files anywhere the process has write access by using `../` in relative paths.
