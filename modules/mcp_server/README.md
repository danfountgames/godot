# Godot MCP Server

A built-in [Model Context Protocol](https://modelcontextprotocol.io) server for Godot Engine 4.6. It lets LLM coding agents run and debug games, inspect the live scene tree, evaluate expressions, take screenshots, and automate UI — all through a standard protocol that works with any MCP client.

The server starts automatically when the editor opens. No external process or plugin required.

## What It Can Do

**71 tools** across multiple categories:

| Category | Tools | Examples |
|----------|-------|---------|
| **Project & Editor** | 6 | Project overview, scan filesystem, reimport, get/resolve UIDs |
| **GDScript** | 2 | Check a single file for errors, check all project scripts |
| **Debug & Inspection** | 10 | Run/stop project, get scene tree, inspect node properties, read output/errors, session summary |
| **Automation** | 5 | Send input events, click UI controls, evaluate expressions at runtime, wait N frames, take screenshots |

**10 resources** via `godot://` URIs:

| URI | Content |
|-----|---------|
| `godot://project/info` | Project name, version, paths, engine version |
| `godot://project/settings` | Project configuration |
| `godot://project/file-tree` | Full directory listing |
| `godot://project/input-map` | Input action bindings |
| `godot://game/status` | Running state, uptime, frame count |
| `godot://game/scene-tree` | Live scene tree (requires running game) |
| `godot://game/output` | stdout/print output |
| `godot://game/errors` | Runtime errors and warnings |
| `godot://file/{path}` | Read any project file by path |
| `godot://game/node/{node_id}/properties` | All properties of a scene node |

**SSE streaming** for long-running operations with progress notifications and cancellation.

## Quick Start

### 1. Build Godot with MCP enabled

```bash
scons platform=linuxbsd target=editor module_mcp_server_enabled=yes -j$(nproc)
```

The module is enabled by default. To explicitly disable it:

```bash
scons module_mcp_server_enabled=no
```

### 2. Open your project

Launch the editor normally. The MCP server starts on `127.0.0.1:6009` and writes a discovery file with the auth token:

| OS | Discovery file |
|----|---------------|
| Linux | `~/.local/share/godot/mcp_server/discovery.json` |
| macOS | `~/Library/Application Support/Godot/mcp_server/discovery.json` |
| Windows | `%APPDATA%\Godot\mcp_server\discovery.json` |

The discovery file contains:
```json
{
  "endpoint": "http://127.0.0.1:6009/mcp",
  "token": "<bearer-token>",
  "pid": 12345,
  "godot_version": "4.6.stable"
}
```

### 3. Connect your LLM client

The endpoint and token from the discovery file are all you need. Every MCP client reads them the same way — the examples below show where to put them.

## LLM Client Setup

### Claude Code

Claude Code discovers MCP servers from its config file. Add the Godot server using a small script that reads the discovery file:

```bash
claude mcp add godot-engine \
  -s user \
  -e GODOT_MCP_DISCOVERY="$HOME/.local/share/godot/mcp_server/discovery.json" \
  -- node -e "
    const fs = require('fs');
    const disc = JSON.parse(fs.readFileSync(process.env.GODOT_MCP_DISCOVERY));
    process.env.MCP_ENDPOINT = disc.endpoint;
    process.env.MCP_TOKEN = disc.token;
    // Claude Code handles the rest via the MCP transport
  "
```

Or manually edit `~/.claude/claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "godot-engine": {
      "url": "http://127.0.0.1:6009/mcp",
      "headers": {
        "Authorization": "Bearer <token-from-discovery.json>"
      }
    }
  }
}
```

Replace `<token-from-discovery.json>` with the actual token value from the discovery file.

### Cursor

Add to `.cursor/mcp.json` in your project root (or `~/.cursor/mcp.json` globally):

```json
{
  "mcpServers": {
    "godot-engine": {
      "url": "http://127.0.0.1:6009/mcp",
      "headers": {
        "Authorization": "Bearer <token-from-discovery.json>"
      }
    }
  }
}
```

### Windsurf

Add to `~/.codeium/windsurf/mcp_config.json`:

```json
{
  "mcpServers": {
    "godot-engine": {
      "serverUrl": "http://127.0.0.1:6009/mcp",
      "headers": {
        "Authorization": "Bearer <token-from-discovery.json>"
      }
    }
  }
}
```

### Any MCP Client (Generic)

The server implements MCP 2025-06-18 with Streamable HTTP transport. Point any compatible client at:

- **Endpoint:** `http://127.0.0.1:6009/mcp`
- **Auth:** `Authorization: Bearer <token>` header (token from discovery file)
- **Content-Type:** `application/json`
- **Session:** Server returns `Mcp-Session-Id` header on initialize; include it on all subsequent requests

## Editor Settings

Configurable via **Editor > Editor Settings > Network > MCP Server**:

| Setting | Default | Description |
|---------|---------|-------------|
| `enabled` | `true` | Enable/disable the MCP server |
| `port` | `6009` | TCP port to listen on |
| `host` | `127.0.0.1` | Bind address (localhost only for security) |
| `use_thread` | `true` | Run server on a dedicated thread |
| `max_clients` | `8` | Maximum concurrent connections |
| `session_timeout_sec` | `300` | Idle session expiry (seconds) |

## Security

- **Localhost only** — binds to `127.0.0.1`, rejects non-local Host headers
- **Bearer token auth** — random 256-bit token generated per editor session
- **CORS** — rejects non-localhost origins, never sends `Access-Control-Allow-Origin: *`
- **Path sandboxing** — all file operations confined to `res://`, blocks `..` traversal, null bytes, URL-encoded bypasses, and `.godot/` internal directories
- **Expression denylist** — `runtime/evaluate` blocks `OS.execute`, `FileAccess.open`, `DirAccess.open`, etc.
- **DNS rebinding protection** — validates Host header against localhost allowlist

## Running the Tests

The integration tests use pytest against a running headless editor:

```bash
# Terminal 1: start the editor headless with the test project
./bin/godot.linuxbsd.editor.x86_64 --editor --headless \
  --path modules/mcp_server/tests/mcp_test_project/

# Terminal 2: run the tests
cd modules/mcp_server/tests
python3 -m pytest test_protocol.py test_tools.py test_resources.py \
  test_security.py test_edge_cases.py test_sse.py test_workflows.py -v
```

Current status: **141 passed, 0 failed, 1 skipped**.

## Protocol Details

- **MCP version:** 2025-06-18
- **Transport:** Streamable HTTP (JSON-RPC 2.0 over HTTP POST)
- **SSE:** Server-Sent Events for progress streaming on long-running tools
- **Session lifecycle:** `initialize` -> `notifications/initialized` -> work -> `DELETE /mcp`

For full protocol details, tool schemas, and implementation architecture, see [README_MCP.md](README_MCP.md).
