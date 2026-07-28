# NEXT

The original specification is finished: every requirement is implemented and all but
two are verified here (the two need hardware this machine is not — see the bottom of
this file).

The interface tranche tracked in `.agent/INTERFACE_LEDGER.md` is finished: **all 41
items are VERIFIED**, including the five sub-clauses an audit found the ledger rows had
glossed over — pointer drag and scroll, touch cancellation, error stack traces, audio
stacking across a burst, and a save-corruption fixture proven against a game that has
saves. 60 tools.

Nothing in this repository is outstanding that can be done on this machine. New work
should start from `docs/godot-ai-clone-spec.md`, from
`docs/godot-ai-agent-interface-spec.md`, or from a user request — not from this file.

One wart is recorded rather than fixed, because changing it would break clients:
`Godot_WriteTextFile` takes `text` and `Godot_WriteUserFile` takes `content`. The
template's AGENTS.md now warns about it.

Two things need a machine this is not, and both are below.

## Still blocked on hardware or a remote runner

- Confirm `.github/workflows/godot_ai.yml` goes green on GitHub Actions. — C1. Every
  command in it passes locally.
- On a Windows host, run the cross-compiled relay against a Godot editor, and the same
  on macOS. — R8. Both backends compile in CI, and `platform::initialize()` is now
  actually called, which it was not before the HTTP work went in, so the Windows path
  has never run *correctly*.
