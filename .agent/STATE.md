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

S-12 (next): packaging (C2), then the remaining coverage gaps (U1/U2 dialog tests,
F6/F7 persistence and audit-file tests).

S-11 (done): the relay's platform seam. Every socket, stdio, filesystem and signal
call now goes through `platform::`, with a POSIX backend that behaves exactly as
before (all 39 tests still pass) and a Winsock backend that cross-compiles under
mingw. The hard part was waiting: Windows cannot poll a console handle and a socket
together, so its backend reads stdin on a thread.

S-10 (done): deferred tool responses and `Godot_AskUser`. A tool can now return a
token instead of a result; the service holds the client's request id and answers when
the token completes or its deadline passes. Exactly one response per call is
guaranteed: late answers are dropped, and a disconnecting client abandons its tokens.

S-09 (done): `Godot_CaptureViewport`. Refuses headless rather than returning a blank
image, saves a PNG in the project, and returns it inline as an MCP image block when
small enough. Tools can now supply their own content blocks via `_content`.

S-08 (done): the approvals dialog (U2), command palette and Tools menu entries (U1),
and the relay's one-shot `--call` mode (U3). The dialog and commands are constructed
in every headless e2e run, so they cannot crash the editor, but the UI itself cannot
be clicked without a display - recorded as IMPLEMENTED, not VERIFIED.

S-07 (done): `Godot_ReadOutputLog` (T9) and the persistent/runtime property split
(T15). `Godot_SetSceneProperty` is undoable and survives a save; `Godot_SetRuntimeProperty`
and `Godot_GetRuntimeSceneTree` drive the running game and say `persistent: false`
in their results as well as their descriptions. A path lookup was added to
`EditorDebuggerTree`, which previously only exposed the user's current selection.

S-06 (done): checkpoints. Snapshots are taken by the protocol layer before any
mutating tool runs, from paths the tool itself declares, and stored outside the
project. Restore puts files back byte for byte and removes files the tool created.

S-05 (done): skills. Discovery across project/plugin/user roots, frontmatter
parsing with editor-version gating, deny-by-default trust, on-demand supporting
resources, and `Godot_ListSkills`/`Godot_ReadSkill`. The shipped example skill is
copied into the e2e project and read back over the protocol.

S-04 (done): `Godot_ManageNode` (create/delete/rename/reparent) plus
`Godot_UndoLastAction`/`Godot_RedoLastAction`, all through `EditorUndoRedoManager`
following the scene tree dock's own patterns. The unit test found a real crash: the
editor tools guarded on `EditorInterface::get_singleton()`, which exists in any
editor build including the test binary, while its methods dereference `EditorNode`.
Both are now required before any editor call.

S-03 (done): end-to-end verification. `tools/relay/tests/run_editor_e2e.py` launches a
headless editor, waits for the instance descriptor, and drives the full MCP session
through the real relay. It found and fixed a real defect: joining onto `res://`
produced `res:/…`, which also made `Godot_SearchProject` silently return nothing.

## Ledger IDs in this slice

C2 (next). Just completed: R8 → IMPLEMENTED (was BLOCKED; the Windows path now
exists and is compile-verified, only runtime verification remains blocked).

## Last verified state

- Commit `7d8fa581f5`, pushed to `origin/claude/godot-ai-clone-spec-6iz0ly`.
- `tools/relay/build.sh` clean; `python3 tools/relay/tests/run_tests.py` → 39/39 pass.
- Module suite: 50 cases, 341 assertions, all pass. Full engine suite from the
  repository root: 922 cases, 2,395,010 assertions, all pass (no regression).
- `python3 tools/relay/tests/run_editor_e2e.py` → 31/31 checks pass against a live
  headless editor, including a create/rename/reparent/undo/save/delete/undo round
  trip verified against the saved scene file, and the shipped example skill read back
  over the protocol.
- Documentation (`modules/godot_ai/README.md`), `AGENTS.md` and
  `.github/workflows/godot_ai.yml` are in place. The workflow has not yet been
  observed running on GitHub Actions.

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
  cross-compiled in CI so it cannot rot, but nothing has executed it; same for any
  screenshot output, which needs a display.

## Last completed command

`python3 tools/relay/tests/run_editor_e2e.py` → all checks passed; committed and
pushed as `7d8fa581f5`.

## Next command

Start C2 by seeing what the editor build already installs:

```sh
grep -rn "install\|InstallAs" SConstruct platform/linuxbsd/SCsub | head
```
