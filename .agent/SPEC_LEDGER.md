# Specification ledger

Derived from `docs/godot-ai-clone-spec.md`. Statuses: `NOT_STARTED`, `IN_PROGRESS`,
`IMPLEMENTED`, `VERIFIED`, `BLOCKED`. `IMPLEMENTED` never means complete: a
requirement becomes `VERIFIED` only when observable behaviour exists, the relevant
tests pass, failure paths are covered, and documentation is current.

Spec-section shorthand: **MCP** = "Proposed MCP-compatible design for Godot",
**SEC** = "Security model and safeguards", **PRI** = "Feasibility, priorities…",
**PKG** = "Packaging and migration strategy", **CI** = "Suggested CI and test plan".

## Foundation

| ID | Spec | Requirement | Deps | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|---|---|
| F1 | PKG/MCP | Editor-only module builds and registers an EditorPlugin | — | VERIFIED | `modules/godot_ai/{config.py,SCsub,register_types.cpp}` | engine build | editor links; module tests run | none |
| F2 | MCP | Service lifecycle: loopback listener, start/stop, settings, instance descriptor | F1 | VERIFIED | `mcp_service.cpp` | `tools/relay/tests/run_editor_e2e.py` | headless editor advertises port + project; relay connects | none |
| F3 | MCP | NDJSON JSON-RPC framing + dispatch on the editor side | F2 | VERIFIED | `mcp_service.cpp` `_poll_peer`/`_handle_line` | `run_editor_e2e.py` | 12/12 live checks over real NDJSON frames | none |
| F4 | MCP | Tool registry: register/unregister, duplicates, schemas, capabilities | F1 | VERIFIED | `mcp_tool_registry.cpp`, `mcp_schema.cpp` | `tests/test_mcp_registry.h` | 24 doctest cases pass | none |
| F5 | SEC | Project-root confinement incl. traversal and symlink escape | F1 | VERIFIED | `mcp_paths.cpp` | `tests/test_mcp_paths.h` | traversal/scheme/symlink cases pass | none |
| F6 | SEC | Permission model: capabilities, allow/ask/deny, client approval, read-only | F4 | VERIFIED | `mcp_permissions.cpp`, `mcp_service.cpp` | `tests/test_mcp_paths.h`, `test_mcp_protocol.h`, `test_mcp_audit.h` | policy resolution, read-only, approval-mode narrowing, deny-by-default client approval | approval *persistence* rides on EditorSettings, which does not exist headlessly |
| F7 | SEC | Audit log with secret redaction | F6 | VERIFIED | `mcp_audit.cpp`, `mcp_tool.cpp` | `tests/test_mcp_audit.h` | allowed and refused calls both recorded, appends, one object per line, secrets redacted at source | none |
| F8 | SEC/PRI | Checkpoints created before mutation and restorable | F5,F6 | VERIFIED | `mcp_checkpoints.cpp`, `mcp_protocol.cpp`, `tools/mcp_checkpoint_tools.cpp` | `tests/test_mcp_checkpoints.h`, `run_editor_e2e.py` | restore compares contents byte for byte; created files are removed again; live round trip through the protocol | git-backed variant not implemented (snapshot only) |

## Protocol

| ID | Spec | Requirement | Deps | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|---|---|
| P1 | MCP | `initialize` + capability negotiation + version fallback | F3 | VERIFIED | `mcp_protocol.cpp` `_handle_initialize` | `test_mcp_protocol.h` | supported/unsupported version cases pass | none |
| P2 | MCP | `tools/list` with input/output schemas | F4,P1 | VERIFIED | `_handle_tools_list` | `test_mcp_protocol.h` | listing case passes | none |
| P3 | MCP | `tools/call` with structured content | P2 | VERIFIED | `_handle_tools_call` | `test_mcp_protocol.h` | structuredContent + text content asserted | none |
| P4 | MCP | Errors: unknown method/tool, malformed/missing/extra args, denial | P3 | VERIFIED | `mcp_protocol.cpp` | `test_mcp_protocol.h` | 4 error subcases + permission denial | none |
| P5 | MCP | `notifications/tools/list_changed` | F4,P1 | VERIFIED | `mcp_tool_registry.cpp`, `MCPService::_on_tools_changed` | `tests/test_mcp_protocol.h`, `run_tests.py` | registry announces add/remove and stays silent on a no-op; the frame is a valid id-less notification; the relay delivers one to a client | none |
| P6 | CI | Cancellation and clean termination semantics | P3 | VERIFIED | `mcp_protocol.cpp`, `mcp_deferred.cpp`, `MCPService::_poll_deferred` | `tests/test_mcp_deferred.h`, `run_editor_e2e.py` | exactly one response per call, late answers dropped, overdue calls failed, abandoned on disconnect | none |

## Relay

| ID | Spec | Requirement | Deps | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|---|---|
| R1 | MCP/PKG | Standalone binary, no engine dependency | — | VERIFIED | `tools/relay/src/*` | `tools/relay/tests/run_tests.py` | clean `-Werror` build; 35/35 pass | none |
| R2 | CI | stdio framing: partial reads, multiple frames, CRLF, malformed | R1 | VERIFIED | `relay.cpp` | framing tests | 35/35 pass | none |
| R3 | MCP | Editor discovery and explicit selectors | R1 | VERIFIED | `relay_discover_instances`/`relay_select_instance` | selection tests | socket/project/pid/ambiguity cases pass | none |
| R4 | CI | Unavailable, stale, hung, mismatched, reconnect | R3 | VERIFIED | `ensure_connected`, `perform_handshake` | availability tests | 35/35 pass | none |
| R5 | MCP | Documented flag set | R1 | VERIFIED | `relay_parse_options` | CLI tests | per-option usage errors, exit code 2 | none |
| R6 | CI | Clean shutdown, no orphans, socket release | R1 | VERIFIED | `Relay::run`, `disconnect` | lifecycle tests | EOF and SIGTERM both exit 0 | none |
| R7 | SEC/CI | stdout purity | R1 | VERIFIED | `write_stdout_line`, `handle_editor_line` | purity tests | every stdout line is a JSON object | none |
| R8 | CI/PKG | macOS and Windows relay behaviour | R1 | IMPLEMENTED | `tools/relay/src/platform*.{h,cpp}` | POSIX backend covered by all 64 relay tests on Linux and native arm64 macOS; Windows backend cross-compiled in CI | macOS relay 64/64; Windows builds clean under mingw | runtime verification on a Windows host |

## Tools

| ID | Spec | Requirement | Deps | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|---|---|
| T1 | MCP | `Godot_ListScenes` | F4,F5 | VERIFIED | `tools/mcp_project_tools.cpp` | `tests/test_mcp_tools.h`, `run_editor_e2e.py` | paths round-trip through another tool; live listing matches disk | none |
| T2 | MCP | `Godot_OpenScene` | T1 | VERIFIED | `tools/mcp_editor_tools.cpp` | `run_editor_e2e.py` | live open returns the real scene root | none |
| T3 | MCP | `Godot_SaveScene` | T2 | VERIFIED | `tools/mcp_editor_tools.cpp` | `run_editor_e2e.py` | saved scene on disk contains the created node | none |
| T4 | MCP | `Godot_GetEditedSceneTree` | T2 | VERIFIED | `tools/mcp_editor_tools.cpp` | `run_editor_e2e.py` | live tree matches the scene on disk | none |
| T5 | MCP | `Godot_ManageNode` with undo/redo | T4,F8 | VERIFIED | `tools/mcp_scene_tools.cpp` | `tests/test_mcp_tools.h`, `run_editor_e2e.py` | create/rename/reparent/delete + undo asserted against live scene state and the saved file | none |
| T6 | MCP | `Godot_ListAssets` | F5 | VERIFIED | `tools/mcp_project_tools.cpp` | `tests/test_mcp_tools.h` | extension filtering covered | none |
| T7 | MCP | `Godot_ReadTextFile` | F5 | VERIFIED | `tools/mcp_project_tools.cpp` | `tests/test_mcp_tools.h`, `run_editor_e2e.py` | reads, directory/missing/escape refusals | none |
| T8 | MCP | `Godot_WriteTextFile` + filesystem refresh | F5,F8 | VERIFIED | `tools/mcp_project_tools.cpp` | `tests/test_mcp_tools.h`, `run_editor_e2e.py` | content asserted on disk, not from the tool report | none |
| T9 | MCP | `Godot_ReadOutputLog` | F2 | VERIFIED | `tools/mcp_output_tools.cpp`, accessor added to `editor/editor_log.h` | `run_editor_e2e.py` | the service startup message is read back through the tool, with type classification and filtering | none |
| T10 | MCP | `Godot_PlayCurrentScene` / `Godot_PlayMainScene` | F6 | VERIFIED | `tools/mcp_editor_tools.cpp` | `run_editor_e2e.py` | play reports the game as running against a live editor | none |
| T11 | MCP | `Godot_StopPlaying` | T10 | VERIFIED | `tools/mcp_editor_tools.cpp` | `run_editor_e2e.py` | under a virtual display, stop reports `was_playing: true` and leaves nothing running; headless the postcondition alone is asserted, because that game may already have exited | none |
| T12 | MCP | `Godot_CaptureViewport` incl. headless rejection | F6 | VERIFIED | `tools/mcp_capture_tools.cpp` | `run_editor_e2e.py` (both modes) | under a virtual display it saves a real 1152x648 PNG and returns it as an inline image block, asserted by magic bytes and size; headless it refuses and names the reason and the fix | none |
| T13 | MCP | `Godot_AskUser` | F2 | VERIFIED | `tools/mcp_ask_user_tool.cpp`, `mcp_deferred.cpp` | `tests/test_mcp_deferred.h`, `run_editor_e2e.py` | a real pointer click on a choice button returns that choice, Escape comes back as `cancelled: true`, and an unanswered question times out while the editor keeps serving | typed free text cannot be verified here: without a window manager no window takes X input focus, so a `LineEdit` never receives characters |
| T14 | MCP | `Godot_SearchProject` | F5 | VERIFIED | `tools/mcp_project_tools.cpp` | `tests/test_mcp_tools.h`, `run_editor_e2e.py` | match line numbers, case sensitivity, empty query | none |
| T15 | SEC | Runtime vs persistent edits kept distinct | T5,T10 | VERIFIED | `tools/mcp_property_tools.cpp`, lookup added to `editor/debugger/editor_debugger_tree.*` | `run_editor_e2e.py` | scene edit survives save/reopen; against a running game the runtime tree is read live and a runtime edit leaves the scene file byte-identical; refusals with no game running still hold | none |

## Skills, UX, docs, packaging

| ID | Spec | Requirement | Deps | Status | Remaining |
|---|---|---|---|---|---|
| S1 | MCP | `SKILL.md` discovery across project/user/plugin roots | F5 | VERIFIED | `mcp_skills.cpp` `get_roots`/`discover` | `tests/test_mcp_skills.h`, `run_editor_e2e.py` | project/plugin/user roots; shipped example skill discovered live | none |
| S2 | MCP | Frontmatter parsing incl. version gating and tool list | S1 | VERIFIED | `mcp_skills.cpp` `parse`/`version_satisfied` | `tests/test_mcp_skills.h` | malformed, unterminated, nameless, disabled, quoted, version-gated cases | none |
| S3 | SEC | Skills untrusted by default; allow/deny persisted | S2,F6 | VERIFIED | `MCPSkills::is_allowed`/`set_allowed` | `tests/test_mcp_skills.h` | denied by default; instructions and resources both refused until allowed | approval UI (U2) |
| S4 | MCP | Supporting resources loaded on demand | S2 | VERIFIED | `MCPSkills::read_resource` | `tests/test_mcp_skills.h`, `run_editor_e2e.py` | on-demand load; traversal out of the skill folder refused | none |
| S5 | MCP | Skills exposed over the protocol and refreshed live | S3,P5 | VERIFIED | `tools/mcp_skill_tools.cpp` | `run_editor_e2e.py` | Godot_ListSkills/Godot_ReadSkill exercised over the real protocol | live re-scan on change not asserted |
| U1 | PRI | Command palette entries | F2 | VERIFIED | `run_editor_ui_e2e.py` opens the palette with its real shortcut, finds each command by typing its name, and runs it: "Clients and Skills" opens the dialog, "Show Service Status" writes the service's state to the Output panel where `Godot_ReadOutputLog` reads it back, and "Restart Service" leaves the service accepting connections. The Tools menu entry is registered from the same call and is not clicked |
| U2 | SEC | Settings/status UI incl. approvals | F6,S3 | VERIFIED | `run_editor_ui_e2e.py` drives the real dialog: an unapproved client is refused, clicking its row is the only thing that approves it, and it can then connect; a discovered skill is unreadable until the same dialog allows it. Rendering was checked by eye and found a real defect — the action buttons had no icon, so the column was empty and nothing could be clicked (fixed). The decision text remains covered by `mcp_skill_status_text()` unit tests |
| U3 | PRI | Headless execution hook | F3 | VERIFIED | `--call`/`--arguments` one-shot mode in the relay; 4 relay tests plus a live e2e call, with distinct exit codes for tool failure and unreachable editor |
| D1 | PKG | Developer + user documentation | — | VERIFIED | none for the shipped surface — `modules/godot_ai/README.md` covers architecture, client setup, permissions, the tool catalogue, the registration API and troubleshooting; extend it as tools land |
| D2 | PKG | `AGENTS.md` repository guidance artifact | — | VERIFIED | none — `AGENTS.md` present, imported by `CLAUDE.md` |
| D3 | MCP | Example Godot `SKILL.md` that actually loads | S2 | VERIFIED | none — `misc/godot_ai/skills/scene-cleanup/` is copied into the e2e project and read back over the protocol, so the shipped file is the one proven to load |
| D4 | — | `CLAUDE.md` continuity protocol (required deliverable) | — | VERIFIED | none — section present and kept current |
| C1 | CI | CI wiring for relay tests, module tests, clean build | R2,F4 | IMPLEMENTED | `.github/workflows/godot_ai.yml` covers the relay, the virtual display, the module suite and both end-to-end modes; every command passes locally, but the workflow has not been observed green on GitHub Actions from here |
| C2 | PKG | Packaging: install layout, licences, clean checkout | R1,D1 | VERIFIED | `tools/relay/package.sh`, `tools/relay/tests/run_clean_checkout.py` | bundle contains the binaries, MIT notices, INSTALL.md and the example skill; the tracked tree builds, tests and packages from scratch |

## Optional (explicitly marked optional by the specification)

| ID | Requirement | Status |
|---|---|---|
| ID | Requirement | Status | Evidence |
|---|---|---|---|
| O1 | In-editor hosted LLM chat UI | VERIFIED | `mcp_chat.{h,cpp}` (conversation, persistence, attachments, turn state), `mcp_chat_dock.{h,cpp}` (the dock), sampling plumbing in `mcp_service.cpp` and `mcp_protocol.cpp`. The editor has no model: it borrows the connected client's through `sampling/createMessage`, so no credential ever enters the editor. 5 doctest cases cover the request shape, attachment inlining/confinement/truncation, staged-attachment ownership, the send state machine, and persistence including a corrupt transcript. `run_editor_ui_e2e.py` types into the real dock, catches the sampling request the editor sends, answers as the client, and proves the answer became part of the conversation by inspecting the *next* turn's history. Cancellation's effect on the conversation is unit-tested; the `notifications/cancelled` frame itself is not exercised end to end |
| O2 | Packaged agent backends / AI Gateway equivalent | VERIFIED | `tools/relay/src/backends.{h,cpp}`, 7 cases in `tests/test_backends.py`. `--list-backends` / `--install-backend` / `--check-backends` write and audit an MCP client's configuration: the entry names the binary that wrote it, carries the options that decide which editor is driven, and pins the relay and bridge versions so a stale entry is reported rather than left to fail. Credential storage is *refused* — the HTTP entry references `${GODOT_AI_HTTP_TOKEN}` and `--install-backend` rejects `--http-token` outright, writing nothing |
| O3 | Export-template AI integration | VERIFIED | The decision is editor-only, and it is enforced rather than asserted: `config.py` `can_build` refuses every non-editor build, checked across the platform matrix in `tools/tests/run_tests.py`. A real `target=template_release` build (12m56s) links with the module present and contains none of its symbols — `Godot_ManageNode` appears 7 times in the editor binary and 0 times in the template — and a test re-checks that whenever a template is on disk |
| O4 | Multi-user / remote Streamable HTTP MCP | VERIFIED | `tools/relay/src/http_server.{h,cpp}`, 12 cases in `tests/test_http.py`. `POST /mcp` with `Mcp-Session-Id` sessions, `DELETE` to end one. Each session owns its own `Relay` and therefore its own editor connection, which is what isolates concurrent clients; four interleaved sessions keep their replies straight. Auth is a bearer token compared in constant time, loopback-only unless `--http-allow-remote`. Server-initiated SSE streams are deliberately absent: nothing here pushes to a client |

## Beyond the specification

Requested during implementation, not derived from `docs/godot-ai-clone-spec.md`. Held
to the same bar as everything above.

| ID | Requirement | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|
| X1 | A virtual display, so an agent on a machine with no screen can verify what the editor draws | VERIFIED | `tools/virtual_display.py` | `tools/tests/run_tests.py` (14 cases), `run_editor_e2e.py` | starts `Xvfb`, waits until it answers, supplies software-GL environment and the matching renderer; reuses a working `DISPLAY`, refuses to trust one that answers nothing, degrades honestly with no X server. With it the end-to-end run verifies a real screenshot, a running game's scene tree, and a question answered by clicking — all of which previously only had refusal paths | typed text input needs a window manager focus model this environment does not provide (see T13) |
| X2 | The editor reports whether it can render | VERIFIED | `tools/mcp_editor_tools.cpp` | `run_editor_e2e.py` | `Godot_GetEditorStatus` returns `display_server` and `can_render`, so a client can tell whether the visual tools will work without calling one and reading the refusal; `Godot_CaptureViewport`'s headless refusal names the fix | none |
| X3 | macOS editor is branded and packaged as GodotAI | VERIFIED | `platform/macos/SCsub`, `misc/dist/macos/editor_info_plist.template`, `misc/dist/macos_tools.app`, `misc/scripts/make_godot_ai_macos_icon.*` | native arm64 bundle build; bundled `*[godot_ai]*` suite | `bin/GodotAI.app` has `GodotAI` as bundle/display/executable name, embeds the reproducible AI-badged ICNS at every macOS icon size, verifies with `codesign`, launches as a commit-stamped `4.3.dev.custom_build`, and passes 71 cases / 451 assertions | none |
| X4 | Semantic Inspector documentation capture for Resources and scene nodes | VERIFIED | `tools/mcp_capture_tools.cpp`, deferred cancellation cleanup in `mcp_deferred.cpp` | `tests/test_mcp_tools.h`, `tests/test_mcp_deferred.h`, `run_editor_e2e.py` (native macOS + headless) | a native Retina editor expands `GradientTexture2D.gradient` into its sub-inspector, highlights raw property `offsets`, centers it with exact requested context, and saves/returns a 262x230 Inspector-only PNG; a second call opens another tab's `capture_scene.tscn`, selects `Section/Target`, and captures its `text` property. Both restore the original `Main` tab and prior Inspector/dock state; headless calls refuse explicitly | none |
| X5 | Semantic Scene tree documentation capture | VERIFIED | `tools/mcp_capture_tools.cpp` | `tests/test_mcp_tools.h`, `run_editor_e2e.py` (native macOS + headless) | opens another tab's named scene, traverses and unfolds the ancestor chain for `Section/Target`, selects/highlights the exact Tree row, centers it with independent context above/below, saves and returns a Scene-dock-only PNG, then demonstrably restores the original `Main` tab, selection, folds, scroll and dock tab; headless call refuses explicitly | none |
