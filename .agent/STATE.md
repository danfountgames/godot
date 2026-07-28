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

S-03: end-to-end verification. Launch a headless editor on a scratch project with
`GODOT_AI_HOME` and `GODOT_AI_AUTO_APPROVE=1`, confirm the instance descriptor is
written, then drive `initialize` → `tools/list` → `tools/call` through the relay and
capture the transcript under `.agent/evidence/`.

## Ledger IDs in this slice

F2, F3, P5, T1, T2, T4, T7 (upgrade from IMPLEMENTED to VERIFIED).

## Last verified state

- Commit `0487520976`, pushed to `origin/claude/godot-ai-clone-spec-6iz0ly`.
- `tools/relay/build.sh` clean; `python3 tools/relay/tests/run_tests.py` → 35/35 pass.
- Editor build clean; `--headless --test --test-case="*[godot_ai]*"` → 24 cases,
  160 assertions, all pass.

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
- macOS/Windows relay verification and any GPU/viewport work (screenshots) cannot run
  in this container; expect those to stay BLOCKED with recorded reasons.

## Last completed command

`git push -u origin claude/godot-ai-clone-spec-6iz0ly` → commit `0487520976`.

## Next command

Launch the headless editor on a scratch project and confirm the instance descriptor
appears:

```sh
GODOT_AI_HOME=<scratch>/aihome GODOT_AI_AUTO_APPROVE=1 \
  bin/godot.linuxbsd.editor.dev.x86_64 --headless --path <scratch>/testproj --editor &
ls <scratch>/aihome/instances
```
