# CLAUDE.md

Project instructions for Claude Code sessions working in this repository.

This is a fork of **Godot Engine 4.3-dev** that adds a Unity-style AI tooling stack
(MCP server in the editor, an external stdio relay, skills, permissions, checkpoints).

Tool-neutral repository rules live in @AGENTS.md — read it as well; this file adds
only the Claude Code-specific workflow on top.

## Autonomous implementation continuity

Every session working on the AI tooling specification MUST start with this protocol.
Do not rely on chat history; the repository is the source of truth.

1. Read the primary specification: `docs/godot-ai-clone-spec.md`.
2. Read this `CLAUDE.md` and any nested `CLAUDE.md` in the subtree you are editing.
3. Read `.agent/SPEC_LEDGER.md` (requirement status).
4. Read `.agent/STATE.md` (current reality, active failures, in-flight operation).
5. Read `.agent/NEXT.md` (the immediate next action).
6. Inspect Git state: `git status --short`, `git log --oneline -5`, current branch.
7. Reconcile the written state against repository reality. **Repository contents and
   passing tests win over written claims.** Correct stale notes immediately.
8. Run the smallest useful health check (see *Canonical commands*; the relay test
   suite is the fastest signal and needs no engine build).
9. Resume the first concrete unblocked task from `.agent/NEXT.md`.
10. Update `.agent/` files throughout the session, after every meaningful result.
11. **Flush state before** stopping, compacting context, launching a risky or
    long-running operation, or returning control to the user. Set
    `In-flight operation` in `.agent/STATE.md` before any long command.

Persistent state lives in `.agent/`. Never scatter progress notes elsewhere, and
never put task progress, logs, or temporary next actions in `CLAUDE.md`.

## Work preservation (learned the hard way)

- **Commit AND push after every verified slice.** `git push -u origin <branch>`.
  A session already lost several hours of unpushed work to a destroyed working
  tree; a commit that only exists locally is not preserved.
- **Never run a recursive delete rooted at the current working directory.**
  In particular, `DirAccess::create(DirAccess::ACCESS_FILESYSTEM)` starts at the
  process CWD, so calling `erase_contents_recursive()` on it deletes the repository.
  That exact mistake in a test fixture erased this working tree, `.git` included.
  Delete test scratch directories with `mcp_test_remove_tree()`
  (`modules/godot_ai/tests/test_mcp_fs_helpers.h`), which refuses any path that is
  not under the cache dir and does not carry the `godot_ai_test_` marker.
- Test fixtures must create scratch data under
  `OS::get_singleton()->get_cache_path()`, never under the project or CWD.

## Repository layout (fork additions)

| Path | Purpose |
|---|---|
| `docs/godot-ai-clone-spec.md` | Authoritative specification |
| `modules/godot_ai/` | Editor-side MCP service, tool registry, tools, permissions |
| `modules/godot_ai/tests/` | doctest unit tests, auto-included when `tests=yes` |
| `tools/relay/` | Standalone `godot-ai-relay` stdio↔TCP bridge (no engine dependency) |
| `tools/relay/tests/` | Python integration tests for the relay (fast, no engine build) |
| `.agent/` | Persistent implementation-control workspace |

## Canonical commands

Build dependencies on a bare Ubuntu container (once; `apt-get update` first, or
`libasound2-dev` 404s on the stale index):
`libxcursor-dev libxinerama-dev libxi-dev libxrandr-dev libasound2-dev`, plus
`pip install scons`.

```sh
# Editor build. SCU build is REQUIRED for acceptable speed: ~8 min clean on 4 cores.
scons platform=linuxbsd target=editor dev_build=yes debug_symbols=no scu_build=yes tests=yes -j$(nproc)

# This module's tests (doctest). Runnable from any directory.
bin/godot.linuxbsd.editor.dev.x86_64 --headless --test --test-case="*[godot_ai]*"

# Full engine suite. Must run from the repository root: several in-tree suites
# resolve their test data relative to the working directory.
./bin/godot.linuxbsd.editor.dev.x86_64 --headless --test

# Relay build (seconds, no engine dependency).
tools/relay/build.sh

# Relay integration tests (fast loop, no engine build).
python3 tools/relay/tests/run_tests.py

# Whole stack: launches a headless editor and drives it through the real relay.
python3 tools/relay/tests/run_editor_e2e.py
```

Run the module's own suite from any directory; **run the full engine suite from the
repository root** (`./bin/godot… --headless --test`), because several in-tree suites
resolve their test data relative to the working directory. The reason the working
directory used to be dangerous is fixed at the source — fixtures delete through
`mcp_test_remove_tree()` — not by choosing a different directory.

## Engineering rules

- **Prefer the fast loop.** Relay/protocol work is verifiable in seconds via
  `tools/relay/tests/run_tests.py`. Only rebuild the engine when editor C++ changed.
- Record `In-flight operation` in `.agent/STATE.md` before starting a build.
  Incremental rebuilds after a module-only change take well under a minute.
- This tree is Godot **4.3**, not 4.6. The editor source is **flat**
  (`editor/editor_file_system.cpp`), not `editor/file_system/...`. The specification
  quotes 4.6-era paths; map them onto this tree, do not "fix" the spec.
- Follow Godot conventions: licence header on every new file, `p_` parameter prefix,
  `_` private-method prefix, `memnew`/`memdelete`, `Ref<>` for RefCounted,
  `String`/`Dictionary`/`Array` over STL in engine code.
- Editor mutations must run on the main thread, go through `EditorUndoRedoManager`,
  and preserve node ownership. Never hand-edit `.tscn`/`.tres` as text when a
  structured editor API exists.
- Keep the relay free of Godot dependencies; it must build standalone with a C++17
  compiler. **The relay owns stdio, the editor owns the socket** — that is what keeps
  engine prints out of a client's protocol stream.
- Clean-room only. Reproduce Unity's *behaviour* from the specification; never copy
  proprietary Unity implementation code, and use `Godot_*` tool names.

## Safety constraints

- Tools must never execute arbitrary shell commands.
- All filesystem tool paths are confined to the project root and re-validated after
  symlink resolution (`MCPPaths::resolve`).
- Every tool declares a capability class and honours the permission model
  (`read_project`, `read_runtime`, `edit_files`, `edit_scene`, `run_project`,
  `dangerous_exec`); `dangerous_exec` is deny-by-default and cannot be claimed by
  plugin-registered tools.
- Runtime (play-mode) edits and persistent scene edits must remain distinguishable
  in tool names, schemas, and documentation.
