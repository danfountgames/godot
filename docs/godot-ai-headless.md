# Driving this editor with no screen and nobody watching

Everything the tooling does is reachable from a shell. No MCP client, no editor window,
nobody to click a dialog. That is a first-class mode rather than a degraded one: CI runs
here, scripted batches run here, and an agent driving the editor from a terminal runs
here.

Every command below was run against a real editor on this branch, and the output is
quoted as it came back.

## The shortest thing that works

```sh
# 1. An editor, serving. --headless is Godot's own flag; --editor is what makes it an
#    editor session rather than a game.
GODOT_AI_APPROVE_CLIENTS=1 \
  bin/godot.linuxbsd.editor.dev.x86_64 --headless --path <project> --editor &

# 2. One tool call. The relay finds the editor by the project it has open.
bin/godot-ai-relay --call Godot_GetEditorStatus --project <project>
```

The editor announces what it will allow, on stdout, before anything connects:

```
--- Godot AI service listening on 127.0.0.1:6010 ---
Godot AI: unattended (no display, or declared); clients pre-approved; policy read_project=allow edit_files=deny
```

That second line only appears when something is configured. It is printed rather than
only logged to the editor's Output panel, because in the run it describes there is
nobody to look at a panel — a CI job's stdout is the record of what the agent was
permitted to do.

## The ways in

Start with the skills. They are the intended entry point for the same reason they are
served as MCP prompts — a named job with the primitives composed underneath is more
reliable than assembling the same sequence out of ninety-six of them — and `--call`
never sees the `initialize` instructions that say so.

```sh
bin/godot-ai-relay --list-prompts --project <project>
bin/godot-ai-relay --prompt investigate-a-crash --context "the boss arena" --project <project>
```

Only skills the user has allowed are listed; the trust decision gates the CLI exactly as
it gates everything else. `--prompt` exits 1 for a skill that does not exist, so a script
can tell a missing one from a delivered one without parsing.

| Command | For |
|---|---|
| `--list-prompts` / `--prompt <name>` | The skills: named jobs, and their instructions. Start here. |
| `--call <tool> --arguments <json>` | One tool, one result on stdout as JSON, exit. |
| `--batch` | A JSON array of calls on stdin, run over **one** connection. |
| `--describe <tool>` | One tool's schema. Reach for this before a call you are unsure of - the argument names are not always the obvious ones. A near miss suggests the right name. |
| `--list-tools` | Every tool with its schema, as JSON. 99 on this branch. |
| `--mcp` (default) | Serve MCP over stdio to a client. `--http-port` for HTTP instead. |

Exit status is the thing to check: 0 for success, 1 when the editor answered with an
error, 2 when the relay could not reach an editor at all.

Use `--batch` rather than a loop of `--call`. Each `--call` pays a process launch, a
connect and a handshake — about half a second — which is fine for three calls and
ruinous for a thousand. By default a batch stops at the first failure, because a batch
is a sequence and a call after a failed gate proceeds on an assumption that never held;
`--continue-on-error` overrides that when the entries really are independent.

`bin/godot-ai-relay --help` is the full list. `--read-only` requests a session that
refuses every mutating tool, and no policy can widen it back.

## Saying what the agent may do

Deny-by-default is right, and a headless run has no settings dialog to change it in.
State the policy in the environment that starts the editor:

```sh
GODOT_AI_POLICY="read_project=allow,read_runtime=allow,run_project=allow,edit_files=deny"
```

| Variable | Effect |
|---|---|
| `GODOT_AI_POLICY` | Per capability: `allow`, `ask` or `deny`. Highest precedence. |
| `GODOT_AI_APPROVE_CLIENTS=1` | Any client may connect. There is nobody to approve one by hand. |
| `GODOT_AI_UNATTENDED=1` | Declare that nobody is watching even where a display exists. |
| `GODOT_AI_AUTO_APPROVE=1` | The blunt one: approve clients and resolve every prompt to yes. Kept for CI. |
| `GODOT_AI_HOME` | Where the relay keeps its state and instance descriptors. |

The capability names are the ones the tools declare: `read_project`, `read_runtime`,
`edit_files`, `edit_scene`, `run_project`, `simulate_input`, `read_user_data`,
`edit_user_data`.

Four properties of this are deliberate, and each is a way it could have gone wrong:

- **It outranks editor settings.** The operator who launched this process is more
  authoritative about what this run may do than whatever the last interactive session
  happened to leave behind in someone's `editor_settings.tres`.
- **It fails closed, entirely.** One malformed entry grants nothing, including the
  well-formed entries beside it. "The operator meant something and we could not tell
  what" has only one safe reading.
- **It cannot reach `dangerous_exec`.** Refused when the policy is parsed, so a
  configuration that tries is *reported* rather than quietly ignored, and refused again
  before any policy is consulted. No tool in this interface runs an arbitrary shell
  command and no setting can make one.
- **It never widens a narrower session.** A `--read-only` client still refuses what the
  policy allows, and an unapproved client is still refused whatever it says.

A denial names where the decision came from, so you know which file to edit:

```
$ bin/godot-ai-relay --call Godot_WriteTextFile \
    --arguments '{"path":"res://x.txt","text":"hi"}' --project <project>
{"code":-32013,"data":{"capability":"edit_files"},
 "message":"the 'edit_files' capability is set to deny by GODOT_AI_POLICY"}
```

## What behaves differently with nobody there

Two tools need a human. They answer immediately rather than waiting, because an editor
is not a person: `--headless --editor` has a complete `EditorNode` and nobody looking at
it, so a dialog would open on the dummy display server and the caller would wait out its
whole timeout. That used to be five minutes of a CI job spent asking a question no one
could see.

- **`Godot_AskUser`** refuses at once, and says why: *"there is nobody here to answer a
  question… Decide it yourself from what you can observe, or leave the decision to
  whoever reads the report."*
- **`Godot_ProposeChange`** degrades to the dry run it already knew how to produce. The
  plan is still built, validated and risk-grouped; `unattended: true` and `decided:
  false` say plainly that nobody approved anything. Erroring would have thrown away work
  that is still worth having.

Both are measured in the end-to-end suite, in the headless configuration, and both come
back in 0.0s.

## What needs a screen, and how to get one without a monitor

Screenshots, dialogs, the editor's own UI under a pointer, and anything that depends on
the editor drawing a frame. `Godot_GetEditorStatus` reports `can_render`, so an agent can
find out that screenshots are impossible here without taking one and reading the refusal.

A missing screen is not a blocker. `tools/virtual_display.py` starts an X server in
memory:

```sh
python3 tools/virtual_display.py -- bin/godot.linuxbsd.editor.dev.x86_64 --path <project> --editor
```

It needs `xvfb x11-utils libgl1-mesa-dri`; without them it says so and degrades to the
headless path rather than pretending. Reach for it before recording anything as
environmental — doing so once turned up a real product bug that the refusal paths had
been hiding.

## Driving the game itself, headless

The game runs and the closed loop works: launch it, read its live scene tree, read and
write properties on running nodes, click, drag, scroll, tap, drive it by action and key,
profile it, record and replay a session, stop it. The end-to-end suite covers **125 of
its 135 checks in the headless configuration** — near parity, and every difference below
is a real property of having no renderer rather than a gap in the tooling.

### Pause and step, before anything you need to be exact about

The mistake worth avoiding first, because two agents each lost a measurement to it: a
running game moves tens of frames between two of your calls. A property you set is read
back after the world has moved on, and a screenshot, a property and a scene tree fetched
in three calls describe three different moments. That is not a latency problem you can
tune away; it is what "running" means.

```sh
bin/godot-ai-relay --call Godot_PauseRuntime      --arguments '{"paused": true}'  --project <path>
bin/godot-ai-relay --call Godot_StepRuntimeFrames --arguments '{"frames": 1}'     --project <path>
bin/godot-ai-relay --call Godot_PauseRuntime      --arguments '{"paused": false}' --project <path>
```

Paused, every read answers about the same instant and there is no hurry about taking it.
`Godot_StepRuntimeFrames` runs exactly the frames you asked for and stops again, so
"set this, step one frame, read it" is a cause rather than a correlation. It is exact,
not approximate — measured against a body holding a constant 430 px/s, one stepped frame
moves it 7.1667px and five move it 35.8333px, which is speed/60 per frame to four
decimal places.

Two things to know:

- **`Godot_SetTimeScale(0)` is not a pause.** It is a game running with a zero delta, and
  any raised scale changes the physics rather than the pace — Godot multiplies the
  physics delta by it, so a scene at 5x is a coarser scene, not a faster one.
- **Do not difference `physics_frame` to count frames.** The engine's frame counter keeps
  advancing while the game is paused, because `Main::iteration` still runs its ticks and
  only the physics servers and the inherited process callbacks stop. That subtraction
  measures how long *you* took. The `frames` field of a step result is the simulated
  count.

Pause stops nodes that inherit it; a node whose `process_mode` is Always keeps running.
Usually that is a pause menu, but check — in an agent-driven game it is sometimes the
thing being measured.

### Two things that had to be made true

This was not true until the tooling made it true, and both halves are worth knowing
because they change how a headless game behaves.

**A headless editor launches a headless game.** It has to. Without being told which
display driver to use the child went looking for X11, failed to create a DisplayServer,
and segfaulted on the way out — so from the editor's side the game simply vanished, with
`playing` back to false and nothing in the log.

**A headless game is given the project's configured viewport.** The dummy display server
never applies `display/window/size/viewport_width|height`, so a headless game used to lay
its entire interface out inside a 100×100 stub. Every control was then somewhere no
coordinate from the real design pointed at, and `Godot_SendPointerInput` correctly
refused every click — which left an agent able to read a running game and drive it only
by action, never by pointer. `Window::set_size()` does work on the root with no display;
the size was simply never set. The runtime agent now sets it on its first request and
says so:

```
Godot AI: this game has no display, so its viewport was 100x100; set to the
project's configured 1152x648 so laid-out controls are where they belong.
```

### The one thing that genuinely does not work

**Nothing draws.** There is no renderer, the game never produces a frame, and so
`Godot_CaptureGame` and `Godot_CaptureFrameSequence` refuse — with *"the running game has
not rendered a frame yet"*, which is the truth rather than a fudge. Anything downstream
of a picture goes with them: the provenance a capture carries about time scale has no
image to be carried on.

Everything else that once looked like a headless limitation was not one. If you need
pictures, use a virtual display rather than headless — `tools/virtual_display.py` — and
the same suite covers that path with the capture checks enabled.

## One headless difference worth knowing about

Godot does not generate documentation for a project's own script classes when the editor
is in command-line mode, which it decides from the display server being headless. There
are two separate gates for it, in `EditorFileSystem` and again in `EditorHelp`. Both are
reasonable for an editor launched to export a build and exit, and both are wrong for one
launched to be driven.

`Godot_LookupClass` works around it without an engine change: when the class reference
does not have a class, it asks `ScriptServer` whether it is a global script class and
reads the documentation off the script. So the project's own classes are available
headless and under a display alike, and the end-to-end suite checks both.

## Running the suites this way

```sh
python3 tools/relay/tests/run_editor_e2e.py --headless   # forces the no-display path
python3 tools/relay/tests/run_editor_e2e.py              # starts a virtual display if needed
```

The headless run skips the checks that genuinely need a screen and says which — a check
that passes because nothing happened is not a check, and two of them used to.
