---
name: playtest-a-goal
description: Play the running game towards a stated goal and produce a report that says whether it was reached, backed by what was actually pressed and what the game logged.
enabled: true
required_editor_version: ">=4.3"
tools:
  - Godot_StartPlaytest
  - Godot_NotePlaytestObservation
  - Godot_FinishPlaytest
  - Godot_GetRuntimeSceneTree
  - Godot_GetRuntimeProperty
  - Godot_WaitForRuntimeCondition
  - Godot_SendActionInput
  - Godot_SendKeyInput
  - Godot_SendPointerInput
  - Godot_CaptureGame
---

You are playing a Godot game towards one stated goal, and the thing you produce is a
report somebody else will act on.

**Before anything else, state the goal and how you will know it was reached.** "Reach
the second room" is a goal; "test the game" is not. The condition you will check is the
oracle, and it goes in the report so a reader can judge your verdict instead of
believing it.

The game must already be running. If it is not, say so and stop — `Godot_StartPlaytest`
will refuse, and it is right to.

1. `Godot_StartPlaytest` with the goal, a budget in seconds, and the oracle.
2. Look before you act: `Godot_GetRuntimeSceneTree` to see what exists,
   `Godot_GetRuntimeProperty` to read the specific things your oracle depends on.
3. Act with `Godot_SendActionInput` where the project defines actions — it survives a
   remapped key, which `Godot_SendKeyInput` does not. Use keys and the pointer when
   there is no action for what you need.
4. **Wait on conditions, never on time.** `Godot_WaitForRuntimeCondition` waits until
   the game is actually in the state you need. A fixed sleep passes on a fast machine
   and fails on a loaded one, and it hides the difference between slow and broken.
5. Record what you notice as you go with `Godot_NotePlaytestObservation` — especially
   anything surprising, and anything that blocked you. These are your account; keep them
   short and factual.
6. `Godot_CaptureGame` when a picture says something words do not: a wrong layout, a
   character stuck in geometry, an effect that did not fire.
7. `Godot_FinishPlaytest` with your verdict and a summary.

## About the verdict

Use `reached` only if the oracle you stated actually held, and say in the summary how
you checked it. Use `not_reached` when you played properly and did not get there. Use
`blocked` when something stopped the attempt. Use `stop` to end early and keep partial
results.

The report reconciles your verdict against the evidence and will overrule you:

- Claiming `reached` when no input was injected comes back **indeterminate**. If you
  find yourself about to do this, you have concluded from reading the project rather
  than from playing it — which is a legitimate thing to report, but say so, and use
  `blocked` or `indeterminate`.
- Claiming `reached` while the game logged errors comes back **indeterminate**, with the
  count. Investigate the errors before deciding; reaching a goal past an error is not
  the same as reaching it.
- `not_reached` after running out of budget comes back **indeterminate**, because it says
  only that there was not enough time.

None of that is a punishment. Your claim is kept in the report beside the conclusion, so
a reader sees both.

## What makes a report worth reading

The counts and the verdict are read first. The observations are read next, and they are
the only part in your own words — so put the thing a person needs to know in them, not
in a narration of every button you pressed. The list of what you pressed is collected
for you.
