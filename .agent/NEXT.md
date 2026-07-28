# NEXT

The original specification is finished: every requirement is implemented and all but
two are verified here (the two need hardware this machine is not — see the bottom of
this file).

Current work is the interface tranche tracked in `.agent/INTERFACE_LEDGER.md`: making
the product answer everything the game-production template assumes of it. **Every item
in that ledger is now VERIFIED.** One loose end remains, and it is a missing test rather
than a missing capability.

1. **The chat cancellation frame.** Cancel a turn in flight from the dock and assert
   the client is told to stop. The conversation's half is unit-tested; the
   `notifications/cancelled` frame is not. This used to be recorded as needing something
   external, and it does not: `Godot_FindControl` locates the dock's Cancel button and
   `Godot_SendEditorInput` presses it. It is a check nobody has written.

## Still blocked on hardware or a remote runner

- Confirm `.github/workflows/godot_ai.yml` goes green on GitHub Actions. — C1. Every
  command in it passes locally.
- On a Windows host, run the cross-compiled relay against a Godot editor, and the same
  on macOS. — R8. Both backends compile in CI, and `platform::initialize()` is now
  actually called, which it was not before the HTTP work went in, so the Windows path
  has never run *correctly*.
