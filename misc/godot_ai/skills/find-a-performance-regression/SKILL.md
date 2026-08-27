---
name: find-a-performance-regression
description: It used to be fast. Establish that it really is slower now, on the same sequence rather than on a different afternoon's play, and attribute the difference to a change.
enabled: true
required_editor_version: ">=4.3"
tools:
  - Godot_RecordSession
  - Godot_ReplaySession
  - Godot_ListSessions
  - Godot_ProfileWindow
  - Godot_StartProfiler
  - Godot_StopProfiler
  - Godot_GetProfilerStatus
  - Godot_GetPerformanceMetrics
  - Godot_ListCheckpoints
  - Godot_DiffCheckpoint
  - Godot_RestoreCheckpoint
  - Godot_ReadUserFile
  - Godot_PlayMainScene
  - Godot_StopPlaying
---

You are answering "why is it slower than it was". This is not the same job as profiling,
and doing it as profiling is why it usually fails: a profile of the slow version tells you
where the time goes *now*, which is mostly where it went before as well. What you need is
the **difference**, and a difference needs two measurements of the same thing.

Read the `performance-profiling` skill for how to capture and read a profile. This skill
is about what to capture it *around*.

## First: the same sequence, twice

The single most common way this investigation goes wrong is comparing two different plays.
A run where you happened to walk into the big room is slower than one where you did not,
and no amount of profiling will tell you that was the reason.

So fix the sequence:

1. `Godot_PlayMainScene`, `Godot_RecordSession` with `action: "start"`, play the part that
   feels slow, and stop the recording. Name it for the moment, not the bug —
   `wave-three-spawn`, not `slow-bug`.
2. `Godot_ReplaySession` to prove it reproduces at all. A recording that does not replay is
   not a measurement instrument, and you have found that out before spending an hour on it.

Everything below runs the *same session* against each version. If you cannot record a
sequence that reproduces the slowness, say so and stop — measuring something else and
calling it the regression is worse than reporting that you could not pin it down.

## Then: measure both versions on it

For each version — the current one, and the one that was fast:

1. Press play and `Godot_ProfileWindow` over the replayed sequence for triage. Judge on the
   **worst** frame, not the mean: a regression that adds one 40 ms hitch per wave is
   invisible in an average and is exactly what a player notices.
2. If the difference is real, `Godot_StartProfiler` / `Godot_StopProfiler` around the same
   replay and read the export with `Godot_ReadUserFile`.

Getting back to the fast version is the awkward part, and there is no single answer:

- `Godot_ListCheckpoints` and `Godot_RestoreCheckpoint` cover changes these tools made.
  `Godot_DiffCheckpoint` will tell you which files a checkpoint holds before you restore
  it, which is worth reading first.
- Changes made outside these tools are outside their reach. Say so plainly rather than
  implying the comparison covered them.

## Attribute, and be honest about what you cannot

You are looking for something that *changed*, so compare like for like:

- The same function costing more per call — something inside it got heavier.
- The same function called more often — something upstream changed how often it runs.
- A function that is new to the profile — the obvious case, and the rarest.
- Server or GPU time moved rather than script time — the cost is in what the scene now
  asks for, not in the code.
- Memory or resource count climbing across the run — a leak reads as a slow regression
  because the frame time only degrades after a while.

Then say which. "Frame time went from 8 ms to 19 ms, and `_physics_process` on Spawner
went from 40 calls at 0.05 ms to 900 calls at 0.05 ms" is an attribution. "Performance
regressed by 130%" is a number.

## The honest limits, worth saying every time

- A replay re-injects input at the recorded frame spacing. It is close enough for most
  gameplay and not close enough for anything depending on exact timing or an unseeded
  random, and `Godot_ReplaySession` will report `indeterminate` rather than `passed` when
  the run drifted too far to prove anything. An indeterminate replay under a profiler is
  an indeterminate measurement; do not read it as a result.
- A capture taken at a non-default time scale is not evidence about how the game plays.
  Leave the time scale alone during any measurement.
- Two runs on a machine doing something else are two different machines. If the numbers
  move between repeats of the *same* version, say so and repeat until they settle, or
  report that the difference is inside the noise. "Inside the noise" is a real finding and
  a much better one than a confident attribution to the wrong cause.

## What to report

The session name, the two numbers with the version each came from, the attribution, and
the profile export path. Then what you did not cover: versions you could not get back to,
sequences you could not reproduce, devices you did not measure on.
