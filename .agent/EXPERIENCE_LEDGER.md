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
| E2 | Activity dock renders the stream, in-flight call distinguished | VERIFIED | `mcp_activity_dock.{h,cpp}` | `run_editor_e2e.py` covers the data; `.agent/evidence/spike_activity_dock_controls.py` presses the controls | a bottom panel answering the six questions: declared goal, current activity, one row per call with its outcome, what it touched, what changed, Pause/Stop, and Revert on the selected call's checkpoint. Appends rather than rebuilds, so a poll does not throw away the selection | **not verified by pressing its controls.** Rule 1 of this ledger wants `Godot_FindControl` + `Godot_SendEditorInput` under a display, and `xdotool` is not installed here. **Verified by pressing them**, not only by the data behind them: Pause holds the agent and its write is refused with nothing left on disk, Resume releases it and the same write succeeds, Stop holds it too, and a held agent cannot even locate the control that would release it. Two things that took finding: the dock opened 62 pixels tall because its record tree declared no minimum, and pressing anything needs a window manager - without one a maximised window's reported geometry is a fiction, so a real click misses while an injected one lands |
| E3 | Each record names the node paths and `res://` paths it touched | VERIFIED | `MCPActivity::extract_subjects`, `MCPTool::get_activity_subjects` | `tests/test_mcp_activity.h` | doctest for extraction, and `run_editor_e2e.py` asserts real calls carried file subjects — so the dock will have something to reveal | **heuristic for every tool** — the default reads argument keys it does not own. No tool overrides `get_activity_subjects()` yet; the ones that resolve a node by search know a path the arguments never carry |
| E4 | Selecting a record reveals its subjects in the Scene and FileSystem docks | VERIFIED | `MCPActivityDock::_reveal_pressed` | `.agent/evidence/spike_activity_dock_controls.py` | `EditorInterface::select_file` for `res://` subjects, `edit_node` for node subjects, falling back to a root-relative path | verified by clicking: selecting a record names the file the call touched and the intent the agent had declared at the time, and Reveal is accepted on it |
| E5 | Checkpoint scrubber with per-record diff and revert | VERIFIED (no scrubber) | `MCPActivityDock::_diff_pressed/_revert_pressed` | `.agent/evidence/spike_activity_dock_controls.py` | the selected record's checkpoint lists the files it captured, and Revert restores just that one call, saying so | verified by clicking: What Changed names the checkpoint, and Revert This undoes exactly that one call while leaving a later, unrelated one alone. **No scrubber, deliberately.** The user's build order says a thin dock first; a timeline decorating an unproven workflow comes later |
| E6 | Activity survives a dock restart, not an editor restart | IMPLEMENTED | `MCPActivity` is process-wide | `tests/test_mcp_activity.h` | the buffer outlives any dock that renders it and dies with the editor, which is the requirement | none |
| E7 | `Godot_GetActivity` exposes the same stream as a tool | VERIFIED | `tools/mcp_activity_tools.cpp` | `run_editor_e2e.py` | called against a live editor; reply shape, sequence polling and refusal recording all asserted | none |

## C — Intent and control

| ID | Requirement | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|
| C1 | The agent declares a goal and a current activity, stamped on every call | VERIFIED | `mcp_agent_state.{h,cpp}`, `Godot_SetIntent` | `tests/test_mcp_agent_state.h`, `run_editor_e2e.py` | a call made after the intent was set carries it; an earlier record keeps what was true when *it* ran | none |
| C2 | The user can pause or stop the agent, and it is enforced | VERIFIED | `MCPAgentControl::may_run`, gate in `mcp_protocol.cpp` | `tests/test_mcp_agent_state.h`, `run_editor_e2e.py` | a stopped agent's write is refused **and the file is not on disk**; the refusal names the reason the user gave | none |
| C3 | Reads that explain what already happened survive a hold | VERIFIED | allowlist in `mcp_agent_state.cpp` | same | `Godot_GetActivity`, `Godot_ListCheckpoints`, `Godot_DiffCheckpoint`, `Godot_GetEditorStatus`, `Godot_ListInstances` still answer while held — otherwise the dock loses its own data source exactly when it matters | none |
| C4 | The agent cannot release itself | VERIFIED | `Godot_SetAgentControl` has no resume action and is not allowlisted | `run_editor_e2e.py` | a stopped agent calling `Godot_SetAgentControl` is refused. A control the held party can lift is advisory, not a control | none |

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
| P1 | `Godot_StartPlaytest` with goal, budget, oracle; returns a handle | IMPLEMENTED (does not launch) | `mcp_playtest.{h,cpp}`, `tools/mcp_playtest_tools.cpp` | `test_mcp_playtest.h`, `run_editor_e2e.py` | opens a window against a game that is **already running**, one at a time | **Deliberately does not launch the game.** Launching needs `run_project` and playing needs `simulate_input`, and a tool declares one capability - the same reasoning as DEC-0010 for replay. It refuses up front when nothing is running, rather than producing an empty report four minutes later |
| P2 | The perceive/decide/act loop runs editor-side over MCP sampling | NOT_STARTED | — | — | — | deliberately after P1/P3: it changes *who drives the loop*, not what the report contains, and the report is the product |
| P3 | `Godot_GetPlaytestReport` with crashes, spikes, coverage, verdict | IMPLEMENTED | `MCPPlaytest::build_report`, `reconcile_verdict` | `test_mcp_playtest.h` (24 cases), `run_editor_e2e.py` | the report carries the goal, every call and every input in the window, the game's own errors and warnings, the agent's observations, and a verdict reconciled against that evidence | **The verdict is checked, not believed.** A success claimed with no input injected, or past a logged error, comes back indeterminate with the reason attached and the original claim kept beside it. Frame-time spikes have detection and tests but no source yet: the profiler recorder is not wired in, so `spikes` is always empty in a live report and the row says so |
| P4 | Stoppable mid-run, reports partial results | IMPLEMENTED | `MCPPlaytest::abandon`, `Godot_FinishPlaytest` verdict `stop` | `test_mcp_playtest.h` | stopping keeps what was collected, marks the report `partial`, and records the reason as the summary | |
| P5 | Playtest input appears in the activity stream and input trace | VERIFIED | `MCPPlaytest::input_in_window` over `MCPActivity` | `run_editor_e2e.py` | free, and by construction: a playtest injects input through the same tools everything else does, so the activity stream already has it and the report reads it back from there | the report cannot show input the stream did not record, which is what makes an agent's account of what it pressed checkable rather than trusted |

## D — Design conversation

| ID | Requirement | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|
| D1 | `Godot_ProposeChange` renders a per-item Apply/Reject checklist | NOT_STARTED | — | — | — | everything |
| D2 | Apply goes through `EditorUndoRedoManager`, one transaction per item | NOT_STARTED | — | — | — | everything |
| D3 | `Godot_OfferVariants`, each behind a checkpoint, switchable live | NOT_STARTED | — | — | — | everything |
| D4 | A variant may be promoted to the persistent scene, or all discarded, in one call | PARTLY (promotion done, variants not) | `tools/mcp_promote_tools.cpp`, `mcp_runtime_paths.{h,cpp}` | `test_mcp_runtime_paths.h`, `run_editor_e2e.py` | `Godot_PromoteRuntimeValue` reads a property from the running game and writes it into the same node in the edited scene, through `EditorUndoRedoManager` and behind a checkpoint | **Built early, at the user's instruction**: "without it, live tuning is theatre because the last act is manual". The join is the interesting part and it is where a promotion could write the right value onto the wrong node, so the runtime-to-scene path rule is its own tested file and refuses when the running scene is not the open one. The value is read from Godot's text form rather than the JSON, because JSON has no Vector2. Variants themselves (D3) are not built |

---

## W — The agent workspace (multi-instance embedding)

Derived from the user's *GodotAI Agent Workspace* specification. Phase Zero is DEC-0011.

| ID | Requirement | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|
| W0 | Prove several game processes can embed at once | VERIFIED | throwaway patch, reverted | manual spike | two games rendered inside one editor window; `.agent/evidence/spike_two_embedded_processes.png` | Linux/X11 only. **macOS is expected to fail** and the reason is now specific — see `.agent/MACOS_EMBEDDING_SPIKE.md`. Windows uses the same PID-keyed reparenting as X11 and should behave like it; Wayland unmeasured |
| W1 | One implementation of the embed command line, reusable by any host | IMPLEMENTED | `embedded_process_apply_arguments()` in `editor/run/embedded_process.cpp` | `run_editor_e2e.py` | extracted from `GameView`, which now delegates to it; the end-to-end run still embeds and drives a real game | not yet called by anything but `GameView` |
| W2 | Several owners can shape a launching instance's arguments | IMPLEMENTED | `EditorRun::add/remove_instance_starting_callback` | `run_editor_e2e.py` | was a single callback slot, so whoever registered last silently displaced the other; now a list called in registration order | no second listener registered yet |
| W3 | Agent-owned processes are distinguishable from the user's own run | IMPLEMENTED | `mcp_runtime_instances.{h,cpp}` | `tests/test_mcp_runtime_instances.h` | 10 cases: registration before launch, pid binding, one pid cannot be claimed twice, a closed instance keeps its record but releases its pid, grouping, serialisation | nothing launches through it yet |
| W4 | Debugger control targets one instance instead of broadcasting | VERIFIED | `MCPRuntimeInstances::set_suspended/next_frame/set_time_scale/set_muted` | `tests/test_mcp_runtime_instances.h`, `.agent/evidence/spike_three_variants.py` | routes by `ScriptEditorDebugger::get_remote_pid()`; refuses rather than broadcasting when it cannot resolve a session. The slice pauses one of three live instances, gets `applied: true`, and finds all three still running — now reproducibly, in both modes, after the two bugs below | none |
| W9 | An agent-launched instance is actually controllable | VERIFIED | `MCPWorkspaceLauncher::launch` starts the debugger server; `Godot_LaunchInstance` waits for the session | `.agent/evidence/spike_three_variants.py` | launch answers only once the session resolves, and reports `debugger_connected` | none |
| W5 | A GodotAI main-screen workspace hosting N embedded tiles | IMPLEMENTED | `mcp_workspace.{h,cpp}`, `tools/mcp_workspace_tools.cpp` | `.agent/evidence/spike_three_variants.py` | a `GodotAI` main screen beside 2D/3D/Script/Game, one `EmbeddedProcess` per tile, each with its own header, status line and Take Control / Pause / Stop. Launch goes through `EditorRun::build_base_arguments()` and `embedded_process_apply_arguments()`, the same builders the play button uses | **tile chrome is overdrawn** — see the z-order note below. No focus layout, no tray, no pinning, no archived result cards |
| W6 | First vertical slice: compare three live runtime variants | VERIFIED | `Godot_LaunchInstance`, `Godot_ListInstances`, `Godot_ControlInstance`, `Godot_StopAllInstances` | `.agent/evidence/spike_three_variants.py` | three isolated processes launched and embedded alongside the user's own game; each got its own id; pausing one reported `applied` and left all three running; stopping one left two; `Godot_StopAllInstances` cleared the agent's three and **the user's own game survived**. Screenshot: `.agent/evidence/spike_three_variants.png` | no promote-the-winner step yet — that is the live-tuning slice |
| W8 | Embedded windows draw over the tile chrome | NOT REPRODUCIBLE - RESOLVED | `mcp_workspace.cpp` sets the embedders to fit their frame; `mcp_workspace.cpp` also names the one real case | `.agent/evidence/spike_workspace_overdraw.py`, `.agent/evidence/spike_three_variants.py`, `spike_workspace_overdraw_{embedded,floating}.png` | **Measured, and it does not happen.** Each of the three tiles holds exactly one game, inside its own embedder; the hidden Game workspace's embedded game is unmapped, which a probe in `EmbeddedProcess::_update_embedded_process` confirmed it asks for and X honours. The original filing was wrong twice over: it came from a screenshot, which cannot tell an embedded window from a floating one, and the first attempt to measure used `xwininfo -root -tree`, which lists **unmapped** windows exactly like mapped ones - so a correctly hidden game read as still on screen. The map state has to be asked for per window. What is real is the remaining case: a game the user starts with Embed on Play **off** is a top-level window that floats over the tiles, which no editor can fix because stacking is the window manager's job. The workspace now says so in its summary line, with the setting to change |
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
| 3 | **An agent-launched game could never be controlled unless a human happened to be playing.** `EditorRun::build_base_arguments()` copies the debugger's server URI into the command line, but the server is only started by the run bar when someone presses play. Launching without it produced a game that ran and embedded perfectly and ignored every targeted control. | The three-variant slice, run *without* the user's game | **FIXED** — the launcher starts the debugger server when nothing has. |
| 4 | **`Godot_LaunchInstance` answered before the game could be controlled.** It returned as soon as the process existed, so an immediate pause was refused, and whether it worked depended on how much else was starting. | Same run | **FIXED** — launch is deferred until the session resolves, and reports `debugger_connected`. |
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
