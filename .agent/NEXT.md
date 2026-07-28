# NEXT

The original specification is finished: every requirement is implemented and all but
two are verified here (the two need hardware this machine is not — see the bottom of
this file).

Current work is the interface tranche tracked in `.agent/INTERFACE_LEDGER.md`: making
the product answer everything the game-production template assumes of it. **Every item
in that ledger is now VERIFIED**, and so is the chat cancellation frame that used to sit
here as a loose end.

Nothing in this repository is outstanding that can be done on this machine. New work
should start from `docs/godot-ai-clone-spec.md`, from
`docs/godot-ai-agent-interface-spec.md`, or from a user request — not from this file.

Two things would be worth doing when a machine allows it, and both are below.

## Still blocked on hardware or a remote runner

- Confirm `.github/workflows/godot_ai.yml` goes green on GitHub Actions. — C1. Every
  command in it passes locally.
- On a Windows host, run the cross-compiled relay against a Godot editor, and the same
  on macOS. — R8. Both backends compile in CI, and `platform::initialize()` is now
  actually called, which it was not before the HTTP work went in, so the Windows path
  has never run *correctly*.
