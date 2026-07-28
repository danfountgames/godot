# NEXT

The original specification is finished: every requirement is implemented and all but
two are verified here (the two need hardware this machine is not — see the bottom of
this file).

Current work is the interface tranche tracked in `.agent/INTERFACE_LEDGER.md`: making
the product answer everything the game-production template assumes of it. Every tool named in that ledger exists, and the chat cancellation frame that used to sit
here as a loose end is covered. What is left is narrower than a whole capability: five
sub-clauses of the interface spec that the ledger rows had glossed over, found by
re-reading the spec's own wording against what was built.

1. **`Godot_SendPointerInput` has no drag and no scroll.** The spec line reads "move,
   press, release, click, drag, scroll". A drag is a press, interpolated motion and a
   release in one call — a game that reads `InputEventMouseMotion` deltas cannot be
   driven without it, which rules out sliders, camera drags and swipes. Scroll needs
   `MouseButton::WHEEL_UP`/`WHEEL_DOWN`. The touch tool already has drag; the pointer
   one does not.
2. **`Godot_SendTouchInput` cannot cancel a touch.** `InputEventScreenTouch::set_canceled()`
   exists in this engine. A touch the OS takes away — a notification, an incoming call —
   is a state a mobile game has to survive and cannot currently be put into.
3. **`Godot_GetRuntimeErrors` has no stack traces.** It keeps the call *site*; the spec
   asks for the call *stack*. `ScriptLanguage::debug_get_stack_level_*` is where the
   rest would come from.
4. **Stacking detection is instantaneous.** The spec says "across an input burst": a
   sound that stacks and clears between two calls is still invisible. Needs sampling
   over a window, like `Godot_ProfileWindow` does for frame time.
5. **G3 is IMPLEMENTED, not VERIFIED.** `Godot_WriteUserFile` is the mechanism for
   writing a deliberately malformed save, but no game in this repository has a save
   system to test a recovery path against. Either build a fixture game with one, or
   leave it honestly unproven.

Two further things need a machine this is not, and both are below.

## Still blocked on hardware or a remote runner

- Confirm `.github/workflows/godot_ai.yml` goes green on GitHub Actions. — C1. Every
  command in it passes locally.
- On a Windows host, run the cross-compiled relay against a Godot editor, and the same
  on macOS. — R8. Both backends compile in CI, and `platform::initialize()` is now
  actually called, which it was not before the HTTP work went in, so the Windows path
  has never run *correctly*.
