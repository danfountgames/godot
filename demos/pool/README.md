# POOL

A breakout game played with rubber rings, where you control the pool's current to bend
shots, vacuum up Likes, launch floating targets into one another and thread impossible
gaps through giant inflatables.

Move the inflatable lounger along the near edge. Rebound the striker into the rings.
Hold **pull** to curve it back towards you, hoover up Likes and drag the loose floats
down at yourself; hold **push** to bend it away, open a formation and keep the dangerous
ones off. Clear every target before you run out of strikers. The meter you fill from
Likes buys either a splash barrier or a Party Wave, and it will not buy both.

## The two rules that carry it

**The striker does not obey the water.** Everything else in this pool drifts, slows and
settles, because a pool full of clutter is the pleasure of the setting. The ball holds
its speed exactly; the current only *steers* it. A breakout ball with drag is a ball that
stops halfway up the board, and the game dies with it.

**The rings have real holes, and you can shoot through them.** A small striker passing
clean through the middle of a big ring is a **thread**: no damage, no bounce, and the
multiplier goes up. Clip the rim instead and you bounce off it and break it. Threading is
the only purely skilful thing in the game, so it is the only thing that builds the
multiplier.

Getting that second rule right in two dimensions needed a rebuild, and the reason is the
most interesting thing in the code. The first version built each rim from twelve circle
colliders with a genuinely empty middle and expected threading to fall out of ordinary
physics. It cannot: **a 2D annulus encloses its own hole**, so there is no direction a
ball can arrive from that reaches the middle without crossing the rim. A full board
produced twenty-eight rim hits and zero threads, and it would never have produced one.

What the picture actually means is depth. The rings are inflatables floating on the
surface and the striker is small and rides low, so a shot through the middle passes
*under* the rim. The rim is therefore not a collider against the striker at all: the ring
is a sensor, and the pass is judged on the striker's line — miss the centre by less than
the hole and you are through it. Loose rings still collide with each other and the walls
as ordinary bodies, which was never the part that was broken.

That rebuild was then decorative for a while, which is worth knowing about. Godot pairs
two bodies when *either* one's mask contains the other's layer, and the targets were
sitting on the same layer as the walls — so the solver went on bouncing the striker off
every rim no matter what the sensor decided. A thread scored, raised the multiplier, and
then the ball clipped anyway. Moving the rings to their own layer is what made the rule
the game is named after actually true.

## The tail, and the drain

A breakout board's last three rings are its worst minutes: they are scattered, they are
behind you, and hunting them is not the game you were just playing. Measured over twenty
boards, the median board ran 161 seconds and **71% of all board time contained no scoring
event at all**; the final quarter took 5.7 times the first, and one board went 210
consecutive seconds with nothing happening.

So the pool drains. Once a board is down to its last few rings and nothing has been hit
for eight seconds, the survivors are cut loose from their anchors and dragged towards the
middle where the striker can reach them — and if the quiet goes on, the drain takes them
and the board ends, at full value, because they were survived rather than skipped. It
arms only in the tail, so it can never be waited out for a free clear of a full board.

Measured after: median board **81 seconds**, dead time **16%**, and the drain finishes
every board it opens on.

## The pull/push trade

Neither is the safe choice, which is the point:

| | gets you | costs you |
|---|---|---|
| **pull** | curves the ball home, vacuums Likes into the meter | drags the loose rings down onto the lounger, where they get in your way |
| **push** | bends the ball away while it is climbing, drives loose rings into anchored ones and breaks them | sends your uncollected Likes over the open edge |

The shield closes the triangle: pull a dangerous pile towards yourself, raise the barrier
at the last moment, and turn the risk into a payout. But the barrier and the Party Wave
draw on the same meter, so staying alive and hitting hard compete.

**None of that table was true in the build until a playtester checked every line of it.**
Pull's cost did not exist: the lounger collided with nothing, so a ring dragged down went
straight through it and out of the pool, where it was deleted *and counted as cleared* —
pull was a free clear button, and 60 dropped balls onto a planted ring cost 0 strikers.
Push had no upside: rings shouldered each other all day and nothing ever damaged
anything, so 45 seconds of held push against a full board destroyed nothing and moved
nothing, while the same 45 seconds took the catch window from 74px to **zero**. And the
Likes it was supposed to send over the edge had no drift at all, so they sat motionless
in the water for ever — 90 to 130 of them uncollected on every board played without pull,
with nothing in the interface to say so.

The fixes are the table: the lounger masks the rings, a ring driven into another above
200 px/s damages it, Likes drift slowly towards the open edge, push only steers the ball
while it is already climbing, and pull's steering came down threefold (it turned a 132px
lounger into a 600px one, which is not a rescue, it is not being able to miss).

## Driving it without hands

Every verb is a property as well as an input, because a drag is a bad unit of intent to
assert about:

| Property | Effect |
|---|---|
| `paddle_x` | Where the lounger is going. It moves at a speed, so it cannot outrun the ball. |
| `current` | −1 full pull … +1 full push, held. |
| `launch_requested` | Let the waiting striker go. |
| `shield_requested` / `party_wave_requested` | Spend the meter. |
| `restart_requested` | Rebuild the board. |

And the board reads back in one call: `score`, `multiplier`, `meter`, `strikers_left`,
`targets_left`, `anchored_left`, `loose_left`, `threads`, `rim_hits`, `ring_impacts`,
`targets_destroyed`, `likes_collected`, `likes_loose`, `board_seconds`, `quiet_seconds`,
`drain_open`, `wave_lock_left`, `striker_position`, `target_positions`,
`loose_positions`, `like_positions`, `last_event`.

`quiet_seconds` — how long since anything scored — earns its place: it is the number that
turned "this board drags" into a measurement, and it is what the drain watches.

By hand: mouse moves the lounger, left button pulls, right button pushes, space launches,
shift raises the barrier, enter releases the wave.

## What is not here yet

Bosses, worlds, power-ups, multiball, and the between-run progression — the Followers
that persist after a run ends. The cocktails taken between boards are in. The brief is
explicit that the plainest possible formation has to be enjoyable for several minutes
before any of that is worth building, and that is the thing currently under test.

## Can it kill you?

It looked for a while as though it could not: a bot that predicts the striker's crossing
point exactly, through the side walls, lost zero strikers over six boards. That was the
bot, not the game. Give it the two things a person actually has — a reaction delay before
the hand follows the eye, and an error in where they think the ball is going — and the
loss condition turns out to be the sharpest dial in the build. Six boards each, three
strikers a board:

| player | reaction | aim error | strikers lost per board | boards cleared |
|---|---|---|---|---|
| bot | none | none | 0 | 6/6 |
| ok | 0.30s | 45px | 2.0 | 5/5 |
| poor | 0.50s | 80px | 3.0 | 0/6 — every board failed, in 7 to 77 seconds |

Three strikers is about right, then: a competent player spends two of them a board, and a
bad one is out before the first board ends. What is still untested is a *person*, who is
neither of these — worse than any of them at prediction, and far better at deciding when
to pull.

There is deliberately **no passive income on the board**. Nothing pays out for sitting
there; the level is a score-driven test of control, and the idle half of the design lives
outside it.

## Running it

```sh
python3 tools/virtual_display.py -- bin/godot.linuxbsd.editor.dev.x86_64 --path demos/pool --editor

# or headless, driven from a shell:
GODOT_AI_APPROVE_CLIENTS=1 GODOT_AI_POLICY="read_project=allow,read_runtime=allow,run_project=allow" \
  bin/godot.linuxbsd.editor.dev.x86_64 --headless --path demos/pool --editor &
bin/godot-ai-relay --call Godot_PlayMainScene --project demos/pool
```
