# NEXT

The build order below is **the user's**, given 2026-08-26, and it supersedes the tier
ordering in `docs/godot-ai-agent-experience-spec.md`. Where the two disagree, this wins.

## The positioning, in the user's words

The product is not seventy-seven tools. It is the closed loop:

> The agent can make a change, run the game, interact with it, discover that the result
> is wrong, diagnose why, revise the change and prove that it now works.

"AI verifies your game" is the **wedge, not the boundary**. The unique capability is that
the agent can observe, operate and modify a *running* game and connect what happened back
to the project. Verification is the best first commercial use of that; live tuning,
debugging, performance investigation and iterative design come from the same capability.

Do not describe the backend as "essentially finished". It is **feature-complete but only
partly revalidated** — see below.

## Revalidation, before the roadmap

| # | Job | State |
|---|---|---|
| 1 | Rebuild on Godot 4.8 and run the full module suite | **Done.** Builds clean; module suite 106 cases / 698 assertions; full engine suite 1523 cases over several runs. Found and fixed a dangling `EditorFileSystem` singleton that crashed the suite roughly one run in four. |
| 2 | Observe `.github/workflows/godot_ai.yml` succeeding on Actions | **DONE. Run 64 is green** (`33028524808`, 2026-08-27), after 63 consecutive failures nobody had looked at. The claim that it had "never been observed running" was wrong: it had run 62 times and the editor job failed every time in about seventy seconds. CI builds with `warnings=extra werror=yes` and the local default does not, so five diagnostics that are warnings here were errors there. With those fixed the run reached the end-to-end step for the first time and failed again, on a `Godot_CaptureInspectorProperty` timeout. **That second diagnosis was also wrong, and cost two more red runs (66, 68) before it was chased down.** The editor is not slow; `Main::iteration()` skips its draw step when nothing is dirty, and on a bare Xvfb runner with no pointer and no compositor nothing ever is, so a poller waiting on drawn frames waits forever while the editor sits correctly idle. Raising the budget from 10s to 90s treated the symptom. The capture pollers now call `Main::force_redraw()`; under four pinned cores locally the three semantic captures answer in 0.7s, 1.7s and 0.4s. **Two lessons: "not observed" is a thing to check, not to record — and a timeout is a question about what the thing was waiting for, not a number to raise.** |
| 3 | Run the Windows relay against a Godot editor | **Not done.** Needs a Windows host. Unchanged. |

## Build order

1. **Finish revalidation.** Items 2 and 3 above.
2. **Minimal Activity dock.** Not a magnificent one. It only has to answer six questions:
   what is the agent trying to achieve; what is it doing now; what file, node or runtime
   object is it touching; what changed; can I stop it; can I undo *that* change.
   Highlight the node and the file. Offer a checkpoint diff and a revert. Pause and stop
   are essential. The scrubber, animation and timeline come later.
   Build it as an **observability system, not a panel**: every operation emits one
   consistent event carrying intention, tool call, affected objects, result, checkpoint
   and error state. The dock is one presentation of that stream; reports, logs and
   external clients are others. The underlying model matters more than the visuals.
3. **One constrained goal-directed playtest workflow** that produces a genuinely useful
   report. Built *together with* the thin dock, not after a finished one. The target
   demonstration: the agent launches a small test game, is asked to reach a state, you
   watch it in the dock, it hits a crash or an unreachable state or a frame spike, and it
   returns a report with screenshots, runtime state, errors and a profiler window that
   you can then replay or inspect.
4. **Reproducible bug-session capture.** Start as bug reproduction — "capture the sequence
   that caused this crash and replay it" — not as a comprehensive regression suite.
   Strict deterministic replay is an *optional stronger mode*, never the minimum promise.
   **Done.** `Godot_CaptureBugSession` reaches backwards into a buffer that is always
   being kept, at both ends of the channel, so nothing has to be armed in advance and a
   capture still works after the game process has gone. What it writes is an ordinary
   session, which is the point: replay runs it unchanged. Each capture records its own
   `replay_level` — `general` when every frame came from the game, `attempt` when the
   unacknowledged tail had to be extrapolated — so the artifact itself never claims the
   stronger mode.
5. **Promote runtime values, and a focused live-tuning workspace.** Promotion is small and
   closes the loop: the agent adjusts a value in the running game, you decide it feels
   right, the temporary value becomes the authored value. Without it, live tuning is
   theatre because the last act is manual. Frame variants as a **live tuning workspace**,
   not as general AI-generated alternatives.
   **Done, both halves.** `Godot_PromoteRuntimeValue` is the last act;
   `Godot_OfferVariants` is the workspace around it — one node, one property, named
   candidates, the original always among them, and the whole thing framed as tuning rather
   than as alternatives, exactly as instructed. It refuses to keep a value that was never
   live and reports how long each one was, so a set that was flipped through says it
   recorded a choice rather than a comparison. `keep` deliberately does not write: it hands
   off to promotion, because the two hold different authority.
6. **Grow the skills library around concrete jobs**: crash investigation, performance
   regression, menu traversal, input-path testing, scene cleanup.
   **Done — all five named jobs have a skill**, plus `playtest-a-goal`,
   `tune-and-keep` and `performance-profiling`. Checking their tool lists turned up
   `Godot_SendActionInput`, named by three of them and implemented by none, which is now
   implemented; `tools/skills/check_skills.py` fails the build on any skill naming a tool
   nothing answers to.

Alongside all of it: **benchmark projects and real success rates**.

## The user's user-journey pass, 2026-08-30

The user asked for a deep dive on the journey and a critique of the whole branch, then
said "Make it", then added "make sure the stem is powerful for headless agents too via
CLI or whatever". The critique is `docs/godot-ai-user-journey.md` and it is a standing
document: it lists seven criticisms and marks which have been answered.

Answered: project memory, the class reference (criticism 6), selection as context, the
panel collision, task-level undo, and headless (`docs/godot-ai-headless.md`).

**Still open, in the order they matter:**

1. **Nobody has made a game with this.** Still the biggest hole, but no longer untouched:
   the benchmarks have now been run once against a real model - see
   `tools/benchmarks/RESULTS.md`. 4/4 solved, 4/4 without collateral, 0 of 4 leaving
   machine-checkable evidence, and four real defects found that no test had:

   - a headless editor launched a game that crashed instantly, breaking the closed loop
     entirely for a headless agent, with the end-to-end suite *skipping* the check that
     would have caught it (fixed);
   - a benchmark task described a symptom that never happens (fixed);
   - `run_selfcheck.py` claimed "every planted defect reproduces" when it only compares
     files, which is how the previous item survived (fixed);
   - nothing in ninety-six tools connected a signal — `Godot_ManageConnection` now
     does, and building it found two more defects that each made it look like it
     worked: a listing swamped by the editor's own observers, and a connection made
     without `CONNECT_PERSIST`, which the editor honours all session and the save
     silently drops (fixed).

   A fifth task, `sticky-pause/reproduce`, was then added and run: a pure behavioural
   symptom, reproduced by pressing `ui_cancel` twice against a live headless game
   (`toggles` stuck at 1), fixed, and re-verified the same way (`toggles` 1 → 2). It is
   the first task where the closed loop does the work rather than confirming it.

   What is still needed is **scale** and **a driver that is not us**. Five tasks is not
   a benchmark, and the agent that ran them is the one that built the tools.
2. **Tools are engine operations, not design intentions** (criticism 3). Skills-first
   discovery is the concrete next step: make the higher-level jobs the prominent surface
   and the 95 primitives the thing they compose. This is the same note `NEXT.md` already
   carries about the tool surface being a reliability risk, and it is still true.
3. **Vision is an afterthought** (criticism 5). Visual diff, before-and-after, a
   filmstrip of a playtest. The user's "fill out the visuals" means *this* - making the
   tool visual - and explicitly not generating game art, which needs credentials this
   editor deliberately does not hold.
4. **The loop is single-player** (criticism 7). Two games embed simultaneously and only
   variant comparison uses more than one. Parallel exploration is unbuilt.
5. **Propose-and-diff as the default mode**, with the plan drawn in the scene rather
   than listed in a modal.
6. **Ambient findings**: read-only observations volunteered without being asked.

Not in the user's six, but landed because the design conversation needed it before the
skills library could lean on it: **propose-then-apply (D1/D2)**. `Godot_ProposeChange`
puts a validated, risk-grouped plan to the user before any of it happens, and applies
none of it — the approved calls are handed back and made the ordinary way, so each keeps
its own permission check, checkpoint and audit record. The grouping honours "not 40
separate approvals": mechanical changes share one tick however many there are, and an
irreversible one never shares.

## Two replay levels

Do not promise "deterministic replay". Raw input is one source of state among many —
variable frame timing, physics, random seeds, async resource loading, network, animation
timing, audio callbacks, wall-clock reads. A replay can diverge after an engine update or
on another machine.

- **Strict.** For games that opt into fixed time steps, known seeds and controlled async
  behaviour. May aim at genuine determinism.
- **General (semantic, resilient).** Records input but waits on *observed conditions*
  rather than assuming identical frames: "wait until the menu is visible, then press this
  control", not "press the button on frame 800". Stores runtime snapshots and visual
  references at important moments. On divergence it reports where and how rather than
  simply failing.

`Godot_WaitForRuntimeCondition` already exists and is the primitive the general level is
built from. What ships today is closer to strict-without-the-guarantees;
`MCPReplayPlan` already refuses to call a drifting run a pass, which is the honest floor
to build the general level on.

## Three risks the specification barely covered

- **Fork adoption friction.** Teams are cautious about a custom engine branch. They will
  ask how fast it follows upstream, whether existing projects open unchanged, whether
  export templates stay compatible, whether the AI module can be removed later, and
  whether they depend on one maintainer. This may be a bigger commercial obstacle than any
  missing feature. Keep changes to core Godot limited, documented and ideally
  upstreamable; keep as much as possible in the module. The `EditorFileSystem` destructor
  fix is exactly the kind of change that should go upstream.
  *(Started: `docs/godot-ai-fork-footprint.md` measures it.* **Nothing in the engine runtime
  is touched** — not `core/`, `scene/`, `servers/`, `drivers/`, `main/`, or any platform
  backend. Outside its own module the fork is 22 files: 16 in `editor/` at +285/−76, and 6
  of macOS branding, with 14 of the 16 purely additive. Both of the document's harder claims
  are now measured: a release template contains none of the tooling, and
  `module_godot_ai_enabled=no` builds an editor that passes the engine's own suite — 1416 of
  1417, the one failure being an upstream IPv6 test that fails identically with the module
  enabled.*)
- **No evaluation.** *(Started: `tools/benchmarks/` exists.)* Counts of tools, tests and
  verified requirements measure engineering coverage, not agent effectiveness. Build benchmark Godot projects and measure: how often
  the agent reaches a stated condition; how often it correctly identifies a regression;
  how reliably it reproduces a crash; how often it modifies the wrong node or property;
  how long a playtest takes; how much model usage it costs; how many false alarms appear
  in reports. Competitors can add runtime tools; a mature evaluation suite is far harder
  to copy.
- **The 77-tool surface is itself a reliability risk.** More similarly-named primitives
  means more chances to pick wrong or to build an invalid sequence. Skills and
  higher-level sessions must become **the normal way the model operates** — "run a
  performance investigation", "reproduce this input sequence" — with the primitives
  composed underneath and available for unusual cases, not equally prominent in every
  interaction.

## The workspace specification

The user's *GodotAI Agent Workspace — UX and Implementation Specification* is the
authoritative UX definition for the dock, the central workspace and the evidence panel.
Its headline decisions: the **dock is the control plane**, a new **GodotAI main-screen
workspace is the view plane**, and the **Activity bottom panel is the evidence plane**.
Several playable instances must not be squeezed into a side dock.

Its Phase Zero has been executed — see **DEC-0011**. Short version: **two game processes
already embed and render inside one editor window simultaneously.** The platform layer
was never single-instance; every backend keys embedding by process id. The limitation is
three editor-layer seams — argument gating to instance 0, one `EmbeddedProcess` per
`GameView`, and debugger operations that broadcast to every session. So the workspace is a
generalisation of existing seams, not a new harness and not a rewrite.

Still unmeasured: macOS (a different `EmbeddedProcessMacOS` path), Windows, Wayland, how
many instances stay usable at once, and anything about tile resize, focus or z-order.

## The terminal, landed 2026-08-26

The user's other instruction: port the agent terminal from `origin/GodotBeamDev` and
"pay very very careful attention to how that branch opens and closes the instances of
the windows since it was very very prone to crashing."

Done, in four layers — pty, VT emulator, widget, panel — each with its defects fixed at
the source rather than guarded against, and each covered by tests. `.agent/STATE.md`
carries the detail; `.agent/DECISIONS.md` DEC-0012 carries the reasoning. The one thing
not exercised here is Claude Code itself, which is not installed in this container.

Three defects the branch shipped, all now pinned by tests:

- The pty reaped its child inside a `const` query and could then signal a pid the
  operating system had already reissued.
- Ctrl-C reached the child as `ESC[3;5u` rather than `0x03`, so it interrupted nothing —
  the single most important key in a panel running an agent.
- The panel's `stop()` was never called on the way out, so every launch left its MCP
  configuration in the cache directory forever.

What the terminal is *for* sits inside the build order above rather than beside it: a
coding agent running in the editor, against the editor, is the shortest path to the
closed loop, and the Activity dock is the thing that makes what it does legible.

## Still blocked on hardware or a remote runner

- **C1** — watch `.github/workflows/godot_ai.yml` go green. Plausible for the first time.
- **R8** — run the cross-compiled relay on Windows. Never executed.
