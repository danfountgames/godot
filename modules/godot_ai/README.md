# Godot AI tooling

An MCP (Model Context Protocol) server built into the Godot editor, so external AI
clients — Claude Code, Cursor, Codex CLI, or anything else that speaks MCP over
stdio — can inspect and drive the editor under the user's control.

This module is the editor half. The client-facing half is `tools/relay/`.
The design this implements is described in `docs/godot-ai-clone-spec.md`.

## Architecture

```
┌────────────────────┐  MCP over stdio  ┌────────────────┐  NDJSON/TCP  ┌──────────────────┐
│ MCP client         │◄────────────────►│ godot-ai-relay │◄────────────►│ Godot editor     │
│ (Claude Code, …)   │   (JSON-RPC 2.0) │  (tiny binary) │  127.0.0.1   │ MCPService       │
└────────────────────┘                  └────────────────┘              └──────────────────┘
```

Two processes, on purpose:

- **The relay owns stdin/stdout.** The Godot editor prints to stdout freely (warnings,
  driver messages, `print()` from `@tool` scripts). If the editor spoke the protocol
  on stdio, any of that would corrupt a client's stream. The relay never writes
  anything to stdout but protocol frames, and drops any frame in either direction that
  is not a JSON object.
- **The editor owns the socket.** `MCPService` is an `EditorPlugin`, so its lifetime
  follows the editor's, exactly like the in-tree debug adapter and language servers.

The relay finds a running editor through a small registry: each editor writes
`$GODOT_AI_HOME/instances/<pid>.json` (default `~/.godot-ai`) describing its pid,
port, project and versions. Descriptors pointing at dead editors are pruned when a
connection is refused.

## Setting up a client

1. Open your project in the editor. The Output panel logs
   `--- Godot AI service listening on 127.0.0.1:<port> ---`.
2. Point your MCP client at the relay:

   ```json
   {
     "mcpServers": {
       "godot": {
         "command": "/path/to/godot/bin/godot-ai-relay",
         "args": ["--mcp", "--client-name", "claude-code"]
       }
     }
   }
   ```

   With several editors open, add `--project /path/to/project` or `--instance <pid>`;
   ambiguity is reported as an error rather than resolved by guessing.
3. The first connection from an unknown client is **refused**, and the client is
   listed as pending. Approve it in *Editor Settings → Network → Godot AI →
   Approved Clients*, then reconnect.

Relay flags: `--editor-socket <port|host:port>`, `--project`, `--instance`,
`--read-only`, `--approval-mode ask|allow|deny`, `--client-name`, `--log-level`,
`--handshake-timeout <ms>`, `--home`. Run `godot-ai-relay --help` for the full list;
all output goes to stderr.

## Permissions

Every tool declares exactly one capability class, and policy is applied per class —
so adding a tool cannot quietly widen what an agent can reach.

| Capability | Default | Meaning |
|---|---|---|
| `read_project` | allow | Read project files, assets, edited scene state |
| `read_runtime` | allow | Read state from the running game |
| `edit_files` | ask | Write files inside the project |
| `edit_scene` | ask | Mutate the edited scene or resources |
| `run_project` | ask | Start/stop play mode |
| `dangerous_exec` | **deny, always** | Arbitrary reach. Cannot be granted |

Policies live in *Editor Settings → Network → Godot AI*. Three rules hold regardless
of settings:

- A `--read-only` session refuses every mutating capability, even if policy says allow.
- A client's `--approval-mode` can **narrow** a decision but never widen a `deny`.
- `dangerous_exec` is always refused, and plugin-registered tools cannot claim it.

Filesystem access is confined to the project root. Paths are normalised, `..`
traversal is refused, and the result is re-checked after symlink resolution, so a
link inside the project cannot be a door out of it. `user://` is deliberately out of
reach.

Every invocation — allowed or refused — is appended to an audit log next to the
instance registry (`$GODOT_AI_HOME/audit/<project>.log`), with argument values whose
names look sensitive (`api_key`, `token`, `password`, …) redacted at source.

## Built-in tools

| Tool | Capability | Purpose |
|---|---|---|
| `Godot_GetEditorStatus` | read_project | Edited scene, play state, project root |
| `Godot_ListScenes` | read_project | Scene files, optionally under a folder |
| `Godot_ListAssets` | read_project | Project files, filtered by extension |
| `Godot_ReadTextFile` | read_project | Read a UTF-8 file from the project |
| `Godot_SearchProject` | read_project | Literal substring search with line numbers |
| `Godot_OpenScene` | read_project | Open a scene as the edited scene |
| `Godot_GetEditedSceneTree` | read_project | Node tree of the edited scene |
| `Godot_SaveScene` | edit_scene | Save the current or all open scenes |
| `Godot_WriteTextFile` | edit_files | Write a file and refresh the editor's view |
| `Godot_PlayCurrentScene` | run_project | Run the edited scene |
| `Godot_PlayMainScene` | run_project | Run the project's main scene |
| `Godot_StopPlaying` | run_project | Stop the running game |
| `Godot_ReadOutputLog` | read_project | Read the Output panel, including game output |
| `Godot_ListSkills` | read_project | Discovered workflow skills and their trust state |
| `Godot_ReadSkill` | read_project | A skill's instructions or a supporting file |
| `Godot_ListCheckpoints` | read_project | Snapshots taken before tools wrote files |
| `Godot_ManageNode` | edit_scene | Create, delete, rename or reparent a node |
| `Godot_UndoLastAction` | edit_scene | Undo the most recent editor action |
| `Godot_RedoLastAction` | edit_scene | Redo the most recently undone action |
| `Godot_RestoreCheckpoint` | edit_files | Put a checkpoint's files back |
| `Godot_CaptureViewport` | edit_files | Save the rendered editor viewport as a PNG |
| `Godot_CaptureInspectorProperty` | edit_files | Inspect a Resource or scene node, expand a raw property chain, highlight its final property, and save a centered Inspector-only crop |
| `Godot_CaptureSceneTreeNode` | edit_files | Open a scene, reveal and highlight an exact NodePath, and save a centered Scene-dock-only crop |

The two semantic documentation captures take `context_above` and `context_below` in
editor UI points. They return the target row's pixel position and height inside the
PNG, so a documentation pipeline can verify the framing without image recognition.
`Godot_CaptureInspectorProperty` accepts either `resource`, or `scene` plus
`node_path`; its `property_chain` uses exact raw Godot property names. Every
intermediate entry must be a non-null Resource and is expanded as a sub-inspector.
Both tools restore the prior scene/object, selection, folds, scroll position and dock
tab after the rendered frame is captured, including when the request times out or the
client disconnects.

Changes made while the game is running are **not persistent**. Tools that affect the
running game are named and documented separately from tools that change the project,
and the server states this in its `initialize` instructions.

### Driving a running game: pause before you measure

The first thing to know about the runtime tools, because two independent playtest agents
each lost a measurement to not knowing it: **a running game moves tens of frames between
two of your calls.** A property you set is read back after the world has moved on, and a
screenshot, a property and a scene tree fetched in three calls describe three different
moments. It is not latency you can tune away; it is what "running" means.

| Tool | Capability | Purpose |
|---|---|---|
| `Godot_PauseRuntime` | run_project | Pause or resume the game, and report which it is |
| `Godot_StepRuntimeFrames` | run_project | Run exactly N frames on the physics or process clock, then pause again |

Paused, every read answers about the same instant. Stepping turns "set this, then
something happened" into "set this, step one frame, and exactly this happened". The count
is exact rather than approximate, and the reason is where the pause lands: `SceneTree`
emits `physics_frame` at the top of `physics_process`, and `PhysicsServer::step()` runs
later in the same `Main::iteration`, so pausing inside the callback cancels that frame's
simulation. The countdown therefore runs to zero and re-pauses on the callback *after*,
which makes N frames happen rather than N−1.

Two traps, both of which have already caught someone here:

- **`Godot_SetTimeScale(0)` is not a pause.** It is a game running with a zero delta.
  Raising it is worse: Godot multiplies the physics delta by the scale, so a scene at 5x
  is a coarser scene rather than a faster one.
- **Do not difference `physics_frame` to count frames.** The engine's frame counter keeps
  advancing while the tree is paused — `Main::iteration` still runs its ticks, and only
  the physics servers and the inherited process callbacks stop. That subtraction measures
  how long the *caller* took; a one-frame step reads as five if you were slow. The
  `frames` field of a step result is the simulated count.

Pause stops nodes that inherit it. A node whose `process_mode` is Always keeps running —
usually a pause menu, but worth checking, because in an agent-driven game it is sometimes
the thing being measured.

### Undoing things

Three different scopes, deliberately not interchangeable:

| Scope | Reverts | How |
|---|---|---|
| Editor undo | in-memory scene edits that have not been saved | `Godot_UndoLastAction` |
| Checkpoints | files a tool wrote | `Godot_RestoreCheckpoint` |
| Version control | your own history | never touched by these tools |

Before any mutating tool runs, the protocol layer snapshots the files that tool
declares it may write, into `$GODOT_AI_HOME/checkpoints/<project>/<id>/` — outside
the project, so a snapshot is never imported or committed. The checkpoint id comes
back in the tool result's `_meta`. `Godot_ManageNode` declares no files: its changes
live in the undo history until you call `Godot_SaveScene`, which is the tool that
snapshots the scene file.

## Skills

Reusable workflow instructions discovered as `SKILL.md` files with YAML frontmatter,
from `res://ai_skills/`, `res://addons/*/ai_skills/` and the user's skill folder, in
that precedence order. A discovered skill is **not** trusted: `Godot_ListSkills`
shows it, but `Godot_ReadSkill` refuses its instructions until you allow it by name
in *Editor Settings → Network → Godot AI → Allowed Skills*. Supporting files load on
demand and cannot escape the skill's own folder. See `misc/godot_ai/skills/` for a
working example.

## Registering your own tools

The registry is exposed as the `MCPToolRegistry` engine singleton, so an editor
plugin can add tools without engine changes. Discovery and execution share one
schema, so what a client is told is exactly what gets enforced: unknown arguments are
rejected, missing required arguments are named, and declared defaults are filled in
before your handler runs.

```gdscript
@tool
extends EditorPlugin

func _enter_tree() -> void:
    MCPToolRegistry.register_tool({
        "name": "MyPlugin_CountNodes",
        "description": "Count nodes of a given class in the edited scene.",
        "capability": "read_project",
        "input_schema": {
            "type": "object",
            "properties": {
                "class_name": {"type": "string", "description": "Node class to count."},
            },
            "required": ["class_name"],
            "additionalProperties": false,
        },
        "output_schema": {
            "type": "object",
            "properties": {"count": {"type": "integer"}},
        },
        "handler": _count_nodes,
    })

func _exit_tree() -> void:
    MCPToolRegistry.unregister_tool("MyPlugin_CountNodes")

func _count_nodes(arguments: Dictionary) -> Dictionary:
    var root := EditorInterface.get_edited_scene_root()
    if root == null:
        # A dictionary with "error" is reported to the client as a tool failure.
        return {"error": "no scene is open"}
    return {"count": _count(root, arguments["class_name"])}

func _count(node: Node, wanted: String) -> int:
    var total := 1 if node.is_class(wanted) else 0
    for child in node.get_children():
        total += _count(child, wanted)
    return total
```

Registering a duplicate name fails rather than replacing the existing tool, so a
plugin cannot hijack a built-in. Registering or unregistering emits
`notifications/tools/list_changed` to every connected client.

Native tools subclass `MCPTool` (see `tools/mcp_project_tools.cpp`) and get the same
treatment.

## Building and testing

```sh
scons platform=linuxbsd target=editor dev_build=yes debug_symbols=no scu_build=yes tests=yes -j$(nproc)
tools/relay/build.sh

python3 tools/relay/tests/run_tests.py       # relay: transport, discovery, lifecycle
python3 tools/tests/run_tests.py             # the virtual display
cd /tmp && <repo>/bin/godot.linuxbsd.editor.dev.x86_64 --headless --test --test-case="*[godot_ai]*"
python3 tools/relay/tests/run_editor_e2e.py  # whole stack against a live editor
```

Run the engine test binary from outside the checkout. Fixtures create scratch data
under the cache directory and delete it through `mcp_test_remove_tree()`, which
refuses any path outside that directory.

## Working without a screen

Some of this toolset is only meaningful on screen: the three `Godot_Capture*` editor
tools photograph rendered UI, `Godot_AskUser` puts a dialog in front of someone, and a launched game has
to stay alive long enough to report its scene tree. A container has none of that, and
an editor started there runs headless — the visual tools then refuse, correctly but
uselessly.

`tools/virtual_display.py` supplies the missing screen. It starts an X server that
renders into memory, points Mesa's software OpenGL at it, and gives the editor the
renderer that stack can actually serve:

```sh
python3 tools/virtual_display.py --probe          # what is possible on this machine
python3 tools/virtual_display.py -- bin/godot.linuxbsd.editor.dev.x86_64 \
    --path /path/to/project --editor              # run the editor with a display
```

Nothing is simulated: the editor opens a real display, draws through a real GL
implementation, and a screenshot taken through it is what a user would have seen.
`run_editor_e2e.py` calls the same module, so it verifies screenshots and runtime
inspection anywhere `Xvfb` is installed, and falls back to checking the refusal paths
where it is not (`--headless` forces that fallback).

Ask the editor which it has rather than guessing: `Godot_GetEditorStatus` reports
`display_server` and `can_render`.

Install the dependencies with `apt-get install -y xvfb x11-utils libgl1-mesa-dri`.

## Serving more than one client, over HTTP

The stdio relay is a child process of one client. When that is the wrong shape - a
client that is not a child process, or several at once - the same binary serves MCP
over HTTP instead:

```sh
godot-ai-relay --http-port 7345                      # token generated and printed to stderr
godot-ai-relay --http-port 7345 --http-token <token> # or choose one
```

`POST /mcp` with a JSON-RPC message; `initialize` answers with an `Mcp-Session-Id`
header that later requests must carry, and `DELETE /mcp` ends the session. Every
session gets its own connection to the editor, which is what keeps concurrent clients
from reading each other's replies.

Two refusals are deliberate. Without a valid `Authorization: Bearer` header nothing
happens - the endpoint runs editor tools, so an open one is a remote code execution
service. And binding anywhere but loopback needs `--http-allow-remote` and a token you
chose yourself. Server-initiated streams (the SSE half of Streamable HTTP) are not
implemented: nothing in this toolset pushes to a client.

## Configuring a client

Rather than hand-editing an MCP client's configuration:

```sh
godot-ai-relay --list-backends
godot-ai-relay --install-backend stdio --backend-config ~/.config/<client>/mcp.json
godot-ai-relay --check-backends --backend-config ~/.config/<client>/mcp.json
```

The entry names the binary that wrote it, carries over the options that decide *which*
editor is driven (`--project`, `--read-only`, `--client-name`), and records the relay
and bridge versions it was generated for so `--check-backends` can tell you when it has
gone stale. Existing servers in the file are left alone.

The HTTP entry references `${GODOT_AI_HTTP_TOKEN}` rather than a token: configuration
files get copied, synced and pasted into bug reports, so `--install-backend` refuses
to write a secret into one.

## The chat panel

*AI Chat* is an editor dock, and a command palette entry (**Godot AI: Chat**) that
opens it with the caret already in the input.

The editor has no model. It has no API key, no vendor account, and no business
acquiring either — so the panel does not call a model at all. It asks the *client*
to, through MCP's `sampling/createMessage`: whichever agent is already connected to
this editor has a model, and sampling exists precisely so a server can borrow one.
Credentials stay where they already are, the choice of model stays where the user
already made it, and the panel works with whatever client is attached rather than one
this fork happened to bundle. With no sampling-capable client connected, the panel
says exactly that instead of failing quietly.

The conversation is kept in the editor's own settings directory, not in the project,
so it survives closing the editor without becoming an asset that gets imported or
committed. *Attach Edited Scene* stages the current scene for the next message; its
contents are read when the message is sent, go through the same project-root
confinement as every tool path, and are truncated with a note rather than silently
cut. A turn in flight can be cancelled, which tells the client to stop and refuses any
answer that arrives afterwards.

## Starting a game project

`misc/godot_ai/project_template/` is a bootstrap project for agent-driven game
production: a near-empty Godot project plus the `AGENTS.md` / `CLAUDE.md` instructions
an autonomous agent needs to build a game in it with these tools. Copy it, write
`docs/GAME_SPEC.md`, and point a client at the editor. Its README covers the
permission grant a first unattended session needs, and the parts of the interface that
do *not* exist — there is no input-injection tool, so real-input verification comes
from the host harness.

## Exported games

They get none of this. The module is editor-only (`config.py` `can_build`), because
every tool here drives an editor and a shipped game has no editor to drive - a game
that shipped an MCP server would be shipping a remote control for itself. The build
enforces it and `tools/tests/run_tests.py` checks both halves: that the module refuses
to build for any export template, and that a template binary on disk contains none of
its symbols.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `no running Godot editor … was found` | No editor open, or it uses a different `GODOT_AI_HOME` |
| `several editor instances are running` | Pass `--project` or `--instance` |
| `client '…' is not approved` | Approve it in Editor Settings and reconnect |
| `bridge protocol mismatch` | Relay and editor are from different builds; update both |
| `… did not answer the bridge handshake` | Editor is busy or wedged; raise `--handshake-timeout` |
| `… needs approval for the '…' capability` | Set that capability to `allow`, or use `--approval-mode allow` |
| `resolves outside the project directory` | Working as intended: tools cannot leave the project |

For diagnostics, run the relay with `--log-level debug`; it logs every forwarded
frame to stderr, never to stdout.

## Implementation notes

- `MCPProtocol::handle_message()` is a pure function over (message, session,
  delegate). It has no sockets and no editor globals, which is what makes the whole
  JSON-RPC surface unit-testable.
- `MCPService` polls from `NOTIFICATION_INTERNAL_PROCESS` behind a re-entrancy guard,
  because tools call editor operations that can pump the main loop.
- The relay and the editor share a bridge protocol version (`RELAY_BRIDGE_VERSION` /
  `MCPProtocol::BRIDGE_VERSION`); change them together.
- This is a clean-room implementation of the *behaviour* described in the
  specification. No proprietary implementation code was copied, and tool names are
  Godot-specific.
