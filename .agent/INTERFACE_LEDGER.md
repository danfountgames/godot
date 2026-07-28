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
| A2 | `Godot_SendPointerInput` | simulate_input | VERIFIED | `tools/mcp_input_tools.cpp`. move/press/release/click through `Input::parse_input_event()`. Proven in `run_editor_e2e.py` by a button that records both its `pressed` signal *and* whether `_gui_input` saw an `InputEventMouseButton` — a shortcut implementation passes the first and fails the second. Refuses with no game running, and off-window coordinates | none. Drag and scroll landed later: a drag is a press, interpolated motion and a release in one call, and the engine's motion coalescing is **flushed between steps** - without that, ten motions arrive as one event with the summed delta and a game with a movement threshold sees a single jump past it. Proven against a fixture control that counts only motion carrying a held button, so a drag degraded into two endpoints scores zero. Scroll sends real `WHEEL_*` press and release pairs, counted the same way |
| A3 | `Godot_SendKeyInput` | simulate_input | VERIFIED | `tools/mcp_input_tools.cpp`, `_send_key` in the agent. type/press/release/tap through `Input::parse_input_event()`, each character carrying its unicode value because that is what a `LineEdit` reads. Proven in `run_editor_e2e.py` by clicking a field, typing, and reading the field's own `text` back out of the game — nothing in the test sets that text. Refuses unknown key names | modifiers are implemented but not asserted |
| A4 | `Godot_SendTouchInput` | simulate_input | VERIFIED | down/up/tap/drag with a finger index, through `Input::parse_input_event()`. Covered in `run_editor_e2e.py`, including the off-window refusal | gesture thresholds are the game's business, not the tool's. Cancellation landed later: `action: "cancel"` sends `pressed = false` **and** `canceled = true`, which is how the engine models it — `InputEvent::is_released()` is `!pressed && !canceled`, so a cancelled touch is deliberately not a release. The e2e asserts both counters in both directions, so a tool that sent an ordinary release and called it a cancellation fails, and so does one that calls everything a cancellation |
| A5 | `Godot_SendGamepadInput` | simulate_input | VERIFIED | buttons, axes, and `joy_connection_changed` for a controller appearing or going away — the last matters because a controller unplugged mid-game must not strand a player in a menu they can no longer move through. Unknown actions refused | device change is delivered, not yet asserted against a game that reacts to it |
| A6 | `Godot_GetInputTrace` | read_runtime | VERIFIED | every injected event recorded in the game with its frame and millisecond, bounded to the last 256. The e2e asserts all four input kinds appear after sending them, which makes a claimed interaction checkable after the fact instead of arguable | none |

## B — Seeing the running game

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| B1 | `Godot_CaptureGame` | read_runtime | VERIFIED | `tools/mcp_input_tools.cpp`, `_capture` in the agent. The game saves a PNG of its own viewport and reports the path; the editor reads it back and returns it inline when small enough — pixels do not travel the debugger bus, which is a message channel. Asserted in `run_editor_e2e.py` on PNG magic bytes, plausible size and the inline block | frame sequences are B2 |
| B2 | `Godot_CaptureFrameSequence` | read_runtime | VERIFIED | the watcher captures on consecutive frames inside the game, each tagged with frame number and millisecond. One screenshot cannot show whether feedback began within 100ms or whether a transition dropped a frame; a sequence can. The e2e asserts the frames are distinct, ordered, and real PNGs | none |
| B3 | `Godot_CaptureEditorWindow` (incl. popups) | read_project | VERIFIED | `DisplayServer::screen_get_image`, so dialogs and the game's own window appear — they are separate OS windows and invisible to a viewport capture. Asserted in the e2e to be at least as large as the viewport capture, and to be a real PNG. Refuses headless, and refuses platforms whose display server cannot capture | none |
| B4 | Capture metadata on every image | — | VERIFIED | `mcp_capture_metadata.{h,cpp}`, applied by all four capture tools. Every image now carries `source` (`editor_viewport`, `editor_screen` or `game_window`), `subject`, `captured_at`, and a `note` for anything that limits it as evidence. Game captures add the frame number, the window size, the scene, and the **time scale** — which is the point: a frame taken at 2x is not evidence about pacing and nothing in the picture will ever reveal that, so the tool says it. The e2e asserts each tool claims its own source, that a normal-speed capture carries no speed warning, and that one taken at 2x does | no in-image watermark; the record travels beside the file, not inside it |

## C — Runtime inspection

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| C1 | `Godot_GetRuntimeProperty` | read_runtime | VERIFIED | `tools/mcp_input_tools.cpp`, `_get_property` in the agent. Returns the value JSON can carry plus Godot's own text form, which round-trips for every type. Refuses unknown nodes and unknown properties. **It immediately earned its place**: reading back what `Godot_SetRuntimeProperty` claimed to have written showed the write had never happened | none |
| C2 | `Godot_GetRuntimeNodeInfo` | read_runtime | VERIFIED | class, script, groups, children, visibility, and a Control's on-screen rect. The e2e uses that rect to aim a real click at the node's reported centre instead of a coordinate copied from the fixture — semantic targeting without making the input fake | partially covers I2 |
| C3 | `Godot_WaitForRuntimeCondition` | read_runtime | VERIFIED | `MCPRuntimeWatcher` checks every frame inside the game and answers once, on satisfaction or deadline. Covered both ways in `run_editor_e2e.py`: a condition that becomes true, and one that never does — where the failure names the value it actually found (`it is 2`) rather than only saying it timed out | none |
| C4 | `Godot_GetRuntimeErrors` (structured) | read_runtime | VERIFIED | an engine error handler installed in the game keeps file, line, function, message and kind — where `Godot_ReadOutputLog` has the same events only as prose. Checked against an error the test deliberately causes, not against an empty list, which would pass whether the feature worked or not. Note that `push_error` reports the engine's call site; only a genuine script fault carries a `.gd` path | none. Stack traces landed later: every entry now carries `stack`, gathered from every registered `ScriptLanguage` **inside the error handler**, because by the time anyone asks the stack has unwound. Each frame has its language, source, function and line. Proven against an error raised three calls deep, asserting the intermediate functions appear and that the order is innermost-first — the call site alone would give one frame and fail |
| C5 | `Godot_SetTimeScale` | run_project | VERIFIED | speeds up a long route without waiting through it, and says plainly that it is not for a playtest or a timing measurement — physics steps, animation and input timing all change, and a bug that only appears at normal speed is exactly the kind it hides. Zero and absurd scales refused | not refused *during* a playtest; that is the agent's discipline, not the tool's |

## D — Performance

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| D1 | `Godot_GetPerformanceMetrics` | read_runtime | VERIFIED | fps, frame and physics time, static memory, object and node counts, draw calls. Carries a `note` saying one call is one sample, so a caller does not read an instant as a budget verdict | windows and verdicts are D2/D3 |
| D2 | `Godot_ProfileWindow` (distribution, worst frame) | read_runtime | VERIFIED | samples frame time across a window and reports mean *and worst*. Judging on the worst is the point: a mean that meets a budget while one frame in sixty takes 40ms is a mean hiding a stutter the player can feel | percentiles beyond mean/worst not kept |
| D3 | Budget comparison verdicts | read_runtime | VERIFIED | `budget_frame_ms` turns the sample into a verdict rather than numbers to interpret, and names the worst frame when it fails. Exercised both ways in the e2e — a budget nothing could miss and one nothing could meet | none |

## E — Audio

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| E1 | `Godot_GetAudioState` (buses, playbacks, peaks) | read_runtime | VERIFIED | bus names, volumes, mute/solo and current peak levels — peaks are how "did a sound play" is answered without hearing it. Carries a note saying what it cannot tell you, because whether audio *sounds right* is not something this can establish and pretending otherwise would be worse than the gap | per-playback detail not exposed |
| E2 | Duplicate/stacking detection | read_runtime | VERIFIED | folded into `Godot_GetAudioState`, so an agent that already calls it gets the answer without having to know to ask. Every audio player in the scene is listed with its stream, bus, volume, polyphony limit and playback position, and any stream sounding on more than one player at once is reported in `stacked` with the players named. Players are found by class name and read through properties, because the three player classes are siblings rather than subclasses and a project may have a fourth of its own. Streams with no resource path are excluded: they have no identity to compare, and counting them would invent duplicates. The e2e drives the fixture from silence to one playback to two of the same sound and asserts all three — a check that only saw the stacked case would pass against a tool that called everything stacked | a single player with `max_polyphony` above 1 can overlap itself and that cannot be counted from outside it; the note says so. Also **instantaneous only** — the spec line reads "across an input burst", which needs sampling over a window while input is sent, so a sound that stacks and clears between two calls is still invisible |

## F — Tests

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| F1 | `Godot_RunSceneTest` | run_project | VERIFIED | `tools/mcp_scene_test_tools.cpp`. Plays a test scene through `play_custom_scene`, watches it from inside the game, and stops it afterwards either way — a test scene left running would be inherited by whatever ran next. A test is a *scene*, never a shell command: a test runner is exactly where the no-arbitrary-execution rule would be quietly broken. Multi-stage, so it needed a bridge that can answer into a callback (`MCPRuntimeBridge::request`) instead of minting a second deferred token the protocol layer would also try to answer. Refuses a second concurrent run, a non-scene path, and a scene that declares none of the contract — the last matters, because reporting '0 failed' about a scene that ran no cases reads as a pass | no way to run every test scene in one call |
| F2 | `Godot_ListSceneTests` | read_project | VERIFIED | recursive discovery by file-name convention (`test_*.tscn`), with the directory and prefix both overridable. The e2e pins the exact list, checks a prefix nothing uses finds nothing rather than falling back to the default, and refuses a file given as a directory | case names are unknown until a scene runs, and this says so rather than guessing |
| F3 | Structured per-case results | run_project | VERIFIED | each case comes back as `{name, passed, message, duration_ms}`, plus counts, `succeeded`, the cases' own total time and the wall time including engine startup. `succeeded` is false when nothing ran, so an empty result cannot be read as a pass. The e2e asserts the failing case keeps both its name and its message — '1 failed' is not something anyone can act on | no stack traces; a case reports its own message |

## G — Saves and `user://`

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| G1 | `Godot_ListUserFiles`, `Godot_ReadUserFile` | read_user_data | VERIFIED | `tools/mcp_user_data_tools.cpp`, `MCPPaths::resolve_user` applying the project tools' confinement to the user directory instead. The e2e asserts the boundary from three directions — `user://../..`, a `res://` path, and an absolute one — because this is the one directory these tools reach that no version control is watching | none |
| G2 | `Godot_WriteUserFile`, `Godot_DeleteUserFile` | edit_user_data | VERIFIED | write/read/list round trip in the e2e. Delete demands `confirm=true` and refuses directories outright: there is no checkpoint layer for user data, so a deleted save is gone, and a recursive delete rooted at a directory is the operation that has already destroyed a working tree once here | user data is **not** checkpointed; recorded rather than implied |
| G3 | Save-corruption fixtures | edit_user_data | IMPLEMENTED | `Godot_WriteUserFile` is the mechanism — writing a truncated or malformed save is exactly how a recovery path gets tested. No game in this repository has a save system to exercise it against, so it is implemented and unproven rather than verified | needs a game with saves |

## H — Project and window configuration

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| H1 | `Godot_GetProjectSetting`, `Godot_SetProjectSetting` | edit_files | VERIFIED | reads one setting or lists the ones this project actually sets — not the thousands of engine defaults that would bury them. Writes match the existing type before saving, because `project.godot` is typed and a viewport width written as a string silently does nothing. The e2e reads the change back out of the file, not out of the tool's report, and asserts a checkpoint was taken | none |
| H2 | `Godot_SetGameWindowSize` | run_project | VERIFIED | resizes the running game so a resolution matrix does not need a relaunch per size, and reports the size that was *applied* alongside the one requested — a window manager is free to refuse, and a matrix built on requested sizes proves nothing. Absurd sizes refused | none |
| H3 | `Godot_GetGameWindowInfo` | read_runtime | VERIFIED | window and viewport size, aspect, content scale. Asserted against `Godot_CaptureGame` in the e2e: the two must agree about how big the game is, or one of them is lying about what a screenshot means | none |

## I — Editor UI automation

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| I1 | `Godot_SendEditorInput` | simulate_input | VERIFIED | `tools/mcp_editor_ui_tools.cpp`. Pointer and keyboard events into the *editor*, in the screen coordinates `Godot_FindControl` reports, through `Input::parse_input_event()` with the event's window id set — which is how `DisplayServerX11` routes an event to one window rather than broadcasting it. Kept a separate tool from the game-side input for the same reason runtime and persistent property edits are separate: clicking the editor changes the project and clicking the game does not, and the caller must never have to infer which just happened. Native and embedded dialogs need opposite coordinate handling and both occur, so the target is resolved by hit-testing screen rects and the event is addressed to the window that owns an OS window. Proven in `run_editor_ui_e2e.py`: a click at the Close button's reported rectangle closes the approvals dialog, and an Escape sent the same way dismisses it — the dialog going away is the assertion, so a misrouted event fails. Refuses headless (no dispatch function exists, so events would vanish silently), off-window points, and unknown key names | drag and scroll now exist here too, with the same flush-between-steps handling, and a drag must start and end in the same window. They are asserted on **delivery** rather than effect - this runs against the live editor, and a drag with a visible outcome would be a drag that moved one of its docks. The behavioural proof that a drag carries real motion lives in the game-side test. Still no way to address a control directly instead of by coordinate |
| I2 | `Godot_FindControl` (→ screen rectangle) | read_project | VERIFIED | `tools/mcp_editor_ui_tools.cpp`. Finds editor Controls by text, name, class or tooltip, **and the rows of Tree and ItemList widgets** — an approvals row and the buttons drawn inside it are not nodes, so nothing else could locate them, and the buttons carry no text at all: their label is only a tooltip. Returns screen rectangles and centres. Refuses a search with no criteria rather than answering with every Control in the editor. Proven in `run_editor_ui_e2e.py` by replacing the skill approval's measured offsets with a click at the rectangle this reports — the assertion is that the skill became readable, so a wrong rectangle fails | no matching on substrings |
| I3 | `Godot_ListWindows` | read_project | VERIFIED | `tools/mcp_asset_tools.cpp`. Walks the scene tree rather than enumerating `DisplayServer` windows, because the editor embeds its dialogs as subwindows of the main one: a DisplayServer enumeration reports one window while three dialogs are stacked on screen. Reports title, class, node path, rect, visibility, embedded/native and which is the root. Works headless, where a screenshot cannot, so "did a dialog open" is answerable with no display at all — the e2e asserts an open `Godot_AskUser` dialog appears by title. Hidden windows on request only | no way to act on a window; that is I1 |

## J — Asset pipeline

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| J1 | `Godot_GetImportStatus` | read_project | VERIFIED | `tools/mcp_asset_tools.cpp`. Whether the pipeline is busy, its progress, and every file whose import is missing or failed — an importable source with no usable asset beside it is the failure this exists to surface. Broken files are only collected once a scan has finished; mid-scan the answer would be a snapshot of a moving target | does not say *why* an import failed |
| J2 | `Godot_ReimportAsset` | edit_files | VERIFIED | reimports named files, or rescans the whole project when none are named, and answers when the pipeline is **idle** rather than when the request was accepted. The e2e replaces a 2x2 PNG with a 16x16 gradient behind the editor's back and asserts the *importer's output bytes* changed — the first version of that check compared file sizes of two flat-red images, which the importer compresses to nearly the same length, so it would have passed whether the reimport happened or not | group/scene reimport not separately exercised |
| J3 | `Godot_WaitForImportQueue` | read_project | VERIFIED | the explicit wait that replaces sleeping after an asset change. Polled deferred token, so the editor keeps running while it waits. This is what stops a test that passes on an idle machine from failing on a loaded one | none |

## K — Checkpoints and evidence

| ID | Capability | Class | Status | Evidence | Remaining |
|---|---|---|---|---|---|
| K1 | `Godot_CreateCheckpoint` (named, on demand) | edit_files | VERIFIED | snapshots named files under a label, for the moment before a sequence of risky changes rather than only before one tool's write. The e2e creates one, clobbers the file, restores through it, and checks the original contents came back | none |
| K2 | `Godot_DiffCheckpoint` | read_project | VERIFIED | compares the project against a checkpoint and separates changed, unchanged, deleted and created, without restoring anything — "what did that sequence of edits actually do". A file the checkpoint recorded as absent and that exists now is *created*, not changed; a file skipped at snapshot time is reported in neither list, because saying "changed" would be a guess. The e2e drives all three transitions on one file | contents of the difference are not shown, only which files differ |

## Loose ends this tranche closed elsewhere

| ID | Item | Status |
|---|---|---|
| Z-1 | The `notifications/cancelled` frame had never been caught end to end. The conversation's handling of a cancelled turn was unit-tested; the wire frame was not, and the gap was recorded as needing something the environment could not provide. It did not: `run_editor_ui_e2e.py` now leaves a chat turn unanswered, finds the dock's **enabled** Cancel button with `Godot_FindControl` — enabled is the discriminator, since it is only live while a turn is in flight — presses it with `Godot_SendEditorInput`, and asserts the client receives `notifications/cancelled` naming that request. A client left waiting on a request the editor has abandoned burns a model call for an answer nobody will read | VERIFIED |

## Bugs this tranche found in existing tools

| ID | Bug | Status |
|---|---|---|
| Y-1 | `Godot_SetRuntimeProperty` reported success while doing nothing whenever the value's JSON type did not match the property's real type. It went through the debugger's generic `scene:set_object_property`, which hands the value to `Object::set` unconverted — and `Object::set` refuses silently. A `Vector2` given `[64, 32]` simply stayed `(0, 0)`. The old test only asserted that the *scene file* was unchanged, so it never noticed. Now routed through the runtime agent, which knows the property's real type, converts to it, and **reads the value back before answering** | FIXED, covered by `run_editor_e2e.py` |
| Y-2 | `Godot_GetRuntimeSceneTree` returned the editor's cached tree whenever one existed, and the first tree arrives before the main scene is instantiated — so an agent polling while a game booted got a bare `root` for ever | FIXED |
| Y-3 | `Godot_AskUser` left its dialog on screen after the request timed out. The token was already answered, so every button on the dialog was inert: it invited an answer, accepted the click, and did nothing. Found by `Godot_ListWindows` — the very first window listing showed a dialog from a test that had finished minutes earlier. The dialog now watches its own token and closes when it stops being pending | FIXED, covered by `run_editor_e2e.py` |
| Y-4 | `Godot_AskUser` read `timeout_seconds` by subscripting a const `Dictionary`. A caller who omitted it got `0` rather than the schema's declared default of 300, and `MAX(1, 0)` turned that into a question the user had **one second** to answer. The declared default and the applied default now agree | FIXED |
| Y-6 | `Godot_FindControl` reported every rectangle offset by the editor window's position. `CanvasItem::get_screen_transform()` runs through `Window::get_popup_base_transform()`, which already folds in each window's placement and walks out through embedders — *except* for a window that embeds its subwindows, where it returns identity on purpose. Both arrangements occur (the editor uses real OS windows for dialogs normally and embeds them in single-window mode), so the origin has to be added conditionally. The wrong version looked entirely plausible; only clicking at the reported coordinates showed it | FIXED, covered by `run_editor_ui_e2e.py` |
| Y-5 | The e2e's checkpoint test passed `content` where `Godot_WriteTextFile`'s schema says `text`, and did not check the reply. The write was rejected, so the restore it was meant to exercise had nothing to undo and the assertion could not fail | FIXED |

## New capability classes

| ID | Item | Status | Remaining |
|---|---|---|---|
| X-1 | `simulate_input` capability class, ask-by-default | VERIFIED | none — added to `MCPCapability`, defaults to `ask`, and is distinct from `run_project` because input can do anything a player can |
| X-2 | `read_user_data` / `edit_user_data` classes | VERIFIED | both ask-by-default. Reading is `ask` where reading the *project* is `allow`, because this is the player's data rather than the developer's source |
| X-3 | Template `AGENTS.md` updated as capabilities land | VERIFIED | the template listed 25 tools and told agents input injection, runtime reads, performance and `user://` did not exist. All of that is now built, so leaving it would have been worse than the original error — it would have told an agent to fall back to a host harness for something the product does. Now lists 55 tools, and the "does not provide" section is down to audio's limits, a test runner, editor-side input, and shell execution. The count in the template header had also drifted four behind the registry; it is now checked against a live `tools/list` rather than against the table |
