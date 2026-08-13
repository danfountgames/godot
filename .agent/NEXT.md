# NEXT

The original specification is finished: every requirement is implemented and all but
two are verified here (the two need hardware this machine is not — see the bottom of
this file).

The interface tranche tracked in `.agent/INTERFACE_LEDGER.md` is finished: **all 41
items are VERIFIED**, including the five sub-clauses an audit found the ledger rows had
glossed over — pointer drag and scroll, touch cancellation, error stack traces, audio
stacking across a burst, and a save-corruption fixture proven against a game that has
saves. 63 tools after the profiler capture slice.

Nothing in this repository is outstanding that can be done on this machine. New work
should start from `docs/godot-ai-clone-spec.md`, from
`docs/godot-ai-agent-interface-spec.md`, or from a user request — not from this file.

One wart is recorded rather than fixed, because changing it would break clients:
`Godot_WriteTextFile` takes `text` and `Godot_WriteUserFile` takes `content`. The
template's AGENTS.md now warns about it.

Two things still need an external environment, and both are below.

## Still blocked on hardware or a remote runner

- Confirm `.github/workflows/godot_ai.yml` goes green on GitHub Actions. — C1. Every
  command in it passes locally.
- On a Windows host, run the cross-compiled relay against a Godot editor. — R8. The
  POSIX backend now passes all 64 relay tests on native macOS as well as Linux, and a
  native arm64 `GodotAI.app` build passes its bundled GodotAI test suite. The Windows
  backend still has never run *correctly*; it only cross-compiles in CI.
