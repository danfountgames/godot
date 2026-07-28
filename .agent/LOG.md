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

### Next
- S-05: skills (S1–S3, D3), then checkpoints (F8), then `Godot_ReadOutputLog` and the
  runtime/persistent property split (T9, T15), screenshots (T12) and ask-user (T13).
