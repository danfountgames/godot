# Agent Interface completion specification

## Why this document exists

`misc/godot_ai/project_template/AGENTS.md` instructs an autonomous agent to build a
game and *prove* it works: real player input, screenshots of the running game, runtime
inspection, profiling, audio checks, save verification, resolution matrices. Writing
that instruction file forced an audit of what the editor actually exposes, and the
audit found the instructions assume an interface substantially larger than the one
that exists.

The template was corrected to describe the interface truthfully. This document
describes the other half of the fix: **the capabilities the fork must gain so the
instructions become achievable through the product rather than through a host-side
harness.**

The distinction that governs every entry here is the one the template already makes:

> The agent may build the game efficiently through semantic editor tools, but it may
> not use those same shortcuts as proof that the player experience works.

So these tools are split deliberately. Construction tools may take the short path.
**Verification tools must traverse the same path a player's hardware would**, or they
are not evidence.

## Where the missing pieces live

Everything that inspects or drives a *running game* has to cross a process boundary:
"Play" launches a second process. The editor already talks to it over the remote
debugger — that is how `Godot_GetRuntimeSceneTree` works today.

That channel is the right place for all of it. The design is:

```
MCP client → relay → editor (MCPService) → EditorDebuggerNode
                                              │  debugger message
                                              ▼
                                        running game process
                                              │
                                        MCPRuntimeAgent  (capture handler)
                                              │
                              Input.parse_input_event(), Viewport, Performance, …
```

`MCPRuntimeAgent` is compiled under `TOOLS_ENABLED` and installs itself only when the
process is running as a game rather than an editor. A game launched from the editor is
the same executable, so it is present there; an **exported game contains none of it**,
which preserves the property that the AI tooling never ships inside a product.

Two rules constrain every runtime capability:

1. **Input is injected through `Input::parse_input_event()`**, the same entry point the
   platform layer uses for real hardware. Not by calling a control's callback, not by
   emitting `pressed`, not by setting the destination state.
2. **A capability that cannot be delivered honestly is not delivered.** Where the
   engine genuinely cannot supply something (hearing audio, a GPU-side profiler on a
   software rasteriser), the tool reports the limitation rather than approximating it.

---

# A — Real input injection

The largest gap. `AGENTS.md` makes real-input validation mandatory for every
player-facing flow, and today nothing in the MCP surface can deliver an input event.
Verification depends entirely on a host-side `xdotool` harness, which needs a display,
does not exist on Windows or macOS runners, and cannot target a control semantically.

| ID | Capability | Notes |
|---|---|---|
| A1 | Runtime agent channel: editor ↔ running game debugger transport | Prerequisite for A2–A6, C, D, E, G, I |
| A2 | `Godot_SendPointerInput` — move, press, release, click, drag, scroll | Through `parse_input_event`; press and release are distinct events |
| A3 | `Godot_SendKeyInput` — key press/release, modifiers, and text entry | Text typed as real key events, not by setting `text` |
| A4 | `Godot_SendTouchInput` — touch down/move/up, multi-touch, gesture thresholds, cancellation | |
| A5 | `Godot_SendGamepadInput` — buttons, axes, connect and disconnect | Device change is a required test in `AGENTS.md` |
| A6 | `Godot_GetInputTrace` — what was sent, when, and what the game did with it | Evidence, and the thing that distinguishes a real click from a claimed one |

**Targeting.** Coordinates alone make brittle tests; semantic targeting alone is not
real input. The answer is both: `Godot_FindControl` (I2) resolves a control to its
*screen rectangle*, and the pointer tool then delivers a genuine event at that point.
The test says what it means; the event is still real.

**Refusals that must exist.** No running game; the game is not accepting input
(loading, modal); coordinates outside the window; a device index that does not exist.

---

# B — Seeing the running game

`Godot_CaptureViewport` photographs the **editor**. Every screenshot an agent takes of
"the game" today is either the editor viewport or requires a host-side grab.

| ID | Capability | Notes |
|---|---|---|
| B1 | `Godot_CaptureGame` — the running game's own viewport | The screenshot `AGENTS.md` asks for on nearly every goal |
| B2 | `Godot_CaptureFrameSequence` — N frames at an interval | Transitions, animation, "did feedback begin within 100 ms" |
| B3 | `Godot_CaptureEditorWindow` — the whole editor window including popups and dialogs | Today's capture misses dialogs, which are separate OS windows |
| B4 | Capture metadata — build, scene, state, resolution, scale, sequence | `AGENTS.md` requires it on every screenshot record |

---

# C — Runtime inspection

`Godot_GetRuntimeSceneTree` and `Godot_SetRuntimeProperty` exist. Reading back, waiting
for a condition, and understanding a node do not.

| ID | Capability | Notes |
|---|---|---|
| C1 | `Godot_GetRuntimeProperty` — read a property from the running game | Currently write-only, which makes verification circular |
| C2 | `Godot_GetRuntimeNodeInfo` — class, script, groups, signals, visibility, geometry | |
| C3 | `Godot_WaitForRuntimeCondition` — wait until a property matches, with a timeout | Replaces the sleeps `AGENTS.md` forbids |
| C4 | `Godot_GetRuntimeErrors` — structured errors and warnings with stack traces | `Godot_ReadOutputLog` returns prose; this returns structure |
| C5 | `Godot_SetTimeScale` — deterministic fast-forward for long routes | Must be refused during a black-box playtest |

---

# D — Performance

`AGENTS.md` asks for frame time, slow frames, memory, object counts, draw calls,
physics cost, load time and transition time. None is reachable today.

| ID | Capability | Notes |
|---|---|---|
| D1 | `Godot_GetPerformanceMetrics` — a sample from the `Performance` singleton | FPS, frame time, memory, objects, nodes, draw calls |
| D2 | `Godot_ProfileWindow` — sample continuously across a window and return the distribution | Worst frame matters more than the mean |
| D3 | Budget comparison — pass/fail against declared targets | So a result is a verdict, not a number to interpret |

---

# E — Audio

An agent cannot listen. It can still verify everything except the subjective part, and
must say which part it could not check.

| ID | Capability | Notes |
|---|---|---|
| E1 | `Godot_GetAudioState` — buses, volumes, mute state, active playbacks, peak levels | Peak level is how "did a sound actually play" is answered |
| E2 | Duplicate/stacking detection across an input burst | `AGENTS.md` requires this specific check |

---

# F — Tests

`AGENTS.md` tells the agent to run tests; no tool can.

| ID | Capability | Notes |
|---|---|---|
| F1 | `Godot_RunSceneTest` — run a project test scene and return its structured result | A scene, not a shell command — the no-arbitrary-execution rule holds |
| F2 | `Godot_ListSceneTests` — discover test scenes by convention or manifest | |
| F3 | Structured results — per-case pass/fail, message, duration | |

---

# G — Saves and `user://`

Filesystem tools are confined to the project root, so the directory where saves live is
unreachable — and `AGENTS.md` requires save verification, corrupt-save handling, and
settings persistence.

| ID | Capability | Notes |
|---|---|---|
| G1 | `Godot_ListUserFiles`, `Godot_ReadUserFile` | Own capability class, `read_user_data` |
| G2 | `Godot_WriteUserFile`, `Godot_DeleteUserFile` | `edit_user_data`, ask-by-default, checkpointed |
| G3 | Save-corruption fixtures — write malformed data deliberately | The recovery path in `AGENTS.md` cannot be tested otherwise |

---

# H — Project and window configuration

Resolution and aspect-ratio matrices are required by `AGENTS.md`; nothing can change a
resolution.

| ID | Capability | Notes |
|---|---|---|
| H1 | `Godot_GetProjectSetting`, `Godot_SetProjectSetting` | Checkpointed; `project.godot` is a project file |
| H2 | `Godot_SetGameWindowSize` — resize the running game | The resolution matrix, without relaunching per size |
| H3 | `Godot_GetGameWindowInfo` — size, scale, safe area, aspect | Needed to interpret a capture |

---

# I — Editor UI automation

The approvals dialog, the command palette and the chat panel are already driven by a
host harness in this repository's own tests. An agent working on a *game* has the same
need for that game's editor-side UI.

| ID | Capability | Notes |
|---|---|---|
| I1 | `Godot_SendEditorInput` — real input into the editor window | Same pipeline as A2–A5, editor target |
| I2 | `Godot_FindControl` — resolve a control by name/path/text to its screen rectangle | Makes A2 semantic without making it fake |
| I3 | `Godot_ListWindows` — editor and game windows, with geometry | Dialogs and popups are separate windows |

---

# J — Asset pipeline

`AGENTS.md` has a live asset update loop that currently has to be inferred from the
output log.

| ID | Capability | Notes |
|---|---|---|
| J1 | `Godot_GetImportStatus` — queue state, per-asset result, errors | |
| J2 | `Godot_ReimportAsset` — force a reimport and wait for completion | |
| J3 | `Godot_WaitForImportQueue` — an explicit wait instead of a sleep | |

---

# K — Checkpoints and evidence

| ID | Capability | Notes |
|---|---|---|
| K1 | `Godot_CreateCheckpoint` — a named checkpoint on demand | Today they are only automatic |
| K2 | `Godot_DiffCheckpoint` — what changed since one | |

---

# Capability classes

New tools need honest classes. Two new ones are required, because saves are not project
files and injected input is not a read:

| Class | Default | Covers |
|---|---|---|
| `read_runtime` | allow | C1–C4, D, E, B1, B2, H3 |
| `simulate_input` | **ask** | A2–A5, I1 — it can do anything a player can do, including deleting save data through the game's own UI |
| `read_user_data` | ask | G1 |
| `edit_user_data` | ask | G2, G3 |
| `edit_files` | ask | H1, J2, K1 |
| `run_project` | ask | C5, F1, H2 |

`dangerous_exec` remains deny-always and unclaimable by plugins. No tool added here
executes a shell command.

---

# Order of work

Ordered by how much each unblocks, not by size:

1. **A1** — the runtime agent channel. Everything runtime-side depends on it.
2. **A2, A3** — pointer and keyboard. The mandatory verification path in `AGENTS.md`.
3. **B1** — seeing the game. Half the evidence requirements need it.
4. **C1, C3** — read back and wait. Without these, input tests assert nothing and sleep.
5. **I2** — semantic targeting, so tests stop hard-coding pixels.
6. **A6** — input traces, so a claimed click is distinguishable from a real one.
7. **D1** — performance sampling.
8. **G1, G2** — saves.
9. **H1–H3** — resolutions.
10. **A4, A5** — touch and gamepad.
11. **B2, B3, C2, C4, E1, F1–F3, J1–J3, K1–K2** — the remainder.

# Done criteria per capability

Unchanged from `AGENTS.md`: the behaviour exists and is reachable through the real
product path; its schema is declared and enforced; failure and permission-denied paths
are covered; automated tests pass and the end-to-end scripts still pass; the ledger
records the status with real evidence; the work is committed and pushed.

One addition specific to input: **every input tool must be proven to travel the real
pipeline**, by a test that would fail if the implementation called a callback directly.
The test for "a button was clicked" asserts on the button's own `pressed` signal
arriving *and* on `_gui_input` having seen a `InputEventMouseButton` — a shortcut
implementation passes the first and fails the second.
