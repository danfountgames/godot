# Plan: Per-Agent Runtime Tool Toggle

## Problem

The embedded Claude Code terminal launches agents that have full access to all
96 MCP tools, including runtime tools that start/stop/control the running game.
There's no way to restrict an agent from touching the game — if you want a
"code-only" agent that edits scripts but can't launch or interact with the
running game, you're out of luck.

## Goal

A toggle on each agent tab in the editor that controls whether that agent's MCP
session is allowed to call runtime tools. When off, the agent can still read
files, edit scenes, analyze code — but cannot start the game, send input, read
the scene tree at runtime, evaluate expressions, or use time control.

## Architecture: How Agents Connect Today

```
AgentPanel (one per tab)
  └─ TerminalWidget
      └─ PTYManager → forks `claude` process
          │
          │  HTTP to localhost:63315
          │  Mcp-Session-Id: <64-byte hex>
          ▼
MCPProtocol (singleton)
  ├─ sessions: HashMap<String, MCPSessionState>
  │     key = session_id (64-byte CSPRNG hex)
  │     value = { initialized, last_activity, notification_queue }
  │
  └─ tool_registry: MCPToolRegistry
        └─ call_tool(params) → handler(args) → result
```

Each Claude Code process makes its own HTTP connection and gets its own
`session_id` during MCP `initialize`. Subagents (planner, builder, etc.) are
internal to Claude Code — the MCP server sees them as one session.

**The session_id is the identifier we key permissions on.**

## Tool Categories to Gate

Tools that require a running game and should be togglable:

| Namespace | Count | Examples |
|-----------|-------|---------|
| `runtime/*` | ~20 | `run_project`, `stop_project`, `evaluate`, `get_screenshot`, `get_node_properties`, `browse_scene_tree` |
| `automation/*` | ~4 | `interact_with_ui`, `click_position` |
| `runtime/input/*` | ~5 | `send_key`, `send_joypad`, `type_text`, `send_input_sequence` |
| `runtime/ui/*` | ~12 | `get_control_info`, `set_text`, `select_option`, `focus` |
| `runtime/time/*` | ~6 | `suspend`, `resume`, `frame_step`, `set_time_scale` |
| `runtime/signals/*` | ~3 | `get_node_signals`, `emit_signal` |
| `debug/*` (introspection) | 5 | `describe_class`, `browse_tree`, `get`, `set`, `call` |
| `debug/*` (debugger) | ~5 | `get_break_state`, `step`, `continue`, `get_breakpoints` |

Tools that remain always available (editor-only, no running game needed):

| Namespace | Examples |
|-----------|---------|
| `editor/*` | file read/write, scene editing, script validation |
| `scene/*` | scene tree manipulation (editor-time) |
| `doc/*` | documentation lookup |
| `analysis/*` | static analysis, dead code, complexity |
| `testing/*` | script checking (not runtime tests) |
| `memory/*` | memory snapshots |
| `shader/*` | shader tools |
| `help` | tool listing |
| `export/*` | build/export |

## Design

### 1. Session permissions in MCPSessionState

```cpp
// mcp_protocol.h
struct MCPSessionState {
    String session_id;
    bool init_response_sent = false;
    bool initialized = false;
    uint64_t last_activity = 0;
    Vector<String> notification_queue;

    // NEW: per-session tool permissions
    bool runtime_tools_enabled = true;  // default: full access
};
```

### 2. Blocked namespace list

Rather than tagging each tool individually, check the tool name prefix at
dispatch time. Simple, no changes to tool registration needed.

```cpp
// mcp_tool_registry.h (or mcp_protocol.cpp, inline)
static bool is_runtime_tool(const String &p_name) {
    return p_name.begins_with("runtime/")
        || p_name.begins_with("automation/")
        || p_name.begins_with("debug/");
    // debug/ tools need a running game for introspection.
    // editor/ debug tools (breakpoints) could go either way.
}
```

Note: `debug/describe_class` works without a running game (reads parser data).
We may want a finer split: `debug/describe_class` always allowed,
`debug/browse_tree`, `debug/get`, `debug/set`, `debug/call` require runtime.
Handle this with a small allowlist inside `is_runtime_tool()`.

### 3. Interception point: process_request()

Two paths need gating (both in `mcp_protocol.cpp`):

**Path A — SSE dispatch (line ~746):**
```cpp
if (method == "tools/call" && !is_notification) {
    String tool_name = params.get("name", "");

    // NEW: check runtime tool permission for this session
    if (is_runtime_tool(tool_name) && !session_state.runtime_tools_enabled) {
        session->queue_response(MCP_HTTP_200,
            make_result_body(make_tool_error(
                "Runtime tools are disabled for this agent session. "
                "Enable the runtime toggle in the agent panel to use " + tool_name),
                request_id),
            origin_header);
        session->reset_request();
        return;
    }

    // ... existing SSE negotiation ...
}
```

**Path B — standard dispatch (line ~787):**
Same check before `process_action(json_request)`.

Actually, cleaner: gate in `MCPToolRegistry::call_tool()` by passing a
permissions context, OR gate in the `_handle_tools_call` wrapper which has
access to the session. The wrapper is better — keeps the registry stateless.

**Recommended: gate in `_handle_tools_call()`**

`_handle_tools_call()` is the JSON-RPC method handler called by
`process_action()`. Problem: it doesn't currently know which session is
calling. We need to thread the session_id through.

**Alternative: gate in `process_request()` before either dispatch path.**

This is simpler — `process_request()` already has the session_id and the
parsed tool name. One check, covers both SSE and non-SSE paths:

```cpp
// In process_request(), after JSON parsing, before Step 9b:

if (method == "tools/call") {
    Dictionary params = json_request.get("params", Dictionary());
    String tool_name = params.get("name", "");

    if (is_runtime_tool(tool_name)) {
        auto it = sessions.find(session_id_header);
        if (it != sessions.end() && !it->value.runtime_tools_enabled) {
            String body = make_error_body(
                JSONRPC::INVALID_REQUEST,
                vformat("Tool '%s' is blocked: runtime tools are disabled "
                        "for this session. Enable the runtime toggle in "
                        "the agent panel.", tool_name),
                request_id);
            session->queue_response(MCP_HTTP_200, body, origin_header);
            session->reset_request();
            return;
        }
    }
}
```

### 4. AgentPanel ↔ MCPProtocol link

AgentPanel needs to know its agent's session_id so it can toggle permissions.
Two approaches:

**Option A: AgentPanel discovers the session_id**

When Claude Code connects and calls `initialize`, the MCP server generates a
session_id. AgentPanel doesn't currently know this ID. We'd need to:
1. Have MCPProtocol emit a signal when a new session initializes
2. AgentPanel matches by timing/port (fragile) or by passing a token

**Option B: AgentPanel pre-assigns a session token**

AgentPanel generates a unique token, passes it to Claude Code as an
environment variable (e.g., `MCP_AGENT_TOKEN=abc123`). Claude Code includes
it in the `initialize` request's `clientInfo` or as a custom header. MCPProtocol
maps token → session_id on initialize.

**Option C (simplest): MCPProtocol tracks most-recent session**

Since there's currently only one agent tab, the most recently initialized
session is "the agent." AgentPanel toggles permissions on that session.

For multi-tab future: each AgentPanel gets a UUID, passes it as
`MCP_AGENT_ID` env var to the claude process, and Claude sends it in the
`clientInfo.name` field during initialize. MCPProtocol stores the mapping.

**Recommended: Option B (pre-assigned token) — future-proof for multi-tab.**

```cpp
// agent_panel.cpp — launch()
String agent_id = generate_uuid();  // or CryptoCore random hex
env.push_back("MCP_AGENT_ID=" + agent_id);
this->agent_id = agent_id;  // store for toggle lookup
```

```cpp
// mcp_protocol.cpp — handle_initialize()
// Extract clientInfo from initialize params
Dictionary client_info = params.get("clientInfo", Dictionary());
String agent_id = client_info.get("agentId", "");
// ... or check for custom header, or env-injected field
if (!agent_id.is_empty()) {
    session_state.agent_id = agent_id;
}
```

Wait — Claude Code's `initialize` payload is controlled by Anthropic's SDK,
not by us. We can't inject custom fields into `clientInfo`. But we CAN:

1. Pass the agent_id as a **custom HTTP header** (e.g., `X-Agent-Id`).
   Problem: Claude Code's MCP client doesn't support custom headers.

2. Pass it as **query parameter**: `http://localhost:63315/mcp?agent_id=abc`.
   The MCP proxy (`godot_mcp_proxy.py`) could append this. Or we modify the
   MCP config JSON URL.

3. **Use the MCP server port itself.** Launch a separate MCP listener per
   agent tab on different ports. Heavy — overkill for now.

4. **Use the Bearer auth token as the identifier.** Each AgentPanel generates
   a unique token, passes it to Claude Code via the MCP config JSON. The
   MCPProtocol maps bearer token → agent_id → session permissions.

   This works TODAY with no changes to Claude Code.

**Recommended: Option 4 — unique Bearer token per agent tab.**

```json
// MCP config generated by AgentPanel::_build_mcp_config_json()
{
  "mcpServers": {
    "godot": {
      "url": "http://localhost:63315/mcp",
      "headers": {
        "Authorization": "Bearer agent_<uuid>"
      }
    }
  }
}
```

```cpp
// mcp_protocol.cpp — process_request(), auth check
// Today: single auth_token, reject if mismatch.
// New: if token starts with "agent_", look up in agent_tokens map.
//      Store agent_id on the MCPSessionState when session initializes.
```

### 5. UI: Toggle on the agent panel

```
┌──────────────────────────────────────────────────┐
│ [▶ Launch]  [■ Stop]  [🔄 Restart]              │
│                                                  │
│ ┌──────────────────────────────────────────────┐ │
│ │  ☑ Runtime tools   (start/stop/control game) │ │
│ └──────────────────────────────────────────────┘ │
│                                                  │
│ ┌──────────────────────────────────────────────┐ │
│ │  $ claude terminal output...                 │ │
│ │  ...                                         │ │
│ └──────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────┘
```

A single `CheckBox` in the toolbar row. When toggled:
1. AgentPanel calls `MCPProtocol::set_runtime_tools_enabled(agent_id, bool)`
2. MCPProtocol finds the session by agent_id, sets `runtime_tools_enabled`
3. Optional: notify the agent session via SSE that tools changed
   (`notifications/tools/changed` — the tool list itself doesn't change,
   but we could send a custom notification or log message)

### 6. Feedback to the agent

When a tool call is blocked, the agent gets a clear error:

```json
{
  "jsonrpc": "2.0",
  "error": {
    "code": -32600,
    "message": "Tool 'runtime/run_project' is blocked: runtime tools are disabled for this session. Enable the runtime toggle in the agent panel."
  },
  "id": "req-42"
}
```

The agent should understand from this that it can't use runtime tools and
should focus on editor-only work (file editing, analysis, etc.).

Optionally also filter `tools/list` to hide blocked tools entirely, so the
agent never even tries to call them. This means `MCPToolRegistry::list_tools()`
needs session context too — or we filter in `_handle_tools_list()`.

## Implementation Steps

### Step 1: MCPSessionState permissions field
- Add `bool runtime_tools_enabled = true` to `MCPSessionState`
- Add `String agent_id` to `MCPSessionState`

### Step 2: Agent ID via Bearer token
- `AgentPanel::_build_mcp_config_json()` — generate unique token per launch
- Store `agent_id` on AgentPanel instance
- `MCPProtocol::process_request()` — on auth, if token starts with `agent_`,
  extract agent_id and store on session during initialize

### Step 3: Permission check in process_request()
- Add `is_runtime_tool()` static function
- Insert check before Step 9b (SSE path) and Step 9c (standard path)
- Return clear error when blocked

### Step 4: Toggle API on MCPProtocol
- `void set_runtime_tools_enabled(const String &p_agent_id, bool p_enabled)`
- Finds session by agent_id, sets the flag
- Optionally sends SSE log notification to the agent

### Step 5: UI checkbox on AgentPanel
- Add `CheckBox *runtime_toggle` to AgentPanel
- Wire to `_on_runtime_toggle_changed(bool)`
- Call `MCPProtocol::set_runtime_tools_enabled(agent_id, toggled)`
- Default: checked (enabled)

### Step 6: Filter tools/list (optional)
- When runtime_tools_enabled is false, `_handle_tools_list()` omits
  runtime tools from the response
- Send `notifications/tools/changed` on toggle so agent re-fetches

## Files Modified

| File | Change |
|------|--------|
| `mcp_protocol.h` | `agent_id` and `runtime_tools_enabled` on MCPSessionState; `set_runtime_tools_enabled()` method; `is_runtime_tool()` static |
| `mcp_protocol.cpp` | Permission check in `process_request()`; agent_id extraction from bearer token; `set_runtime_tools_enabled()` impl |
| `terminal/agent_panel.h` | `agent_id` member; `runtime_toggle` CheckBox |
| `terminal/agent_panel.cpp` | Generate agent_id in launch; build config with bearer token; toggle handler |
| `mcp_tool_registry.h` | (optional) accept filter predicate in `list_tools()` |
| `mcp_tool_registry.cpp` | (optional) filtered `list_tools()` overload |

## Open Questions

1. **Granularity**: should `debug/describe_class` (works without a running game)
   be allowed even when runtime tools are disabled? Probably yes — split the
   debug/ tools into "needs game" vs "editor-time."

2. **Multi-tab**: this plan is single-tab today but the bearer token approach
   scales to N tabs. Should we build the multi-tab TabContainer now or later?

3. **Persistence**: should the toggle state persist across editor restarts?
   Probably not — default to enabled on each launch.

4. **External clients**: should external Claude Code sessions (not launched
   from the editor) respect the toggle? They'd use a different auth token,
   so they'd be a different "agent" with their own permission set. Default
   to runtime enabled for external clients (backward compat).
