# Bootstrap project for agent-driven game production

A near-empty Godot project plus the instruction files an autonomous coding agent needs
to build a game in it with this fork's editor tooling.

Copy it, write the specification, point an agent at it:

```sh
cp -r misc/godot_ai/project_template ~/games/my-game
cd ~/games/my-game
git init && git add -A && git commit -m "Bootstrap"
$EDITOR docs/GAME_SPEC.md          # this is the part only you can do
```

Then open the project in this fork's editor, connect your MCP client through
`godot-ai-relay`, and give the agent a prompt roughly this size:

> Build the complete game described by `docs/GAME_SPEC.md`. Use the connected Godot
> Agent Interface and follow all instructions in `AGENTS.md`. Begin directly without
> asking me to approve a plan. Use real editor operations, real running-game input,
> screenshot inspection, independent review agents, and repeated
> implementation/playtest loops. Continue autonomously until the full Definition of
> Done is satisfied. Escalate only a genuine external blocker as defined in
> `AGENTS.md`.

## What is here

| Path | Purpose |
|---|---|
| `AGENTS.md` | The instructions. Tool-neutral; Codex reads it directly |
| `CLAUDE.md` | A thin adapter that imports `AGENTS.md` and adds Claude Code specifics |
| `docs/GAME_SPEC.md` | The template you replace with your game |
| `project.godot` | Minimal project, renderer set for machines without a GPU |
| `scenes/main.tscn` | A placeholder for the agent to replace |
| `.agent/` | Seeded production memory: goals, state, next actions, decisions, tooling, test matrix, playtest and visual logs, issues, evidence index |

One canonical instruction file, imported rather than duplicated: `CLAUDE.md` begins
with `@AGENTS.md`, so there is no second copy to drift.

## Before the first session

The editor refuses unknown clients and asks before mutating anything, so an autonomous
session needs permission granted once — this is a decision only you can make:

- approve the client and set the mutating capabilities to `allow` in
  *Editor Settings → Network → Godot AI* (the **Godot AI: Clients and Skills** command
  palette entry opens the same dialog), or
- run the relay with `--approval-mode allow` for a session you are supervising.

`GODOT_AI_AUTO_APPROVE=1` bypasses first-connection approval and skill trust for CI and
unattended runs. It is a deliberate opt-in, not a default.

On a machine with no screen, give the editor one — otherwise every visual and
real-input goal is unverifiable:

```sh
python3 tools/virtual_display.py --probe
python3 tools/virtual_display.py -- bin/godot.linuxbsd.editor.dev.x86_64 \
    --path ~/games/my-game --editor
```

## What was corrected from the original draft

The draft this came from assumed an interface richer than the one that exists. Those
assumptions are the expensive kind: an agent plans a verification strategy around a
tool, then discovers mid-session that it was never there. The corrections:

- **There is no virtual input tool.** Nothing in the MCP surface injects mouse,
  keyboard, touch or gamepad input. Real-input validation — which `AGENTS.md` still
  requires — comes from the host (`xdotool` against a real or virtual display), and
  `tools/relay/tests/run_editor_ui_e2e.py` is a working example to copy.
- **`Godot_CaptureViewport` photographs the editor, not the running game.** A game in
  its own window, and editor dialogs, are separate OS windows and will not appear.
  Capture those at the host level.
- **There is no profiler, video capture, input-trace recorder, audio inspection, or
  test-runner tool.** Performance and audio claims must come from instrumentation the
  agent adds, and the limitation must be recorded rather than papered over.
- **Filesystem tools cannot reach `user://`**, where saves normally live.
- **Mutating tools are `ask` by default**, so an unattended agent is refused until
  permission is granted. The draft never mentioned this, and it is the single most
  likely way a first session stalls.
- **The launched game inherits the project's renderer, not the editor's command line**
  — which is why `project.godot` here asks for `gl_compatibility`.
- Two host-input behaviours that cost real debugging time are documented: nothing
  restores X input focus when a popup closes, and typed characters only land after the
  target field is clicked.
- `BLOCKED` is narrowed to genuinely external conditions. "Needs a display" is not one
  of them, because the fork ships a display.

The structure, voice and standards of the original are otherwise intact — in
particular the separation that matters most: the agent may build the game efficiently
through semantic editor tools, but it may not use those same shortcuts as proof that
the player experience works.
