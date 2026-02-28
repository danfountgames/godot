# PLAN: Memory / ObjectDB Profiling Tools

**Branch:** `mcp-server`
**Priority:** P0 — Highest impact, unique capability
**Effort:** Medium (3-5 days)
**Dependencies:** `modules/objectdb_profiler/` (already in 4.6 fork)

---

## Motivation

The MCP server already has `runtime/get_session_summary` which internally calls
`bridge->send_get_performance()` and returns orphan count, object count, FPS, etc.
But this data is:

1. **Buried** inside a monolithic summary — the LLM can't ask "just give me memory stats"
2. **Point-in-time only** — no way to snapshot, diff, or trend
3. **Shallow** — just counters, no per-class breakdown or object-level detail

Meanwhile, the Godot 4.6 fork already has `modules/objectdb_profiler/` with:

- `SnapshotCollector::snapshot_objects()` — captures ALL live objects from `ObjectDB`
- `GameStateSnapshot` — deserialized snapshot with per-object data
- `SnapshotDataObject` — per-object: class name, properties, inbound/outbound references
- `ObjectDBProfilerDebuggerPlugin` — editor-side capture of snapshot data via debugger messages

**We are compiled into this engine. We can call these APIs directly from C++.**
No other MCP server can do this.

---

## New File

```
tools/mcp_memory_tools.h
tools/mcp_memory_tools.cpp
```

---

## New Tools (7 tools)

### Tool 1: `memory/get_stats`

**Purpose:** Quick, lightweight memory & object health check.

**Parameters:** None (game must be running)

**Returns:**
```json
{
  "fps": 60.1,
  "frame_time_msec": 16.6,
  "memory": {
    "static_bytes": 52428800,
    "static_mb": 50.0,
    "static_max_mb": 55.2,
    "video_mem_mb": 128.4
  },
  "objects": {
    "total": 4521,
    "nodes": 3800,
    "orphan_nodes": 47,
    "resources": 821
  },
  "physics": {
    "active_objects_2d": 12,
    "collision_pairs_2d": 34,
    "active_objects_3d": 0,
    "collision_pairs_3d": 0
  },
  "render": {
    "objects_in_frame": 156,
    "draw_calls": 89,
    "primitives_in_frame": 23456
  }
}
```

**Implementation:**
```cpp
// Extend the game-side handler to return ALL Performance monitors.
// Current mcp:get_performance only sends 7 values.
// New message: mcp:get_memory_stats returns ~20 values.
//
// Game-side GDScript (injected via existing bridge pattern):
//   var stats = []
//   stats.append(Performance.get_monitor(Performance.TIME_FPS))
//   stats.append(Performance.get_monitor(Performance.TIME_PROCESS))
//   stats.append(Performance.get_monitor(Performance.MEMORY_STATIC))
//   stats.append(Performance.get_monitor(Performance.MEMORY_STATIC_MAX))
//   stats.append(Performance.get_monitor(Performance.OBJECT_COUNT))
//   stats.append(Performance.get_monitor(Performance.OBJECT_NODE_COUNT))
//   stats.append(Performance.get_monitor(Performance.OBJECT_ORPHAN_NODE_COUNT))
//   stats.append(Performance.get_monitor(Performance.OBJECT_RESOURCE_COUNT))
//   stats.append(Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME))
//   stats.append(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME))
//   stats.append(Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME))
//   stats.append(Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED))
//   stats.append(Performance.get_monitor(Performance.PHYSICS_2D_ACTIVE_OBJECTS))
//   stats.append(Performance.get_monitor(Performance.PHYSICS_2D_COLLISION_PAIRS))
//   stats.append(Performance.get_monitor(Performance.PHYSICS_3D_ACTIVE_OBJECTS))
//   stats.append(Performance.get_monitor(Performance.PHYSICS_3D_COLLISION_PAIRS))
//   EngineDebugger.send_message("mcp:memory_stats_result", stats)
//
// Bridge-side: parse into structured Dictionary.
```

**LLM description:**
> Quick memory and performance health check. Returns object counts (total, nodes,
> orphan nodes, resources), memory usage in bytes and MB, render stats (draw calls,
> primitives), and physics stats. Use this as the first step when investigating
> performance issues. If orphan_nodes > 0, nodes are leaking — follow up with
> memory/get_orphans. Much lighter than runtime/get_session_summary.

**Annotations:** `readOnly=true, destructive=false, idempotent=true`

---

### Tool 2: `memory/get_orphans`

**Purpose:** Trigger `Node.print_orphan_nodes()` and capture the output.

**Parameters:** None (game must be running)

**Implementation:**
```cpp
// 1. Note the current output ring buffer cursor.
// 2. Send evaluate: "Node.print_orphan_nodes()"
// 3. Wait a few frames for output to flush.
// 4. Read new output lines since the saved cursor.
// 5. Parse lines matching "STRAY NODE:" pattern.
// 6. Return structured orphan list.
```

**Returns:**
```json
{
  "orphan_count": 47,
  "orphans": [
    {"name": "BulletTrail", "class": "GPUParticles2D", "object_id": 12345, "count": 32},
    {"name": "HitEffect", "class": "AnimatedSprite2D", "object_id": 67890, "count": 15}
  ],
  "suggestion": "32 BulletTrail orphans suggest remove_child() without queue_free() in bullet lifecycle."
}
```

**LLM description:**
> Detect and list orphan nodes in the running game. Orphan nodes are the #1 source
> of memory leaks in Godot — nodes removed from the tree with remove_child() but
> never freed with queue_free(). Returns a structured list of orphan classes and
> counts. Use after memory/get_stats shows orphan_nodes > 0.

**Annotations:** `readOnly=true, destructive=false, idempotent=true`

---

### Tool 3: `memory/take_snapshot`

**Purpose:** Full ObjectDB snapshot — every live object with class, properties, references.

**Parameters:**
- `label` (string, required): Human-readable label (e.g., "before_boss_fight")

**Implementation — Option A (C++ direct, preferred):**
```cpp
// We're in the editor process. The objectdb_profiler module is loaded.
// We can call SnapshotCollector::snapshot_objects() to trigger a snapshot
// on the game side via the existing debugger message protocol.
//
// 1. Send "objectdb_snapshot:snapshot" message to game via debugger session.
// 2. Game-side SnapshotCollector iterates ObjectDB::debug_objects().
// 3. Data streams back in chunks (6MB each) via "objectdb_snapshot:snapshot_data".
// 4. Editor-side reassembles into GameStateSnapshot.
// 5. We intercept this data (or read from the saved .odb_snapshot file).
// 6. Store a reference keyed by `label`.
//
// The objectdb_profiler module already handles all the chunking/reassembly.
// We just need to:
//   a. Trigger a snapshot (send the right debugger message)
//   b. Wait for completion
//   c. Read the resulting GameStateSnapshot
//   d. Serialize a summary to JSON for the LLM

// Since the existing ObjectDBProfilerDebuggerPlugin captures to
// user://objectdb_snapshots/, we can also read from there.
```

**Implementation — Option B (lightweight GDScript fallback):**
```cpp
// If we don't want to depend on the objectdb_profiler module:
// 1. Use bridge->send_evaluate() to collect Performance counters.
// 2. Use bridge->request_scene_tree() for full node hierarchy.
// 3. Combine into our own snapshot format.
// 4. Store locally in HashMap<String, Dictionary> keyed by label.
//
// This is simpler but less detailed (no RefCounted tracking, no references graph).
```

**Recommended approach:** Option A for rich data, Option B as fallback when
objectdb_profiler module is not enabled.

**Returns:**
```json
{
  "label": "before_boss_fight",
  "timestamp": "2026-02-14T10:30:00",
  "summary": {
    "total_objects": 4521,
    "total_nodes": 3800,
    "orphan_nodes": 47,
    "total_resources": 821,
    "memory_static_mb": 50.0,
    "top_classes": [
      {"class": "Node2D", "count": 1200},
      {"class": "Sprite2D", "count": 890},
      {"class": "GPUParticles2D", "count": 340},
      {"class": "AnimatedSprite2D", "count": 280}
    ]
  },
  "stored": true,
  "tip": "Take another snapshot after an action, then use memory/diff to compare."
}
```

**LLM description:**
> Capture a full memory snapshot of the running game's object database. Labels the
> snapshot for later comparison. Includes total object/node/resource counts, memory
> usage, and a class-level breakdown of the most common object types. Use the
> snapshot→action→snapshot→diff workflow to find leaks:
> 1. memory/take_snapshot label="before"
> 2. (game does something)
> 3. memory/take_snapshot label="after"
> 4. memory/diff label_a="before" label_b="after"

**Annotations:** `readOnly=true, destructive=false, idempotent=false`

---

### Tool 4: `memory/diff`

**Purpose:** Compare two snapshots, auto-detect leaks.

**Parameters:**
- `label_a` (string, required): First (earlier) snapshot label
- `label_b` (string, required): Second (later) snapshot label

**Implementation:**
```cpp
// If using Option A (ObjectDB snapshots):
//   Load both GameStateSnapshot objects.
//   Compare object sets:
//     - Objects in B but not in A = created
//     - Objects in A but not in B = freed
//     - Group by class for the delta
//
// If using Option B (lightweight):
//   Compare stored Dictionaries:
//     - Delta on all counter fields
//     - Delta on scene tree nodes (flatten + set diff)
//     - Delta on group memberships
//
// In both cases, run automated warning analysis:

// Warnings engine:
struct Warning {
    String severity;  // "HIGH", "MEDIUM", "LOW", "OK"
    String type;      // "orphan_leak", "object_growth", "resource_growth", "memory_spike"
    String message;   // Human-readable explanation
    String fix;       // Concrete fix suggestion
};

// Rules:
// orphan_nodes_delta > 0        → HIGH / orphan_leak
// object_count_delta > 100      → MEDIUM / object_growth
// resource_count_delta > 50     → MEDIUM / resource_growth
// memory_delta_mb > 50          → HIGH / memory_spike
// (configurable thresholds via parameters)
```

**Returns:**
```json
{
  "label_a": "before_boss_fight",
  "label_b": "after_boss_fight",
  "time_between": "45.2s",
  "delta": {
    "objects": {"total": +340, "nodes": +280, "orphans": +47, "resources": +13},
    "memory_mb": +12.4,
    "top_growing_classes": [
      {"class": "GPUParticles2D", "before": 10, "after": 350, "delta": +340},
      {"class": "AnimatedSprite2D", "before": 5, "after": 52, "delta": +47}
    ],
    "top_shrinking_classes": []
  },
  "warnings": [
    {
      "severity": "HIGH",
      "type": "orphan_leak",
      "message": "47 new orphan nodes. GPUParticles2D grew by 340 instances.",
      "fix": "Check BulletTrail lifecycle — likely remove_child() without queue_free()."
    }
  ]
}
```

**LLM description:**
> Compare two previously-taken memory snapshots and identify what changed. Returns
> deltas for object counts, memory usage, and per-class instance counts. Automatically
> detects leak patterns and provides fix suggestions. Warning severities: HIGH (definite
> leak), MEDIUM (suspicious growth), LOW (minor concern), OK (healthy).

**Annotations:** `readOnly=true, destructive=false, idempotent=true`

---

### Tool 5: `memory/track_trend`

**Purpose:** Take N performance samples over time, compute growth rates.

**Parameters:**
- `samples` (integer, optional, default 10): Number of samples
- `interval_ms` (integer, optional, default 2000): Milliseconds between samples
- `max_duration_ms` (integer, optional, default 30000): Hard timeout

**Implementation:**
```cpp
// This is a long-running tool → use ProgressHandler.
// 1. For i in 0..samples:
//    a. Call bridge->send_get_performance() (or the extended stats version)
//    b. Record timestamp + all counters
//    c. Report progress via p_ctx->report_progress(i, samples, "Sample N of M")
//    d. Sleep interval_ms (or use bridge->send_wait_frames(interval_frames))
//    e. Check p_ctx->is_cancelled()
// 2. Compute trend:
//    - Linear regression on object_count over time → objects/sec growth rate
//    - Same for memory, orphans, nodes
//    - Classify: "stable" (growth < 1/sec), "concerning" (1-10/sec), "leaking" (>10/sec)
```

**Returns:**
```json
{
  "samples": [...],
  "trend": {
    "status": "leaking",
    "elapsed_ms": 20000,
    "objects": {"start": 4000, "end": 4800, "growth": 800, "per_second": 40.0},
    "orphans": {"start": 0, "end": 47, "growth": 47, "per_second": 2.35},
    "memory_mb": {"start": 50.0, "end": 62.4, "growth": 12.4, "per_second": 0.62},
    "fps": {"start": 60, "end": 45, "change": -15},
    "concerns": [
      "Objects growing at 40.0/sec — unbounded spawning",
      "FPS dropped by 15 — leak causing performance degradation"
    ]
  }
}
```

**LLM description:**
> Poll memory counters repeatedly over a time period and compute growth trends.
> Returns all raw samples plus automated trend analysis. Status values: "stable"
> (no growth), "concerning" (mild growth), "leaking" (definite unbounded growth).
> Use this to confirm a suspected leak is ongoing, not just a one-time allocation.
> Longer sampling periods give more confident results.

**Annotations:** `readOnly=true, destructive=false, idempotent=true`
**Progress handler:** Yes (long-running, use SSE)

---

### Tool 6: `memory/detect_leaks`

**Purpose:** One-shot leak detection — snapshot, wait, snapshot, diff, diagnose.

**Parameters:**
- `duration_seconds` (number, optional, default 10): How long to let the game run between snapshots

**Implementation:**
```cpp
// 1. Take snapshot A (internal, not saved to disk unless requested)
// 2. Wait duration_seconds (via bridge->send_wait_frames or timer)
// 3. Take snapshot B
// 4. Run diff + warning analysis
// 5. Return combined result
//
// Long-running tool → ProgressHandler
```

**LLM description:**
> All-in-one leak detection. Takes a snapshot, lets the game run for the specified
> duration, takes another snapshot, diffs them, and returns a diagnosis. This is the
> "easy button" — use it when you want a quick answer to "is there a memory leak?"
> For more control, use the manual take_snapshot → diff workflow.

**Annotations:** `readOnly=true, destructive=false, idempotent=false`
**Progress handler:** Yes

---

### Tool 7: `memory/class_breakdown`

**Purpose:** Get a per-class breakdown of all live objects (requires ObjectDB access).

**Parameters:**
- `sort_by` (string, optional, default "count"): "count" or "class"
- `limit` (integer, optional, default 30): Max classes to return

**Implementation:**
```cpp
// Option A: Direct ObjectDB access (C++)
// ObjectDB::debug_objects() iterates every object.
// We can call this from the editor side IF the game is paused.
// BUT ObjectDB is in the game process, not the editor process.
//
// Option B: Via debugger message
// Send a message to the game asking it to iterate all objects and group by class.
// Game-side handler:
//   var class_counts = {}
//   # ObjectDB::debug_objects is not exposed to GDScript.
//   # BUT we can approximate via Performance counters + scene tree analysis.
//
// Option C: Via ObjectDB profiler module
// Trigger a full snapshot, then summarize by class from the GameStateSnapshot.
// Most accurate but heaviest.
//
// Recommended: Option C when available, Option B (approximate) as fallback.
```

**Returns:**
```json
{
  "total_objects": 4521,
  "classes": [
    {"class": "Node2D", "count": 1200, "percentage": 26.5},
    {"class": "Sprite2D", "count": 890, "percentage": 19.7},
    {"class": "Resource", "count": 450, "percentage": 10.0},
    {"class": "GPUParticles2D", "count": 340, "percentage": 7.5}
  ]
}
```

**LLM description:**
> Get a breakdown of all live objects in the game grouped by class. Shows which
> classes have the most instances. Useful for identifying unexpected object
> accumulation — if "BulletTrail" has 10,000 instances, something isn't cleaning up.

**Annotations:** `readOnly=true, destructive=false, idempotent=true`

---

## Bridge Changes

### New debugger messages (game → editor):

```
"mcp:memory_stats_result"   → Extended performance data (20+ fields)
"mcp:orphan_list_result"    → Parsed orphan node list
"mcp:class_breakdown_result" → Per-class object counts (if using Option B)
```

### New bridge methods:

```cpp
Dictionary send_get_memory_stats(int p_timeout_msec = 5000);
Dictionary send_get_orphan_list(int p_timeout_msec = 10000);
Dictionary send_get_class_breakdown(int p_timeout_msec = 15000);
```

### Snapshot storage:

```cpp
// In-memory snapshot store (keyed by label)
HashMap<String, Dictionary> snapshot_store;  // For lightweight snapshots
// OR
HashMap<String, Ref<GameStateSnapshot>> snapshot_store;  // For full ObjectDB snapshots
```

---

## Integration with ObjectDB Profiler Module

The key question: **how tightly to couple with `modules/objectdb_profiler/`.**

### Option A: Direct dependency (tight coupling)
```cpp
// In config.py:
env.module_add_dependencies("mcp_server", ["jsonrpc", "gdscript", "objectdb_profiler"])

// In mcp_memory_tools.cpp:
#include "modules/objectdb_profiler/snapshot_collector.h"
#include "modules/objectdb_profiler/editor/snapshot_data.h"
```

**Pro:** Full access to GameStateSnapshot, per-object reference graphs, class hierarchy.
**Con:** Hard dependency — MCP server breaks if objectdb_profiler is disabled.

### Option B: Optional dependency (loose coupling) ← Recommended
```cpp
// In config.py: No new dependency.

// In mcp_memory_tools.cpp:
#ifdef MODULE_OBJECTDB_PROFILER_ENABLED
#include "modules/objectdb_profiler/snapshot_collector.h"
#include "modules/objectdb_profiler/editor/snapshot_data.h"
#endif

// Use rich data when available, fall back to Performance counters when not.
```

**Pro:** Graceful degradation. MCP server works with or without objectdb_profiler.
**Con:** Two code paths to maintain.

---

## Game-Side Script Additions

The existing bridge injects a game-side handler script. We need to extend it
(or add a companion) for memory-specific messages:

```gdscript
# Handles "mcp:get_memory_stats" from editor
func _handle_get_memory_stats():
    var stats = []
    stats.append(Performance.get_monitor(Performance.TIME_FPS))
    stats.append(Performance.get_monitor(Performance.TIME_PROCESS))
    stats.append(Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS))
    stats.append(Performance.get_monitor(Performance.MEMORY_STATIC))
    stats.append(Performance.get_monitor(Performance.MEMORY_STATIC_MAX))
    stats.append(Performance.get_monitor(Performance.OBJECT_COUNT))
    stats.append(Performance.get_monitor(Performance.OBJECT_NODE_COUNT))
    stats.append(Performance.get_monitor(Performance.OBJECT_ORPHAN_NODE_COUNT))
    stats.append(Performance.get_monitor(Performance.OBJECT_RESOURCE_COUNT))
    stats.append(Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME))
    stats.append(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME))
    stats.append(Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME))
    stats.append(Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED))
    stats.append(Performance.get_monitor(Performance.PHYSICS_2D_ACTIVE_OBJECTS))
    stats.append(Performance.get_monitor(Performance.PHYSICS_2D_COLLISION_PAIRS))
    stats.append(Performance.get_monitor(Performance.PHYSICS_3D_ACTIVE_OBJECTS))
    stats.append(Performance.get_monitor(Performance.PHYSICS_3D_COLLISION_PAIRS))
    stats.append(Performance.get_monitor(Performance.AUDIO_OUTPUT_LATENCY))
    EngineDebugger.send_message("mcp:memory_stats_result", stats)
```

---

## Testing

New test file: `tests/test_memory_tools.py`

```python
class TestMemoryStats:
    def test_get_stats_while_running(self, client, running_game):
        result = client.call_tool("memory/get_stats")
        assert result["objects"]["total"] > 0
        assert result["objects"]["nodes"] > 0
        assert result["memory"]["static_mb"] > 0

    def test_get_stats_while_stopped(self, client):
        result = client.call_tool("memory/get_stats")
        assert result["isError"]

class TestSnapshots:
    def test_take_and_diff(self, client, running_game):
        client.call_tool("memory/take_snapshot", {"label": "a"})
        # Spawn some objects via evaluate
        client.call_tool("runtime/evaluate", {"expression": "for i in 100: Node2D.new()"})
        client.call_tool("runtime/wait_frames", {"frames": 5})
        client.call_tool("memory/take_snapshot", {"label": "b"})
        diff = client.call_tool("memory/diff", {"label_a": "a", "label_b": "b"})
        assert diff["delta"]["objects"]["total"] >= 100
        assert any(w["type"] == "object_growth" for w in diff["warnings"])

class TestLeakDetection:
    def test_detect_leaks_healthy(self, client, running_game):
        result = client.call_tool("memory/detect_leaks", {"duration_seconds": 3})
        # Idle game shouldn't leak
        assert any(w["severity"] == "OK" for w in result["diagnosis"])
```

---

## File Changes Summary

| File | Change |
|------|--------|
| `tools/mcp_memory_tools.h` | **NEW** — class MCPMemoryTools |
| `tools/mcp_memory_tools.cpp` | **NEW** — 7 tools + handlers |
| `mcp_protocol.cpp` | Add `MCPMemoryTools::register_tools(&tool_registry);` |
| `mcp_debugger_bridge.h` | Add `send_get_memory_stats()`, `send_get_orphan_list()`, snapshot store |
| `mcp_debugger_bridge.cpp` | Handle new messages, implement new bridge methods |
| `config.py` | No change (optional objectdb_profiler via #ifdef) |
| `tests/test_memory_tools.py` | **NEW** — memory tool tests |
| `README.md` | Update tool count, add memory tools section |
