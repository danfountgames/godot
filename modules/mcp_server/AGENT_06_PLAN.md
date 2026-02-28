# Agent 06: Session-Aware SSE Notification Delivery

## Overview

Replace the stub `"stub_session"` subscription tracking with real MCP session IDs, fix the broadcast-clear notification model to be per-session, and wire up cleanup hooks so subscriptions are purged on session termination/expiry.

**Branch:** `feature/mcp-server`
**Base commit:** `fc9757110d`

---

## Problem Statement

The MCP server currently has two stub patterns that break multi-session resource subscriptions:

### Stub 1: `"stub_session"` placeholder (mcp_protocol.cpp:906-908, 921)

```cpp
// mcp_protocol.cpp line 906-908
// Subscription tracking is a stub. Pass a placeholder session ID.
// Full session-aware subscription delivery will be implemented in AGENT_06.
return resource_registry.handle_subscribe(uri, "stub_session");
```

All subscriptions from all clients are attributed to a single fake session `"stub_session"`, making it impossible to deliver notifications to the correct client or clean up per-session.

### Stub 2: Broadcast-clear notification flush (mcp_resource_registry.cpp:298-302)

```cpp
// NOTE: This means if multiple sessions are subscribed, only the first
// to flush gets notifications. AGENT_06 must change this to per-session
// queues when SSE delivery is implemented.
pending_notifications.clear();
```

`flush_notifications()` clears the global `pending_notifications` vector after the first session flushes, so subsequent sessions never see the notifications.

### Missing: Cleanup on session termination/expiry

Neither `terminate_session()` (mcp_protocol.cpp:724-758) nor `gc_stale_sessions()` (mcp_protocol.cpp:260-301) call `resource_registry.unsubscribe_all(session_id)`. The method exists but is never invoked from the protocol layer.

---

## Architecture

### Current Data Flow (Broken)

```
resource change
    |
    v
MCPResourceRegistry::notify_changed(uri)
    |  (adds to global pending_notifications if subscribed)
    v
flush_notifications("stub_session")      <-- wrong session
    |  (returns URIs, clears ALL pending)  <-- clears for everyone
    v
??? (never called from protocol layer)
```

### Target Data Flow (Agent 06)

```
resource change
    |
    v
MCPResourceRegistry::notify_changed(uri)
    |  (adds to per-session pending sets for all subscribed sessions)
    v
MCPProtocol::flush_sse_notifications()   [called every poll()]
    |  for each session with pending notifications:
    |    build JSON-RPC notification string
    |    queue to MCPSessionState::notification_queue
    v
SSE delivery to connected GET streams
    |  (already working via queue_sse_event)
    v
Session terminate / GC
    |  calls resource_registry.unsubscribe_all(session_id)
    v
Clean
```

---

## Changes by File

### 1. `mcp_resource_registry.h`

**Goal:** Replace global `pending_notifications` with per-session notification sets.

```
REMOVE:
  Vector<String> pending_notifications;
  Mutex notification_mutex;
  Vector<String> flush_notifications(const String &p_session_id);

ADD:
  HashMap<String, HashSet<String>> pending_per_session;  // session_id -> set of URIs
  Mutex notification_mutex;
  Vector<String> flush_notifications(const String &p_session_id);
```

The API signature stays the same -- `flush_notifications(session_id)` returns a `Vector<String>` of URIs that changed for that session. But the internal storage becomes per-session so flushing one session doesn't destroy another's pending data.

**Full diff of changed members:**

```cpp
// BEFORE (in private section):
Vector<String> pending_notifications;
Mutex notification_mutex;

// AFTER:
HashMap<String, HashSet<String>> pending_per_session; // session_id -> set of changed URIs
Mutex notification_mutex;
```

### 2. `mcp_resource_registry.cpp`

**Goal:** Rewrite `notify_changed()` and `flush_notifications()` to use per-session storage.

#### 2a. `notify_changed(const String &p_uri)` -- rewrite

Current implementation adds URI to a global vector. New implementation iterates subscriptions and adds the URI to each subscribed session's pending set.

```cpp
void MCPResourceRegistry::notify_changed(const String &p_uri) {
    MutexLock slock(subscription_mutex);  // Always lock subscription_mutex first.
    MutexLock nlock(notification_mutex);

    if (!subscriptions.has(p_uri)) {
        return;
    }

    // For every session subscribed to this URI, add it to their pending set.
    for (const String &session_id : subscriptions[p_uri]) {
        pending_per_session[session_id].insert(p_uri);
    }
}
```

**Thread safety:** Maintains the existing lock ordering (subscription_mutex before notification_mutex) to prevent ABBA deadlocks.

#### 2b. `flush_notifications(const String &p_session_id)` -- rewrite

Current implementation iterates global pending_notifications and clears ALL. New implementation extracts and clears only the target session's pending set.

```cpp
Vector<String> MCPResourceRegistry::flush_notifications(const String &p_session_id) {
    MutexLock slock(subscription_mutex);  // Always lock subscription_mutex first.
    MutexLock nlock(notification_mutex);

    Vector<String> result;
    if (pending_per_session.has(p_session_id)) {
        for (const String &uri : pending_per_session[p_session_id]) {
            result.push_back(uri);
        }
        pending_per_session.erase(p_session_id);
    }
    return result;
}
```

#### 2c. `unsubscribe_all(const String &p_session_id)` -- update

Add cleanup of `pending_per_session` for the removed session.

```cpp
void MCPResourceRegistry::unsubscribe_all(const String &p_session_id) {
    MutexLock slock(subscription_mutex);  // subscription_mutex first.
    MutexLock nlock(notification_mutex);   // then notification_mutex.

    // Remove session from all subscription sets.
    Vector<String> empty_uris;
    for (KeyValue<String, HashSet<String>> &E : subscriptions) {
        E.value.erase(p_session_id);
        if (E.value.is_empty()) {
            empty_uris.push_back(E.key);
        }
    }
    for (const String &uri : empty_uris) {
        subscriptions.erase(uri);
    }

    // Clear any pending notifications for this session.
    pending_per_session.erase(p_session_id);
}
```

#### 2d. Section comment updates

Remove `(stubs -- delivery requires SSE from AGENT_06)` and `(queue only -- delivery via AGENT_06 SSE)` from section headers. Replace with clean descriptions now that delivery is implemented.

### 3. `mcp_protocol.cpp`

#### 3a. Pass real session ID to subscribe/unsubscribe

The `handle_resources_subscribe()` and `handle_resources_unsubscribe()` methods are dispatched via `process_action()` -> `set_method()` callables, which means they receive `p_params` from the JSON-RPC request but **do not** directly have access to the MCP session ID from the HTTP header.

**The problem:** These are registered as JSON-RPC method handlers via `set_method("resources/subscribe", callable_mp(...))`. The base JSONRPC class calls them with only the `params` dict from the JSON body. The session ID is in the HTTP `Mcp-Session-Id` header, not in the JSON body.

**Solution:** Inject the session ID into the `params` dictionary before dispatch, or change the architecture. The cleanest approach that matches the existing pattern:

**Option A (Recommended): Inject session ID into params during dispatch**

In `process_request()`, before calling `process_action()`, inject the validated session ID into the request params:

```cpp
// In process_request(), just before Step 9 (JSON-RPC dispatch):
// Inject the MCP session ID into params so resource subscription handlers
// can access it. This is safe because we've already validated the session.
if (json_request.has("params") && json_request["params"].get_type() == Variant::DICTIONARY) {
    Dictionary params = json_request["params"];
    params["_mcp_session_id"] = session_id_header;
    json_request["params"] = params;
} else {
    Dictionary params;
    params["_mcp_session_id"] = session_id_header;
    json_request["params"] = params;
}
```

Then in the handlers:

```cpp
Dictionary MCPProtocol::handle_resources_subscribe(const Dictionary &p_params) {
    String uri = p_params.get("uri", "");
    String session_id = p_params.get("_mcp_session_id", "");

    if (uri.is_empty()) {
        // ... existing error handling ...
    }
    if (session_id.is_empty()) {
        Dictionary err;
        err["code"] = INVALID_PARAMS;
        err["message"] = "Internal error: missing session context";
        Dictionary response;
        response["error"] = err;
        return response;
    }

    return resource_registry.handle_subscribe(uri, session_id);
}
```

Same pattern for `handle_resources_unsubscribe()`.

**Option B (Alternative): Direct dispatch (bypass process_action)**

Handle `resources/subscribe` and `resources/unsubscribe` directly in `process_request()` before the `process_action()` call, similar to how `initialize`, `ping`, and `notifications/initialized` are handled. This is simpler but slightly different from the current pattern.

**Recommendation:** Option A is better because:
- It doesn't require special-casing more methods in process_request()
- The `_mcp_session_id` prefix convention makes it clear this is injected metadata
- Other handlers that might need session context in the future can use the same pattern
- It preserves the unified dispatch through process_action()

#### 3b. Wire `flush_sse_notifications()` to resource registry

The existing `flush_sse_notifications()` only delivers from `MCPSessionState::notification_queue`. We need to also pull resource change notifications from the registry and convert them to JSON-RPC notification messages.

Add a resource notification flush step at the **beginning** of `flush_sse_notifications()`:

```cpp
void MCPProtocol::flush_sse_notifications() {
    // Step 1: Pull resource change notifications from the registry
    // and convert them to JSON-RPC "notifications/resources/updated" messages.
    for (KeyValue<String, MCPSessionState> &E : sessions) {
        MCPSessionState &state = E.value;
        if (!state.initialized) {
            continue;
        }

        Vector<String> changed_uris = resource_registry.flush_notifications(state.session_id);
        for (const String &uri : changed_uris) {
            // MCP spec: "notifications/resources/updated" notification
            // { "jsonrpc": "2.0", "method": "notifications/resources/updated", "params": { "uri": "..." } }
            Dictionary notification;
            notification["jsonrpc"] = "2.0";
            notification["method"] = "notifications/resources/updated";
            Dictionary params;
            params["uri"] = uri;
            notification["params"] = params;

            String json_str = JSON::stringify(notification);
            if (state.notification_queue.size() < 1000) {
                state.notification_queue.push_back(json_str);
            }
        }
    }

    // Step 2: Deliver queued notifications to SSE streams (existing code).
    for (KeyValue<String, MCPSessionState> &E : sessions) {
        MCPSessionState &state = E.value;
        if (state.notification_queue.is_empty()) {
            continue;
        }
        // ... existing SSE delivery code ...
    }
}
```

#### 3c. Call `unsubscribe_all()` on session termination

In `terminate_session()`, add before `sessions.erase(session_id_header)`:

```cpp
// Clean up resource subscriptions for this session.
resource_registry.unsubscribe_all(session_id_header);
```

#### 3d. Call `unsubscribe_all()` on session GC

In `gc_stale_sessions()`, add before `sessions.erase(sessions_to_remove[i])`:

```cpp
// Clean up resource subscriptions for expired sessions.
resource_registry.unsubscribe_all(sessions_to_remove[i]);
```

#### 3e. Remove AGENT_06 stub comments

Remove all `AGENT_06` references and `stub_session` comments.

### 4. `mcp_resource_registry.h` -- comment updates

Update the section comments:
- `// -- Subscription tracking (stub -- delivery requires SSE) --` -> `// -- Subscription tracking --`
- `// -- Notification dispatch (queue only, delivery via SSE later) --` -> `// -- Notification dispatch --`

---

## Files NOT Changed

- **mcp_session.h/cpp** -- SSE transport already works. `queue_sse_event()` and `begin_sse_stream()` are correct.
- **mcp_types.h** -- No new constants needed.
- **mcp_protocol.h** -- No API changes. `flush_sse_notifications()` already declared.
- **mcp_tool_registry.h/cpp** -- Unrelated.
- **mcp_debugger_bridge.h/cpp** -- Unrelated.
- **tools/*.cpp** -- Unrelated.
- **mcp_server_plugin.cpp** -- Unrelated.

---

## Thread Safety Considerations

### Lock Ordering (Unchanged)

The existing convention is preserved: **subscription_mutex** is always acquired before **notification_mutex**. This prevents ABBA deadlocks between `notify_changed()` (which needs both) and `flush_notifications()` (which needs both).

### Poll-Thread-Only Data

`MCPSessionState::notification_queue`, the `sessions` HashMap, and the `clients` HashMap are only accessed from the editor main thread (via `poll()` called from `MCPServerPlugin::_process()`). They do NOT need mutex protection.

`MCPResourceRegistry::subscriptions` and `pending_per_session` are accessed from the poll thread (via flush/subscribe/unsubscribe) and potentially from resource change notifiers. They are protected by their respective mutexes.

### notify_changed() Call Sites

Currently `notify_changed()` is not called from anywhere in the codebase -- it's a public API waiting for event hooks. Future callers (e.g., file system watchers, debugger bridge state changes) may call from any thread. The mutex-protected implementation is safe for this.

---

## MCP Spec Compliance

### MCP 2025-06-18 Specification References

1. **resources/subscribe** (Section 6.4): Client subscribes to resource changes. Server stores the subscription per-session.

2. **resources/unsubscribe** (Section 6.4): Client removes a subscription.

3. **notifications/resources/updated** (Section 6.4): Server sends a JSON-RPC notification when a subscribed resource changes. The notification format:
   ```json
   {
     "jsonrpc": "2.0",
     "method": "notifications/resources/updated",
     "params": {
       "uri": "godot://game/status"
     }
   }
   ```

4. **Streamable HTTP Transport** (Section 8.2): Server-initiated notifications are delivered via SSE on the GET `/mcp` stream. The SSE event format:
   ```
   event: message
   data: {"jsonrpc":"2.0","method":"notifications/resources/updated","params":{"uri":"godot://game/status"}}

   ```

5. **Session lifecycle**: Subscriptions are implicitly scoped to the session. When a session terminates, all subscriptions are removed.

---

## Test Plan

### Manual Testing

1. **Single-session subscribe/notify:**
   - Initialize session, open SSE stream
   - Subscribe to `godot://game/status`
   - Start/stop game (triggers status change)
   - Verify SSE stream receives `notifications/resources/updated` with correct URI

2. **Multi-session isolation:**
   - Initialize two sessions (A, B) with SSE streams
   - Session A subscribes to `godot://game/status`
   - Session B subscribes to `godot://game/output`
   - Trigger status change -> only A gets notification
   - Trigger output change -> only B gets notification

3. **Session termination cleanup:**
   - Subscribe to a resource, then DELETE the session
   - Verify no crash, no leaked subscriptions
   - Re-subscribe with a new session -> works correctly

4. **Session expiry cleanup:**
   - Subscribe to a resource, let session timeout expire
   - Verify `gc_stale_sessions()` calls `unsubscribe_all()`
   - No leaked state in `pending_per_session`

5. **Unsubscribe:**
   - Subscribe, then unsubscribe
   - Trigger resource change
   - Verify NO notification delivered

---

## Implementation Order

1. **mcp_resource_registry.h** -- Change `pending_notifications` to `pending_per_session`
2. **mcp_resource_registry.cpp** -- Rewrite `notify_changed()`, `flush_notifications()`, `unsubscribe_all()`
3. **mcp_protocol.cpp** -- Inject session ID into params (3a)
4. **mcp_protocol.cpp** -- Update `handle_resources_subscribe()` and `handle_resources_unsubscribe()` (3a)
5. **mcp_protocol.cpp** -- Add resource flush step to `flush_sse_notifications()` (3b)
6. **mcp_protocol.cpp** -- Wire cleanup in `terminate_session()` and `gc_stale_sessions()` (3c, 3d)
7. **mcp_protocol.cpp** -- Remove all AGENT_06 / stub_session comments (3e)
8. **mcp_resource_registry.h** -- Update section comments (4)
9. **Compile and test**

---

## Estimated Scope

- **Files modified:** 3 (mcp_resource_registry.h, mcp_resource_registry.cpp, mcp_protocol.cpp)
- **Lines added:** ~50
- **Lines removed:** ~30
- **Net change:** ~+20 lines
- **Risk:** Low -- changes are localized to subscription/notification plumbing. SSE transport is already tested and working. No API changes visible to clients.
