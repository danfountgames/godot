# STATE

Current repository reality. Concise and current, not chronological.

## Primary specification

`docs/godot-ai-clone-spec.md` (exists at the expected path; no competing design
document in the repository).

## Engine baseline

- Godot **4.3-dev**, flat `editor/` layout (spec quotes 4.6-era paths — see DEC-0002).
- In-tree precedents followed: `editor/debugger/debug_adapter/` (EditorPlugin +
  TCPServer + poll on `NOTIFICATION_INTERNAL_PROCESS` with a re-entrancy guard),
  `modules/gdscript/register_types.cpp` (`EditorNode::add_init_callback` →
  `add_editor_plugin`).
- Module doctest headers under `modules/<name>/tests/test_*.h` are auto-included when
  building with `tests=yes`.

## Current milestone

M1 — Foundation and protocol core. Relay and editor module both exist and pass their
own suites; what remains for M1 is a live end-to-end exchange through the relay
against a running editor.

## Current vertical slice

S-17 (in progress): closing the gap between what the game-production template asks for
and what the interface provides. `docs/godot-ai-agent-interface-spec.md` breaks it down;
`.agent/INTERFACE_LEDGER.md` tracks it. 59 tools are advertised over `tools/list`.
Group I is complete: the editor's own
interface is listable (`Godot_ListWindows`), addressable by what it says
(`Godot_FindControl`) and actionable (`Godot_SendEditorInput`). Group B is complete too:
every capture now carries its own provenance, and group F: the project's own test scenes
are discoverable and runnable without a shell, and group E: stacked audio is detected
structurally. **40 of the ledger's 41 items are VERIFIED**; G3 (save-corruption fixtures) is
IMPLEMENTED and unproven, because no game here has a save system to corrupt. The chat
cancellation frame is caught end to end - the tools built for group I are what made that
possible, by locating and pressing the dock's Cancel button.

Six real bugs have fallen out of it so far, all fixed and covered — see the
"Bugs this tranche found" table in the interface ledger. Two were found by
`Godot_ListWindows` on its first run: a timed-out `Godot_AskUser` left a dead dialog on
screen whose buttons did nothing, and the same tool gave a caller who omitted
`timeout_seconds` a one-second deadline instead of the declared 300. The latest was in
`Godot_FindControl` itself — every rectangle came back offset by the editor window's
position, because `get_screen_transform()` already includes that placement except when
the window embeds its subwindows. Only clicking at the reported coordinates showed it;
the numbers looked entirely reasonable.

S-16 (done): the bootstrap project template.

S-15 (done): the virtual display. The gaps that had been recorded as environmental
were not: a container can have a screen if the repository gives it one. Adding
`tools/virtual_display.py` and wiring it into the end-to-end run turned four
requirements from "refusal path tested" into verified behaviour, and found a real
product bug on the way — the editor only requests the remote scene tree while its
Remote panel is visible, so `Godot_GetRuntimeSceneTree` had to ask for it and wait
(`MCPDeferred::begin_polled`).

Now verified against a live editor drawing on a real display: a 1152x648 screenshot
returned as an inline image, a running game's scene tree read live, a runtime property
edit that leaves the scene file byte-identical, `was_playing` on stop, and a question
answered by a genuine pointer click on a choice button (Escape returns
`cancelled: true`).

What remains is honest:

- **T13 free text** — not yet driven by typing. This was recorded as impossible and
  that was wrong: typing works once the field is clicked (see `run_editor_ui_e2e.py`),
  so it is a check that has not been written.
- **R8** — the Windows backend compiles in CI but has never been *run*; macOS neither.
- **C1** — every workflow command passes locally; never observed green on Actions.
- **O1 cancellation** — now caught end to end in `run_editor_ui_e2e.py`: a turn is left
  unanswered, the dock's Cancel button is found by `Godot_FindControl` (the *enabled*
  one, which is only enabled while a turn is in flight) and pressed by
  `Godot_SendEditorInput`, and the client is asserted to receive
  `notifications/cancelled` naming that request. This was recorded as environmental for
  a long time and was not.

## Ledger IDs in this slice

All 55 specification requirements are VERIFIED except R8 (needs another operating
system) and C1 (needs GitHub Actions itself), both IMPLEMENTED. X1 and X2 cover the
virtual display and the editor's render-capability reporting.

## Last verified state

All of the following on the current working tree, in one sweep:

- `python3 tools/relay/tests/run_tests.py` → 59/59 pass.
- `python3 tools/tests/run_tests.py` → 16/16 pass (the virtual display).
- Module suite: 62 cases, 425 assertions, all pass.
- Full engine suite from the repository root: 940 cases, 2,395,117 assertions, all
  pass — no regression from the editor accessors this module added.
- `python3 tools/relay/tests/run_editor_ui_e2e.py` → all checks pass, driving the real
  editor by keyboard and pointer: the palette, the approvals dialog, a chat turn
  answered by the connected client's model, a skill allowed by clicking the rectangle
  `Godot_FindControl` reported for its button, the approvals dialog closed twice through
  `Godot_SendEditorInput` — once by a click on its Close button, once by Escape — and a
  chat turn cancelled from the dock, with the client receiving
  `notifications/cancelled` for that exact request.
- `python3 tools/relay/tests/run_editor_e2e.py` → all checks pass on a display the
  script starts itself, including a real 1152x648 screenshot, a running game's scene
  tree (`root > Main > Player, Hud, Field, Target, EnemySpawner`), a runtime edit that
  leaves the scene file byte-identical, and a question answered by clicking its dialog.
  59 tools are advertised over `tools/list`.
- `python3 tools/relay/tests/run_editor_e2e.py --headless` → all checks pass, with the
  visual tools refusing as they should.
- Documentation (`modules/godot_ai/README.md`), `AGENTS.md`, `CLAUDE.md` and
  `.github/workflows/godot_ai.yml` are current. The workflow now installs
  `xvfb x11-utils libgl1-mesa-dri xdotool` and runs both end-to-end modes; it has still
  never been observed running on GitHub Actions.

## Working-tree expectations

Clean. Everything is committed and pushed. Scratch material for the end-to-end run
lives outside the repository, under the session scratchpad.

## Active failures

None.

## In-flight operation

None.

## Risks

- **Work loss.** A test fixture previously deleted the working tree including `.git`
  (DEC-0006). The relay and module had to be rewritten from scratch. Commit and push
  after every verified slice; never run a recursive delete rooted at the CWD.
- Run the engine test binary from outside the repository (`cd /tmp`) as defence in
  depth.
- Windows and macOS relay behaviour cannot be *run* here. The Windows backend is
  cross-compiled in CI so it cannot rot, but nothing has executed it.
- **Do not record a gap as environmental without trying.** Screenshots, dialogs and
  runtime inspection were all recorded that way and none of them had to be; the
  display was one `apt-get install xvfb` away, and treating it as unreachable hid a
  real bug. `tools/virtual_display.py` exists now — use it.

## Last completed command

The full sweep above, then commit and push.

## Next command

Confirm the ledger still matches reality before doing anything else:

```sh
python3 tools/relay/tests/run_tests.py && python3 tools/tests/run_tests.py \
    && python3 tools/relay/tests/run_editor_e2e.py
```

The visual checks need `xvfb x11-utils libgl1-mesa-dri xdotool`; without them the run
still passes, having quietly degraded to the refusal paths, so check the first line of
its output to see which mode it ran in.
