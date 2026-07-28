# AGENTS.md

Tool-neutral guidance for coding agents working in this repository. Claude Code
sessions should also read `CLAUDE.md`, which imports this file's rules and adds the
continuity protocol.

## Repository purpose

This is a fork of Godot Engine 4.3 that adds Unity-style AI editor tooling:

- an MCP server inside the editor, with a schema-declared tool registry
- a local relay binary that bridges MCP stdio clients to the running editor
- capability-based permissions, client approval, and an audit trail
- (planned) skills, checkpoints, runtime inspection, and screenshot tools

The authoritative definition of the product is `docs/godot-ai-clone-spec.md`.
Implementation status per requirement lives in `.agent/SPEC_LEDGER.md`.

## Priority directories

- `modules/godot_ai/` — editor-side service, registry, protocol, permissions, tools
- `modules/godot_ai/tools/` — the built-in `Godot_*` tools
- `modules/godot_ai/tests/` — doctest cases, auto-included when built with `tests=yes`
- `tools/relay/` — the standalone `godot-ai-relay` binary and its tests
- `editor/` — engine editor code this module drives (4.3 layout: flat, not `editor/scene/…`)
- `.agent/` — persistent implementation state; read it before starting

## Build commands

```sh
# Editor. scu_build=yes is required for a tolerable build time (~8 min on 4 cores).
scons platform=linuxbsd target=editor dev_build=yes debug_symbols=no scu_build=yes tests=yes -j$(nproc)

# Relay: seconds, no engine dependency.
tools/relay/build.sh
```

First-time Linux dependencies: `libxcursor-dev libxinerama-dev libxi-dev
libxrandr-dev libasound2-dev`, plus `pip install scons`. Run `apt-get update` first.

## Test commands

```sh
python3 tools/relay/tests/run_tests.py                 # fastest signal, no engine build
cd /tmp && <repo>/bin/godot.linuxbsd.editor.dev.x86_64 --headless --test --test-case="*[godot_ai]*"
python3 tools/relay/tests/run_editor_e2e.py            # whole stack, ~30s
```

- Run the relay suite before the editor suite; a broken transport makes editor
  failures unreadable.
- Run the end-to-end script after any change to tool behaviour, path handling, or
  the protocol. It has already caught a defect that unit tests missed.
- Run the engine test binary from **outside** the checkout.

## Safety rules

- No tool may execute arbitrary shell commands.
- Every filesystem path goes through `MCPPaths::resolve`, which confines it to the
  project root and re-checks after symlink resolution. Do not bypass it.
- Every tool declares one capability class; `dangerous_exec` is deny-by-default and
  cannot be claimed by plugin-registered tools.
- Mutating tools must honour read-only sessions and, once checkpoints exist, must
  create one before changing project state.
- Keep relay stdout free of everything except protocol frames; diagnostics go to
  stderr.
- Never run a recursive delete rooted at the current working directory. Test
  fixtures delete through `mcp_test_remove_tree()`, which refuses anything outside
  the cache directory. (A fixture that ignored this once erased the whole checkout.)

## Sequencing constraints

- Editor mutations run on the main thread, go through `EditorUndoRedoManager`, and
  preserve node ownership. Never hand-edit `.tscn`/`.tres` as text when a structured
  editor API exists.
- The service polls from `NOTIFICATION_INTERNAL_PROCESS` behind a re-entrancy guard,
  because tools can pump the main loop.
- The relay and the editor share a bridge protocol version; change both together and
  bump `RELAY_BRIDGE_VERSION` / `MCPProtocol::BRIDGE_VERSION`.

## Conventions

- Godot style: licence header on new files, `p_` parameters, `_` private methods,
  `memnew`/`memdelete`, `Ref<>` for RefCounted, engine containers over STL.
- The relay is the exception: plain C++17 and the standard library, no engine headers.
- Clean-room implementation. Reproduce the specified *behaviour*; do not copy
  proprietary implementation code, and use `Godot_*` tool names.

## Done criteria

A change is not done until:

- the behaviour exists and is reachable through the real product path
- its schema is declared and enforced (discovery and execution use one schema)
- failure and permission-denied paths are covered, not just the happy path
- automated tests pass, and the end-to-end script still passes
- `.agent/SPEC_LEDGER.md` reflects the new status with real evidence
- the work is committed **and pushed**
