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
| E1 | Live activity stream from the service | VERIFIED | `mcp_activity.{h,cpp}`, hooks in `mcp_protocol.cpp` | `tests/test_mcp_activity.h` | 10 doctest cases, plus `run_editor_e2e.py`: 51 records after a real session, every outcome in the allowed set, polling by `after_sequence` returns only newer records | **a deferred tool's record closes as `deferred`, not when its work ends** — the protocol hands the caller a token and this layer is never told when it resolves. Closing that loop needs the token plumbed to the service's completion path |
| E2 | Activity dock renders the stream, in-flight call distinguished | NOT_STARTED | — | — | — | everything |
| E3 | Each record names the node paths and `res://` paths it touched | VERIFIED | `MCPActivity::extract_subjects`, `MCPTool::get_activity_subjects` | `tests/test_mcp_activity.h` | doctest for extraction, and `run_editor_e2e.py` asserts real calls carried file subjects — so the dock will have something to reveal | **heuristic for every tool** — the default reads argument keys it does not own. No tool overrides `get_activity_subjects()` yet; the ones that resolve a node by search know a path the arguments never carry |
| E4 | Selecting a record reveals its subjects in the Scene and FileSystem docks | NOT_STARTED | — | — | — | everything |
| E5 | Checkpoint scrubber with per-record diff and revert | NOT_STARTED | — | — | — | `Godot_DiffCheckpoint` and `Godot_RestoreCheckpoint` exist; the timeline UI does not |
| E6 | Activity survives a dock restart, not an editor restart | NOT_STARTED | — | — | — | everything |
| E7 | `Godot_GetActivity` exposes the same stream as a tool | VERIFIED | `tools/mcp_activity_tools.cpp` | `run_editor_e2e.py` | called against a live editor; reply shape, sequence polling and refusal recording all asserted | none |

## S — Sessions: record and replay

| ID | Requirement | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|
| S1b | Recording covers **editor-injected input only**; nothing may imply a human can author a trace by playing | VERIFIED | `mcp_sessions.{h,cpp}` | `tests/test_mcp_sessions.h` | stamped in the meta, and `run_editor_e2e.py` asserts the *tool reply* carries the note, so a caller cannot miss it | none — but keep it true as the tools get written |
| S1 | `Godot_RecordSession` start/stop | VERIFIED | `mcp_sessions.{h,cpp}`, `tools/mcp_session_tools.cpp` | `tests/test_mcp_sessions.h`, `run_editor_e2e.py` | live: opens as the expected slug, refuses a second concurrent recording, captures injected input, closes with counts | none |
| S2 | Trace is frame-locked, not wall-clock | VERIFIED | `MCPSessions::append_events` | `tests/test_mcp_sessions.h` | an event with no `frame` is refused at write time, not discovered at replay time | none |
| S3 | `Godot_AssertRuntimeState` captures assertions during recording | VERIFIED | `tools/mcp_session_tools.cpp` | `tests/test_mcp_sessions.h`, `run_editor_e2e.py` | reads a real property out of the running game and stores it against the frame it was observed at | none |
| S4 | `Godot_ReplaySession` re-injects and reports first divergence | IMPLEMENTED | `mcp_replay.{h,cpp}`, `tools/mcp_session_tools.cpp` | `tests/test_mcp_replay.h` (11 cases), `run_editor_e2e.py` | live replay against a running game returned `failed` naming `press_count` with both the recorded and the observed value | **not VERIFIED, by this ledger's own rule 2**: the end-to-end run records and replays inside one editor process. Recording in one process and replaying in another is what proves the trace format, and nothing does that yet |
| S5 | Speed multiplier, with the achieved rate reported | NOT_STARTED | — | — | — | everything. `Godot_ReplaySession` takes no `speed` argument at all — it was not built, and no row here should suggest otherwise |
| S6 | Non-determinism reported as `indeterminate`, never as `passed` | IMPLEMENTED | `MCPReplayPlan::get_verdict` | `tests/test_mcp_replay.h` | a run that drifts past tolerance returns `indeterminate` with the frame count and a note saying it must not be read as a pass; ordinary jitter still passes; a divergence outranks drift | the live path has not produced an `indeterminate` yet — the e2e run diverged first. Forcing real drift against a running game is what would verify it |
| S7 | `Godot_ListSessions` | VERIFIED | `tools/mcp_session_tools.cpp` | `tests/test_mcp_sessions.h`, `run_editor_e2e.py` | lists the session the same run just recorded | none |
| S8 | JSONL under `user://godot_ai_sessions/`, reading guide in the reply | VERIFIED | `mcp_sessions.cpp` | `tests/test_mcp_sessions.h` | `meta.json` + `trace.jsonl` + `asserts.jsonl`; a newline inside a value survives the round trip; removal refuses any slug escaping the root; the reading guide ships in the stop and replay replies | none |
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

## W — The agent workspace (multi-instance embedding)

Derived from the user's *GodotAI Agent Workspace* specification. Phase Zero is DEC-0011.

| ID | Requirement | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|
| W0 | Prove several game processes can embed at once | VERIFIED | throwaway patch, reverted | manual spike | two games rendered inside one editor window; `.agent/evidence/spike_two_embedded_processes.png` | Linux/X11 only. **macOS is expected to fail** and the reason is now specific — see `.agent/MACOS_EMBEDDING_SPIKE.md`. Windows uses the same PID-keyed reparenting as X11 and should behave like it; Wayland unmeasured |
| W1 | One implementation of the embed command line, reusable by any host | IMPLEMENTED | `embedded_process_apply_arguments()` in `editor/run/embedded_process.cpp` | `run_editor_e2e.py` | extracted from `GameView`, which now delegates to it; the end-to-end run still embeds and drives a real game | not yet called by anything but `GameView` |
| W2 | Several owners can shape a launching instance's arguments | IMPLEMENTED | `EditorRun::add/remove_instance_starting_callback` | `run_editor_e2e.py` | was a single callback slot, so whoever registered last silently displaced the other; now a list called in registration order | no second listener registered yet |
| W3 | Agent-owned processes are distinguishable from the user's own run | IMPLEMENTED | `mcp_runtime_instances.{h,cpp}` | `tests/test_mcp_runtime_instances.h` | 10 cases: registration before launch, pid binding, one pid cannot be claimed twice, a closed instance keeps its record but releases its pid, grouping, serialisation | nothing launches through it yet |
| W4 | Debugger control targets one instance instead of broadcasting | VERIFIED | `MCPRuntimeInstances::set_suspended/next_frame/set_time_scale/set_muted` | `tests/test_mcp_runtime_instances.h` | routes by `ScriptEditorDebugger::get_remote_pid()`, the same join `GameView` already uses; refuses rather than broadcasting when it cannot resolve a session | none. The three-variant slice pauses one of three live instances against a real editor, gets `applied: true`, and finds all three still running afterwards |
| W5 | A GodotAI main-screen workspace hosting N embedded tiles | IMPLEMENTED | `mcp_workspace.{h,cpp}`, `tools/mcp_workspace_tools.cpp` | `.agent/evidence/spike_three_variants.py` | a `GodotAI` main screen beside 2D/3D/Script/Game, one `EmbeddedProcess` per tile, each with its own header, status line and Take Control / Pause / Stop. Launch goes through `EditorRun::build_base_arguments()` and `embedded_process_apply_arguments()`, the same builders the play button uses | **tile chrome is overdrawn** — see the z-order note below. No focus layout, no tray, no pinning, no archived result cards |
| W6 | First vertical slice: compare three live runtime variants | VERIFIED | `Godot_LaunchInstance`, `Godot_ListInstances`, `Godot_ControlInstance`, `Godot_StopAllInstances` | `.agent/evidence/spike_three_variants.py` | three isolated processes launched and embedded alongside the user's own game; each got its own id; pausing one reported `applied` and left all three running; stopping one left two; `Godot_StopAllInstances` cleared the agent's three and **the user's own game survived**. Screenshot: `.agent/evidence/spike_three_variants.png` | no promote-the-winner step yet — that is the live-tuning slice |
| W8 | Embedded windows draw over the tile chrome | NOT_STARTED | — | — | `.agent/evidence/spike_three_variants.png` | **Found by W6.** An embedded game is a native child window, so it renders above the editor's own controls: a neighbouring game covers the header, status line and buttons of the tiles around it. `GameView` never hits this because it has exactly one embedder filling the screen. The workspace needs chrome laid out clear of every embedded rect - a header strip above the grid rather than inside each tile, or insets the embedders never occupy |
| W7 | macOS multi-embedding | BLOCKED | — | — | source analysis only | Needs a Mac. `GameViewDebuggerMacOS` holds one `EmbeddedProcessMacOS` and its handler type drops the `p_session` that `capture()` receives, so N games' `game_view:set_context_id` messages all land on one embedder and the last wins. macOS needs that handshake because it shares a CALayer context instead of reparenting a window. Full spike plan and fix sketch in `.agent/MACOS_EMBEDDING_SPIKE.md` |

## Quick wins, tracked here so they do not get lost

| ID | Item | Status | Notes |
|---|---|---|---|
| Q1 | Build the engine on 4.8 and re-establish the editor-side baseline | VERIFIED | Builds clean (9m46s, SCU, 4 cores, 0 errors). Module suite **74 cases / 526 assertions** on the merged code — identical to the 4.3 numbers, so the merge caused no module regression. Relay 64/64. Full engine suite **1491 cases**, one failure (`[IP] resolve_hostname`, no DNS in this container). It also turned up the intermittent SIGSEGV below, which is the whole reason this row existed. |
| Q2 | Observe `.github/workflows/godot_ai.yml` green on Actions (spec C1) | UNBLOCKED, not done | **The likely reason it never went green is now fixed.** `run_editor_e2e.py` had never passed on Linux: its drag test sent a fixed 550→850 while a game under a virtual display gets an 846-wide window, so the drag was refused for ending one pixel outside it and the run failed for a reason unrelated to dragging. CI runs exactly that script on exactly that kind of display. The ends are now clamped to the window the game actually got. Still needs someone to push and watch a run. |
| Q3 | Accept both `text` and `content` on the two write tools | IMPLEMENTED | `MCPSchema::read_aliased_string`; 7 doctest subcases. Two spellings that *disagree* are refused rather than silently resolved. Not VERIFIED until the end-to-end script calls each tool with the other tool's spelling. |
| Q4 | `Godot_PromoteRuntimeValue` — write a live-tuned value back to the scene | NOT_STARTED | Small; overlaps D4. |
| Q5 | Grow the skill library past two skills | NOT_STARTED | Machinery outruns content: only `performance-profiling` and `scene-cleanup` ship. |

## Bugs this tranche has found

| # | Bug | Found by | Status |
|---|---|---|---|
| 2 | **`run_editor_e2e.py` could not pass on Linux.** The drag check sent a fixed 550→850 pointer drag; under a virtual display the game window is 846x475, so the tool correctly refused a drag ending outside it and the suite failed on a check about drag *motion*. Two assertions downstream also hardcoded the 300px span. | Running the end-to-end script on this container for the first time | **FIXED** — the far end is clamped to the real window and the span assertion derives from it. The suite now passes on Linux, with and without a display. |
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
