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

## How this relates to the fork

The draft this came from assumed an interface richer than the one that existed. Rather
than only correcting the document, most of those assumptions have since been **built**
— see `docs/godot-ai-agent-interface-spec.md` and `.agent/INTERFACE_LEDGER.md` in the
engine repository for what exists, what does not, and why.

What the fork gained because this template asked for it:

- **Real input into the running game**: `Godot_SendPointerInput`, `SendKeyInput`,
  `SendTouchInput`, `SendGamepadInput`, delivered through the same entry point the
  platform layer uses for physical hardware — plus `Godot_GetInputTrace`, so a claimed
  interaction is checkable rather than arguable.
- **Seeing the game**: `Godot_CaptureGame` for the running game, and
  `Godot_CaptureEditorWindow` for the whole screen including dialogs.
- **Runtime state**: `GetRuntimeProperty`, `GetRuntimeNodeInfo`,
  `WaitForRuntimeCondition` (so nothing sleeps), `GetRuntimeErrors` with file and line,
  `GetPerformanceMetrics`, `GetGameWindowInfo`, `SetGameWindowSize`.
- **Saves**: the `user://` tools, so save/load and corrupt-save recovery can be tested
  at all.
- **Project settings and named checkpoints.**

What is still genuinely absent is listed in `AGENTS.md`, and is short: audio
inspection, a test runner, frame-sequence capture, editor-side input injection, and
arbitrary shell execution — the last deliberately and permanently.

Two things the draft never mentioned, both still true and both worth knowing before a
first session:

- **Mutating tools are `ask` by default**, so an unattended agent is refused until
  permission is granted. This is the single most likely way a first session stalls.
- **The launched game inherits the project's renderer, not the editor's command line**
  — which is why `project.godot` here asks for `gl_compatibility`.

And two host-input behaviours that cost real debugging time, which still apply when
driving the *editor's* own UI: nothing restores X input focus when a popup closes, and
typed characters only land after the target field is clicked.

`BLOCKED` is narrowed to genuinely external conditions. "Needs a display" is not one of
them, because the fork ships a display.

The structure, voice and standards of the original are otherwise intact — in
particular the separation that matters most: the agent may build the game efficiently
through semantic editor tools, but it may not use those same shortcuts as proof that
the player experience works.
