# Godot AI tooling

An MCP (Model Context Protocol) server built into the Godot editor, so an agent in
the editor's terminal — or any external MCP client — can inspect and drive the live
editor under the user's control. The design this implements is described in
`docs/godot-ai-clone-spec.md`.

## Architecture

```
┌────────────────────┐  Streamable HTTP MCP  ┌──────────────────────────┐
│ Agent Terminal or  │◄─────────────────────►│ Godot editor / MCPService│
│ external client    │    127.0.0.1 + token  │ (the MCP server)         │
└────────────────────┘                       └──────────────────────────┘
            │                                             ▲
            └─ stdio-only clients: godot --godot-ai-stdio ┘
```

The editor serves Streamable HTTP MCP itself. The Agent Terminal connects directly;
there is no relay process in that product path. A client that only speaks stdio runs
the same editor binary as `godot --godot-ai-stdio`. That mode is entered before engine
initialisation and uses only engine-free C++17, so Godot's ordinary prints can never
corrupt protocol stdout.

The stdio gateway finds a running editor through a small registry: each editor writes
`$GODOT_AI_HOME/instances/<pid>.json` (default `~/.godot-ai`) describing its pid,
bridge port, HTTP port and token, project, and versions. The descriptor is mode 0600;
descriptors pointing at dead editors are pruned when a connection is refused.

## Setting up a client

1. Open your project in the editor. The Output panel logs
   `--- Godot AI service listening on 127.0.0.1:<port> ---`.
2. Point a stdio MCP client at the editor binary's gateway mode:

   ```json
   {
     "mcpServers": {
       "godot": {
        "command": "/path/to/bin/godot",
        "args": ["--godot-ai-stdio", "--mcp", "--client-name", "claude-code"]
       }
     }
   }
   ```

   With several editors open, add `--project /path/to/project` or `--instance <pid>`;
   ambiguity is reported as an error rather than resolved by guessing.
3. The first connection from an unknown client is **refused**, and the client is
   listed as pending. Approve it in *Editor Settings → Network → Godot AI →
   Approved Clients*, then reconnect.

Gateway flags: `--editor-socket <port|host:port>`, `--project`, `--instance`,
`--read-only`, `--approval-mode ask|allow|deny`, `--client-name`, `--log-level`,
`--handshake-timeout <ms>`, and `--home`. It also has focused scripting modes:
`--call`, `--batch`, `--list-tools`, `--describe`, `--list-prompts`, and `--prompt`.
Run `godot --godot-ai-stdio --help` for the full list; diagnostics go to stderr.

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
| `Godot_GetEditorStatus` | read_project | Edited scene, selection, workspace, open script, play state, project root |
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
| `Godot_RecallProjectMemory` | read_project | Recall durable project facts recorded by earlier sessions |
| `Godot_UpdateProjectMemory` | edit_files | Record, replace, or forget one durable project fact |
| `Godot_LookupClass` | read_project | Query this build's API reference and project script classes |
| `Godot_ManageConnection` | edit_scene | Persistently connect or disconnect a scene signal |
| `Godot_CaptureViewport` | edit_files | Save the rendered editor viewport as a PNG |
| `Godot_CaptureInspectorProperty` | edit_files | Inspect a Resource or scene node, expand a raw property chain, highlight its final property, and save a centered Inspector-only crop |
| `Godot_CaptureSceneTreeNode` | edit_files | Open a scene, reveal and highlight an exact NodePath, and save a centered Scene-dock-only crop |
| `Godot_CompareCaptures` | edit_files | Measure two captures and optionally save an annotated diff |
| `Godot_RecordRuntimeSeries` | read_runtime | Sample one live property at game-frame rate |
| `Godot_FindRuntimeNodes` | read_runtime | Find transient nodes by class, name, or position |
| `Godot_PauseRuntime` | run_project | Pause, resume, or query the running game |
| `Godot_StepRuntimeFrames` | run_project | Advance an exact number of physics or process frames and pause again |

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

### Undoing things

Three different scopes, deliberately not interchangeable:

| Scope | Reverts | How |
|---|---|---|
| Editor undo | in-memory scene edits that have not been saved | `Godot_UndoLastAction` |
| Checkpoints | files a tool wrote | `Godot_RestoreCheckpoint` |
| Task checkpoint | all files written under one `Godot_SetIntent` task | `Godot_RestoreCheckpoint` with that task key |
| Version control | your own history | never touched by these tools |

Before any mutating tool runs, the protocol layer snapshots the files that tool
declares it may write, into `$GODOT_AI_HOME/checkpoints/<project>/<id>/` — outside
the project, so a snapshot is never imported or committed. The checkpoint id comes
back in the tool result's `_meta`. `Godot_ManageNode` declares no files: its changes
live in the undo history until you call `Godot_SaveScene`, which is the tool that
snapshots the scene file.

## Skills

Reusable workflow instructions are discovered as `SKILL.md` files with YAML
frontmatter, from `res://ai_skills/`, `res://addons/*/ai_skills/`, the user's skill
folder, and the library embedded in the editor binary. A filesystem skill is **not**
trusted: `Godot_ListSkills` shows it, but its instructions stay unavailable until the
user allows it in the Godot AI dialog. Built-ins arrived with the trusted editor binary
and are usable by default, but the user can revoke them. Supporting files load on
demand and cannot escape the skill's own folder.

Allowed skills are also MCP prompts, so clients can enter through a named job rather
than compose a large primitive tool surface from scratch. The stdio gateway exposes
the same path with `--list-prompts` and `--prompt <name> --context <text>`. See
`misc/godot_ai/skills/` for the shipped library.

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

python3 tools/relay/tests/run_tests.py       # embedded gateway: transport, discovery, CLI
python3 tools/tests/run_tests.py             # the virtual display
cd /tmp && <repo>/bin/godot.linuxbsd.editor.dev.x86_64 --headless --test --test-case="*[godot_ai]*"
python3 tools/relay/tests/run_editor_e2e.py  # whole stack against a live editor
```

Run the engine test binary from outside the checkout. Fixtures create scratch data
under the cache directory and delete it through `mcp_test_remove_tree()`, which
refuses any path outside that directory.

## Working without a screen

Some of this toolset is only meaningful on screen: editor capture tools photograph
rendered UI and `Godot_AskUser` puts a dialog in front of someone. In a genuinely
headless editor, captures refuse immediately, questions refuse rather than waiting for
a user who cannot exist, and proposals return a dry run. Games still launch through
the headless display driver with the project's configured viewport, so runtime state,
input, sampling, playtests, and scene tests remain useful without pixels.

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

## Direct HTTP clients

The editor listens on loopback for Streamable HTTP MCP (port 6110 by default, probing
nearby ports when occupied). `POST /mcp` with a JSON-RPC message; `initialize` answers
with an `Mcp-Session-Id` header that later requests must carry, and `DELETE /mcp` ends
the session. Each session has independent permissions and deferred calls.

The current port and bearer token are in the editor's mode-0600 instance descriptor.
Without `Authorization: Bearer <token>`, the endpoint returns 401. It cannot bind to a
non-loopback address. `GODOT_AI_HTTP_TOKEN` may supply the per-run token; otherwise the
editor generates one. Never write the token into a client configuration—reference an
environment variable, as the Agent Terminal does with `${GODOT_AI_MCP_TOKEN}`.

## The Agent Terminal

The Agent Terminal is the editor's one conversation surface. It runs the user's coding
agent in a real pseudo-terminal, gives supported clients a secretless direct-HTTP MCP
configuration, and briefs Claude Code at launch on the run → observe → diagnose → fix
loop. The token exists only in the child environment; the generated config contains an
environment-variable reference. Unknown commands are started exactly as typed instead
of being handed guessed vendor-specific flags.

The strip above the terminal shows the active intent, latest tool and affected object,
and whether the agent is running, paused, or stopped. Stop is enforced by the service,
not just painted in the UI: a stopped session cannot release itself. The Activity
panel remains the full audit/evidence view.

## Starting a game project

`misc/godot_ai/project_template/` is a bootstrap project for agent-driven game
production: a near-empty Godot project plus the `AGENTS.md` / `CLAUDE.md` instructions
an autonomous agent needs to build a game in it with these tools. Copy it, write
`docs/GAME_SPEC.md`, and point a client at the editor. Its README covers permission,
the authored/runtime distinction, and the evidence expected before calling work done.

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
| `bridge protocol mismatch` | Gateway and running editor are from different builds; update and restart the editor binary |
| `… did not answer the bridge handshake` | Editor is busy or wedged; raise `--handshake-timeout` |
| `… needs approval for the '…' capability` | Set that capability to `allow`, or use `--approval-mode allow` |
| `resolves outside the project directory` | Working as intended: tools cannot leave the project |

For gateway diagnostics, add `--log-level debug`; it logs forwarded frames to stderr,
never to stdout.

## Implementation notes

- `MCPProtocol::handle_message()` is a pure function over (message, session,
  delegate). It has no sockets and no editor globals, which is what makes the whole
  JSON-RPC surface unit-testable.
- `MCPService` polls from `NOTIFICATION_INTERNAL_PROCESS` behind a re-entrancy guard,
  because tools call editor operations that can pump the main loop.
- The embedded gateway and the editor share a bridge protocol version (`RELAY_BRIDGE_VERSION` /
  `MCPProtocol::BRIDGE_VERSION`); change them together.
- This is a clean-room implementation of the *behaviour* described in the
  specification. No proprietary implementation code was copied, and tool names are
  Godot-specific.
