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
| 2 | Observe `.github/workflows/godot_ai.yml` succeeding on Actions | **Not done — but unblocked.** `run_editor_e2e.py` had never been able to pass on Linux: a hardcoded drag coordinate exceeded the 846px window a virtual display gives, so the suite failed on an unrelated check. Fixed; it now passes on Linux with a display and headless. That was very likely the blocker. Someone still has to push and watch a run. |
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
5. **Promote runtime values, and a focused live-tuning workspace.** Promotion is small and
   closes the loop: the agent adjusts a value in the running game, you decide it feels
   right, the temporary value becomes the authored value. Without it, live tuning is
   theatre because the last act is manual. Frame variants as a **live tuning workspace**,
   not as general AI-generated alternatives.
6. **Grow the skills library around concrete jobs**: crash investigation, performance
   regression, menu traversal, input-path testing, scene cleanup.

Alongside all of it: **benchmark projects and real success rates**.

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
- **No evaluation.** Counts of tools, tests and verified requirements measure engineering
  coverage, not agent effectiveness. Build benchmark Godot projects and measure: how often
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
