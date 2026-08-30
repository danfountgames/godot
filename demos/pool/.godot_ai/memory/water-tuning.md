---
subject: Water tuning for the pool, measured against the running game
updated: 2026-08-30T17:04:09Z
---

max_shot_speed 320   glide_damp 0.35   glide_speed 80   grab_damp 4.5

Against a 560px pool: crossing 2.73s, everything at rest 3.45s after the shot. The brief
asks for 2.5-3.0s and 5-6s.

How they were found, because the first guess was wrong by four times. Under linear
damping g a ring launched at v0 covers (v0/g)(1-e^-gt). The original 1150 px/s crossed in
0.6s; solving for 2.75s over 560px gives 320 px/s at g=0.35.

The glide threshold is the part that is easy to get wrong. At the original 220 px/s the
water grabbed after about 300px and nothing could cross the pool at all - the two targets
were in direct conflict until the threshold came down to 80.

Two defects the running game exposed and reading the files would not have:
- Rings had no PhysicsMaterial, so bounce was 0 and friction 1. A ring at rest was a
  frozen body, a frozen body is static, and a head-on hit stopped dead - the per-frame
  speed trace showed 534 to 23 px/s in a single frame.
- settle_time measured the striker being returned to the shooting edge rather than the
  shot, because the return un-settles it and restarts the motion timer. It read a flat
  0.13s however violent the shot was.

Measure a shot from inside the game. Every property read from outside is its own round
trip, so three samples of a 0.6s shot arrive after it is already over.
