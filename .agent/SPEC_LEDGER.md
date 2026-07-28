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
| F6 | SEC | Permission model: capabilities, allow/ask/deny, client approval, read-only | F4 | IMPLEMENTED | `mcp_permissions.cpp`, `mcp_service.cpp` | `tests/test_mcp_paths.h`, `test_mcp_protocol.h` | policy evaluation fully covered | interactive approval UI (U2); approval persistence untested |
| F7 | SEC | Audit log with secret redaction | F6 | IMPLEMENTED | `mcp_audit.cpp`, `mcp_tool.cpp` | redaction covered in `test_mcp_registry.h` | summaries redact `api_key` | no test that the log file is written//rotated |
| F8 | SEC/PRI | Checkpoints created before mutation and restorable | F5,F6 | NOT_STARTED | — | — | — | all |

## Protocol

| ID | Spec | Requirement | Deps | Status | Code | Tests | Evidence | Remaining |
|---|---|---|---|---|---|---|---|---|
| P1 | MCP | `initialize` + capability negotiation + version fallback | F3 | VERIFIED | `mcp_protocol.cpp` `_handle_initialize` | `test_mcp_protocol.h` | supported/unsupported version cases pass | none |
| P2 | MCP | `tools/list` with input/output schemas | F4,P1 | VERIFIED | `_handle_tools_list` | `test_mcp_protocol.h` | listing case passes | none |
| P3 | MCP | `tools/call` with structured content | P2 | VERIFIED | `_handle_tools_call` | `test_mcp_protocol.h` | structuredContent + text content asserted | none |
| P4 | MCP | Errors: unknown method/tool, malformed/missing/extra args, denial | P3 | VERIFIED | `mcp_protocol.cpp` | `test_mcp_protocol.h` | 4 error subcases + permission denial | none |
| P5 | MCP | `notifications/tools/list_changed` | F4,P1 | IMPLEMENTED | `MCPService::_on_tools_changed` | relay forwarding covered | relay test asserts client delivery | live editor-side emission not asserted |
| P6 | CI | Cancellation and clean termination semantics | P3 | IMPLEMENTED | `mcp_protocol.cpp` notification branch | `test_mcp_protocol.h` | cancelled notification produces no response | no live cancellation test |

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
| R8 | CI/PKG | macOS and Windows relay behaviour | R1 | BLOCKED | POSIX sockets in `relay.cpp` | — | Linux only in this environment | Winsock port; macOS/Windows hosts unavailable here (see STATE.md) |

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
| T8 | MCP | `Godot_WriteTextFile` + filesystem refresh | F5,F8 | VERIFIED | `tools/mcp_project_tools.cpp` | `tests/test_mcp_tools.h`, `run_editor_e2e.py` | content asserted on disk, not from the tool report | checkpoint integration (F8) |
| T9 | MCP | `Godot_ReadOutputLog` | F2 | NOT_STARTED | — | — | — | all |
| T10 | MCP | `Godot_PlayCurrentScene` / `Godot_PlayMainScene` | F6 | IMPLEMENTED | `tools/mcp_editor_tools.cpp` | — | — | e2e play lifecycle |
| T11 | MCP | `Godot_StopPlaying` | T10 | IMPLEMENTED | `tools/mcp_editor_tools.cpp` | — | — | e2e play lifecycle |
| T12 | MCP | `Godot_CaptureViewport` incl. headless rejection | F6 | NOT_STARTED | — | — | — | all |
| T13 | MCP | `Godot_AskUser` | F2 | NOT_STARTED | — | — | — | all |
| T14 | MCP | `Godot_SearchProject` | F5 | VERIFIED | `tools/mcp_project_tools.cpp` | `tests/test_mcp_tools.h`, `run_editor_e2e.py` | match line numbers, case sensitivity, empty query | none |
| T15 | SEC | Runtime vs persistent edits kept distinct | T5,T10 | IN_PROGRESS | tool descriptions and `initialize` instructions state the distinction; `Godot_ManageNode` documents that saving is separate | — | — | `Godot_SetRuntimeProperty`/`Godot_SetSceneProperty` still absent |

## Skills, UX, docs, packaging

| ID | Spec | Requirement | Deps | Status | Remaining |
|---|---|---|---|---|---|
| S1 | MCP | `SKILL.md` discovery across project/user/plugin roots | F5 | VERIFIED | `mcp_skills.cpp` `get_roots`/`discover` | `tests/test_mcp_skills.h`, `run_editor_e2e.py` | project/plugin/user roots; shipped example skill discovered live | none |
| S2 | MCP | Frontmatter parsing incl. version gating and tool list | S1 | VERIFIED | `mcp_skills.cpp` `parse`/`version_satisfied` | `tests/test_mcp_skills.h` | malformed, unterminated, nameless, disabled, quoted, version-gated cases | none |
| S3 | SEC | Skills untrusted by default; allow/deny persisted | S2,F6 | VERIFIED | `MCPSkills::is_allowed`/`set_allowed` | `tests/test_mcp_skills.h` | denied by default; instructions and resources both refused until allowed | approval UI (U2) |
| S4 | MCP | Supporting resources loaded on demand | S2 | VERIFIED | `MCPSkills::read_resource` | `tests/test_mcp_skills.h`, `run_editor_e2e.py` | on-demand load; traversal out of the skill folder refused | none |
| S5 | MCP | Skills exposed over the protocol and refreshed live | S3,P5 | VERIFIED | `tools/mcp_skill_tools.cpp` | `run_editor_e2e.py` | Godot_ListSkills/Godot_ReadSkill exercised over the real protocol | live re-scan on change not asserted |
| U1 | PRI | Command palette entries | F2 | NOT_STARTED | all |
| U2 | SEC | Settings/status UI incl. approvals | F6,S3 | NOT_STARTED | all |
| U3 | PRI | Headless execution hook | F3 | NOT_STARTED | all |
| D1 | PKG | Developer + user documentation | — | VERIFIED | none for the shipped surface — `modules/godot_ai/README.md` covers architecture, client setup, permissions, the tool catalogue, the registration API and troubleshooting; extend it as tools land |
| D2 | PKG | `AGENTS.md` repository guidance artifact | — | VERIFIED | none — `AGENTS.md` present, imported by `CLAUDE.md` |
| D3 | MCP | Example Godot `SKILL.md` that actually loads | S2 | VERIFIED | none — `misc/godot_ai/skills/scene-cleanup/` is copied into the e2e project and read back over the protocol, so the shipped file is the one proven to load |
| D4 | — | `CLAUDE.md` continuity protocol (required deliverable) | — | VERIFIED | none — section present and kept current |
| C1 | CI | CI wiring for relay tests, module tests, clean build | R2,F4 | IMPLEMENTED | `.github/workflows/godot_ai.yml` added and locally equivalent commands all pass; not yet observed green on GitHub Actions |
| C2 | PKG | Packaging: install layout, licences, clean checkout | R1,D1 | NOT_STARTED | all |

## Optional (explicitly marked optional by the specification)

| ID | Requirement | Status |
|---|---|---|
| O1 | In-editor hosted LLM chat UI | NOT_STARTED |
| O2 | Packaged agent backends / AI Gateway equivalent | NOT_STARTED |
| O3 | Export-template AI integration | NOT_STARTED |
| O4 | Multi-user / remote Streamable HTTP MCP | NOT_STARTED |
