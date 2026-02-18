# MCP Server Status Bottom Panel -- Design Document

## Overview

This document describes the design of an editor bottom panel for real-time
monitoring of the MCP (Model Context Protocol) server built into the Godot
editor. The panel appears alongside Output, Debugger, and other built-in panels
in the editor's bottom panel area. It provides live visibility into server
status, connected clients, request traffic, tool activity, and debugger bridge
health.

---

## 1. Architecture Summary (Existing Codebase)

Before designing the panel, here is a summary of the existing MCP server
architecture that the panel must integrate with.

### Class Hierarchy

```
MCPServerPlugin (EditorPlugin)
  |-- MCPProtocol (JSONRPC) -- singleton, owns the TCP server
  |     |-- TCPServer          -- accepts TCP connections
  |     |-- HashMap<int, Ref<MCPSession>> clients  -- TCP connections
  |     |-- HashMap<String, MCPSessionState> sessions  -- MCP protocol sessions
  |     |-- MCPToolRegistry    -- plain class, tool definitions + dispatch
  |     |-- MCPResourceRegistry  -- plain class, resource definitions + subscriptions
  |     `-- MCPDebuggerBridge* -- raw pointer, owned by MCPServerPlugin
  |
  `-- Ref<MCPDebuggerBridge> (EditorDebuggerPlugin)
        |-- OutputRingBuffer output_buffer
        |-- OutputRingBuffer error_buffer
        |-- HashMap<String, PendingRequest*> pending_requests
        `-- SafeFlag game_running, game_launching, game_paused
```

### Threading Model

- **Poll thread**: When `use_thread` is true (default), `MCPProtocol::poll()`
  runs on a background thread at ~20 Hz (50ms delay). When false, it runs on
  the main editor thread via `NOTIFICATION_INTERNAL_PROCESS`.
- **Main thread**: All Godot UI updates, EditorDebuggerPlugin callbacks
  (`setup_session`, `capture`, signal handlers), and deferred calls run on the
  main thread.
- **Tool execution**: Currently synchronous on the poll thread. Future phases
  may use worker threads.

### Key Data Points Available

| Data | Source | Thread Safety |
|------|--------|---------------|
| Server running | `MCPServerPlugin::started` | Main thread only |
| Host/port | `MCPServerPlugin::host`, `port` | Main thread only |
| TCP connections | `MCPProtocol::clients` | Poll thread (mutex needed) |
| MCP sessions | `MCPProtocol::sessions` | Poll thread (mutex needed) |
| Tool registry | `MCPToolRegistry::tools` | Read-only after init |
| Resource registry | `MCPResourceRegistry::resources` | Read-only after init |
| Game running | `MCPDebuggerBridge::game_running` | SafeFlag (atomic) |
| Game paused | `MCPDebuggerBridge::game_paused` | SafeFlag (atomic) |
| Pending requests | `MCPDebuggerBridge::pending_requests` | Mutex-guarded |
| Output/errors | `OutputRingBuffer` | Mutex-guarded |

---

## 2. Panel Registration

### How It Plugs Into the Editor

The panel registers as a bottom panel via `EditorPlugin::add_control_to_bottom_panel()`,
which internally creates an `EditorDock` and adds it to `EditorBottomPanel` (a
`TabContainer`).

```
MCPServerPlugin (constructor or NOTIFICATION_READY)
  |
  +--  Create MCPStatusPanel* panel = memnew(MCPStatusPanel)
  +--  add_control_to_bottom_panel(panel, "MCP")
  |
  +--  Pass pointers: panel->set_protocol(&protocol)
  |                   panel->set_debugger_bridge(debugger_bridge.ptr())
```

The panel is destroyed via `remove_control_from_bottom_panel()` in the
`MCPServerPlugin` destructor or `NOTIFICATION_EXIT_TREE`.

### File Structure

```
modules/mcp_server/
  editor/
    mcp_status_panel.h        -- MCPStatusPanel class declaration
    mcp_status_panel.cpp      -- UI construction + update logic
```

The `SCsub` file must be updated to compile files from the `editor/` subdirectory.

---

## 3. UI Layout

### ASCII Art Mockup

```
+===========================================================================+
| MCP  (bottom panel tab)                                                   |
+===========================================================================+
|                                                                           |
| +-- Server Status ----------+ +-- Debugger Bridge -----+ +-- Clients ---+|
| | [*] Running               | | Game: [*] Running      | | 2 connected  ||
| | 127.0.0.1:6009            | | Pending msgs: 0        | |              ||
| | Uptime: 00:14:32          | | Heartbeat: OK (frame   | | [session_id] ||
| | Tools: 42  Resources: 11  | |   #2847)               | | [session_id] ||
| | [Stop Server]             | |                         | |              ||
| +----------------------------+ +-------------------------+ +--------------+|
|                                                                           |
| +-- Request Log --------[Filter: ________] [Method: All v] [Status: All v]|
| |   [Search: ______________]  [x] Auto-scroll   [Clear]   [Export]       ||
| |------------------------------------------------------------------------|
| | Time       | Method            | Client   | Status  | Duration | Expand ||
| |------------|-------------------|----------|---------|----------|--------||
| | 14:32:01.4 | tools/call        | sess:a1b | OK 200  | 23ms     | [>]   ||
| | 14:32:01.1 | tools/list        | sess:a1b | OK 200  | 2ms      | [>]   ||
| | 14:31:58.7 | initialize        | sess:a1b | OK 200  | 1ms      | [>]   ||
| | 14:31:55.2 | tools/call        | sess:c3d | ERR     | 145ms    | [>]   ||
| |   [expanded: request JSON / response JSON shown here]                  ||
| | 14:31:50.0 | resources/read    | sess:a1b | OK 200  | 12ms     | [>]   ||
| | ...                                                                    ||
| +------------------------------------------------------------------------+|
|                                                                           |
| +-- Tool Activity Summary -----------------------------------------------+|
| | Tool Name              | Calls | Avg Time | Last Called   | Last Status ||
| |------------------------|-------|----------|---------------|-------------||
| | editor/scan_filesystem |    12 |    18ms  | 14:32:01      | OK          ||
| | runtime/get_scene_tree   |     5 |   340ms  | 14:31:55      | OK          ||
| | gdscript/check         |     3 |    45ms  | 14:30:12      | OK          ||
| | runtime/run_project      |     1 |   120ms  | 14:28:00      | OK          ||
| +------------------------------------------------------------------------+|
+===========================================================================+
```

### Layout Structure (Top to Bottom)

**Row 1: Status Bar** -- Three side-by-side panels in an HBoxContainer.

**Row 2: Request Log** -- The main area, a Tree control with filters and search.

**Row 3: Tool Activity Summary** -- A compact Tree/table showing aggregated
tool call statistics.

---

## 4. UI Components -- Node Tree

```
MCPStatusPanel (VBoxContainer)
  |
  +-- top_bar (HBoxContainer)
  |     |
  |     +-- server_status_panel (VBoxContainer, fixed width ~220px)
  |     |     +-- status_indicator (HBoxContainer)
  |     |     |     +-- status_dot (TextureRect)  -- green/red circle
  |     |     |     +-- status_label (Label)  -- "Running" / "Stopped"
  |     |     +-- address_label (Label)  -- "127.0.0.1:6009"
  |     |     +-- uptime_label (Label)  -- "Uptime: 00:14:32"
  |     |     +-- stats_label (Label)  -- "Tools: 42  Resources: 11"
  |     |     +-- toggle_button (Button)  -- "Stop Server" / "Start Server"
  |     |
  |     +-- VSeparator
  |     |
  |     +-- debugger_status_panel (VBoxContainer, fixed width ~200px)
  |     |     +-- game_status (HBoxContainer)
  |     |     |     +-- game_dot (TextureRect)  -- green/gray/yellow
  |     |     |     +-- game_label (Label)  -- "Game: Running" etc.
  |     |     +-- pending_label (Label)  -- "Pending msgs: 0"
  |     |     +-- heartbeat_label (Label)  -- "Heartbeat: OK (frame #N)"
  |     |
  |     +-- VSeparator
  |     |
  |     +-- clients_panel (VBoxContainer, expand to fill)
  |           +-- clients_header (HBoxContainer)
  |           |     +-- clients_label (Label)  -- "Connected Clients"
  |           |     +-- client_count_badge (Label)  -- styled "[2]"
  |           +-- clients_tree (Tree, 3 columns, compact)
  |                 -- columns: Session ID | Duration | Last Activity
  |
  +-- HSeparator
  |
  +-- log_toolbar (HBoxContainer)
  |     +-- filter_label (Label)  -- "Filter:"
  |     +-- search_edit (LineEdit)  -- free-text search, placeholder "Search..."
  |     +-- method_filter (OptionButton)  -- "All", "tools/call", "tools/list", ...
  |     +-- status_filter (OptionButton)  -- "All", "Success", "Error", "In Progress"
  |     +-- HSeparator (spacer, expand)
  |     +-- auto_scroll_check (CheckBox)  -- "Auto-scroll", default ON
  |     +-- clear_button (Button)  -- "Clear"
  |     +-- export_button (Button)  -- "Export" (saves log to file)
  |
  +-- request_log_tree (Tree, SIZE_EXPAND_FILL, main area)
  |     -- columns: Time | Method | Client | Status | Duration | Details toggle
  |     -- TreeItem children for expandable JSON
  |
  +-- HSeparator
  |
  +-- tool_summary_tree (Tree, fixed height ~120px)
        -- columns: Tool Name | Calls | Avg Time | Last Called | Last Status
```

### Godot Control Node Types

| Widget | Godot Type | Purpose |
|--------|-----------|---------|
| Panel root | `VBoxContainer` | Top-level container |
| Status bar | `HBoxContainer` | Horizontal row of status sections |
| Server status | `VBoxContainer` | Server state, address, uptime |
| Status dot | `TextureRect` | Green/red circle (8x8 programmatic) |
| Status labels | `Label` | Text display |
| Toggle button | `Button` | Start/Stop server |
| Debugger bridge | `VBoxContainer` | Game state, pending messages |
| Client list | `Tree` | Compact table of connected clients |
| Log toolbar | `HBoxContainer` | Search, filter, auto-scroll controls |
| Search | `LineEdit` | Free-text filter |
| Method filter | `OptionButton` | Dropdown for method name filter |
| Status filter | `OptionButton` | Dropdown for success/error/in-progress |
| Auto-scroll | `CheckBox` | Toggle auto-scroll |
| Request log | `Tree` | Main scrolling log with expandable rows |
| Tool summary | `Tree` | Aggregated statistics table |

---

## 5. Data Flow: MCP Server Internals to UI

### 5.1 The Fundamental Challenge: Thread Safety

The MCP server poll loop runs on a background thread. The UI runs on the main
thread. Data must flow from the poll thread to the UI safely.

**Strategy: Thread-safe intermediate buffer + main-thread timer polling.**

The panel does NOT directly read MCPProtocol internals. Instead:

1. MCPProtocol emits structured event records into a thread-safe ring buffer.
2. The panel's `_notification(NOTIFICATION_INTERNAL_PROCESS)` handler (main
   thread, ~60 Hz) drains the buffer and updates the UI.
3. Heavy data (connected clients list, tool stats) is snapshot-copied under
   mutex at a reduced rate (every 500ms).

### 5.2 Event Record Structure

A new struct captures each request/response event:

```cpp
// In mcp_status_data.h (new file)

struct MCPRequestEvent {
    uint64_t timestamp_usec;      // OS::get_ticks_usec()
    String method;                // "tools/call", "initialize", etc.
    String session_id;            // First 8 chars of session ID
    String client_ip;             // Source IP (from TCP peer)
    int http_status;              // 200, 400, 404, 500, etc.
    bool is_error;
    uint64_t duration_usec;       // Time from request received to response sent
    String request_json;          // Full request body (truncated to 4KB for UI)
    String response_json;         // Full response body (truncated to 4KB for UI)
    String tool_name;             // For tools/call: which tool was invoked
};
```

### 5.3 Ring Buffer for Events

```cpp
// In mcp_status_data.h

class MCPEventBuffer {
    static const int CAPACITY = 2000;  // Keep last 2000 events
    Vector<MCPRequestEvent> events;
    uint64_t next_seq = 1;
    int write_pos = 0;
    int count = 0;
    mutable Mutex mutex;

public:
    void push(const MCPRequestEvent &p_event);  // Thread-safe
    Vector<MCPRequestEvent> read_since(uint64_t p_cursor, int p_limit = 200) const;
    uint64_t latest_seq() const;
    void clear();
};
```

### 5.4 Where Events Are Emitted

Events are emitted from `MCPProtocol::process_request()` at strategic points:

| Emission Point | What It Captures |
|----------------|------------------|
| After `process_request()` dispatches and queues a response | Method, status, duration, request/response JSON |
| After `dispatch_tool_with_progress()` sends final SSE event | Tool call with streaming |
| On client connect/disconnect | Client lifecycle |
| On session create/terminate | Session lifecycle |

The event buffer lives on `MCPProtocol` as a member:

```cpp
// In mcp_protocol.h, add:
MCPEventBuffer event_buffer;
```

### 5.5 Client Snapshot Structure

```cpp
struct MCPClientSnapshot {
    String session_id;         // First 8 chars + "..."
    String peer_address;       // IP:port from TCP peer
    uint64_t connected_since;  // Timestamp
    uint64_t last_activity;    // Timestamp
    bool is_sse_stream;        // GET SSE stream vs regular HTTP
};
```

A snapshot method on MCPProtocol returns this data:

```cpp
// Thread-safe: acquires internal mutex, copies, returns
Vector<MCPClientSnapshot> MCPProtocol::get_client_snapshots() const;
```

### 5.6 Tool Statistics

Tool activity is tracked in a HashMap on MCPProtocol, updated atomically:

```cpp
struct MCPToolStats {
    int call_count = 0;
    uint64_t total_duration_usec = 0;
    uint64_t last_call_time_usec = 0;
    bool last_was_error = false;
};

// In MCPProtocol:
Mutex tool_stats_mutex;
HashMap<String, MCPToolStats> tool_stats;
```

Updated at the end of `call_tool()` and `call_tool_with_progress()`.

---

## 6. Signals and Hooks Required

### 6.1 New Additions to MCPProtocol

```cpp
// mcp_protocol.h -- new public members

// Event buffer for the status panel.
MCPEventBuffer event_buffer;  // Thread-safe ring buffer

// Client snapshot (thread-safe copy under lock).
Vector<MCPClientSnapshot> get_client_snapshots() const;

// Tool statistics (thread-safe copy under lock).
HashMap<String, MCPToolStats> get_tool_stats() const;

// Server metadata.
bool is_running() const;
String get_listen_address() const;
int get_listen_port() const;
uint64_t get_start_time_usec() const;
int get_session_count() const;
int get_client_count() const;
```

### 6.2 Instrumentation Points in process_request()

The `process_request()` method needs instrumentation at the end, just before
returning. This is a single block of code that captures the event:

```cpp
// At the end of MCPProtocol::process_request(), after response is queued:
{
    MCPRequestEvent evt;
    evt.timestamp_usec = OS::get_singleton()->get_ticks_usec();
    evt.method = method;
    evt.session_id = session_id_header.is_empty() ? "-" : session_id_header.substr(0, 8);
    evt.http_status = /* parsed from response status */;
    evt.is_error = /* http_status >= 400 */;
    evt.duration_usec = evt.timestamp_usec - request_start_time;
    evt.request_json = session->request_body.left(4096);
    evt.response_json = /* response body, truncated */;
    evt.tool_name = /* if tools/call, extract from params */;
    event_buffer.push(evt);
}
```

Similarly for `dispatch_tool_with_progress()`.

### 6.3 Hooks in MCPServerPlugin

The `MCPServerPlugin` needs to:

1. Expose `started`, `host`, `port` to the panel (or provide getters).
2. Provide a `start()` / `stop()` public API for the toggle button (currently
   private -- make them callable from the panel, or add a `toggle_server()`
   method).
3. Store the server start timestamp for uptime calculation.

### 6.4 No New Signals Needed

The design deliberately avoids Godot signals for the hot path. Signals involve
`Callable` dispatch and are not ideal for high-frequency cross-thread
communication. Instead, the panel polls a thread-safe buffer, which is simpler
and more performant.

---

## 7. Panel Update Logic

### 7.1 Update Timer Strategy

The panel uses `NOTIFICATION_INTERNAL_PROCESS` with rate limiting:

```cpp
void MCPStatusPanel::_notification(int p_what) {
    if (p_what == NOTIFICATION_INTERNAL_PROCESS) {
        uint64_t now = OS::get_singleton()->get_ticks_usec();

        // Fast updates: request log (every frame, ~16ms at 60fps)
        _update_request_log();

        // Slow updates: everything else (every 500ms)
        if (now - last_slow_update > 500000) {
            last_slow_update = now;
            _update_server_status();
            _update_clients();
            _update_debugger_bridge();
            _update_tool_summary();
        }
    }
}
```

### 7.2 Request Log Updates

```
_update_request_log():
  1. Read new events from event_buffer.read_since(last_event_cursor)
  2. For each new event:
     a. Create a TreeItem row in request_log_tree
     b. Apply color coding based on status
     c. If filters are active, check method/status/search text before adding
  3. If auto_scroll is ON, scroll to bottom
  4. Cap total rows at 2000; prune oldest when exceeded
```

### 7.3 Color Coding

| Status | Color | Godot Color Constant |
|--------|-------|---------------------|
| Success (2xx) | Green | `Color(0.4, 0.9, 0.4)` (or theme `success_color`) |
| Error (4xx/5xx) | Red | `Color(0.9, 0.3, 0.3)` (or theme `error_color`) |
| In-progress | Yellow | `Color(0.9, 0.8, 0.3)` (or theme `warning_color`) |
| Notification (202) | Gray | `Color(0.6, 0.6, 0.6)` |

Colors should be pulled from the editor theme where possible for consistency.

### 7.4 Expandable Rows

When a user clicks the expand toggle on a log row:

1. A child `TreeItem` is created below the row.
2. The child contains a `custom_draw` cell or uses `set_text()` with the full
   JSON, formatted with indentation.
3. JSON is truncated to 4KB per field (request/response) to avoid UI lag.
4. Collapsing removes the child `TreeItem`.

Implementation detail: Use `Tree::set_column_expand()` with the last column as
the expand toggle. Each log `TreeItem` stores the full `MCPRequestEvent` in
metadata via `set_metadata()`.

---

## 8. Detailed Component Specifications

### 8.1 Server Status Section

**Displays:**
- Status dot: 8x8 circle, green when `protocol->is_running()`, red otherwise.
  Generated programmatically via `Image::create()` + `ImageTexture::create_from_image()`.
- Status text: "Running" / "Stopped"
- Listen address: `host + ":" + itos(port)`, shown only when running
- Uptime: Calculated as `(now - server_start_time_usec) / 1000000`, formatted
  as `HH:MM:SS`
- Tool/resource count: `protocol->get_tool_registry()->get_tool_count()` and
  resource count (add `get_resource_count()` to `MCPResourceRegistry`)
- Toggle button: Calls `MCPServerPlugin::toggle_server()` via a stored pointer

### 8.2 Connected Clients Section

**Tree columns:**

| Column | Width | Content |
|--------|-------|---------|
| Session | 100px | First 8 hex chars of session ID, or "-" |
| Type | 50px | "HTTP" or "SSE" |
| Duration | 80px | "2m 14s" format |
| Last Activity | 80px | "3s ago" format |

**Behavior:**
- Updated every 500ms via `protocol->get_client_snapshots()`
- Client count badge shows total count: "[3]" styled with theme
- Tree is rebuilt on each update (cheap for <= 8 clients, which is the max)

### 8.3 Live Request Log

**Tree columns:**

| # | Column | Width | Expand |
|---|--------|-------|--------|
| 0 | Time | 90px | No |
| 1 | Method | 160px | No |
| 2 | Client | 80px | No |
| 3 | Status | 70px | No |
| 4 | Duration | 70px | No |
| 5 | Details | 40px | No |

**Column 0 (Time):** Format `HH:MM:SS.d` (one decimal for sub-second precision).

**Column 1 (Method):** The JSON-RPC method name. For `tools/call`, append the
tool name in parentheses: `tools/call (editor/scan_filesystem)`.

**Column 2 (Client):** First 8 hex chars of the MCP session ID.

**Column 3 (Status):** HTTP status code with color. "200 OK", "400 Bad Request",
"IN PROG" (yellow, for SSE streams not yet completed).

**Column 4 (Duration):** Milliseconds: "23ms", "1.2s" for > 1000ms.

**Column 5 (Details):** A clickable "[>]" / "[v]" toggle. Clicking creates/removes
a child `TreeItem` showing the full request and response JSON.

**Filtering:**
- `search_edit`: Filters on method name, tool name, session ID, or response text.
  Applied as a case-insensitive substring match.
- `method_filter`: Populated dynamically with observed method names, plus "All".
- `status_filter`: "All", "Success (2xx)", "Error (4xx+)", "In Progress".
- Filters are applied when adding new items AND retroactively: changing a
  filter rebuilds the visible tree from the full event buffer.

**Row limit:** Maximum 2000 visible rows. When exceeded, the oldest rows are
removed from the Tree (but the ring buffer retains all 2000 events for
re-filtering).

### 8.4 Tool Activity Summary

**Tree columns:**

| # | Column | Width | Content |
|---|--------|-------|---------|
| 0 | Tool Name | 200px | Full tool name, e.g. "editor/scan_filesystem" |
| 1 | Calls | 60px | Call count, right-aligned |
| 2 | Avg Time | 80px | Average response time in ms |
| 3 | Last Called | 100px | HH:MM:SS timestamp |
| 4 | Last Status | 80px | "OK" (green) / "Error" (red) |

**Behavior:**
- Sorted by call count descending (most-used tools at top)
- Updated every 500ms via `protocol->get_tool_stats()`
- The most recently called tool row is highlighted with a subtle background
  color for 3 seconds after its last call

### 8.5 Debugger Bridge Status

**Displays:**
- Game status dot: Green when `game_running`, yellow when `game_launching`,
  gray when stopped.
- Game status text: "Running", "Launching", "Paused", "Stopped"
- Pending messages: Count of entries in `MCPDebuggerBridge::pending_requests`
  (expose via `get_pending_count()` method).
- Heartbeat: "OK (frame #N)" where N is `get_game_frame_count()`, or "No game"
  when stopped. Show "Stale" in yellow if frame count has not changed for > 5s
  while game is marked as running.

---

## 9. Performance Considerations

### 9.1 UI Update Rate Limiting

| Data | Update Frequency | Rationale |
|------|-----------------|-----------|
| Request log | Every frame (~60Hz) | Responsive feel; only processes new events |
| Server status | Every 500ms | Uptime display only needs ~1Hz |
| Client list | Every 500ms | Client count rarely changes |
| Tool summary | Every 500ms | Aggregated stats; no urgency |
| Debugger bridge | Every 500ms | Game state changes are infrequent |

### 9.2 Memory Budget

- Ring buffer: 2000 events * ~1KB each = ~2MB worst case
- Tree items: 2000 rows * ~200 bytes per TreeItem = ~400KB
- JSON strings in metadata: 2000 * 8KB (4KB request + 4KB response) = 16MB max
  - Mitigation: Store full JSON lazily; only load into metadata when row is
    expanded. Keep truncated summary in the event struct.
  - Revised: Store first 512 bytes of request/response in the event struct.
    Full JSON is only captured if `panel_is_visible` flag is true.

### 9.3 Conditional Instrumentation

When the MCP panel is not visible (tab not selected), the event buffer should
still collect events (for reviewing later), but skip capturing full
request/response JSON to save memory:

```cpp
bool capture_json = panel && panel->is_visible_in_tree();
evt.request_json = capture_json ? session->request_body.left(4096) : "";
evt.response_json = capture_json ? response_body.left(4096) : "";
```

The panel sets a `SafeFlag` on the protocol indicating visibility. The protocol
checks this flag (atomic read, no lock) before capturing expensive JSON strings.

### 9.4 Tree Item Recycling

When the Tree exceeds 2000 rows, remove items from the top (oldest) rather than
clearing and rebuilding. This avoids a full rebuild which could cause UI stutter.

### 9.5 Throttling Under Load

If the event buffer receives more than 100 events per second (possible during
bulk tool calls), batch them into a single UI update per frame. The
`_update_request_log()` method processes all events that arrived since the last
frame in one pass, creating all TreeItems before any layout recalculation.

### 9.6 No Impact When Panel Is Not Compiled

The event buffer and instrumentation are compiled only when `TOOLS_ENABLED` is
defined (editor builds). Export templates have zero overhead.

---

## 10. Hooks and Modifications to Existing Classes

### 10.1 MCPProtocol (mcp_protocol.h / .cpp)

**New members:**

```cpp
// Status panel data feed
MCPEventBuffer event_buffer;
Mutex status_mutex;
HashMap<String, MCPToolStats> tool_stats;
uint64_t server_start_time_usec = 0;
SafeFlag panel_visible;  // Set by panel when it becomes visible

// New public methods:
Vector<MCPClientSnapshot> get_client_snapshots() const;
HashMap<String, MCPToolStats> get_tool_stats() const;
bool is_running() const;
String get_listen_address() const;
int get_listen_port() const;
uint64_t get_start_time_usec() const;
int get_session_count() const;
int get_client_count() const;
MCPEventBuffer *get_event_buffer() { return &event_buffer; }
void set_panel_visible(bool p_visible);
```

**Instrumentation in `process_request()`:**

Add event emission at the end of each response path (before `return`). This is
~15 lines of code per emission point, affecting:

1. End of `process_request()` -- standard dispatch path (one location, covers
   all methods)
2. End of `dispatch_tool_with_progress()` -- SSE tool dispatch path

**Instrumentation in `start()` / `stop()`:**

Record `server_start_time_usec` in `start()`. Clear in `stop()`.

**Instrumentation in `call_tool()` and `call_tool_with_progress()`:**

Update `tool_stats` HashMap after each tool execution.

### 10.2 MCPServerPlugin (mcp_server_plugin.h / .cpp)

**New members:**

```cpp
MCPStatusPanel *status_panel = nullptr;
Button *panel_button = nullptr;
```

**Constructor changes:**

```cpp
// After protocol and debugger bridge setup:
status_panel = memnew(MCPStatusPanel);
status_panel->set_protocol(&protocol);
status_panel->set_debugger_bridge(debugger_bridge.ptr());
status_panel->set_server_plugin(this);
panel_button = add_control_to_bottom_panel(status_panel, "MCP");
```

**Destructor changes:**

```cpp
if (status_panel) {
    remove_control_from_bottom_panel(status_panel);
    memdelete(status_panel);
    status_panel = nullptr;
}
```

**New public method:**

```cpp
// Called by the panel's Start/Stop button.
void toggle_server();
```

### 10.3 MCPToolRegistry (mcp_tool_registry.h / .cpp)

No changes needed. Tool stats are tracked in `MCPProtocol`, not the registry.

### 10.4 MCPResourceRegistry (mcp_resource_registry.h / .cpp)

**New public method:**

```cpp
int get_resource_count() const { return resources.size(); }
```

### 10.5 MCPDebuggerBridge (mcp_debugger_bridge.h / .cpp)

**New public method:**

```cpp
int get_pending_request_count() const {
    MutexLock lock(request_mutex);
    return pending_requests.size();
}
```

### 10.6 MCPSession (mcp_session.h)

No changes needed. Client IP is obtained from `connection->get_connected_host()`
when building snapshots.

---

## 11. New File Structure

```
modules/mcp_server/
  editor/
    mcp_status_panel.h          -- MCPStatusPanel class declaration
    mcp_status_panel.cpp        -- UI construction, update logic, event handling
    mcp_status_data.h           -- MCPRequestEvent, MCPEventBuffer, MCPClientSnapshot,
                                   MCPToolStats struct definitions
    mcp_status_data.cpp         -- MCPEventBuffer implementation
```

### 11.1 mcp_status_panel.h (Key API)

```cpp
#pragma once

#include "mcp_status_data.h"
#include "scene/gui/box_container.h"

class Button;
class CheckBox;
class Label;
class LineEdit;
class MCPDebuggerBridge;
class MCPProtocol;
class MCPServerPlugin;
class OptionButton;
class TextureRect;
class Tree;
class TreeItem;

class MCPStatusPanel : public VBoxContainer {
    GDCLASS(MCPStatusPanel, VBoxContainer)

private:
    // External references (not owned).
    MCPProtocol *protocol = nullptr;
    MCPDebuggerBridge *debugger_bridge = nullptr;
    MCPServerPlugin *server_plugin = nullptr;

    // -- Server Status --
    TextureRect *status_dot = nullptr;
    Label *status_label = nullptr;
    Label *address_label = nullptr;
    Label *uptime_label = nullptr;
    Label *stats_label = nullptr;
    Button *toggle_button = nullptr;

    // -- Debugger Bridge --
    TextureRect *game_dot = nullptr;
    Label *game_label = nullptr;
    Label *pending_label = nullptr;
    Label *heartbeat_label = nullptr;

    // -- Clients --
    Label *client_count_label = nullptr;
    Tree *clients_tree = nullptr;

    // -- Request Log --
    LineEdit *search_edit = nullptr;
    OptionButton *method_filter = nullptr;
    OptionButton *status_filter = nullptr;
    CheckBox *auto_scroll_check = nullptr;
    Button *clear_button = nullptr;
    Button *export_button = nullptr;
    Tree *request_log_tree = nullptr;

    // -- Tool Summary --
    Tree *tool_summary_tree = nullptr;

    // -- State --
    uint64_t last_slow_update = 0;
    uint64_t last_event_cursor = 0;
    int64_t last_heartbeat_frame = -1;
    uint64_t last_heartbeat_change_time = 0;

    // Cached status dot textures.
    Ref<Texture2D> dot_green;
    Ref<Texture2D> dot_red;
    Ref<Texture2D> dot_yellow;
    Ref<Texture2D> dot_gray;

    // -- Methods --
    void _build_ui();
    void _create_dot_textures();

    void _update_server_status();
    void _update_clients();
    void _update_debugger_bridge();
    void _update_request_log();
    void _update_tool_summary();

    void _on_toggle_button_pressed();
    void _on_clear_pressed();
    void _on_export_pressed();
    void _on_filter_changed(int p_index);
    void _on_search_changed(const String &p_text);
    void _on_log_item_activated();  // Row double-click / expand toggle

    bool _event_matches_filters(const MCPRequestEvent &p_event) const;
    String _format_timestamp(uint64_t p_usec) const;
    String _format_duration(uint64_t p_usec) const;
    String _format_relative_time(uint64_t p_usec) const;
    Color _color_for_status(int p_http_status) const;

    TreeItem *_add_log_entry(const MCPRequestEvent &p_event);
    void _rebuild_log_from_buffer();  // Called when filters change

    void _notification(int p_what);

protected:
    static void _bind_methods();

public:
    void set_protocol(MCPProtocol *p_protocol) { protocol = p_protocol; }
    void set_debugger_bridge(MCPDebuggerBridge *p_bridge) { debugger_bridge = p_bridge; }
    void set_server_plugin(MCPServerPlugin *p_plugin) { server_plugin = p_plugin; }

    MCPStatusPanel();
    ~MCPStatusPanel();
};
```

---

## 12. Build System Integration

### 12.1 SCsub Changes

The existing `SCsub` must be updated to compile the new files:

```python
# In modules/mcp_server/SCsub, add:
env_mcp.add_source_files(env.modules_sources, "editor/*.cpp")
```

### 12.2 register_types.cpp

No changes needed. The panel is created by `MCPServerPlugin` (already registered
via `EditorPlugins::add_by_type<MCPServerPlugin>()`), not via `register_types`.

---

## 13. Implementation Plan

### Phase 1: Data Infrastructure (mcp_status_data.h/cpp)

1. Define `MCPRequestEvent`, `MCPEventBuffer`, `MCPClientSnapshot`, `MCPToolStats`.
2. Implement `MCPEventBuffer` (thread-safe ring buffer, same pattern as
   `OutputRingBuffer`).
3. Add `event_buffer`, `tool_stats`, `server_start_time_usec`, `panel_visible`
   to `MCPProtocol`.
4. Add `get_client_snapshots()`, `get_tool_stats()`, `is_running()`, etc. to
   `MCPProtocol`.
5. Add `get_pending_request_count()` to `MCPDebuggerBridge`.
6. Add `get_resource_count()` to `MCPResourceRegistry`.

### Phase 2: Instrumentation

1. Add event emission at the end of `process_request()`.
2. Add event emission at the end of `dispatch_tool_with_progress()`.
3. Add tool stats updates in `call_tool()` and `call_tool_with_progress()`.
4. Record `server_start_time_usec` in `MCPProtocol::start()`.

### Phase 3: Panel UI (mcp_status_panel.h/cpp)

1. Implement `_build_ui()`: construct the full node tree from Section 4.
2. Implement `_create_dot_textures()`: generate green/red/yellow/gray dots.
3. Implement slow-path update methods: `_update_server_status()`,
   `_update_clients()`, `_update_debugger_bridge()`, `_update_tool_summary()`.
4. Implement `_update_request_log()`: drain event buffer, create Tree rows.
5. Implement filtering: `_event_matches_filters()`, `_rebuild_log_from_buffer()`.
6. Implement expand/collapse for log rows.
7. Implement `_on_toggle_button_pressed()`, `_on_clear_pressed()`,
   `_on_export_pressed()`.

### Phase 4: Integration

1. Modify `MCPServerPlugin` constructor to create and register the panel.
2. Modify `MCPServerPlugin` destructor to remove and free the panel.
3. Add `toggle_server()` to `MCPServerPlugin`.
4. Update `SCsub` for new files.
5. Test with actual MCP client connections.

### Phase 5: Polish

1. Theme integration: use editor theme colors for status indicators.
2. Keyboard shortcuts for the panel.
3. Export functionality: save log to JSON or CSV file.
4. Accessibility: proper labels and tooltips.
5. Localization: wrap all user-visible strings in `TTR()`.

---

## 14. Edge Cases and Error Handling

### 14.1 Server Not Yet Started

When the server has not started (e.g., disabled in settings), the panel shows:
- Status: red dot, "Stopped"
- Address: "(not configured)"
- Toggle button: "Start Server"
- Log: empty, with hint text "MCP server is not running."

### 14.2 Thread Safety During Shutdown

When `MCPServerPlugin::stop()` is called:
1. The poll thread stops (thread_running cleared, thread.wait_to_finish()).
2. The panel may still be updating on the main thread.
3. The panel checks `protocol != nullptr` and `protocol->is_running()` before
   accessing any data. After `stop()`, `is_running()` returns false and all
   data access methods return empty results.

### 14.3 Panel Created Before Server Starts

The panel is created in the MCPServerPlugin constructor, but the server starts
later (in `NOTIFICATION_INTERNAL_PROCESS` when the editor is ready). The panel
handles this gracefully by showing "Stopped" until `is_running()` returns true.

### 14.4 Rapid Reconnections

If a client rapidly connects and disconnects, the event buffer handles this
naturally. The client list updates every 500ms, so very short-lived connections
may not appear in the client list but will appear as events in the log.

### 14.5 Very Long Tool Executions

For long-running tools (e.g., `runtime/get_session_summary` which blocks on
debugger round-trips), the log shows an "IN PROG" entry that updates to the
final status when the tool completes. This is achieved by:
1. Emitting a "started" event when the tool dispatch begins.
2. Emitting a "completed" event when it finishes.
3. The panel correlates them by request ID and updates the row in place.

---

## 15. Future Enhancements (Out of Scope for V1)

- **Request/response diff viewer**: Side-by-side JSON comparison.
- **Performance graphs**: Sparkline charts for requests-per-second, response
  latency over time.
- **Client session inspector**: Click a client to see all its requests.
- **Export to HAR format**: Standard HTTP Archive format for analysis tools.
- **Notifications panel**: Show server-initiated notifications (SSE events).
- **Dark/light theme auto-detection**: The programmatic dot textures should
  respect the editor theme.
- **Connection to running game indicator in top bar**: A small icon in the
  editor's top toolbar showing MCP server status at a glance.
