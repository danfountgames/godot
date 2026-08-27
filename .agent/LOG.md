# LOG

Append-only. Concise entries; large output goes to `.agent/evidence/`.

## 2026-07-28 — Session 1

### Discovery
- Spec at `docs/godot-ai-clone-spec.md`; no competing document, no pre-existing
  `CLAUDE.md`, `AGENTS.md`, `.claude/` or `.agent/`.
- Engine baseline: Godot 4.3-dev, flat `editor/` layout → DEC-0002 path remapping.
- Toolchain: installed scons via pip and five X11/ALSA dev packages via apt.
- Baseline editor build 7m42s; full doctest suite green before any changes.

### S-01 relay (R1–R7)
- Implemented `tools/relay/` and 35 integration tests against a scriptable fake
  editor. Two real defects found and fixed while testing: the handshake wait was a
  single 10s poll that blocked shutdown (now sliced and interruptible, with
  `--handshake-timeout`), and handshake failures did not distinguish fatal from
  transient (rejection/mismatch are now remembered, timeouts stay retryable).
- Two harness defects also found: the fake editor accepted only one connection, and
  Python `close()` without `shutdown()` never sent FIN — both recorded in TACTICS.

### Incident — working tree destroyed
- The path-test fixture called `erase_contents_recursive()` on a `DirAccess` created
  with `ACCESS_FILESYSTEM`, which starts at the process working directory. Running
  the test binary from the repository root deleted the entire tree, `.git` included.
- Only the spec commit had been pushed, so the relay and the whole editor module were
  lost and had to be rewritten from context.
- Recovery: re-cloned from origin, rewrote `CLAUDE.md`/`.agent`, then the relay
  (35/35 passing again), then the module (24 cases passing). Each step committed and
  pushed before starting the next.
- Rules adopted: DEC-0006 guarded `mcp_test_remove_tree()`; commit-and-push per
  verified slice; run the test binary from outside the repository.

### S-02 editor module (F1, F4, F5, P1–P4)
- `modules/godot_ai/`: registry, schema validation, permissions, path confinement,
  protocol handler, service, audit log, twelve built-in tools.
- Protocol handling was deliberately separated from transport (DEC-0005) so the whole
  JSON-RPC surface is covered by doctest without sockets.
- Commit `0487520976`, pushed.

### S-03 end-to-end (F2, F3, T1, T2, T4, T6, T7, T8, T14)
- Drove a full MCP session through the real relay against a headless editor on a
  scratch project. The editor advertised itself on 6010, the relay connected, and
  initialize/tools/list/tools/call all worked on the first attempt.
- The run found a real defect the unit tests had missed: joining a child onto
  `res://` produced `res:/scenes/main.tscn`. Listings looked plausible but every
  later lookup of those paths failed, which is why `Godot_SearchProject` returned no
  matches at all. Fixed with `res_join()`, and covered by tool tests that assert a
  listed path round-trips through another tool.
- Added `tools/relay/tests/run_editor_e2e.py` so the whole-stack check is repeatable;
  it verifies effects on disk rather than trusting tool reports, and covers the
  refusal paths. Transcript kept at `.agent/evidence/e2e-transcript.jsonl`.
- Commit `7d8fa581f5`, pushed.

### S-05 documentation, AGENTS.md, CI (D1, D2, C1)
- `modules/godot_ai/README.md`: architecture and why the process split exists, client
  setup, the permission table with the three rules that hold regardless of settings,
  the tool catalogue, a working GDScript registration example, build/test commands
  and a troubleshooting table keyed by the exact error strings the code emits.
- `AGENTS.md` carries the tool-neutral rules; `CLAUDE.md` imports it with @AGENTS.md
  and keeps only the Claude Code continuity workflow.
- `.github/workflows/godot_ai.yml` runs the relay suite first (fast, no engine
  build), then the editor build, module tests and the end-to-end script.

### S-04 structural scene editing (T3, T5)
- `Godot_ManageNode` (create/delete/rename/reparent) and
  `Godot_UndoLastAction`/`Godot_RedoLastAction`, following the patterns in
  `editor/scene_tree_dock.cpp`: `add_do_reference` on creation, owner restoration for
  every owned descendant on undo of a delete, index restoration on reparent, and
  refusals for the scene root, internal nodes, nodes owned by an instanced sub-scene,
  and reparenting into a descendant.
- The unit test caught a real crash: `require_editor()` guarded on
  `EditorInterface::get_singleton()`, but that singleton is created by
  `register_editor_types()` and therefore exists in the headless test binary, while
  its methods dereference `EditorNode`. Both singletons are now required. The same
  flaw was present in every editor tool.
- e2e extended to a full round trip: create → rename → reparent → undo (asserting the
  original parent is restored) → save (asserting the node reached the .tscn on disk)
  → delete → undo, plus refusals that must leave the scene untouched. 18/18 checks.

### S-05 skills (S1–S5, D3)
- `MCPSkills`: discovery across project (`res://ai_skills`), plugin
  (`addons/*/ai_skills`) and user roots in precedence order, a small YAML-frontmatter
  parser reporting the offending line number, editor-version gating, deny-by-default
  trust, and supporting resources loaded on demand and confined to the skill folder.
- Duplicate names keep the first and flag the rest rather than silently shadowing.
  Broken skills are returned with a `problem` instead of vanishing, so a user can see
  why their file did not load.
- `Godot_ListSkills` / `Godot_ReadSkill` expose them over the protocol.
- The shipped `misc/godot_ai/skills/scene-cleanup/` is copied into the e2e project and
  read back through the relay, so D3 is a fact about the artifact, not a fixture.
- `GODOT_AI_AUTO_APPROVE=1` now also trusts discovered skills; it has one meaning
  throughout: "no human is present to decide", and must be set deliberately.

### S-06 checkpoints (F8, T8)
- `MCPCheckpoints` snapshots files before a mutating tool runs, into
  `$GODOT_AI_HOME/checkpoints/<project>/<id>/` with a manifest. Outside the project
  on purpose: a snapshot inside `res://` would be imported and could be committed.
- The protocol layer creates the checkpoint, not the tools, so no mutating tool can
  bypass it; tools only declare which files they may write via
  `get_checkpoint_paths()`. A failure to snapshot refuses the call rather than
  running without a way back.
- Scope is deliberate and documented: undo covers unsaved scene edits, checkpoints
  cover files, version control is never touched. `Godot_SaveScene` snapshots the
  scene file because saving is the moment an edit becomes a file change.
- `Godot_ListCheckpoints` / `Godot_RestoreCheckpoint`, with restore putting contents
  back byte for byte and removing files the tool had created.

### S-07 output log (T9)
- `EditorLog` kept its messages private, so the fork adds read accessors rather than
  scraping the RichTextLabel. This is the kind of hook the specification says a fork
  exists to provide.
- `Godot_ReadOutputLog` filters by problem level and substring, then keeps the newest
  N matches - filtering after truncating would return fewer matches than requested.
- Also corrected a piece of guidance that was wrong: the **full** engine suite must
  run from the repository root, because the GDScript runner, completion and LSP
  suites resolve test data relative to the working directory. Running it from /tmp
  produced failures that looked like regressions and were not. The module's own suite
  runs from anywhere, and fixture safety comes from `mcp_test_remove_tree()`, not
  from the choice of working directory.

### S-07b persistent vs runtime properties (T15)
- `Godot_SetSceneProperty` coerces the incoming JSON against the property's *current*
  value, so the tool never has to know Godot's whole type system, and goes through
  undo/redo like every other scene edit.
- `Godot_SetRuntimeProperty` and `Godot_GetRuntimeSceneTree` drive the running game
  through the debugger. `EditorDebuggerTree` only exposed the user's current
  selection, so a path lookup was added to it.
- The distinction the specification insists on is carried in three places: the tool
  names, the descriptions, and a `persistent` field in every result - an agent that
  reads only the output still learns whether the change survives.
- A schema bug surfaced in e2e: `value` was declared as an object, so `[128, 64]` was
  rejected. Added `MCPSchema::any_property()` for arguments whose shape depends on
  what they address.

### S-08 approvals UI, palette entries, one-shot mode (U1, U2, U3)
- `MCPApprovalsDialog` is the one place a user can see what is waiting on them:
  pending clients and every discovered skill, each with the reason it is not usable
  and an allow/revoke button. Editor Settings can already show the raw arrays, but
  that is not a decision surface - a client waiting to connect never appears there.
- Three command palette entries (approvals, status, restart) plus a Tools menu item.
- The relay grew a one-shot `--call <tool> --arguments <json>` mode for scripts and
  CI: it runs a complete MCP session and prints the result, with distinct exit codes
  for tool failure (1) and an unreachable editor (2), so a script can tell them apart.
  In that mode stdout carries the result rather than protocol traffic, which is
  documented in the usage text.
- U1/U2 are recorded as IMPLEMENTED rather than VERIFIED: the editor constructs both
  in every headless run, so they cannot crash it, but nothing here can click a button.

### S-09 screenshots (T12)
- `Godot_CaptureViewport` refuses when the display server is headless rather than
  returning a blank image presented as if it were the editor.
- Tools can now supply their own MCP content blocks through `_content`, which is how
  the PNG is returned inline; a screenshot has no useful text rendering.
- Found a genuine engine trap while testing: `Dictionary::operator[]` inserts a null
  for a missing key even through a `const Dictionary &`, because the private data
  pointer does not propagate constness. `get_checkpoint_paths()` runs before schema
  validation, so an unguarded read poisoned the arguments and the call was rejected
  with a confusing "'path' must be of type string" about a key the caller never sent.
  Fixed, promoted to CLAUDE.md, and pinned by a test that checks every mutating tool.
- T12 is IMPLEMENTED, not VERIFIED: nothing here has a display, so the refusal is
  verified and the image itself is not.

### S-10 deferred responses and ask-user (P6, T13)
- Everything else here runs to completion inside one call on the main thread. A modal
  cannot: it needs the main loop to keep running, so blocking in the tool would freeze
  the editor and the dialog it just opened. Tools can now return a token instead of a
  result; `MCPService` holds the request id and answers when the token resolves.
- Exactly one response per call is the invariant: a late answer is dropped rather than
  queued, an overdue call fails with a timeout, and a disconnecting client abandons
  its tokens so a dialog cannot answer into a dead socket.
- The e2e proves the whole path without a human: a 2s timeout shows the request was
  genuinely held, the response arrives later, and the editor keeps serving other calls
  meanwhile.
- Two test-side defects fixed on the way: the guarded delete only accepted a marker on
  the *leaf* of a path, so fixtures could not clean up their own subdirectories; and
  the timeout test asserted on a sub-millisecond deadline against a millisecond clock.

### S-11 platform seam and Windows backend (R8)
- Every socket, stdio, filesystem, environment and signal call in the relay now goes
  through `platform::`, so relay.cpp reads identically on both platforms and a
  Windows regression cannot hide inside the protocol logic.
- The POSIX backend is a straight extraction: all 39 relay tests still pass, which is
  what makes the refactor safe to believe.
- The awkward part is waiting. On POSIX a socket and stdin are both pollable fds; on
  Windows `WSAPoll` handles sockets only, so the backend reads stdin on a thread and
  `wait_for_input()` hides the difference.
- `build.sh --windows` cross-compiles with mingw and CI now runs it. R8 moves from
  BLOCKED to IMPLEMENTED: the code exists and compiles, and only *running* it on a
  Windows or macOS host remains outside this environment.

### S-12 packaging and audit coverage (C2, F6, F7)
- `package.sh` assembles the distributable: relay binaries, example skills, README,
  INSTALL.md, and the MIT licence and copyright notices that redistributing a derived
  Godot binary requires.
- `run_clean_checkout.py` exports the tracked tree with `git archive`, then builds,
  tests and packages inside it. It caught an uncommitted `package.sh` on its very
  first run, which is exactly the class of mistake it exists to catch.
- Audit tests close F7: allowed and refused calls are both recorded, entries append,
  a multi-line argument cannot break the one-object-per-line format, and client
  approval is denied by default with only the exact automation opt-in accepted.

### S-13 approvals-dialog decision logic (U2)
- The dialog decided *and* drew; the decision is now `mcp_skill_status_text()`, which
  returns the status line plus whether a toggle button would mean anything and whether
  the item is waiting on the user. The dialog renders what it returns.
- That makes the substantive half testable headlessly: a broken or version-gated skill
  offers no "Allow" button, because offering one for something that cannot load would
  be a lie. U1/U2 stay IMPLEMENTED - rendering and clicking still need a display.

### S-14 specification re-audit
- Closed P5 by testing the two links that were untested: the registry announces every
  add and remove and stays silent on a no-op, and the frame is a valid id-less
  notification. The relay side was already covered.
- Verified play/stop against a live editor (T10, T11). Two things are reported rather
  than asserted because this environment cannot show them: `was_playing` on stop, and
  the remote scene tree of a headless game.
- Final tally: 45 of 55 requirements VERIFIED. The rest are environmental (need a
  display, a Windows host, or GitHub Actions) or explicitly optional.

### Next
- Nothing is blocked on more implementation here. See NEXT.md.
  the approvals UI (U1, U2), screenshots (T12), ask-user (T13), and the Winsock port
  that would unblock R8.

## S-15 — the repository supplies its own display

### Done
- Added `tools/virtual_display.py`: starts `Xvfb`, waits until the display genuinely
  answers, supplies the software-GL environment and the renderer arguments that stack
  can serve. Reuses a working `DISPLAY`, refuses to trust one that answers nothing,
  and returns an unusable display rather than raising when no X server exists.
  Usable as a library, as a command wrapper, and as `--probe`.
- `tools/tests/run_tests.py`: 14 cases over real X servers, including the
  degraded-environment path with the X server mocked away.
- `run_editor_e2e.py` now starts a display itself; `--headless` forces the old path.
  Both modes pass.
- Turned four requirements from "refusal path tested" into verified behaviour:
  a real 1152x648 screenshot returned inline (T12), a running game's scene tree read
  live and a runtime edit that leaves the scene file byte-identical (T15),
  `was_playing` on stop (T11), and a question answered by a real pointer click with
  Escape reported as a cancellation (T13).
- `Godot_GetEditorStatus` now reports `display_server` and `can_render`;
  `Godot_CaptureViewport`'s headless refusal names the fix (X2).
- CI installs `xvfb x11-utils libgl1-mesa-dri xdotool` and runs both end-to-end modes.

### Found
- A real product bug the refusal paths had been hiding: the editor only requests the
  remote scene tree while its Remote panel is visible, and nothing signals when it
  arrives. `MCPDeferred::begin_polled()` was added so the runtime tools request it and
  wait.
- A launched game inherits the *project's* renderer, not the editor's command line, so
  it died on Vulkan under software rendering until the test project asked for
  `gl_compatibility`.
- A successful capture returns an image block and no text; the e2e's failure-message
  helper indexed `content[0]["text"]` eagerly and crashed on success.

### Not done
- Typed free text into the ask-user dialog: without a window manager no window takes X
  input focus, so a `LineEdit` never receives characters. `Return` and `Escape` still
  work, and choice questions are fully clickable. `openbox` does not fix it and makes
  click geometry wrong.
- U1/U2 UI clicking: now possible, not yet written.

### Next
- See NEXT.md. The remaining gaps are tests not yet written (U1, U2), another
  operating system (R8), or GitHub Actions itself (C1).

## 2026-08-04 — S-18 macOS GodotAI branding and native build

- The generated editor bundle is now `bin/GodotAI.app`; its displayed bundle name,
  executable, and icon resource are consistently named `GodotAI`.
- Added a reproducible Swift/AppKit icon generator and ICNS assembly script. It keeps
  the existing macOS Godot artwork pixel-faithful and adds a violet AI badge with a
  cyan rim; all 16–1024 px representations retain alpha and the badge remains legible.
- Installed MoltenVK 1.4.2 and built the native arm64 editor with Vulkan/Metal,
  OpenGL, tests, SCU, and bundle generation enabled. Current Homebrew packages only
  an arm64 xcframework slice, so the build used a temporary compatibility layout for
  Godot 4.3's older universal-slice detector.
- Current Apple Clang exposed three stale vendored-source defects. Applied the same
  narrow fixes present in current official Godot source: two invalid Embree diagnostic
  stream operators and libpng's obsolete classic-Mac `fp.h` branch.
- Evidence: macOS relay suite 64/64; bundle plist, arm64 Mach-O, icon checksum and
  ad-hoc signature verified; bundled GodotAI suite 69 cases / 433 assertions; bundle
  smoke launch reports `4.3.dev.custom_build.5e0a468c3`.

## 2026-08-27 — S-21 continued: bug capture, and two defects it uncovered

### Retroactive bug capture (build-order item 4)

- `Godot_CaptureBugSession` writes out the input that led to what just went wrong, with
  nothing armed in advance. Recording has to be started first, and you do not know
  something is a bug until it happens — by which time an unarmed recorder has nothing.
- What it writes is an ordinary session, so `Godot_ReplaySession` runs it unchanged. A
  bug report and a regression test are the same file, produced from opposite ends.
- Two buffers, because a crash destroys one of them. The game's own trace is
  authoritative while it lives (real frames, and nothing the game refused). The editor
  now also mirrors every input it dispatches, hooked into the single choke point in
  `MCPRuntimeBridge`, and that survives the process. Cleared when a game *starts*, not
  when one stops — the stop is exactly when the mirror is the only copy left.
- The four `_send_*` handlers now echo the frame they recorded, which is how the mirror
  learns where an event landed. A mirror with no frames cannot be replayed.
- The event a game died on is never acknowledged, and it is also the one that caused the
  bug. It is placed by extrapolating from the frame rate the buffer actually observed and
  marked estimated in the record, the metadata and the reply; `replay_level` then reads
  `attempt`, and the fidelity line says a replay of that tail is a reproduction attempt.
- Evidence: 18 doctest cases; three live e2e checks including one taken after the game
  exited. Module suite 225 cases / 3337 assertions. 89 tools advertised.

### The e2e run was throwing away the evidence it needed

- The editor's stdout went to a pipe nobody read and nobody printed, so a crash arrived
  as "editor disconnected" and nothing else. An unread pipe also blocks the writer at
  64 KiB, so this was a latent hang as well as a blind spot.
- It is now drained on a thread and the tail printed on failure. It paid for itself in
  the same run.

### A crash in the replay plan, found by being the first to replay twice

- `MCPReplayPlan::load()` reset two fields and left the rest to `start()`. A plan read
  between the two carried the previous run's verdict. A failed run followed by a session
  with **no assertions** — which is precisely what a retroactive capture produces — left
  `first_divergence` at 0, so the tool's first poll called the new run finished and
  `to_report()` indexed an assertion list `load()` had just emptied. Editor down.
- `load()` now forgets the whole of the previous run; a plan that has not started is not
  finished; the index is bounds-checked as well. Two regression cases pin it.

### The intermittent CI failure, and a diagnosis that was wrong twice

- `Godot_CaptureInspectorProperty` had been failing on runs 66 and 68 with "gave up after
  90005 ms; the editor drew 5 frame(s) in that time". The first diagnosis — a slow
  software renderer — had already bought the budget a rise from 10s to 90s. It was wrong.
- `Main::iteration()` skips its draw step entirely when nothing is dirty. On a developer's
  machine a cursor blink or a compositor keeps the counter moving; on a bare Xvfb runner
  with no pointer and no window manager, nothing does. The editor was not behind, it was
  idle, and the poller was watching a counter that would never move.
- The capture pollers now call `Main::force_redraw()` on every tick while an operation is
  in flight. Measured with all four cores pinned by busy loops — the condition that
  reproduced the timeout locally — the three semantic captures answer in 0.7s, 1.7s and
  0.4s. The e2e run prints those timings pass or fail, so drift is visible as drift.
- **Lesson worth keeping: a timeout is a question about what the thing was waiting for,
  not a number to raise.**

### Next
- Build-order item 5's second half: the live-tuning workspace and variants (D3) around
  the promotion that works. Then D1/D2 propose-then-apply, then P2.

## 2026-08-27 — S-21 continued: the live tuning workspace

### Godot_OfferVariants (build-order item 5's second half, D3/D4)

- One tool with actions - offer, switch, note, keep, discard, status - rather than five
  tools. The tool surface is itself a reliability risk, and these are six moments in one
  gesture rather than six capabilities.
- Framed as the user asked: a live tuning workspace, one node and one property and a
  handful of named candidates, not general AI-generated alternatives.
- The original is captured before anything changes and is always a candidate. Comparing
  against what the game already had is the comparison that gets forgotten.
- A candidate that was never live cannot be kept, and the refusal names the tool to use
  instead. Keeping a value nobody played is editing the scene by a longer route.
- Discard restores the original *in the running game*, or says plainly it could not.
- The set holds the clock, so it measures how long each value was live. Under 500 ms is
  flagged per candidate and summarised as "a choice rather than a comparison". Reported,
  never enforced - overruling the person tuning is not the tool's job.
- `keep` writes nothing. It hands off to `Godot_PromoteRuntimeValue`, because this tool is
  `read_runtime` and promotion is `edit_scene`; one tool holding both would hold more
  authority than it declares. It warns when the game is not currently holding the value
  being kept, so promoting afterwards cannot quietly write the last thing tried.
- No checkpoint per variant, against the spec's wording. A checkpoint snapshots project
  files and a runtime property change writes none; the checkpoint belongs at promotion,
  which already takes one. The spec assumed variants were scene edits. Live tuning is not.
- Evidence: 17 doctest cases on the pure core, 8 live e2e checks driving the whole loop,
  including promoting after a keep and checking the *kept* value is what reached the scene.

### A round trip that never worked

- Building discard found a defect older than this whole tranche. `get_property` sends the
  value twice, as JSON and as Godot's text. `coerce()` took **neither** form of a
  structured type back: JSON turns a Vector2 into the string `(128, 64)`, and
  `can_convert` says - correctly, for a general string - that a String is not a Vector2.
- So the two halves of the interface disagreed. You could read a position out of a running
  game and not write the same string back in, and every round trip of a structured
  property failed silently while reporting success.
- `coerce()` now tries Godot's own parser for a string whose target is not a string type.
  `test_mcp_runtime_coerce.h` asserts the invariant over eight types: whatever the runtime
  prints, the runtime accepts back.
- Recorded rather than hidden while in there: Godot's String-to-Color conversion accepts
  anything and answers black, so a typo in a colour name sets black and reports success.
  That is the engine's behaviour and not this module's to override - refusing it would
  refuse "red" - but there is now a test that says so out loud.

### CI

- Run 69 (bug capture) and run 70 (the redraw fix) are both fully green. On run 70 the
  end-to-end step took 69 seconds on the same runner where `Godot_CaptureInspectorProperty`
  had been hitting a 90-second timeout, which confirms the diagnosis on the machine that
  produced it rather than only on this one.

### Next
- D1/D2: propose-then-apply with risk-based grouping. The user's words: "not 40 separate
  approvals".
- P2: moving the playtest's perceive/decide/act loop into the editor over MCP sampling.

## 2026-08-27 — S-21 continued: propose-then-apply (D1/D2)

### Godot_ProposeChange

- An ordered list of changes, each with a description and the exact call that would make
  it. Every one checked against the real tool and its real schema, then grouped by risk
  and shown in the editor for a decision.
- **Risk is computed, not declared.** A tool author who has to remember to mark something
  dangerous will one day forget, and the capability class plus the checkpoint declaration
  already say most of it. Three levels: mechanical (reversible and narrow), substantial
  (reversible but broad or destructive), irreversible (nothing puts it back).
- The nuance the whole feature rests on: a scene edit declares no checkpoint files because
  it lives in the undo history until the scene is saved. Reading "no files" as "nothing
  can put it back" would have misclassified the most ordinary edit there is and put forty
  renames behind forty approvals — the exact thing the user said not to build.
- Grouping: mechanical items share one key whatever they touch; substantial items are
  keyed by subject so two scenes are two decisions; irreversible items are keyed by their
  own index and can never share a checkbox.
- **It applies nothing.** The hold, the permission decision, the checkpoint and the audit
  record all live in the protocol's call path, and a tool that ran other tools would
  bypass all four. The approved calls go back to the agent and are made normally.
- Validation is schema-level and the description says so, rather than letting a green plan
  imply more than it checked: a rule a tool only applies when it runs is not in the schema.
- Evidence: 18 doctest cases, 4 live e2e checks including declining a plan and applying its
  defaults — which is the check that a default Apply cannot delete anything, since only the
  reversible group starts ticked.

### Two dialog defects, both found by trying to press the thing

The e2e could not press the dialog's buttons. Three wrong diagnoses before the right one,
so both findings are written into the code where the next person will meet them.

1. **The dialog was 1698 pixels tall on an 800-pixel screen**, with its buttons a thousand
   pixels below the bottom of the display. A `Label` with autowrap reports its minimum
   *width* as its longest word and then its minimum *height* as what the text needs at that
   width — which for a paragraph is enormous. Every wrapping label now carries an explicit
   width, and the window carries a maximum size as well as a minimum.
2. **A click on `AcceptDialog`'s own button bar never arrives** under a bare Xvfb. The
   window is where X says it is, the button is where Godot says it is, the two coordinate
   spaces agree to the pixel, and the press does not land. Buttons in the dialog *body* -
   the shape `Godot_AskUser` already uses - work. `Godot_SendEditorInput` does not reach a
   separate native window either; it is fine for the editor's main window, which is what
   the Activity dock checks use it for.

The e2e now asks the editor where its own button is with `Godot_FindControl` and clicks
there with a real pointer: coordinates from the editor, click from X. A keystroke is no
good — with no window manager, X input focus is PointerRoot and `xdotool key` follows the
pointer.

Both were diagnosed with a throwaway probe that opens the dialog and prints what X and
Godot each think is where, rather than by another guess inside a sixty-second run. That
remains the fastest way to settle a question about the screen.

### Next
- P2: moving the playtest's perceive/decide/act loop into the editor over MCP sampling.
- Then whatever the benchmarks say is weakest, which is the point of having them.

## 2026-08-27 — S-21 continued: the skills library, and a tool three skills invented

### Build-order item 6

- `test-an-input-path` and `find-a-performance-regression` added; `scene-cleanup` rewritten
  around `Godot_ProposeChange`, which is now a real tool rather than a step the skill was
  hand-rolling. All five jobs the user named have a skill.
- The regression skill is deliberately not a profiling skill. A profile of the slow version
  says where the time goes now, which is mostly where it went before; the answer needs the
  *same sequence* measured twice, so the skill is built on record-and-replay and says that
  an indeterminate replay under a profiler is an indeterminate measurement.

### Godot_SendActionInput

- Checking the skills' tool lists found `Godot_SendActionInput` named by three of them and
  implemented by none. Implemented rather than deleted, because it is the right primitive:
  a game reads an action, so a test written against the key passes until somebody rebinds
  it and then fails for a reason that has nothing to do with jumping.
- An action the project does not define is refused, with the actions it does define named.
- **The first implementation was wrong and the e2e caught it in one run.**
  `Input::action_press` only sets the singleton's internal state: `is_action_pressed`
  becomes true and `_input` is never called, so a game reading the event — most games, and
  every menu — sees nothing at all. It now injects an `InputEventAction` through the
  ordinary pipeline, which updates both. The fixture counts `is_action_pressed` from the
  event *and* the held state from `Input`, so neither half can pass alone.
- Recorded in the trace as kind `action`, which replay and retroactive capture both map.

### tools/skills/check_skills.py

- Reads tool names out of the C++ that registers them and skills out of their own front
  matter, and fails on any name nothing answers to. No build needed, so it runs in the fast
  CI job.
- Its own first run reported four real tools missing — the check was wrong, not the skills:
  two tools pick their name from a flag (`Godot_PlayMainScene`/`Godot_PlayCurrentScene`,
  `Godot_UndoLastAction`/`Godot_RedoLastAction`) and the pattern only understood a plain
  `return "X";`. It now reads the whole `get_tool_name` body.

### Also

- An e2e check pinned the scene-cleanup skill's opening sentence in order to prove the
  front matter was stripped, so improving the skill failed it. It now asserts what it
  meant to.

### Next
- P2: the playtest's perceive/decide/act loop, editor-side over MCP sampling.
