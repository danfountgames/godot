# AGENTS.md

Tool-neutral guidance for coding agents working in this repository. Claude Code
sessions should also read `CLAUDE.md`, which imports this file's rules and adds the
continuity protocol.

## Repository purpose

This is a fork of Godot Engine 4.8-dev that adds Unity-style AI editor tooling:

- an MCP server inside the editor, with a schema-declared tool registry
- a local relay binary that bridges MCP stdio clients to the running editor
- capability-based permissions, client approval, and an audit trail
- filesystem-discovered skills, trusted only after the user allows them
- checkpoints taken before any tool writes to the project
- goal-directed playtests whose verdict is reconciled against what was actually pressed
- promoting a value tuned in the running game into the authored scene
- a terminal panel running a coding agent against this editor, over the relay
- benchmark projects with planted defects, and a scorecard that counts collateral damage
  as well as successes
- an approvals dialog and a chat dock (`mcp_approvals_dialog.*`, `mcp_chat_dock.*`),
  screenshots, ask-user, and runtime property inspection — all shipped and verified
  against a live editor; this line used to say "planned" and had been stale for a while

The authoritative definition of the product is `docs/godot-ai-clone-spec.md`.
Implementation status per requirement lives in `.agent/SPEC_LEDGER.md`.

## Priority directories

- `modules/godot_ai/` — editor-side service, registry, protocol, permissions, tools
- `modules/godot_ai/tools/` — the built-in `Godot_*` tools
- `modules/godot_ai/tests/` — doctest cases, auto-included when built with `tests=yes`
- `tools/relay/` — the standalone `godot-ai-relay` binary and its tests
- `tools/virtual_display.py` — an in-memory X server, so an editor can draw where there
  is no screen; `tools/tests/` covers it
- `editor/` — engine editor code this module drives (4.8 layout: nested — `editor/scene/`,
  `editor/docks/`, `editor/file_system/`)
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
python3 tools/tests/run_tests.py                       # virtual display, no engine build
python3 tools/skills/check_skills.py                   # shipped skills name tools that exist
python3 tools/benchmarks/tests/run_tests.py            # benchmark scoring, no engine build
python3 tools/benchmarks/run_selfcheck.py              # the benchmarks still measure something
bin/godot.linuxbsd.editor.dev.x86_64 --headless --test --test-case="*[godot_ai]*"
./bin/godot.linuxbsd.editor.dev.x86_64 --headless --test   # full suite, from the repo root
python3 tools/relay/tests/run_editor_e2e.py            # whole stack, ~40s
python3 tools/relay/tests/run_editor_e2e.py --headless # the same, forced without a display
python3 tools/relay/tests/run_editor_ui_e2e.py         # the editor's own UI, by keyboard and pointer
python3 tools/relay/tests/run_replay_two_editors.py    # record in one editor, replay in another
```

The end-to-end run starts a virtual display (`tools/virtual_display.py`) when the
machine has no screen, so it verifies the visual tools rather than only their refusals.
It needs `xvfb x11-utils libgl1-mesa-dri`; without them it degrades to the headless
path and says so. Run any editor by hand the same way:

```sh
python3 tools/virtual_display.py -- bin/godot.linuxbsd.editor.dev.x86_64 --path <project> --editor
```

- Run the relay suite before the editor suite; a broken transport makes editor
  failures unreadable.
- Run the end-to-end script after any change to tool behaviour, path handling, or
  the protocol. It has already caught a defect that unit tests missed.
- The module's own suite runs from any directory. The **full** engine suite must run
  from the repository root: several in-tree suites resolve their test data relative to
  the working directory and fail with "Invalid test directory" elsewhere.

## Working on macOS

Every command above names `linuxbsd` and assumes X11. On a Mac none of that applies —
substitute the following. This section is the whole delta; the rules, safety
constraints and done criteria are unchanged.

```sh
# Editor. Same flags, different platform; -j comes from sysctl, not nproc.
scons platform=macos target=editor dev_build=yes debug_symbols=no scu_build=yes tests=yes -j$(sysctl -n hw.ncpu)

# Add the branded bundle. Writes bin/GodotAI.app from misc/dist/macos_tools.app,
# stamps Info.plist from the template, and ad-hoc signs it (bundle_sign_identity
# defaults to "-"). It deletes and rebuilds bin/GodotAI.app each time.
scons platform=macos target=editor dev_build=yes scu_build=yes tests=yes generate_bundle=yes -j$(sysctl -n hw.ncpu)

# The binary is architecture-suffixed. On Apple silicon:
bin/godot.macos.editor.dev.arm64 --headless --test --test-case="*[godot_ai]*"
./bin/godot.macos.editor.dev.arm64 --headless --test   # full suite, from the repo root
```

Toolchain: Xcode command line tools plus `scons` (Homebrew installs it at
`/opt/homebrew/bin/scons`). The Linux `apt-get` dependency list is not needed and has
no macOS equivalent to install.

What changes about testing:

- **The relay, skills, benchmark and virtual-display-free suites run unchanged.**
  `tools/relay/build.sh` compiles with `$CXX -std=c++17` and no Linux-specific flags;
  the POSIX backend passes all 64 relay cases natively on arm64.
- **`run_editor_e2e.py` already knows about macOS.** It resolves
  `bin/godot.macos.editor.dev.<arch>` and uses `NativeMacOSDisplay` — the window server
  *is* the display, so there is no `DISPLAY`, no Xvfb, and no `--headless` fallback to
  apologise for. Run it exactly as written.
- **Do not reach for `tools/virtual_display.py`.** It is X11-only (it manages
  `/tmp/.X11-unix` sockets) and there is nothing for it to do on a machine that already
  has a screen. The instruction elsewhere that "a missing screen is not an external
  blocker" is a Linux-container rule; on macOS the screen is simply present.
- **`run_editor_ui_e2e.py` skips.** It drives the editor through `xdotool`, which is an
  X11 tool. The skip is clean and documented — it is not a failure, and it is not a
  reason to look for a macOS keyboard-automation substitute unless the task is that
  substitute.

Measured on this repository, native arm64, 2026-08-27: the module suite passes 74 cases
/ 526 assertions and the relay suite 64/64.

macOS-specific open work — game-process embedding, which needs a `CAContext` handshake
that X11 and Windows do not — has its own briefing in `.agent/MACOS_EMBEDDING_SPIKE.md`.
Read that before touching `platform/macos/editor/embedded_game_view_plugin.{h,mm}`.

## Safety rules

- No tool may execute arbitrary shell commands.
- Every filesystem path goes through `MCPPaths::resolve`, which confines it to the
  project root and re-checks after symlink resolution. Do not bypass it.
- Every tool declares one capability class; `dangerous_exec` is deny-by-default and
  cannot be claimed by plugin-registered tools.
- Mutating tools must honour read-only sessions. Files they may write are declared
  through `MCPTool::get_checkpoint_paths()`; the protocol layer snapshots them, so a
  tool must never write a file it did not declare.
- Keep relay stdout free of everything except protocol frames; diagnostics go to
  stderr.
- The HTTP transport is loopback-and-token by default. Do not add a path that serves
  MCP without authorisation, and never write a token into a generated client
  configuration - reference an environment variable instead.
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
