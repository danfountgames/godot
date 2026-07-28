# NEXT

The original specification is finished: every requirement is implemented and all but
two are verified here (the two need hardware this machine is not — see the bottom of
this file).

Current work is the interface tranche tracked in `.agent/INTERFACE_LEDGER.md`: making
the product answer everything the game-production template assumes of it. Six items
remain. Take them in this order — each is a whole slice, ending in a build, the full
sweep, a ledger update, and a push.

1. **I1 — `Godot_SendEditorInput`.** Editor-side input, deliberately separate from the
   game-side tools in name, capability and documentation, exactly as the runtime and
   persistent edit tools are kept apart. The two halves it needs already exist:
   `Godot_ListWindows` says what is open and `Godot_FindControl` says where it is, so
   this is the piece that acts. Keep `simulate_input` as the capability class, and mind
   the coordinate rule `Godot_FindControl` had to learn — a window that embeds its
   subwindows reports its contents in *its own* client coordinates.
2. **B4 — capture metadata on every image.** Every tool that returns an image should
   say what it is a picture of: which window or viewport, at what size, at what game
   time or frame. A screenshot with no provenance is evidence of nothing, and the
   playtest loop in the template pins conclusions to captures.
3. **F1–F3 — scene tests.** `Godot_ListSceneTests`, `Godot_RunSceneTest`, and
   structured per-case results. The template tells agents to run tests from the shell
   because the interface has no runner; this closes that. Per-case results matter more
   than the runner: "3 failed" is not actionable, a named case with a message is.
4. **E2 — duplicate/stacking detection.** Whether the same sound is playing several
   times over itself. `Godot_GetAudioState` reports bus peaks, which cannot tell one
   loud playback from four stacked ones — and stacking is the audio bug an agent
   working without ears is most likely to ship.

## Still blocked on hardware or a remote runner

- Confirm `.github/workflows/godot_ai.yml` goes green on GitHub Actions. — C1. Every
  command in it passes locally.
- On a Windows host, run the cross-compiled relay against a Godot editor, and the same
  on macOS. — R8. Both backends compile in CI, and `platform::initialize()` is now
  actually called, which it was not before the HTTP work went in, so the Windows path
  has never run *correctly*.
- Catch `notifications/cancelled` end to end: cancel a chat turn in flight from the
  dock and assert the client is told to stop. The conversation's half is unit-tested;
  the frame is not. Needs the Cancel button located on screen the way
  `click_first_action()` finds the approvals dialog's buttons, which `Godot_FindControl`
  now makes straightforward.
