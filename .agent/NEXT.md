# NEXT

At most five ordered actions. The first must be immediately executable.

1. Launch a headless editor on a scratch project with `GODOT_AI_HOME` and
   `GODOT_AI_AUTO_APPROVE=1` and confirm `<home>/instances/<pid>.json` appears with
   the listening port. — F2 — verified by the descriptor existing and naming a
   reachable port.
2. Drive `initialize` → `tools/list` → `tools/call Godot_GetEditorStatus` →
   `tools/call Godot_ListScenes` through `bin/godot-ai-relay`, saving the transcript
   to `.agent/evidence/e2e-transcript.jsonl`. — F3, P1, P2, P3, T1 — verified by a
   non-error result for each call.
3. Add `tools/relay/tests/run_editor_e2e.py` so that flow is repeatable in CI, and
   wire both suites into a CI workflow. — C1 — verified by the script passing from a
   clean checkout.
4. Implement `Godot_ManageNode` (create/delete/reparent/rename) through
   `EditorUndoRedoManager`, with undo/redo tests. — T5 — verified by scene state
   before/after undo.
5. Implement `SKILL.md` discovery and the allow/deny trust state. — S1, S2, S3 —
   verified by discovery tests over a fixture skill tree.
