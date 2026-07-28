# Agent Interface ledger

Derived from `docs/godot-ai-agent-interface-spec.md`: the capabilities the fork must
gain so that `misc/godot_ai/project_template/AGENTS.md` describes something achievable
through the product rather than through a host-side harness.

Statuses: `NOT_STARTED`, `IN_PROGRESS`, `IMPLEMENTED`, `VERIFIED`, `BLOCKED`.
`IMPLEMENTED` never means complete. `BLOCKED` is only for a genuinely external
condition.

**Verification rule specific to this tranche.** An input tool is not `VERIFIED` by a
test that could pass against a shortcut implementation. The test must assert that the
event travelled the real pipeline — the receiving control saw an `InputEvent`, not just
that its signal fired.

## A — Real input injection

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| A1 | Runtime agent channel (editor ↔ running game) | — | VERIFIED | `mcp_runtime_agent.{h,cpp}` in the game process, `mcp_runtime_bridge.{h,cpp}` in the editor, over the remote debugger. Request/reply correlation with deferred tokens; a game that stops mid-request fails its callers immediately rather than making them wait out the timeout. Exercised by every A2 check in `run_editor_e2e.py` | none |
| A2 | `Godot_SendPointerInput` | simulate_input | VERIFIED | `tools/mcp_input_tools.cpp`. move/press/release/click through `Input::parse_input_event()`. Proven in `run_editor_e2e.py` by a button that records both its `pressed` signal *and* whether `_gui_input` saw an `InputEventMouseButton` — a shortcut implementation passes the first and fails the second. Refuses with no game running, and off-window coordinates | touch/keys are A3–A4 |
| A3 | `Godot_SendKeyInput` | simulate_input | VERIFIED | `tools/mcp_input_tools.cpp`, `_send_key` in the agent. type/press/release/tap through `Input::parse_input_event()`, each character carrying its unicode value because that is what a `LineEdit` reads. Proven in `run_editor_e2e.py` by clicking a field, typing, and reading the field's own `text` back out of the game — nothing in the test sets that text. Refuses unknown key names | modifiers are implemented but not asserted |
| A4 | `Godot_SendTouchInput` | simulate_input | NOT_STARTED | — | all |
| A5 | `Godot_SendGamepadInput` | simulate_input | NOT_STARTED | — | all |
| A6 | `Godot_GetInputTrace` | read_runtime | NOT_STARTED | — | all |

## B — Seeing the running game

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| B1 | `Godot_CaptureGame` | read_runtime | VERIFIED | `tools/mcp_input_tools.cpp`, `_capture` in the agent. The game saves a PNG of its own viewport and reports the path; the editor reads it back and returns it inline when small enough — pixels do not travel the debugger bus, which is a message channel. Asserted in `run_editor_e2e.py` on PNG magic bytes, plausible size and the inline block | frame sequences are B2 |
| B2 | `Godot_CaptureFrameSequence` | read_runtime | NOT_STARTED | — | all |
| B3 | `Godot_CaptureEditorWindow` (incl. popups) | read_project | NOT_STARTED | — | all |
| B4 | Capture metadata on every image | — | NOT_STARTED | — | all |

## C — Runtime inspection

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| C1 | `Godot_GetRuntimeProperty` | read_runtime | VERIFIED | `tools/mcp_input_tools.cpp`, `_get_property` in the agent. Returns the value JSON can carry plus Godot's own text form, which round-trips for every type. Refuses unknown nodes and unknown properties. **It immediately earned its place**: reading back what `Godot_SetRuntimeProperty` claimed to have written showed the write had never happened | none |
| C2 | `Godot_GetRuntimeNodeInfo` | read_runtime | NOT_STARTED | — | all |
| C3 | `Godot_WaitForRuntimeCondition` | read_runtime | NOT_STARTED | — | all |
| C4 | `Godot_GetRuntimeErrors` (structured) | read_runtime | NOT_STARTED | — | all |
| C5 | `Godot_SetTimeScale` | run_project | NOT_STARTED | — | all |

## D — Performance

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| D1 | `Godot_GetPerformanceMetrics` | read_runtime | NOT_STARTED | — | all |
| D2 | `Godot_ProfileWindow` (distribution, worst frame) | read_runtime | NOT_STARTED | — | all |
| D3 | Budget comparison verdicts | read_runtime | NOT_STARTED | — | all |

## E — Audio

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| E1 | `Godot_GetAudioState` (buses, playbacks, peaks) | read_runtime | NOT_STARTED | — | all |
| E2 | Duplicate/stacking detection | read_runtime | NOT_STARTED | — | all |

## F — Tests

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| F1 | `Godot_RunSceneTest` | run_project | NOT_STARTED | — | all |
| F2 | `Godot_ListSceneTests` | read_project | NOT_STARTED | — | all |
| F3 | Structured per-case results | run_project | NOT_STARTED | — | all |

## G — Saves and `user://`

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| G1 | `Godot_ListUserFiles`, `Godot_ReadUserFile` | read_user_data | NOT_STARTED | — | all |
| G2 | `Godot_WriteUserFile`, `Godot_DeleteUserFile` | edit_user_data | NOT_STARTED | — | all |
| G3 | Save-corruption fixtures | edit_user_data | NOT_STARTED | — | all |

## H — Project and window configuration

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| H1 | `Godot_GetProjectSetting`, `Godot_SetProjectSetting` | edit_files | NOT_STARTED | — | all |
| H2 | `Godot_SetGameWindowSize` | run_project | NOT_STARTED | — | all |
| H3 | `Godot_GetGameWindowInfo` | read_runtime | NOT_STARTED | — | all |

## I — Editor UI automation

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| I1 | `Godot_SendEditorInput` | simulate_input | NOT_STARTED | — | all |
| I2 | `Godot_FindControl` (→ screen rectangle) | read_project | NOT_STARTED | — | all |
| I3 | `Godot_ListWindows` | read_project | NOT_STARTED | — | all |

## J — Asset pipeline

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| J1 | `Godot_GetImportStatus` | read_project | NOT_STARTED | — | all |
| J2 | `Godot_ReimportAsset` | edit_files | NOT_STARTED | — | all |
| J3 | `Godot_WaitForImportQueue` | read_project | NOT_STARTED | — | all |

## K — Checkpoints and evidence

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| K1 | `Godot_CreateCheckpoint` (named, on demand) | edit_files | NOT_STARTED | — | all |
| K2 | `Godot_DiffCheckpoint` | read_project | NOT_STARTED | — | all |

## Bugs this tranche found in existing tools

| ID | Bug | Status |
|---|---|---|
| Y-1 | `Godot_SetRuntimeProperty` reported success while doing nothing whenever the value's JSON type did not match the property's real type. It went through the debugger's generic `scene:set_object_property`, which hands the value to `Object::set` unconverted — and `Object::set` refuses silently. A `Vector2` given `[64, 32]` simply stayed `(0, 0)`. The old test only asserted that the *scene file* was unchanged, so it never noticed. Now routed through the runtime agent, which knows the property's real type, converts to it, and **reads the value back before answering** | FIXED, covered by `run_editor_e2e.py` |
| Y-2 | `Godot_GetRuntimeSceneTree` returned the editor's cached tree whenever one existed, and the first tree arrives before the main scene is instantiated — so an agent polling while a game booted got a bare `root` for ever | FIXED |

## New capability classes

| ID | Item | Status | Remaining |
|---|---|---|---|
| X-1 | `simulate_input` capability class, ask-by-default | VERIFIED | none — added to `MCPCapability`, defaults to `ask`, and is distinct from `run_project` because input can do anything a player can |
| X-2 | `read_user_data` / `edit_user_data` classes | NOT_STARTED | all |
| X-3 | Template `AGENTS.md` updated as capabilities land | NOT_STARTED | all |
