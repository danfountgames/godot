---
name: measure-a-feel-target
description: Turn a stated feel target - "the shot should cross in about three seconds", "everything settles within five" - into a number read off the running game, then tune until it holds.
enabled: true
required_editor_version: ">=4.3"
tools:
  - Godot_RecordRuntimeSeries
  - Godot_FindRuntimeNodes
  - Godot_GetRuntimeSceneTree
  - Godot_GetRuntimeProperty
  - Godot_SetRuntimeProperty
  - Godot_PromoteRuntimeValue
  - Godot_SaveScene
  - Godot_PlayMainScene
  - Godot_StopPlaying
  - Godot_RecallProjectMemory
  - Godot_UpdateProjectMemory
  - Godot_SetIntent
---

A design brief states feel as numbers: *a full-power shot crosses the pool in about two
and a half to three seconds*, *everything is at rest within five or six seconds of the
last collision*, *the jump peaks at two and a half tiles*. None of them is visible in a
file. All of them are readable off a running game in about a minute, and the loop that
does it is the same every time.

This is the sibling of `tune-and-keep`. That one is for a value only a person can judge;
this one is for a value with a stated target, where the answer is a measurement and the
argument is over.

## Start by recalling

`Godot_RecallProjectMemory`. Someone may already have measured this, and the note will
say what the numbers mean and how they were found. `Godot_SetIntent` with the goal, so
the checkpoints from here belong to one task.

## 1. Do not sample from outside. Record from inside.

The single most expensive mistake available here. Every `Godot_GetRuntimeProperty` is a
round trip, so a handful of them span seconds; the thing being watched is usually over in
less, and each read lands on a different frame from the one before. Three samples of a
0.6-second shot produce a table that looks exactly like a physics bug and is an artefact
of the sampling.

`Godot_RecordRuntimeSeries` is the tool. Name the node, the property, the frame count and
the clock; the game records at frame rate and the whole window comes back in one reply.

```
Godot_RecordRuntimeSeries
  path=/root/Main/Player property=velocity component=length frames=180 clock=physics
```

- **`clock=physics`** for anything a body does — a shot, a jump arc, a collision chain.
  `process` for anything visual: a tween, a fade, a camera move.
- **`component`** picks one number out of a vector. Without it you get a wall of
  `"(-117.19, -359.15)"`, and "how fast is it going" is usually the question.
- **Start the recording, then do the thing.** The call answers when the window is full,
  so the input goes in while it is recording.
- **`missing`** counts frames where the node or property could not be read. A non-zero
  count on a node you expected to exist is itself the finding.

If the node was spawned by a script, it has no stable name — `@RigidBody2D@270` changes
every run. `Godot_FindRuntimeNodes` addresses it by class, by a fragment of its name, or
by position.

## 2. Read the shape, not just the endpoints

A series is worth more than the number the target asks for, because the shape says *why*
the number is what it is. From a real one:

```
380 378 376 ... 310 308     a clean exponential: this is drag doing its job
308 -> 47                   one frame: a collision, and a lossy one
47 164                      and then a merge put the speed back up
100  99  98 ...  81         a long flat tail: something is barely moving for a while
 72  67  62  57  53 ... 5   a sharp knee: the second drag regime taking hold
```

Every one of those was a design question answered. Look for the knee, the one-frame
cliff, and the flat tail before you touch a value.

## 3. Change the value in the running game, not in the file

Anything the game exposes as an exported property can be set with
`Godot_SetRuntimeProperty` while it runs. Prefer a game that also exposes a way to
re-run the thing being measured — a `restart_requested` flag, a respawn, a reset — so a
candidate can be tried without relaunching. Three or four candidates measured this way
take about ninety seconds; the same work by editing and relaunching takes twenty minutes
and you will try fewer.

Try candidates as a **set**, and record all of them, not just the winner. A real run:

| candidate | chains |
|---|---|
| as built | 1, 1, 2 |
| lighter field | 2, 1, 1 |
| lighter, lower threshold | 2, 1, 2 |
| lighter, lower, faster | 3, 2, 2 |

and then, pushing the same direction further, the surprise:

| threshold 50 | 4, 3, 3 |
| threshold 35 | 1, 3, 2 |

Lowering it further made it **worse**. Only a set shows that; a single change in the
direction that helped last time hides it, and this one happened to answer a question the
brief had listed as open.

## 4. Solve for the number when you can

Guessing costs a cycle each time. Some feel targets have arithmetic behind them, and one
line of it beats four experiments. Under linear damping `g`, something launched at `v0`
covers `(v0/g)(1 - e^-gt)`. A 560px pool crossed in 2.75s therefore wants about 320 px/s
at `g = 0.35`, and the first guess of 1150 crossed in 0.6.

Then measure, because the arithmetic ignores collisions, thresholds and everything else
in the scene. It gets you to the right order of magnitude in one step, not to the answer.

## 5. Watch for targets that fight each other

Two stated numbers can be jointly impossible with the mechanism you have, and that is a
finding to report rather than a value to keep hunting for. Measured once: a hard drag
threshold at 220 px/s meant the water grabbed after 300px, so *nothing could cross a
560px pool at all* — the crossing target and the settling target were in direct conflict
until the threshold came down. Say so plainly when it happens; the fix is a design
decision, not a number.

## 6. Keep the value, and keep why

`Godot_PromoteRuntimeValue` writes the tuned value from the running game into the edited
scene, then `Godot_SaveScene`. A value that only exists in a process that is about to
exit is not a result.

Then `Godot_UpdateProjectMemory`, and record **how it was found**, not just what it is.
The number will be questioned; the derivation is what answers that. Write down the
measurement, the arithmetic if there was any, the candidates that lost, and any target
that turned out to fight another one.

## What not to do

- **Do not raise the time scale to measure something physical.** Godot multiplies the
  physics delta by it, so a scene at 5x is a coarser scene, not a faster one. Bodies
  travel further per step and land differently. Every number you take is then about a
  simulation nobody will play.
- **Do not report a number without the run it came from.** "About three seconds" is an
  impression. `crossing_time 2.73` with the series behind it is a measurement.
- **Do not tune until it passes and stop there.** Check the shape again afterwards: a
  value that hits the target by making the motion wrong is easy to reach and obvious in
  the series.
