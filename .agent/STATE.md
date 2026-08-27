# STATE

Current repository reality. Concise and current, not chronological.

## Primary specification

`docs/godot-ai-clone-spec.md` (exists at the expected path; no competing design
document in the repository).

## Engine baseline

- Godot **4.8-dev** (`version.py`: 4.8.0-dev), **nested** `editor/` layout —
  `editor/file_system/`, `editor/docks/`, `editor/scene/`. Merged up from 4.3 in
  `117870273`; the module's includes already use the nested paths. DEC-0002's
  4.3 remapping table is superseded by DEC-0009.
- In-tree precedents followed: `editor/debugger/debug_adapter/` (EditorPlugin +
  TCPServer + poll on `NOTIFICATION_INTERNAL_PROCESS` with a re-entrancy guard),
  `modules/gdscript/register_types.cpp` (`EditorNode::add_init_callback` →
  `add_editor_plugin`).
- Module doctest headers under `modules/<name>/tests/test_*.h` are auto-included when
  building with `tests=yes`.

## Current milestone

**M3 — Agent experience.** Defined by `docs/godot-ai-agent-experience-spec.md`, tracked
in `.agent/EXPERIENCE_LEDGER.md`. The problem it addresses is not missing capability —
72 tools exist and nearly all are verified — but **legibility and composition**: no
interface shows the agent working, and nothing strings the primitives into a workflow.
Started. Q1 (build the engine on 4.8) is done and was the gate; E1, E3 and E7 — the
live activity stream and `Godot_GetActivity` — are IMPLEMENTED, as is Q3.

M1 (foundation and protocol) and M2 (the agent interface) are both complete; see the
spec ledger and interface ledger for their evidence.

## Current vertical slice

S-19 (in progress): the agent-experience tranche, opened by
`docs/godot-ai-agent-experience-spec.md` and tracked in `.agent/EXPERIENCE_LEDGER.md`.
Landed so far: the live activity stream (E1/E3/E7, verified end to end), session
record/assert/list/replay (S1–S4, S7, S8), and both write tools accepting `text` and
`content` interchangeably. Building the engine on 4.8 for the first time since the merge
turned up and fixed a real engine bug (the `EditorFileSystem` singleton), and running
`run_editor_e2e.py` on Linux for the first time turned up and fixed a second — a
hardcoded drag coordinate that made the suite unpassable under a virtual display, which
is very likely why C1 has never gone green.

Two rows are deliberately short of VERIFIED and say so: **S4** (replay) is exercised end
to end but inside one editor process, and this tranche's own rule 2 requires two; **S5**
(replay speed multiplier) was not built at all. **S6** (indeterminate verdicts) is
unit-tested hard but has not been produced against a live game.

S-21 (done): the closed loop, in four slices, all verified against a live editor.

- **A goal-directed playtest that produces a checkable report.** `mcp_playtest.{h,cpp}`
  plus four tools. The report's verdict is reconciled against the evidence rather than
  copied from the claim: a success reported with no input injected, or past a logged
  error, comes back indeterminate with the reason, and the original claim is kept beside
  it. P5 came free - a playtest injects through the same input tools as everything else,
  so the activity stream already had it.
- **Runtime-value promotion.** `Godot_PromoteRuntimeValue` carries a value tuned in the
  running game into the edited scene, through `EditorUndoRedoManager` and behind a
  checkpoint. Built early at the user's instruction. The runtime-to-scene path rule is
  its own tested file, because that join is where a promotion writes the right value
  onto the wrong node.
- **Four skills** for the jobs the tools were built for: `playtest-a-goal`,
  `investigate-a-crash`, `traverse-the-menus`, `tune-and-keep`. This is the answer to the
  tool-surface risk the user named; a test now checks every shipped skill parses, since a
  malformed one silently stops being offered.
- **Benchmarks** (`tools/benchmarks/`): three projects with planted defects, four tasks
  with model-free oracles, and a scorecard that counts collateral damage beside
  successes. `run_selfcheck.py` proves the benchmark measures something by building each
  project broken and fixed and requiring every oracle to tell them apart.
- **Retroactive bug capture.** `Godot_CaptureBugSession` writes out the input that led to
  what just went wrong, with nothing armed in advance — because you do not know something
  is a bug until it happens, by which time an unarmed recorder has nothing. What it writes
  is an ordinary session, so `Godot_ReplaySession` re-runs it unchanged: a bug report and a
  regression test are the same file from opposite ends. Two buffers, because a crash
  destroys one of them — the game's own trace while it lives, and an editor-side mirror
  (`mcp_bug_capture.{h,cpp}`, hooked into the one choke point in `MCPRuntimeBridge`) for
  after the process is gone. The event a game died on is never acknowledged and is also
  the one that caused the bug, so it is placed by extrapolating from the frame rate the
  buffer observed, and marked estimated in the record, the metadata and the reply; the
  session's `replay_level` then reads `attempt` rather than `general`. Verified in
  `run_editor_e2e.py` on both paths, including a capture taken after the game exited.

S-20 (done): the terminal, ported from `origin/GodotBeamDev` at the user's
request — "a full agent ai terminal built into the editor via a terminal emulator …
pay very very careful attention to how that branch opens and closes the instances of
the windows since it was very very prone to crashing." Four layers, each landed with
tests, each with the branch's defects fixed at the source rather than guarded against:

1. `terminal/mcp_pty.{h,cpp}` — the pty, rewritten. The original reaped the child inside
   a `const` query and then signalled the same pid afterwards, so it could signal an
   unrelated process. Reaping now happens in a non-const `poll()` that clears the pid in
   the same breath, so a stale signal is unrepresentable rather than avoided.
2. `terminal/mcp_terminal_emulator.{h,cpp}` + vendored libvterm (MIT, module-local) —
   the VT state machine, behind an opaque impl so the header does not leak libvterm and
   the test suite needs only the build flag. `init()` now clears scrollback, output,
   cursor and title, which it did not.
3. `terminal/mcp_terminal_widget.{h,cpp}` with `mcp_terminal_keys` and
   `mcp_terminal_selection` pulled out as pure, testable functions. **Ctrl-C did not
   work on the branch**: libvterm maps letter+MOD_CTRL to a control code itself, and
   being handed an already-mapped one it emits `ESC[3;5u`, which a shell ignores. Every
   platform's reporting form is now normalised back to the letter.
4. `terminal/mcp_agent_terminal_panel.{h,cpp}` + `mcp_agent_launch.{h,cpp}` — the
   bottom panel. Wired to this fork's relay over stdio rather than the branch's HTTP
   endpoint with a `Math::random()` bearer token; there is no credential to leak.

Verified on a virtual display by `.agent/evidence/spike_agent_terminal_panel.py`, whose
screenshot shows a shell prompt, a typed command and its output drawn inside the editor.
That live run found two defects no unit test would have: the panel handed Claude Code's
flags to every command (so a shell died instantly on an unknown option), and the bottom
panel opened at the height of its toolbar with the terminal invisible.

Still to do on this slice: nothing blocking. `claude` is not installed in this
container, so the one thing not exercised is Claude Code itself accepting the generated
configuration — a string in a text field and a file whose shape is unit-tested.

Both done since: **E2/E4/E5 are VERIFIED**, every dock control pressed and its effect
asserted (`.agent/evidence/spike_activity_dock_controls.py`), and **W8 does not
reproduce** (`.agent/evidence/spike_workspace_overdraw.py`).

- **The live tuning workspace.** `Godot_OfferVariants` — one tool with actions, not five,
  because the tool surface is itself a reliability risk. `offer` captures the original and
  takes candidates, `switch` puts one into the running game, `note` records what was seen,
  `keep` names the winner, `discard` puts the original back. It refuses to keep a value
  that was never live, and it measures how long each one *was* live: flipping through four
  numbers in a second is reported as "a choice rather than a comparison". No checkpoint per
  variant, deliberately — a runtime property change writes no file, so the checkpoint
  belongs at promotion, where there already is one.

Next in order, from `.agent/NEXT.md`: D1/D2 propose-then-apply with risk-based grouping,
then P2 — moving the playtest's perceive/decide/act loop into the editor over sampling.
Build-order items 4 (bug capture) and 5 (promotion and the tuning workspace) are both
done and listed above.

## A silent failure that predated everything

Building the tuning workspace's discard path turned up a defect older than any of this
tranche. `get_property` returns a value twice — as JSON and as Godot's own text — and
`MCPRuntimeAgent::coerce` accepted **neither** form of a structured type back: JSON turns
a Vector2 into the string `(128, 64)`, and `Variant::can_convert` says, correctly for a
general string, that a String is not a Vector2. So the two halves of the interface
disagreed. A caller could read a position out and not write the same string back in, and
every round trip of a structured property failed while reporting success. `coerce` now
tries Godot's own parser when the target is not a string type, and a test asserts the
invariant over eight types: whatever the runtime prints, the runtime accepts back.

## Two defects the new work found, both fixed

Wiring `Godot_CaptureBugSession` into the end-to-end run made it the first thing ever to
replay two sessions in one editor, and that **crashed the editor**. `MCPReplayPlan::load()`
reset two fields and left the rest to `start()`, so a plan read between the two carried the
previous run's verdict; a failed run followed by a session with no assertions — which is
exactly what a retroactive capture produces — left `first_divergence` at 0, the tool's
first poll called the new run finished, and `to_report()` indexed an assertion list
`load()` had just emptied. `load()` now forgets the whole of the previous run, a plan that
has not started is not finished, and the index is bounds-checked as well.

The end-to-end script had been capturing the editor's stdout to a pipe it never read and
never printed, so that crash arrived as "editor disconnected" with no evidence at all. It
now drains the pipe on a thread and prints the tail on failure, which is how the above was
diagnosed in one run. An unread pipe also blocks the writer at 64 KiB, so this was a latent
hang as well as a blind spot.

## The intermittent CI failure is fixed, and the diagnosis was wrong twice

`Godot_CaptureInspectorProperty` had been failing on roughly half of recent runs with
"gave up after 90005 ms; the editor drew 5 frame(s) in that time". The budget had already
been raised from 10s to 90s on the theory that a two-core llvmpipe runner is simply slow.
It is not slow: `Main::iteration()` skips its draw step entirely when nothing has marked
anything dirty, and on a bare Xvfb runner with no pointer, no compositor and no window
manager, nothing ever does. The editor had nothing to draw and was correctly not drawing,
while a poller watched a counter that was never going to move. The capture pollers now
call `Main::force_redraw()` on every tick while an operation is in flight. Measured on
this machine with all four cores pinned by busy loops — the condition that reproduced the
timeout locally — the three semantic captures answer in 0.7s, 1.7s and 0.4s. The e2e run
now prints those timings pass or fail, so drift back towards the ceiling is visible before
it becomes a red run.

S-18 (done): the full profiler as a windowed capture — interface-ledger row D4.
`Godot_StartProfiler` / `Godot_StopProfiler` / `Godot_GetProfilerStatus` harvest the
engine's own debugger profiler stream (`mcp_profiler_recorder.{h,cpp}` listening on
`ScriptEditorDebugger`'s `debug_data` signal): per-function script times, server
categories, GPU pass deltas, 1 Hz monitors, CPU and video memory with per-resource
VRAM snapshots, exported as JSON Lines to `user://godot_ai_profiles/` because the
debugger channel silently drops messages over 8 MiB. Whole-window function totals
come from the game's accumulated `servers:profile_total`; both replies carry a
reading guide with jq recipes. Verified by 3 new doctest cases and a live e2e
capture (316 records, 163 KB, all five streams, stop-before-start refused).
The game-production template teaches the workflow (tool table, performance
testing doctrine, an evidence bullet in the Definition of Done), and
`misc/godot_ai/skills/performance-profiling/` ships it as a skill with a full
export-format reference beside it.

S-17 (in progress): closing the gap between what the game-production template asks for
and what the interface provides. `docs/godot-ai-agent-interface-spec.md` breaks it down;
`.agent/INTERFACE_LEDGER.md` tracks it. 63 tools are advertised over `tools/list` (the profiler capture slice added three).
Group I is complete: the editor's own
interface is listable (`Godot_ListWindows`), addressable by what it says
(`Godot_FindControl`) and actionable (`Godot_SendEditorInput`). Group B is complete too:
every capture now carries its own provenance, and group F: the project's own test scenes
are discoverable and runnable without a shell, and group E: stacked audio is detected
structurally. **All 41 items in the interface ledger are VERIFIED**, including the five sub-clauses a
later audit found the rows had glossed over: pointer drag and scroll, touch
cancellation, error stack traces, stacking across a burst, and a save-corruption fixture
proven against a game that actually has saves. The chat
cancellation frame is caught end to end - the tools built for group I are what made that
possible, by locating and pressing the dock's Cancel button.

Six real bugs have fallen out of it, all fixed and covered — see the
"Bugs this tranche found" table in the interface ledger. A later audit of the ledger
against the spec's own wording found something else worth remembering: five rows said
"none remaining" while a named part of the requirement did not exist. Re-read the
requirement, not the row. Two were found by
`Godot_ListWindows` on its first run: a timed-out `Godot_AskUser` left a dead dialog on
screen whose buttons did nothing, and the same tool gave a caller who omitted
`timeout_seconds` a one-second deadline instead of the declared 300. The latest was in
`Godot_FindControl` itself — every rectangle came back offset by the editor window's
position, because `get_screen_transform()` already includes that placement except when
the window embeds its subwindows. Only clicking at the reported coordinates showed it;
the numbers looked entirely reasonable.

S-16 (done): the bootstrap project template.

S-15 (done): the virtual display. The gaps that had been recorded as environmental
were not: a container can have a screen if the repository gives it one. Adding
`tools/virtual_display.py` and wiring it into the end-to-end run turned four
requirements from "refusal path tested" into verified behaviour, and found a real
product bug on the way — the editor only requests the remote scene tree while its
Remote panel is visible, so `Godot_GetRuntimeSceneTree` had to ask for it and wait
(`MCPDeferred::begin_polled`).

Now verified against a live editor drawing on a real display: a 1152x648 screenshot
returned as an inline image, a running game's scene tree read live, a runtime property
edit that leaves the scene file byte-identical, `was_playing` on stop, and a question
answered by a genuine pointer click on a choice button (Escape returns
`cancelled: true`).

What remains is honest:

- **R8** — the Windows backend compiles in CI but has never been *run*. The POSIX
  backend now passes its full relay suite on both Linux and macOS.
- **C1** — every workflow command passes locally; never observed green on Actions.
- **O1 cancellation** — now caught end to end in `run_editor_ui_e2e.py`: a turn is left
  unanswered, the dock's Cancel button is found by `Godot_FindControl` (the *enabled*
  one, which is only enabled while a turn is in flight) and pressed by
  `Godot_SendEditorInput`, and the client is asserted to receive
  `notifications/cancelled` naming that request. This was recorded as environmental for
  a long time and was not.

## Ledger IDs in this slice

All 55 specification requirements are VERIFIED except R8 (needs another operating
system) and C1 (needs GitHub Actions itself), both IMPLEMENTED. All 41 interface-ledger
items are VERIFIED. X1 and X2 cover the
virtual display and the editor's render-capability reporting.

## Last verified state

On this Linux container at 4.8-dev (2026-08-26), with the S-20 terminal slice:

- Editor builds clean, SCU, 4 cores, 0 errors.
- Module suite → **204 cases from the repository root**, 203 / 3213 assertions from
  elsewhere; the difference is the shipped-skills test skipping when it cannot reach
  `misc/godot_ai/skills`, which it says rather than passing by being absent.
- 88 tools are advertised over `tools/list`.
- `python3 tools/relay/tests/run_tests.py` → **64/64 pass**.
- `python3 tools/relay/tests/run_editor_e2e.py` → all checks pass on a real virtual
  display.
- Full engine suite from the repository root → **1590 cases, 1589 pass**. The one
  failure is `[IP] resolve_hostname`, which asks for `localhost` over IPv6; this
  container's `/etc/hosts` has no `::1` entry, so it fails identically on the
  unmodified tree.
- `python3 tools/benchmarks/tests/run_tests.py` → 14 passed.
- `python3 tools/benchmarks/run_selfcheck.py` → 28 checks, all passing.
- **GitHub Actions run 64 is green** (`33028524808`) - the first successful run of the
  workflow, after 63 failures.
- `python3 .agent/evidence/spike_activity_dock_controls.py` → all checks pass, every
  Activity dock control pressed with a real pointer under a window manager.
- `python3 .agent/evidence/spike_workspace_overdraw.py` → all checks pass, both with the
  user's run embedded and without.
- `python3 .agent/evidence/spike_agent_terminal_panel.py` → all checks pass: the panel
  is in the tree, its tab opens it, the command field takes a new command, Start runs a
  real process under a pty, typing into the widget reaches the child and its output is
  drawn, and Stop reports the agent stopped. Screenshot beside the script.

With the S-19 slice, earlier the same day:

- Editor builds clean on 4.8 — 9m46s from scratch, SCU, 4 cores, 0 errors. This is the
  first build since the merge; the module needed no source changes to link.
- Module suite (`--test-case="*[godot_ai]*"`, from `/tmp`) → **106 cases, 698
  assertions**, all pass. On the merged code before this slice it was 74/526, matching
  the 4.3 numbers exactly, so the merge caused no module regression.
- `python3 tools/relay/tests/run_tests.py` → **64/64 pass**.
- Full engine suite from the repository root → **1523 cases**, one failure:
  `[IP] resolve_hostname`, which needs outbound DNS this container does not have and
  fails identically on the unmodified tree. **Ten consecutive runs**, after the
  `EditorFileSystem` singleton fix; the pre-fix binary crashed eight times in ten.
- `python3 tools/tests/run_tests.py` → 14 passed, 2 skipped, 16 total.
- `python3 tools/relay/tests/run_editor_e2e.py` → **all checks pass on a real virtual
  display**, including the new activity and session blocks. This is the first time this
  script has ever passed on Linux.
- `python3 tools/relay/tests/run_editor_e2e.py --headless` → all checks pass.
- 77 tools are advertised over `tools/list`, up from 73.
- `python3 tools/relay/tests/run_editor_ui_e2e.py` → **all 14 checks pass**, driving the
  real editor by keyboard and pointer. `xdotool` and `x11-utils` are installed here now
  (`apt-get install -y xdotool x11-utils`); they were the only reason this suite and the
  X-geometry measurements used to be recorded as unavailable.

On the working tree with the S-18 profiler slice (2026-08-13, native macOS
arm64), at the pre-merge 4.3 baseline:

- `python3 tools/relay/tests/run_tests.py` → 64/64 pass.
- Module suite (`--test-case="*[godot_ai]*"`, run from `/tmp`) → 74 cases,
  526 assertions, all pass.
- `python3 tools/relay/tests/run_editor_e2e.py` → all checks pass on the real
  display, including the new profiler-capture block.
- The full engine suite, `tools/tests`, `run_editor_ui_e2e.py` and the headless
  e2e mode were not re-run for this slice.

From the earlier full sweep (predating S-18; counts are of that time):

- `python3 tools/relay/tests/run_tests.py` → 59/59 pass.
- `python3 tools/tests/run_tests.py` → 16/16 pass (the virtual display).
- Module suite: 62 cases, 425 assertions, all pass.
- Full engine suite from the repository root: 940 cases, 2,395,117 assertions, all
  pass — no regression from the editor accessors this module added.
- `python3 tools/relay/tests/run_editor_ui_e2e.py` → all checks pass, driving the real
  editor by keyboard and pointer: the palette, the approvals dialog, a chat turn
  answered by the connected client's model, a skill allowed by clicking the rectangle
  `Godot_FindControl` reported for its button, the approvals dialog closed twice through
  `Godot_SendEditorInput` — once by a click on its Close button, once by Escape — and a
  chat turn cancelled from the dock, with the client receiving
  `notifications/cancelled` for that exact request.
- `python3 tools/relay/tests/run_editor_e2e.py` → all checks pass on a display the
  script starts itself, including a real 1152x648 screenshot, a running game's scene
  tree (`root > Main > Player, Hud, Field, Target, EnemySpawner`), a runtime edit that
  leaves the scene file byte-identical, and a question answered by clicking its dialog.
  63 tools are advertised over `tools/list` (the profiler capture slice added three).
- `python3 tools/relay/tests/run_editor_e2e.py --headless` → all checks pass, with the
  visual tools refusing as they should.
- Native macOS arm64 editor build with Vulkan/Metal and OpenGL enabled →
  `bin/GodotAI.app`, a valid ad-hoc-signed bundle whose display name, executable and
  icon are all `GodotAI`; the bundled executable reports
  `4.3.dev.custom_build.5e0a468c3`, and its GodotAI suite passes 69 cases / 433
  assertions. The macOS relay suite passes 64/64.
- Documentation (`modules/godot_ai/README.md`), `AGENTS.md`, `CLAUDE.md` and
  `.github/workflows/godot_ai.yml` are current. The workflow now installs
  `xvfb x11-utils libgl1-mesa-dri xdotool` and runs both end-to-end modes; it has still
  never been observed running on GitHub Actions.

## Working-tree expectations

Clean. The three changes this section previously listed as pending have resolved:
the X4/X5 evidence rewrite and the `mcp_editor_ui_tools.cpp` additions both landed
(the latter in the 4.8 merge `117870273`); the untracked scratch script
`tools/relay/tests/call_running_editor.py` was never committed and no longer exists.
`claude/status-i8oaes` is level with `origin`.

This checkout is a **fresh container**: there is no engine build and no relay binary
until you make them. `tools/relay/build.sh` takes seconds; the editor takes ~8 minutes.

## Active failures

None. **CI is green**: run 64 (`33028524808`, 2026-08-27) is the first successful run of
`.github/workflows/godot_ai.yml`, after 63 failures nobody had looked at. Two causes,
both fixed: five `werror` diagnostics that are only warnings in the default local build,
and then a `Godot_CaptureInspectorProperty` timeout on the runner once the build got far
enough for the end-to-end step to run at all.

The intermittent full-suite SIGSEGV that Q1 turned up is **fixed**: `EditorFileSystem`'s
destructor never cleared its own singleton, so after any test that created and destroyed
one (`modules/gltf`, the GDScript LSP tests) `get_singleton()` returned freed memory and
the standard null guard passed. Ten consecutive full-suite runs are clean where the
pre-fix binary crashed eight times in ten. Details and the three lessons are in
`.agent/EXPERIENCE_LEDGER.md`.

One failure remains and is environmental: `[IP] resolve_hostname` fails because this
container's `/etc/hosts` maps `localhost` to `127.0.0.1` only, with no `::1` line, so
the IPv6 half of the test resolves to nothing. `getent ahostsv6 localhost` returns
nothing here. It fails identically on the unmodified tree.

## In-flight operation

None.

## Two traps this environment sets

Both cost real time here, and both look like product bugs until measured properly.

- **A bare Xvfb has no window manager, so "maximised" is a fiction.** The X window keeps
  its original size while the DisplayServer reports the maximised size it asked for -
  measured here as Godot saying 1600x1000 while X said 1152x648. Every rectangle the
  editor reports is then off by the difference, and injected input is wrong in exactly
  the same way, so the tools agree with each other and disagree with reality. Start
  `openbox` (`apt-get install -y --no-install-recommends openbox`) before pressing
  anything with a real pointer. Do not pass `--resolution` to the editor on a bare
  display: it sizes the X window without moving the internal viewport, which produces
  the same mismatch deliberately.
- **`xwininfo -root -tree` lists unmapped windows exactly like mapped ones.** A window
  the editor correctly hid still appears, which is how a hidden game read as drawing
  over the workspace. The map state has to be asked for per window with
  `xwininfo -id <id>`.

## Risks

- **Work loss.** A test fixture previously deleted the working tree including `.git`
  (DEC-0006). The relay and module had to be rewritten from scratch. Commit and push
  after every verified slice; never run a recursive delete rooted at the CWD.
- Run the engine test binary from outside the repository (`cd /tmp`) as defence in
  depth.
- Windows relay behaviour cannot be *run* here. The Windows backend is cross-compiled
  in CI so it cannot rot, but nothing has executed it. The macOS POSIX backend now has
  64/64 passing relay tests on a native arm64 host.
- **Do not record a gap as environmental without trying.** Screenshots, dialogs and
  runtime inspection were all recorded that way and none of them had to be; the
  display was one `apt-get install xvfb` away, and treating it as unreachable hid a
  real bug. `tools/virtual_display.py` exists now — use it.

## Last completed command

`python3 .agent/evidence/spike_agent_terminal_panel.py` — all checks passed, with a
screenshot of a live shell running inside the editor's Agent Terminal panel.

## Next command

For a future protocol or tool-behaviour change, confirm the ledger still matches
reality before doing anything else:

```sh
python3 tools/relay/tests/run_tests.py && python3 tools/tests/run_tests.py \
    && python3 tools/relay/tests/run_editor_e2e.py
```

The visual checks need `xvfb x11-utils libgl1-mesa-dri xdotool`; without them the run
still passes, having quietly degraded to the refusal paths, so check the first line of
its output to see which mode it ran in.
