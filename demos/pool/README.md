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

## The pull/push trade

Neither is the safe choice, which is the point:

| | gets you | costs you |
|---|---|---|
| **pull** | curves the ball home, vacuums Likes into the meter | drags the loose rings down at the edge you are defending |
| **push** | bends the ball away, opens formations, shoves loose rings into anchored ones | sends your uncollected Likes off the board |

The shield closes the triangle: pull a dangerous pile towards yourself, raise the barrier
at the last moment, and turn the risk into a payout. But the barrier and the Party Wave
draw on the same meter, so staying alive and hitting hard compete.

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
`targets_left`, `anchored_left`, `loose_left`, `threads`, `rim_hits`, `targets_destroyed`,
`likes_collected`, `board_seconds`, `striker_position`, `target_positions`,
`loose_positions`, `like_positions`, `last_event`.

By hand: mouse moves the lounger, left button pulls, right button pushes, space launches,
shift raises the barrier, enter releases the wave.

## What is not here yet

Bosses, worlds, the six-board structure, power-ups, multiball, and the between-run
progression — the cocktails and Followers that turn a board into a run. The brief is
explicit that the plainest possible formation has to be enjoyable for several minutes
before any of that is worth building, and that is the thing currently under test.

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
