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
| F8 | SEC/PRI | Checkpoints created before mutation and restorable | F5,F6 | VERIFIED | `mcp_checkpoints.cpp`, `mcp_protocol.cpp`, `tools/mcp_checkpoint_tools.cpp` | `tests/test_mcp_checkpoints.h`, `run_editor_e2e.py` | restore compares contents byte for byte; created files are removed again; a task key restores every checkpoint made under that intent without touching another task | git-backed variant not implemented (snapshot only) |

## Protocol

| ID | Spec | Requirement | Deps | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|---|---|
| P1 | MCP | `initialize` + capability negotiation + version fallback | F3 | VERIFIED | `mcp_protocol.cpp` `_handle_initialize` | `test_mcp_protocol.h` | supported/unsupported version cases pass | none |
| P2 | MCP | `tools/list` with input/output schemas | F4,P1 | VERIFIED | `_handle_tools_list` | `test_mcp_protocol.h` | listing case passes | none |
| P3 | MCP | `tools/call` with structured content | P2 | VERIFIED | `_handle_tools_call` | `test_mcp_protocol.h` | structuredContent + text content asserted | none |
| P4 | MCP | Errors: unknown method/tool, malformed/missing/extra args, denial | P3 | VERIFIED | `mcp_protocol.cpp` | `test_mcp_protocol.h` | 4 error subcases + permission denial | none |
| P5 | MCP | `notifications/tools/list_changed` | F4,P1 | VERIFIED | `mcp_tool_registry.cpp`, `MCPService::_on_tools_changed` | `tests/test_mcp_protocol.h`, `run_tests.py` | registry announces add/remove and stays silent on a no-op; the frame is a valid id-less notification; the relay delivers one to a client | none |
| P6 | CI | Cancellation and clean termination semantics | P3 | VERIFIED | `mcp_protocol.cpp`, `mcp_deferred.cpp`, `MCPService::_poll_deferred` | `tests/test_mcp_deferred.h`, `run_editor_e2e.py` | exactly one response per call, late answers dropped, overdue calls failed, abandoned on disconnect | none |
| P7 | MCP | Allowed skills exposed as `prompts/list` and `prompts/get` | S3,P1 | VERIFIED | `mcp_protocol.cpp`, `mcp_skills.cpp` | `tests/test_mcp_protocol.h`, `run_editor_e2e.py`, gateway tests | discovery and retrieval share the trusted skill source; prompts work over the live editor and `godot --godot-ai-stdio --list-prompts/--prompt` | none |

## Relay

| ID | Spec | Requirement | Deps | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|---|---|
| R1 | MCP/PKG | Stdio transport without engine prints on the stream | — | VERIFIED (reshaped by DEC-0015) | `modules/godot_ai/gateway/*` compiled into the editor; platform mains enter it before engine init | `tools/relay/tests/run_tests.py` (48 cases) against `godot --godot-ai-stdio` | the property that mattered - a protocol-clean stdout - comes from ordering: the gateway runs before anything can print. No separate binary exists | gateway sources must remain engine-free C++17 |
| R2 | CI | stdio framing: partial reads, multiple frames, CRLF, malformed | R1 | VERIFIED | `relay.cpp` | framing tests | 35/35 pass | none |
| R3 | MCP | Editor discovery and explicit selectors | R1 | VERIFIED | `relay_discover_instances`/`relay_select_instance` | selection tests | socket/project/pid/ambiguity cases pass | none |
| R4 | CI | Unavailable, stale, hung, mismatched, reconnect | R3 | VERIFIED | `ensure_connected`, `perform_handshake` | availability tests | 35/35 pass | none |
| R5 | MCP | Documented flag set and focused discovery/one-shot modes | R1,P7 | VERIFIED | `relay_parse_options`, `Relay::{run_list_tools,run_describe,run_list_prompts,run_prompt}` | gateway CLI tests | invalid options exit 2; `--describe` suggests near tools; `--list-prompts`/`--prompt` expose trusted skills without loading all tool schemas | none |
| R6 | CI | Clean shutdown, no orphans, socket release | R1 | VERIFIED | `Relay::run`, `disconnect` | lifecycle tests | EOF and SIGTERM both exit 0 | none |
| R7 | SEC/CI | stdout purity | R1 | VERIFIED | `write_stdout_line`, `handle_editor_line` | purity tests | every stdout line is a JSON object | none |
| R8 | CI/PKG | macOS and Windows gateway behaviour | R1 | IMPLEMENTED | `modules/godot_ai/gateway/platform*.{h,cpp}`, hook in all three platform mains | POSIX path: 48/48 on native arm64 macOS; Linux CI covers the same in-tree gateway | Windows compiles wherever the Windows editor compiles - the gateway is part of that build now, not a separate cross-compile | runtime verification on a Windows host |

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
| T13 | MCP | `Godot_AskUser` | F2 | VERIFIED | `tools/mcp_ask_user_tool.cpp`, `mcp_deferred.cpp` | `tests/test_mcp_deferred.h`, `run_editor_e2e.py` in both modes | native UI opens a real question and an unanswered call times out without wedging the editor; a headless editor refuses immediately instead of waiting for a user who cannot exist | typed free text remains unverified by the macOS automation path |
| T14 | MCP | `Godot_SearchProject` | F5 | VERIFIED | `tools/mcp_project_tools.cpp` | `tests/test_mcp_tools.h`, `run_editor_e2e.py` | match line numbers, case sensitivity, empty query | none |
| T15 | SEC | Runtime vs persistent edits kept distinct | T5,T10 | VERIFIED | `tools/mcp_property_tools.cpp`, lookup added to `editor/debugger/editor_debugger_tree.*` | `run_editor_e2e.py` | scene edit survives save/reopen; against a running game the runtime tree is read live and a runtime edit leaves the scene file byte-identical; refusals with no game running still hold | none |

## Skills, UX, docs, packaging

| ID | Spec | Requirement | Deps | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|---|---|
| S1 | MCP | `SKILL.md` discovery across project/user/plugin and built-in roots | F5 | VERIFIED | `mcp_skills.cpp` `get_roots`/`discover`, generated built-in header | `tests/test_mcp_skills.h`, `run_editor_e2e.py` | project/plugin/user roots preserve precedence; the editor still offers its shipped skills in a fresh project | none |
| S2 | MCP | Frontmatter parsing incl. version gating and tool list | S1 | VERIFIED | `mcp_skills.cpp` `parse`/`version_satisfied` | `tests/test_mcp_skills.h` | malformed, unterminated, nameless, disabled, quoted, version-gated cases | none |
| S3 | SEC | Filesystem skills untrusted by default; built-ins trusted but revokable | S2,F6 | VERIFIED | `MCPSkills::{is_allowed,is_revoked,set_allowed}` | `tests/test_mcp_skills.h` | project/plugin/user instructions and resources refuse until allowed; editor-embedded skills work in a fresh project and can be disabled by name | approval UI (U2) |
| S4 | MCP | Supporting resources loaded on demand | S2 | VERIFIED | `MCPSkills::read_resource` | `tests/test_mcp_skills.h`, `run_editor_e2e.py` | on-demand load; traversal out of the skill folder refused | none |
| S5 | MCP | Skills exposed as tools and prompts | S3,P5,P7 | VERIFIED | `tools/mcp_skill_tools.cpp`, prompt handlers in `mcp_protocol.cpp` | `tests/test_mcp_protocol.h`, `run_editor_e2e.py`, gateway tests | list/read tools and list/get prompts return the same allowed shipped skills over real product paths | live re-scan on change not asserted |
| U1 | PRI | Command palette entries | F2 | VERIFIED | editor plugin menu/command registrations | `run_editor_ui_e2e.py` | keyboard-driven palette checks open Clients and Skills, show status in Output, and restart the service | Linux UI automation only; macOS skips as documented |
| U2 | SEC | Settings/status UI incl. approvals | F6,S3 | VERIFIED | approvals dialog and settings integration | UI E2E plus `mcp_skill_status_text()` tests | an unknown client and untrusted skill remain unusable until explicitly allowed in the real dialog | Linux UI automation only; macOS skips as documented |
| U3 | PRI | Headless execution hook | F3 | VERIFIED | embedded gateway one-shot modes | gateway tests; live E2E | `--call`/`--arguments` have distinct tool-failure and unreachable-editor exits; focused list/describe/prompt modes share the same live registry | none |
| D1 | PKG | Developer + user documentation | — | VERIFIED | `modules/godot_ai/README.md` | review against the live CLI and E2E | current direct-HTTP/embedded-gateway architecture, permissions, selected tools, prompts, Agent Terminal, headless behaviour and testing are documented | extend the selected tool catalogue as jobs land |
| D2 | PKG | `AGENTS.md` repository guidance artifact | — | VERIFIED | `AGENTS.md` | repository review | present and imported by `CLAUDE.md` | none |
| D3 | MCP | Shipped Godot skills that actually load | S2 | VERIFIED | `misc/godot_ai/skills/`, generated built-in header | skills checker, module tests, E2E | every declared tool exists; built-in and project-backed skills are offered through tools and prompts | none |
| D4 | — | `CLAUDE.md` continuity protocol (required deliverable) | — | VERIFIED | `CLAUDE.md` | repository review | continuity protocol present and current for Claude sessions | none |
| C1 | CI | CI wiring for gateway tests, module tests, clean build | R2,F4 | IMPLEMENTED | `.github/workflows/godot_ai.yml` | local equivalents all green | gateway, virtual display, module suite and both E2E modes are wired; the latest imported tranche has not yet been observed on CI | observe the pushed branch on Actions |
| C2 | PKG | Packaging: install layout, licences, clean checkout | R1,D1 | SUPERSEDED (DEC-0015) | — | — | there is nothing separate to package: the gateway ships inside the editor binary and the editor serves HTTP itself. `package.sh` and the clean-checkout packaging test are deleted | none |

## Optional (explicitly marked optional by the specification)

| ID | Requirement | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|
| O1 | In-editor hosted LLM chat UI | REMOVED (DEC-0013) | was `mcp_chat*.{h,cpp}` + sampling plumbing; deleted 2026-08-27 | module suite green at 272 cases after removal; `run_editor_ui_e2e.py` chat block removed | the Agent Terminal is the one conversation surface; a second one duplicated it while doing less. The session still records a client's `sampling` offer, and nothing consumes it | none - the terminal (T rows) carries the conversation requirement |
| O2 | Packaged agent backends / AI Gateway equivalent | REMOVED (DEC-0015) | was `tools/relay/src/backends.{h,cpp}` | — | client configuration is generated by the terminal panel (HTTP, secretless) and external clients point at the editor's HTTP endpoint or `godot --godot-ai-stdio`; a config-file installer for a binary that no longer exists installs nothing | none |
| O3 | Export-template AI integration | VERIFIED | `config.py` editor-only gate | `tools/tests/run_tests.py`; measured template build | every non-editor build is refused; a template binary contains none of the module's symbols | none |
| O4 | Multi-user / remote Streamable HTTP MCP | SUPERSEDED by O5 (DEC-0014/0015) | was `tools/relay/src/http_server.{h,cpp}` | — | the editor serves Streamable HTTP itself with the same session/auth model; the relay-side server went with the relay | O5 carries the requirement |
| O5 | The editor serves Streamable HTTP MCP itself; no relay in the product path | VERIFIED | `mcp_http.{h,cpp}`, listener/dispatch in `mcp_service.cpp`, `mcp_agent_build_http_mcp_config()` | 7 doctest cases; live HTTP e2e: 401s, session lifecycle, 102 tools, real call, read-only refusal, DELETE | same protocol layer, approval gate, permissions and deferred machinery as the bridge; per-run bearer token in a chmod-600 descriptor; config references `${GODOT_AI_MCP_TOKEN}`; terminal launches with no relay search | none |

## Beyond the specification

Requested during implementation, not derived from `docs/godot-ai-clone-spec.md`. Held
to the same bar as everything above.

| ID | Requirement | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|
| X1 | A virtual display, so an agent on a machine with no screen can verify what the editor draws | VERIFIED | `tools/virtual_display.py` | `tools/tests/run_tests.py` (14 cases), `run_editor_e2e.py` | starts `Xvfb`, waits until it answers, supplies software-GL environment and the matching renderer; reuses a working `DISPLAY`, refuses to trust one that answers nothing, degrades honestly with no X server. With it the end-to-end run verifies a real screenshot, a running game's scene tree, and a question answered by clicking — all of which previously only had refusal paths | typed text input needs a window manager focus model this environment does not provide (see T13) |
| X2 | The editor reports whether it can render | VERIFIED | `tools/mcp_editor_tools.cpp` | `run_editor_e2e.py` | `Godot_GetEditorStatus` returns `display_server` and `can_render`, so a client can tell whether the visual tools will work without calling one and reading the refusal; `Godot_CaptureViewport`'s headless refusal names the fix | none |
| X3 | macOS editor is branded and packaged as GodotAI | VERIFIED | `platform/macos/SCsub`, `platform/macos/platform_macos_builders.py`, `misc/dist/macos/editor_info_plist.template`, `misc/dist/macos_tools.app`, `misc/scripts/make_godot_ai_macos_icon.*` | native arm64 bundle build; bundled `*[godot_ai]*` suite | `bin/GodotAI.app` has `GodotAI` as bundle/display/executable name, embeds the reproducible ICNS - now built from the AI-skewed mark in `misc/logo/icon.svg` with the AI badge on top - at every macOS icon size, verifies with `codesign`, launches as a commit-stamped `4.8.dev.custom_build`, and passes 71 cases / 452 assertions | none |
| X4 | Semantic Inspector documentation capture for Resources and scene nodes | VERIFIED | `tools/mcp_capture_tools.cpp`, deferred cancellation cleanup in `mcp_deferred.cpp` | `tests/test_mcp_tools.h`, `tests/test_mcp_deferred.h`, `run_editor_e2e.py` (native macOS + headless) | native macOS E2E expands `GradientTexture2D.gradient`, highlights `offsets`, unfolds explicit contextual properties, and saves a tight Inspector-only PNG; scene-node mode opens another tab, selects `Section/Target`, and captures `text`. Raincaster production verification completed every one of the 65 documentation request IDs as 86 focused PNGs and visually read back each claim; the per-ID result and production-safe constraints are recorded beside the images in `docs/screenshots/notion-docs-2026/VERIFICATION.md`. Calls restore the original scene, Inspector, folds, scroll and dock state; headless calls refuse explicitly | none |
| X5 | Semantic Scene tree documentation capture | VERIFIED | `tools/mcp_capture_tools.cpp` | `tests/test_mcp_tools.h`, `run_editor_e2e.py` (native macOS + headless) | opens another tab's named scene, traverses and unfolds the ancestor chain for `Section/Target`, selects/highlights the exact Tree row, centers it with independent context above/below, saves and returns a Scene-dock-only PNG, then demonstrably restores the original scene, selection, folds, scroll and dock tab. Raincaster ST/OP scene-tree companions were visually checked to contain the requested selected node, required parents/siblings and no desktop or unrelated editor panels; the complete audit is in the Raincaster screenshot folder's `VERIFICATION.md` | none |
| X6 | Port the complete AI editor fork from Godot 4.3 to current upstream Godot 4.8 development | VERIFIED | upstream merge `457470a8d0109b994bb8d6951b3abe5299fb6ee6`; 4.8 API adaptations throughout `modules/godot_ai/`, editor accessors and macOS bundle builder | fresh arm64 editor build; 317 module cases / 3903 assertions; 48/48 gateway; native and headless E2E; full engine suite | 102 tools work through both live modes; the full engine suite passes 1734 cases / 425384 assertions; direct HTTP passes without a relay | native Windows runtime verification remains tracked under R8 |
| X7 | Give a fresh agent durable project context and the real API it is editing | VERIFIED | `mcp_project_memory.{h,cpp}`, `mcp_docs.{h,cpp}`, memory/docs tools, richer editor status | memory/docs doctests; native and headless E2E | bounded project notes round-trip without path escape or index dumping; class lookup covers core and project classes with near misses; status reports selection, workspace and open script | none |
| X8 | Make a task and its evidence inspectable and reversible as one unit | VERIFIED | terminal activity strip, `mcp_image_diff.{h,cpp}`, task restore in `mcp_checkpoints.cpp` | activity/image/checkpoint doctests; native and headless E2E | the terminal shows intent/tool/target/state; capture comparison measures, locates and annotates change; restoring one task leaves another task alone | none |
| X9 | Persist scene signal wiring through a structured editor API | VERIFIED | `tools/mcp_connection_tools.cpp` | module schema/tool tests; native and headless E2E | connect persists to the scene; nonexistent signals/methods, incompatible signatures and duplicate-looking no-ops are refused | none |
| X10 | Observe and control live behaviour at game-frame granularity | VERIFIED | runtime agent/bridge commands; input/property tools | runtime doctests; native and headless E2E | frame-rate property series, armed sampling, transient-node search, pause and exact physics/process stepping all work; vectors/dictionaries remain structured | none |
| X11 | Keep agent workflows useful in fresh, headless and rapidly edited projects | VERIFIED | built-in skill generator/discovery, headless `EditorRun`, script-cache reload | skills check, module suite, both E2E modes | shipped skills survive installation; headless games use the configured viewport; writing an attached script reloads the editor copy immediately | none |
| X12 | Reconcile playtest verdicts against actions and return actionable schema mistakes | VERIFIED | `mcp_playtest.cpp`, `mcp_protocol.cpp`, `mcp_schema.cpp` | playtest/protocol doctests; native and headless E2E | runtime property writes count as playtest actions; failed tool calls do not; claims without play stay indeterminate; missing/near argument names and near tool names are reported distinctly | none |
