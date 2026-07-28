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

Changes made while the game is running are **not persistent**. Tools that affect the
running game are named and documented separately from tools that change the project,
and the server states this in its `initialize` instructions.

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
cd /tmp && <repo>/bin/godot.linuxbsd.editor.dev.x86_64 --headless --test --test-case="*[godot_ai]*"
python3 tools/relay/tests/run_editor_e2e.py  # whole stack against a live editor
```

Run the engine test binary from outside the checkout. Fixtures create scratch data
under the cache directory and delete it through `mcp_test_remove_tree()`, which
refuses any path outside that directory.

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
