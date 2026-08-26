# DECISIONS

Durable decisions that are not obvious from the resulting code.

---

## DEC-0001 — Editor hosts a local TCP listener; the relay owns stdio

- **Date:** 2026-07-28
- **Context:** The specification requires a two-process split (external client launches
  `godot-ai-relay` over stdio; the relay reaches the running editor over local IPC) and
  requires stdout purity. The Godot editor writes to stdout freely, so the editor
  process can never own the MCP stdio stream.
- **Decision:** The editor hosts a JSON-RPC listener on `127.0.0.1` (loopback TCP,
  NDJSON framing). `godot-ai-relay` is a separate, tiny process that owns stdin/stdout
  and forwards to that socket.
- **Alternatives considered:** (a) `godot --mcp` running the protocol in the engine
  process — rejected, engine stdout pollution and a heavyweight process per client;
  (b) Unix domain sockets — rejected as primary transport because Windows support is
  inconsistent in this engine version, and loopback TCP is what DAP/LSP already use
  in-tree; (c) named pipes — per-platform work with no benefit at this stage.
- **Consequences:** A port-based instance registry is needed (DEC-0003). Loopback
  exposure is mitigated by client approval (F6).
- **Spec IDs:** MCP design section, R1–R7, F2, F3.
- **Locations:** `tools/relay/`, `modules/godot_ai/mcp_service.*`.

---

## DEC-0002 — Spec paths are 4.6-era; this tree is 4.3 and paths are remapped

> **SUPERSEDED by DEC-0009 (2026-08-26).** The tree is now 4.8-dev with a nested
> editor layout. The mapping below is historical; do not apply it.

- **Date:** 2026-07-28
- **Context:** The specification's hook table names 4.6-era paths. This checkout is
  4.3-dev with a flat editor layout.
- **Decision:** Implement against this tree's real paths and record the mapping. The
  spec's intent (which subsystem to hook) is authoritative; its literal paths are not.
- **Mapping:** `editor/file_system/editor_file_system.cpp` → `editor/editor_file_system.cpp`;
  `editor/docks/filesystem_dock.cpp` → `editor/filesystem_dock.cpp`;
  `editor/scene/canvas_item_editor_plugin.cpp` → `editor/plugins/canvas_item_editor_plugin.cpp`;
  `editor/scene/3d/node_3d_editor_plugin.cpp` → `editor/plugins/node_3d_editor_plugin.cpp`;
  `editor/docks/scene_tree_dock.cpp` → `editor/scene_tree_dock.cpp`.
- **Consequences:** A future rebase onto 4.4+ must revisit this mapping.

---

## DEC-0003 — Editor instance discovery via a per-instance registry file

- **Date:** 2026-07-28
- **Context:** The relay must find a running editor for a project without being told a
  port, and must detect stale instances.
- **Decision:** The editor writes a JSON descriptor per running instance into
  `$GODOT_AI_HOME/instances/<pid>.json` (default `~/.godot-ai`) containing pid, port,
  project path/name, versions and start time. The relay selects by `--editor-socket`,
  else `--instance`, else `--project`, else a single live instance; ambiguity is an
  error, not a coin flip. Liveness is confirmed by connecting; unreachable descriptors
  are pruned.
- **Alternatives considered:** fixed default port (breaks with multiple editors);
  broadcast discovery (adds network surface for no gain).
- **Consequences:** Descriptors are removed on clean shutdown; crashes leave stale
  files, which the prune path tolerates.
- **Spec IDs:** R3, R4.

---

## DEC-0004 — Relay is C++17 with a hand-written JSON reader, not Python

- **Date:** 2026-07-28
- **Context:** The relay must be a distributable binary with pure stdout, and must
  inspect message `id`/`method` to synthesise errors when the editor is unreachable.
- **Decision:** Standalone C++17 in `tools/relay/`, no engine headers, no third-party
  dependencies, built by `tools/relay/build.sh`.
- **Reason:** Keeps the fast verification loop free of the engine build, the single
  biggest velocity factor in this repository.
- **Consequences:** The relay stays a thin, well-tested pipe; complex JSON handling
  belongs on the editor side.
- **Spec IDs:** R1, R2, R7.

---

## DEC-0005 — Protocol handling is separated from transport so it can be unit-tested

- **Date:** 2026-07-28
- **Context:** Protocol conformance is the specification's largest test surface, and
  socket-driven tests are slow and flaky.
- **Decision:** `MCPProtocol::handle_message()` is a pure function over
  (message, session, delegate). Everything it needs from the outside — client approval,
  per-invocation prompts, project metadata, audit recording — arrives through
  `MCPProtocol::Delegate`. `MCPService` supplies the real delegate and owns the socket.
- **Consequences:** The whole JSON-RPC surface is covered by doctest cases with a
  scripted delegate; the socket layer only has to be right about framing.
- **Spec IDs:** P1–P6, F3, F6.

---

## DEC-0006 — Test scratch directories are deleted through a guarded helper

- **Date:** 2026-07-28
- **Context:** A test fixture called `erase_contents_recursive()` on a `DirAccess`
  created with `ACCESS_FILESYSTEM`. That handle starts at the process working
  directory, so running the test binary from the repository root **deleted the entire
  working tree, `.git` included**, losing all unpushed work.
- **Decision:** Test fixtures never call `erase_contents_recursive()`. They delete via
  `mcp_test_remove_tree()` in `modules/godot_ai/tests/test_mcp_fs_helpers.h`, which
  walks an explicit absolute path and refuses anything that is not under the cache
  directory and does not contain the `godot_ai_test_` marker.
- **Consequences:** Fixtures are slightly more verbose; a fixture bug can no longer
  reach outside its own scratch directory. The incident also established the
  commit-and-push-per-slice rule in `CLAUDE.md`.
- **Spec IDs:** affects all engine-side test entries.

## DEC-0007 — The repository supplies its own display, at the launcher and not inside the engine

- **Date:** 2026-07-28
- **Context:** Screenshots, dialogs and a running game's scene tree all need a screen.
  This container has none, so those requirements had been recorded as environmental
  gaps — the tools' refusal paths were tested, their success paths were not. That is a
  bad trade: the visual half of the toolset is the half a text-only agent can least
  afford to leave unverified, and treating it as unreachable hid a real product bug
  (the editor only requests the remote scene tree while its Remote panel is visible).
- **Decision:** Ship `tools/virtual_display.py`. It starts `Xvfb`, waits until the
  display actually answers, and hands back the environment and renderer arguments the
  editor needs. `run_editor_e2e.py` calls it, so the visual checks run by default
  wherever `Xvfb` exists. The editor process itself is unchanged: it opens a real
  display and renders through Mesa's software OpenGL.
- **Alternatives considered:**
  - *A virtual `DisplayServer` inside the engine.* It would remove the `Xvfb`
    dependency, but it means writing and maintaining a display backend whose output
    nothing else validates — a screenshot from it would prove that the fake backend
    works, not that the editor draws correctly. Rejected: the point of the exercise is
    fidelity to what a user sees.
  - *The editor spawning `Xvfb` itself.* Rejected: the editor would be launching
    processes to fix its own environment, which cuts against "tools never execute
    shell commands" and makes the engine responsible for a deployment concern. A
    launcher is the right layer; `Godot_GetEditorStatus` reports `can_render` and
    `Godot_CaptureViewport`'s refusal names the fix, so a client is told what to do
    without the engine doing it.
  - *Requiring the caller to arrange a display.* Rejected: that is what produced the
    stale "environmental" statuses in the first place.
- **Consequences:** `xvfb x11-utils libgl1-mesa-dri xdotool` become CI dependencies for
  the editor job. Where they are absent, everything still runs and the display checks
  degrade to the refusal paths, so no machine is locked out. Test projects must ask for
  `gl_compatibility`: a launched game inherits the *project's* renderer, not the
  editor's command line, and llvmpipe has no Vulkan.
- **Spec IDs:** T11, T12, T13, T15, U1, U2, C1, X1, X2.

## DEC-0008 — The optional tranche: what each one had to mean here

- **Date:** 2026-07-28
- **Context:** The specification lists four optional features in a table of one-line
  descriptions. Three of them describe a capability; the fourth (export templates)
  describes a *decision*. Implementing them meant first deciding what each one is.
- **Decisions:**
  - **O4, HTTP transport — in the relay, not the editor.** DEC-0001 already put
    transport in the relay, and the editor already accepts several peers, so a session
    per editor connection gives isolation for free. Streamable HTTP is implemented as
    far as it is meaningful: one endpoint, JSON in, JSON out, session header. The SSE
    half is left out because nothing in this toolset pushes to a client, and shipping
    an untested streaming path would be worse than not having one.
  - **O2, packaged backends — write configuration, never credentials.** "Bundle agent
    launchers" could have meant shipping other people's binaries. It does not: the
    relay writes *its own* entry into a client's MCP configuration, pinned to the
    versions that generated it. Credentials are the interesting part, and the answer
    is that we never store them — the HTTP entry references an environment variable,
    and `--install-backend` refuses a token rather than writing one to a file that
    gets copied and pasted into bug reports.
  - **O3, export templates — the decision is "editor-only", and it is enforced.** A
    game that shipped an MCP server would be shipping a remote control for itself.
    `can_build` refuses non-editor builds; a real template build proves the engine
    still links without the module, and that its symbols are absent from the binary.
- **Consequences:** The relay grows a listening socket, so the platform seam gained
  `socket_listen`/`socket_accept`/`wait_for_sockets` and both backends implement them.
  Adding the HTTP entry point also surfaced a latent bug: `platform::initialize()` was
  never called from `main()`, which POSIX forgives and Windows would not have — every
  socket call there would have failed with WSANOTINITIALISED.
- **Spec IDs:** O2, O3, O4, R8.

## DEC-0009 — The tree is rebased onto 4.8; DEC-0002's path remapping is retired

- **Date:** 2026-08-26
- **Context:** Commit `117870273` merged Godot 4.8 development into this fork.
  `version.py` now reads 4.8.0-dev and the editor layout is nested
  (`editor/file_system/`, `editor/docks/`, `editor/scene/`). Three documents still
  asserted a 4.3-dev tree with a flat editor layout, and DEC-0002 still carried a
  remapping table from the spec's 4.6-era paths onto flat ones.
- **Decision:** Record 4.8-dev as the engine baseline. DEC-0002 is **superseded**: the
  specification's nested paths now largely land as written, so no remapping table is
  maintained. Where a spec path still does not resolve, fix the include against the
  real tree rather than reintroducing a table that has already gone stale once.
- **Evidence:** `modules/godot_ai/` already includes
  `editor/file_system/editor_file_system.h`, `editor/docks/scene_tree_dock.h`,
  `editor/scene/scene_tree_editor.h` — the migration was done as part of the merge.
- **Consequences:** No editor build has been produced on this tree since the merge, so
  the module's link against 4.8 is unverified. The relay is unaffected (no engine
  headers) and still passes 64/64. The first editor build after the merge is the real
  test, and any 4.8 API breakage will surface there rather than in the relay loop.

## DEC-0010 — Replay requires a running game rather than starting one

- **Date:** 2026-08-26
- **Context:** `Godot_ReplaySession` needs two authorities if it launches the project:
  `run_project` to start the game and `simulate_input` to drive it. A tool declares
  exactly one capability (`MCPTool::get_capability`), so whichever it named, it would
  hold authority the permission model could not see it using.
- **Decision:** Replay does **not** start the game. It refuses when none is running and
  names the tool to call first. Its one declared capability, `simulate_input`, is then
  the whole of what it does.
- **Consequences:** One more call for the caller, and a composition benefit — the caller
  chooses which scene to replay against. The alternative considered and rejected was
  declaring `run_project` and checking `MCPPermissions::get_policy(SIMULATE_INPUT)` by
  hand: that check cannot see per-session narrowing or a read-only session, so it would
  have looked like a gate without being one.
- **Related:** `Godot_RecordSession` and `Godot_AssertRuntimeState` declare
  `read_runtime`, following the profiler tools, which also write JSON Lines into a
  tool-owned directory under `user://` and treat that as bookkeeping rather than as
  editing the player's data. `Godot_WriteUserFile`, which takes an arbitrary `user://`
  path, remains `edit_user_data`.

## DEC-0011 — Multi-instance embedding is an editor-layer problem, not a platform one

- **Date:** 2026-08-26
- **Context:** The agent-workspace specification requires several live game processes
  displayed at once, and instructs that this be proven before any stage UI is built:
  *"Do not build a polished stage on top of an unproven assumption that the current
  single-instance embedder can simply be multiplied."* A related question was whether
  the editor should become the harness for agent test instances, described as a
  potentially big rewrite.
- **What was measured.** A throwaway patch let `GameView` hold a second
  `EmbeddedProcess`, allowed run instance 1 to receive the embed arguments, and fed it
  the second launched pid. The editor ran on a 1600x900 virtual display and was driven
  through the real relay (`Godot_PlayMainScene`, `Godot_CaptureEditorWindow`).
- **Result: two game processes embedded and rendered inside one editor window,
  simultaneously.** Three Godot processes alive (editor + two games), the editor logged
  embedding the second pid, and the screenshot shows both games drawing. Evidence:
  `.agent/evidence/spike_two_embedded_processes.png`. The patch was then reverted; it
  exists only in this record.
- **Why it works.** The platform layer was never single-instance. Every backend keys
  embedding by process id:
  `DisplayServerX11::embed_process(WindowID, ProcessID, Rect2i, visible, grab_focus)`
  over a `HashMap<ProcessID, EmbeddedProcessData *>`, and Windows, macOS and Wayland
  have the same shape. `EmbeddedProcessBase` is a `Control`, not a singleton, so N
  controls is structurally ordinary.
- **Decision:** Build the workspace by generalising the existing editor seams. Do **not**
  write a separate harness, and do **not** treat this as a rewrite. Three seams carry
  the whole limitation:
  1. `GameView::_update_arguments_for_instance()` returns early unless `p_idx == 0`, so
     only the first instance is ever told to embed.
  2. `GameView` owns exactly one `embedded_process` and one `_update_embed_window_size()`
     path. A second embedder renders, but nothing positions it — visible in the
     screenshot, where the second game floats over the layout instead of tiling.
  3. `GameViewDebugger::set_suspend/next_frame/set_time_scale/reset_time_scale` iterate
     `sessions` and broadcast to every active one. Pausing one instance pauses all.
- **The routing key already exists.** `GameView::_attach_script_debugger()` matches a
  session to a process with `script_debugger->get_remote_pid() == embedded_process->
  get_embedded_pid()`. A per-instance router is that join generalised, not new
  machinery. `EditorRun::pids` is already public and already tracks every launched pid.
- **Not established.** Only Linux/X11 under Xvfb was measured. macOS routes through a
  separate `EmbeddedProcessMacOS` object and `embed_process_update()`, which is more
  per-platform machinery than X11 needs and must be spiked separately. Windows and
  Wayland are unmeasured. Nothing here says how many instances remain usable at once,
  and the second embedder's positioning was never wired up, so nothing is known about
  resize, focus or z-order with several tiles.
- **Consequences:** The workspace's first vertical slice — three live variants — is not
  gated on new platform capability. It is gated on an instance registry, per-tile
  embedder hosts, and a debugger router. That is a tractable amount of work against
  three named seams.
