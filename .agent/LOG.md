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

### Next
- S-07: `Godot_ReadOutputLog` and the runtime/persistent property split (T9, T15),
  the approvals UI (U1, U2), screenshots (T12), ask-user (T13), and the Winsock port
  that would unblock R8.
