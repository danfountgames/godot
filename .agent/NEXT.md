# NEXT

The first two specifications are finished. The third has just opened.

- `docs/godot-ai-clone-spec.md` — 53 of 55 requirements verified. The two that are not
  need hardware this machine is not (see the bottom of this file).
- `docs/godot-ai-agent-interface-spec.md` — all 41 items verified.
- `docs/godot-ai-agent-experience-spec.md` — **new**. Tracked in
  `.agent/EXPERIENCE_LEDGER.md`. Nothing in it is started.

## The immediate next action

**Q1 — build the engine on 4.8 and re-establish the editor-side baseline.**

Commit `117870273` merged Godot 4.8 development into this fork and moved
`modules/godot_ai/` onto the nested editor layout. Nothing has linked it since. Every
editor-side claim in `SPEC_LEDGER.md` and `INTERFACE_LEDGER.md` was verified against
4.3 and is currently unproven on this tree.

```sh
scons platform=linuxbsd target=editor dev_build=yes debug_symbols=no \
      scu_build=yes tests=yes -j$(nproc)
cd /tmp && godot --headless --test --test-case="*[godot_ai]*"   # module suite
python3 tools/relay/tests/run_editor_e2e.py                      # whole stack
```

Do this before touching any code below. A 4.8 API break in the module is both the most
likely problem on this tree and the cheapest one to find.

## Then, in order

The sequencing argument is in the spec's *Sequencing* section and is not repeated here.
Short version: **E before P before D, with S alongside P.**

1. **E1 + E7 — the activity stream and its tool.** No UI yet. The service publishes a
   live record per tool call; `Godot_GetActivity` reads it back. Doing the tool first
   means the dock has something regression-testable underneath it, which rule 1 of the
   ledger's verification rules requires anyway.
2. **E2 + E3 + E4 — the Activity dock.** The single biggest gap in the category; no
   competitor has it. Verify by pressing its controls under a virtual display, not by
   unit-testing the methods behind them.
3. **S1–S8 — session record and replay.** The capability an authoring-only integration
   cannot copy. S6 (honest reporting of non-determinism) is the requirement to protect;
   it is the one that will be tempting to drop to make S4 look finished.
4. **P1–P5 — playtest sessions.** Depends on E1 for P5.
5. **D1–D4 — propose-then-apply and variants.**

Q2 through Q5 are independent of all of the above and can be taken whenever the main
line is blocked.

## Still blocked on hardware or a remote runner

- **C1** — confirm `.github/workflows/godot_ai.yml` goes green on GitHub Actions. Every
  command in it passes locally. Cheap to settle: push and watch one run.
- **R8** — on a Windows host, run the cross-compiled relay against a Godot editor. The
  POSIX backend passes all 64 relay tests on Linux and native macOS arm64. The Windows
  backend has never executed.

## One wart recorded rather than fixed

`Godot_WriteTextFile` takes `text`; `Godot_WriteUserFile` takes `content`. Changing
either would break clients. Q3 is the fix that does not: accept both, warn on the old
one. The template's AGENTS.md warns about it until then.
