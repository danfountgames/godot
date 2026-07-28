# NEXT

The original specification is finished: every requirement is implemented and all but
two are verified here (the two need hardware this machine is not — see the bottom of
this file).

Current work is the interface tranche tracked in `.agent/INTERFACE_LEDGER.md`: making
the product answer everything the game-production template assumes of it. Five items
remain. Take them in this order — each is a whole slice, ending in a build, the full
sweep, a ledger update, and a push.

1. **F1–F3 — scene tests.** `Godot_ListSceneTests`, `Godot_RunSceneTest`, and
   structured per-case results. The template tells agents to run tests from the shell
   because the interface has no runner; this closes that. Per-case results matter more
   than the runner: "3 failed" is not actionable, a named case with a message is.
2. **E2 — duplicate/stacking detection.** Whether the same sound is playing several
   times over itself. `Godot_GetAudioState` reports bus peaks, which cannot tell one
   loud playback from four stacked ones — and stacking is the audio bug an agent
   working without ears is most likely to ship.
3. **The chat cancellation frame.** Cancel a turn in flight from the dock and assert
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
