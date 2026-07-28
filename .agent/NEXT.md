# NEXT

At most five ordered actions. The first must be immediately executable.

1. Add `Godot_SetSceneProperty` (persistent, undoable, `edit_scene`) and
   `Godot_SetRuntimeProperty` (play-mode only, `read_runtime`/`run_project`), with
   the distinction stated in both names and descriptions. — T15 — verified by an e2e
   check that a runtime edit does not survive stopping the game.
3. Add the approvals UI: an editor settings section listing pending clients and
   discovered skills with allow/deny, plus command palette entries for the service
   status. — U1, U2 — verified by tests of the underlying approve/revoke calls.
4. Add `Godot_CaptureViewport`, rejecting cleanly in headless runs. — T12 — verified
   by a headless test asserting the refusal, and a manual check with a display.
5. Port the relay to Winsock behind a thin socket abstraction so R8 stops being
   blocked on Linux-only code, keeping the POSIX path unchanged. — R8 — verified by
   compiling for Windows in CI; runtime verification still needs a Windows host.
