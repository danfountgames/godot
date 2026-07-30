# PLAYTEST LOG

Concise records of meaningful play sessions. No large raw logs.

Each entry: build or commit, goal, tester identity, whether the tester had source
knowledge, launch state, resolution and input method, exact interaction route,
expected outcome, observed outcome, failures, subjective friction, evidence,
follow-up.

Every black-box entry must additionally record:

- **save state at launch** — deleted, or carried over from a previous session. A
  "first-time player" test on a device holding somebody else's progress is a returning-player
  test, whatever it says at the top.
- **affordances confirmed firing beforehand**, from the trace log rather than from the scene
  tree. A control that exists, is visible and does nothing is indistinguishable from a working
  one until something says it fired.

Both fields exist because a pass was once spent discovering that the feature it was meant to
judge had been silently switched off.
