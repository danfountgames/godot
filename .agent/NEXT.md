# NEXT

At most five ordered actions. The first must be immediately executable.

1. Read `editor/scene_tree_dock.cpp` for how the editor performs add/remove/reparent
   through `EditorUndoRedoManager`, then implement `Godot_ManageNode` the same way.
   — T5 — verified by doctest plus an e2e check that undo restores the previous tree.
2. Extend `run_editor_e2e.py` with a mutating round trip: create a node, save, reopen
   the scene, confirm it persisted, then undo. — T5, T3 — verified by scene state on
   disk before and after.
3. Add a CI workflow running the relay suite, the engine module tests and the e2e
   script on Linux. — C1 — verified by a green run from a clean checkout.
4. Implement `SKILL.md` discovery, frontmatter parsing and the allow/deny trust
   state. — S1, S2, S3 — verified by discovery tests over a fixture skill tree,
   including malformed frontmatter and version gating.
5. Implement checkpoints for mutating tools (git-backed when available, snapshot
   otherwise) and prove restoration in a test. — F8 — verified by mutating, restoring
   and comparing file contents.
