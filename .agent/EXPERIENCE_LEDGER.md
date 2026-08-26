# Agent experience ledger

Derived from `docs/godot-ai-agent-experience-spec.md`: the tranche that makes the
agent's work visible, and composes the 72 existing primitives into workflows.

Statuses: `NOT_STARTED`, `IN_PROGRESS`, `IMPLEMENTED`, `VERIFIED`, `BLOCKED`.
`IMPLEMENTED` never means complete. `BLOCKED` is only for a genuinely external
condition — not for "hard", and not for "needs a display", which this repository
solves with `tools/virtual_display.py`.

**Verification rules specific to this tranche.**

1. A UI requirement is verified by finding the control with `Godot_FindControl` and
   pressing it with `Godot_SendEditorInput` under a real display, not by unit-testing
   the method behind the button.
2. A replay requirement is verified across *two* editor processes. Recording and
   replaying in one process proves nothing about the trace format.
3. Read the requirement, not the row. An audit of `INTERFACE_LEDGER.md` found five rows
   claiming "none remaining" while a named clause did not exist.

---

## E — Activity and legibility

| ID | Requirement | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|
| E1 | Live activity stream from the service | IMPLEMENTED | `mcp_activity.{h,cpp}`, hooks in `mcp_protocol.cpp` | `tests/test_mcp_activity.h` | 10 doctest cases pass: running→ok, refusals recorded complete, polling by sequence, capacity eviction, finishing an evicted record is a no-op, sequence numbers survive `clear()` | **a deferred tool's record closes as `deferred`, not when its work ends** — the protocol hands the caller a token and this layer is never told when it resolves. Closing that loop needs the token plumbed to the service's completion path |
| E2 | Activity dock renders the stream, in-flight call distinguished | NOT_STARTED | — | — | — | everything |
| E3 | Each record names the node paths and `res://` paths it touched | IMPLEMENTED | `MCPActivity::extract_subjects`, `MCPTool::get_activity_subjects` | `tests/test_mcp_activity.h` | file and node subjects extracted, prose rejected as a node path, the same path under two keys reported once, arguments not mutated by the read | **heuristic for every tool** — the default reads argument keys it does not own. No tool overrides `get_activity_subjects()` yet; the ones that resolve a node by search know a path the arguments never carry |
| E4 | Selecting a record reveals its subjects in the Scene and FileSystem docks | NOT_STARTED | — | — | — | everything |
| E5 | Checkpoint scrubber with per-record diff and revert | NOT_STARTED | — | — | — | `Godot_DiffCheckpoint` and `Godot_RestoreCheckpoint` exist; the timeline UI does not |
| E6 | Activity survives a dock restart, not an editor restart | NOT_STARTED | — | — | — | everything |
| E7 | `Godot_GetActivity` exposes the same stream as a tool | NOT_STARTED | — | — | — | everything |

## S — Sessions: record and replay

| ID | Requirement | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|
| S1 | `Godot_RecordSession` start/stop | NOT_STARTED | — | — | — | everything |
| S2 | Trace is frame-locked, not wall-clock | NOT_STARTED | — | — | — | `input_trace` already carries the frame; the recorder does not exist |
| S3 | `Godot_AssertRuntimeState` captures assertions during recording | NOT_STARTED | — | — | — | everything |
| S4 | `Godot_ReplaySession` re-injects and reports first divergence | NOT_STARTED | — | — | — | everything |
| S5 | Speed multiplier, with the achieved rate reported | NOT_STARTED | — | — | — | `Godot_SetTimeScale` exists; the honesty check does not |
| S6 | Non-determinism reported as `indeterminate`, never as `passed` | NOT_STARTED | — | — | — | the hard requirement of this group; do not skip it to make S4 look green |
| S7 | `Godot_ListSessions` | NOT_STARTED | — | — | — | everything |
| S8 | JSONL under `user://godot_ai_sessions/`, reading guide in the reply | NOT_STARTED | — | — | — | mirror `mcp_profiler_recorder.cpp`'s export |
| S9 | A playtest crash emits a replay file | NOT_STARTED | — | — | — | depends on P3 |

## P — Playtest sessions

| ID | Requirement | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|
| P1 | `Godot_StartPlaytest` with goal, budget, oracle; returns a handle | NOT_STARTED | — | — | — | everything |
| P2 | The perceive/decide/act loop runs editor-side over MCP sampling | NOT_STARTED | — | — | — | `mcp_chat.cpp` already does sampling; the loop does not exist |
| P3 | `Godot_GetPlaytestReport` with crashes, spikes, coverage, verdict | NOT_STARTED | — | — | — | every input exists as a primitive; nothing aggregates |
| P4 | Stoppable mid-run, reports partial results | NOT_STARTED | — | — | — | cancellation is proven for chat turns; reuse that path |
| P5 | Playtest input appears in the activity stream and input trace | NOT_STARTED | — | — | — | depends on E1 |

## D — Design conversation

| ID | Requirement | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|
| D1 | `Godot_ProposeChange` renders a per-item Apply/Reject checklist | NOT_STARTED | — | — | — | everything |
| D2 | Apply goes through `EditorUndoRedoManager`, one transaction per item | NOT_STARTED | — | — | — | everything |
| D3 | `Godot_OfferVariants`, each behind a checkpoint, switchable live | NOT_STARTED | — | — | — | everything |
| D4 | Promote a variant to the scene, or discard all, in one call | NOT_STARTED | — | — | — | overlaps the `Godot_PromoteRuntimeValue` quick win |

---

## Quick wins, tracked here so they do not get lost

| ID | Item | Status | Notes |
|---|---|---|---|
| Q1 | Build the engine on 4.8 and re-establish the editor-side baseline | IN_PROGRESS | Nothing has linked the module since `117870273`. Gates every code row above. |
| Q2 | Observe `.github/workflows/godot_ai.yml` green on Actions (spec C1) | NOT_STARTED | Open for months because nobody pushed and looked. |
| Q3 | Accept both `text` and `content` on the two write tools | NOT_STARTED | Currently documented as a wart in the template's AGENTS.md. |
| Q4 | `Godot_PromoteRuntimeValue` — write a live-tuned value back to the scene | NOT_STARTED | Small; overlaps D4. |
| Q5 | Grow the skill library past two skills | NOT_STARTED | Machinery outruns content: only `performance-profiling` and `scene-cleanup` ship. |

## Bugs this tranche has found

Nothing yet. The previous tranche found six real product bugs, all fixed and covered;
expect this one to find its own, and record them here rather than in a commit message.
