---
name: judge-a-design
description: Answer a design question about a game that already runs - does the pacing hold up, is this verb worth using, is the choice a real choice - with numbers you measured yourself rather than a reading of the code.
enabled: true
required_editor_version: ">=4.3"
tools:
  - Godot_PlayMainScene
  - Godot_StopPlaying
  - Godot_GetRuntimeSceneTree
  - Godot_FindRuntimeNodes
  - Godot_GetRuntimeProperty
  - Godot_SetRuntimeProperty
  - Godot_PauseRuntime
  - Godot_StepRuntimeFrames
  - Godot_RecordRuntimeSeries
  - Godot_WaitForRuntimeCondition
  - Godot_CaptureGame
  - Godot_RecallProjectMemory
  - Godot_UpdateProjectMemory
  - Godot_SetIntent
---

You have been asked a question about how a game *plays*: whether a level's pacing holds
up over a session, whether one of two verbs is simply better, whether the resource the
design says is contested is actually contested. This is not the same job as fixing a bug,
and the failure modes are different: nothing crashes, nothing is red, and a wrong answer
looks exactly like a right one.

The deliverable is a verdict with numbers under it that somebody else could reproduce.

## Do not read the code for the answer

The most expensive mistake here, and it is a comfortable one. A game's own documentation
states what its mechanics do, its variables are named after what they were meant to be,
and both are frequently wrong in ways only playing reveals.

Worked example from a physics-heavy prototype. Its brief described a pull/push trade —
pull gathers reward and brings danger, push buys safety and sends reward away. Every line
of that was false in the build. Pull had no danger, because the paddle collided with
nothing, so a hazard dragged towards the player passed through them and out of the level,
where it was deleted *and counted as cleared*: pull was a free clear button. Push had no
upside, because objects shouldered each other all day and nothing ever damaged anything.
And the reward push was supposed to sweep away had no drift at all, so it sat motionless
for ever. Three claims, all documented, none true, none visible in a code read.

**Use each verb, alone, and measure what the board looked like before and after.** That is
what found all three.

## 1. Say what would change your mind

Write the question as a number before you collect any. "Is pull worth holding" becomes
"how many rewards are collected per level with the current held, versus not". That
comparison had no overlap — 0 to 3 without, 99 to 112 with — and no amount of prose about
how pull *feels* would have been as useful.

`Godot_SetIntent` with the question. `Godot_RecallProjectMemory` first: someone may have
measured this, and a stale answer is worth knowing about before you contradict it.

## 2. Drive by property, not by pixel

Games built for this expose their verbs as properties as well as inputs, and a property is
a far better unit of intent than a drag: `Godot_SetRuntimeProperty` on `paddle_x` is a
statement about where the player wants the paddle, which is the thing you actually mean.
`Godot_GetRuntimeSceneTree` and `Godot_FindRuntimeNodes` find what is there.

Batch your reads. One property read is a round trip; a control loop needs several a
second, so send them together and read the replies together rather than in series.

**Pause whenever the answer has to be exact.** A running game moves tens of frames
between two of your calls, so a screenshot, a property and a scene tree fetched in three
calls describe three different moments, and a value you set is read back after the world
has moved on. `Godot_PauseRuntime` stops it; `Godot_StepRuntimeFrames` advances a known
number of frames and stops it again. That turns "set this, then something happened" into
"set this, step one frame, and exactly this happened" — which is the difference between a
correlation and a cause. Two agents lost a measurement each for want of it before it
existed.

Do not use `Godot_SetTimeScale` for this. A zero scale is not a pause, it is a game
running with a zero delta, and any raised scale changes the physics rather than merely
the pace.

## 3. Ask the game what state it is in. Never keep your own copy

The single most expensive bug available in this work, because it produces data rather than
an error.

A measurement harness tracked "is the ball live" with a local flag: set it on
launch, clear it when the ball is seen below the play area. Its sample loop stepped over
the frame the old ball vanished on, so the flag stayed set, so it never launched the next
one — and the level sat untouched. That produced two 200-second "stalls" with 96% dead
time, in data that looked entirely plausible, and one of them was very nearly *fixed in
the game* by changing how the ball moves to break an orbit that was never happening.

If the game publishes the state, read the game's. If it does not, that is a finding: say
so, and ask for the readout rather than reconstructing it.

## 4. Play badly on purpose

A controller that predicts perfectly tells you nothing about difficulty. This one lost
zero lives across six levels and the honest conclusion looked like "the loss condition is
decoration". It was not: give the controller the two things a person has — a reaction
delay before the hand follows the eye, and an error in where they think the object is
going — and the same build goes from 0 lives lost per level, to 2, to failing every level
in under a minute.

| player | reaction | aim error | lives lost per level |
|---|---|---|---|
| bot | none | none | 0 |
| ok | 0.30s | 45px | 2.0 |
| poor | 0.50s | 80px | 3.0 |

Pick the error fresh per commitment, not per frame: a person decides where something is
going and then moves, and jitter re-rolled at 9Hz averages out to no error at all.

**Report which player produced each number.** A pacing figure from a perfect bot and one
from a poor player are different measurements, and mixing them is how a design gets tuned
for nobody.

## 5. Measure the gaps, not the totals

A level's problem is almost never its length. It is how much of that length contains
nothing.

Count the time since anything last scored, and report the fraction of the level spent
above a few seconds of that. In one measured level that number was **71%** — a level whose
median length was 161 seconds contained the same 48 to 56 seconds of action however long
it ran — and it is the finding that halved the level and changed the design. The total
length alone said "one to three minutes, as specified" and hid all of it.

Also worth having, and cheap: how long a resource sits at its cap (a meter full 95% of the
time is not a resource), and how long each quarter of the level took (a last quarter 5.7
times the first is a tail, not a climax).

## 6. Check the instrument before you believe the result

When a measurement confirms a fix, that is the moment to be suspicious, not satisfied.

Ask of every surprising number: could my harness produce this on its own? The 200-second
stall above had the answer sitting in the data — the game's own "seconds since anything
happened" was frozen, and it only advances while the ball is in play, so nothing was in
play. A stall in the game and a harness that stopped playing look identical in a summary
and are completely different findings.

Two habits that catch it:

- **Run the null experiment.** Before concluding a change fixed something, run the same
  measurement with the change reverted. Five levels with the guard *off* produced no
  stalls either, which is what showed the guard had never been the reason.
- **Name what each metric actually computes.** A "how shallow was the trajectory" figure
  computed from positions sampled at 9Hz is measuring chords across several bounces, not
  the trajectory, and it will happily report the opposite of the truth.

## 7. Write the verdict so it can be argued with

Lead with the answer in one sentence, then the numbers, then what you would change in
priority order. For each recommendation give the value and the evidence, so the reader can
disagree with the reasoning rather than the conclusion.

State your sample size and your controller honestly. "Median of three levels, same
controller, only the current varied" is a result. "It felt better" is not.

`Godot_UpdateProjectMemory` with what you measured and how, because the next person to ask
this question should start from your numbers.

## One caution about acting on someone else's report

A report can be right about its measurement and stale about its cause: the thing it
diagnosed may already have been fixed while it was being written. Check each finding
against the tree before you act on it, and re-measure rather than assuming the number
still holds.
