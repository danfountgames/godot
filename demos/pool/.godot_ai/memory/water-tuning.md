---
subject: Water tuning for the pool, measured against the running game
updated: 2026-08-30T17:04:09Z
---

Read the numbers off `scripts/main.gd`. They are exported on `PoolGame`, and this note is
not the place they live.

**A note that stores a tuned constant will rot.** An earlier version of this one quoted
`max_shot_speed 320`; the scene now ships 380, and an agent that trusted the note over the
file reported a settle time 1.6s too low and a range 25% short. The note was *persuasively*
written, with the derivation and two war stories attached, which made it more dangerous
rather than less. Store where to read a value and why it is what it is - never the value.

## Why the water is shaped the way it is

Under linear damping g, a ring launched at v0 covers (v0/g)(1 - e^-gt). That one line is
worth more than four experiments: the first guess of 1150 px/s crossed the pool in 0.6s,
and solving for the brief's 2.75s over 560px gives about 320.

The glide threshold is the part that is easy to get wrong. At 220 px/s the water grabbed
after about 300px and nothing could cross the pool at all - the crossing target and the
settling target were in direct conflict until it came down to 80.

The merge threshold is counter-intuitive and was found by measuring, not reasoning.
Lowering it makes chains *shorter*: a slow graze merges, so the active ring is heavy before
it is through the rack. Chains went 4,3,3 at fifty and 1,3,2 at thirty-five.

## What the running game showed that the files did not

- Rings had no PhysicsMaterial, so bounce was 0 and friction 1. A head-on hit stopped
  dead: the per-frame speed trace showed 534 to 23 px/s in a single frame.
- The merge rule read `linear_velocity` inside a contact callback, which is not the
  approach speed - `body_entered` fires around the solver, so the two bodies in one
  contact disagreed. Measured at 203.9 px/s from one and 66.1 from the other against a
  threshold of 90, and the central rule of the game silently never fired.
- `settle_time` measured the striker being returned to the shooting edge rather than the
  shot, and read a flat 0.13s however violent the shot was.

## How to measure any of it again

`Godot_RecordRuntimeSeries` on the active ring, `component: length`, `clock: physics`.
Do not sample from outside: every read is a round trip, so three samples of a shot arrive
after it is over. The `measure-a-feel-target` skill is this loop written down.
