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
| Q1 | Build the engine on 4.8 and re-establish the editor-side baseline | VERIFIED | Builds clean (9m46s, SCU, 4 cores, 0 errors). Module suite **74 cases / 526 assertions** on the merged code — identical to the 4.3 numbers, so the merge caused no module regression. Relay 64/64. Full engine suite **1491 cases**, one failure (`[IP] resolve_hostname`, no DNS in this container). It also turned up the intermittent SIGSEGV below, which is the whole reason this row existed. |
| Q2 | Observe `.github/workflows/godot_ai.yml` green on Actions (spec C1) | NOT_STARTED | Open for months because nobody pushed and looked. |
| Q3 | Accept both `text` and `content` on the two write tools | IMPLEMENTED | `MCPSchema::read_aliased_string`; 7 doctest subcases. Two spellings that *disagree* are refused rather than silently resolved. Not VERIFIED until the end-to-end script calls each tool with the other tool's spelling. |
| Q4 | `Godot_PromoteRuntimeValue` — write a live-tuned value back to the scene | NOT_STARTED | Small; overlaps D4. |
| Q5 | Grow the skill library past two skills | NOT_STARTED | Machinery outruns content: only `performance-profiling` and `scene-cleanup` ship. |

## Bugs this tranche has found

| # | Bug | Found by | Status |
|---|---|---|---|
| 1 | **`EditorFileSystem`'s destructor never cleared its own singleton.** The constructor sets `singleton = this`; the destructor did not unset it, so every later `EditorFileSystem::get_singleton()` returned freed memory. Engine bug, in `editor/file_system/editor_file_system.cpp`, not module code. | Q1 | **FIXED** — `if (singleton == this) { singleton = nullptr; }` |

How it presented: an intermittent SIGSEGV in the full engine suite, `pthread_mutex_lock`
on a mutex address of `0x651`, attributed by doctest to `test_mcp_tools.h:255`
`Godot_WriteTextFile`. The chain was
`WriteTextFileTool::run` → `scan_changes()` → `set_process(true)` →
`Node::_add_to_process_thread_group` → `SceneTree::_add_node_to_process_group` with
`this = 0x521`. The tests in `modules/gltf` and the GDScript LSP each `memnew` and then
`memdelete` an `EditorFileSystem`; from the first of those onward, the singleton was
dangling for the rest of the process, and the usual `if (get_singleton())` guard sailed
straight through it.

Three things about this are worth keeping:

- **A targeted suite is not a suite.** Twelve consecutive `--test-case="*[godot_ai]*"`
  runs were clean while the full suite crashed 8 times in 10. The module's own suite is
  what this project habitually runs and it cannot reach this class of defect at all.
- **gdb hid it.** Three runs under default gdb were clean because it disables ASLR;
  `set disable-randomization off` reproduced it on the first attempt. A debugger that
  makes a heap bug disappear is evidence about the bug, not absence of one.
- **The first guard was wrong and looked right.** Tightening the check from
  `get_singleton()` to `is_inside_tree()` (`mcp_editor_refresh.h`) did not help and the
  crash rate appeared to *rise* — because `is_inside_tree()` was itself reading freed
  memory. Rate changes on a heap bug are not signal. The helper was kept, since guarding
  on "in a tree" is still correct, but it was not the fix.

The thing worth remembering from it: **a targeted suite is not a suite.** Twelve
consecutive `--test-case="*[godot_ai]*"` runs were clean while the full suite was
crashing one time in four. The module's own suite is what this project habitually runs,
and it cannot see this class of defect at all.

The previous tranche found six real product bugs, all fixed and covered. Record new ones
here rather than in a commit message.
