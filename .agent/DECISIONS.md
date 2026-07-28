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
