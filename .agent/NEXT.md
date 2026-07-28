# NEXT

At most five ordered actions. The first must be immediately executable.

1. Implement checkpoints: before any mutating tool runs, snapshot the files it is
   about to touch under `$GODOT_AI_HOME/checkpoints/<project>/<id>/`, with a manifest
   recording tool, arguments summary and file hashes. — F8 — verified by a test that
   mutates, restores, and compares file contents byte for byte.
2. Expose `Godot_ListCheckpoints` and `Godot_RestoreCheckpoint`, and wire creation
   into `MCPProtocol::_handle_tools_call` so no mutating tool can bypass it.
   — F8 — verified by an e2e round trip: write file, restore, confirm old content.
3. Add `Godot_ReadOutputLog` backed by the editor log. — T9 — verified by an e2e
   check that a message printed by the editor appears in the tool's output.
4. Add `Godot_SetSceneProperty` and `Godot_SetRuntimeProperty` so persistent and
   play-mode edits stay distinguishable. — T15 — verified by an e2e check that the
   runtime edit does not survive stopping the game.
5. Add the approvals UI: a settings section listing pending clients and discovered
   skills with allow/deny, plus command palette entries. — U1, U2 — verified by
   manual inspection plus a test of the underlying approve/revoke calls.
