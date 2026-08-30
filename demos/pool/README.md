# POOL — first playable

Top-down billiards in heavy water, where the ring you shot merges with everything it
hits hard enough, and every impact is a musical event.

The rule the whole thing hangs on: the ring you shot is **active** until it comes to
rest, and while it is active any contact above `merge_speed` **absorbs** what it hit.
Everything else **bounces**. Merges conserve area and momentum, so a chain makes you
bigger, heavier and slower — the shot paces itself and there is no timer anywhere.

This is the first playable from section 8 of the brief: one rectangular pool, striker
plus twelve field rings, bounce-or-merge on a speed threshold, chain multiplier and
likes, one jet, three shots.

## Why it is in this repository

It is not a sample. It is the test: **the whole game was built through the MCP tooling
this fork adds**, against a headless editor with no screen, driven from a shell over the
relay. Nothing in `scripts/` or `scenes/` was written by touching a file directly —
every one went through `Godot_WriteTextFile`, `Godot_CreateScene`, `Godot_ManageNode`,
`Godot_SetSceneProperty`, `Godot_ManageConnection` and `Godot_SaveScene`, and every
behavioural claim below was measured by running it and reading it back.

What that exposed, in the tooling and in the game, is written up in
`docs/godot-ai-building-a-game.md`.

## Measured, not asserted

One aimed shot on a settled board, read off the running game:

```
flick    380 378 376 ... 310 308        the glide: clean exponential decay at 0.35/s
merge    107 -> 205                     chain 1  (momentum conserved across the merge)
         205 204 ... 180
merge    113 -> 142 -> 133 -> 100       chains 2, 3, 4
wallow   100  99  98 ...  81            big, heavy, slow
grab      72  67  62  57  53 ... 5      the water takes it, at 4.8/s
phrase   55, 52, 48, 48                 a descending run - the chain as a phrase
```

Chain of 4, multiplier 2.4, everything at rest 0.80s after the last event. The brief
asks for 5 to 6 seconds.

## Driving it without hands

Every verb is reachable as a **property**, not only as input, because a drag is a bad
unit of intent to assert about:

| Property | Effect |
|---|---|
| `queued_shot` | A non-zero `Vector2` launches the active ring along it, length as a fraction of `max_shot_speed`, then clears itself. |
| `jet_held` | Runs the wall jet while true. |
| `restart_requested` | Rebuilds the board on the next frame, picking up any exported value changed since. |

And the board reads back in one call rather than twenty: `active_position`,
`lit_positions`, `ring_positions`, `chain`, `likes`, `lit_left`, `set_cleared`.

The game also measures **itself**, at physics rate, because nothing outside it can:
`shot_trace` is the active ring's speed per frame since the launch, and `settle_time`,
`shot_end_area`, `first_impact_speed` and `last_phrase` are the rest of the shot. Three
property reads from outside arrive after a two-second shot is already over.

## Running it

```sh
# With a screen, or a virtual one:
python3 tools/virtual_display.py -- bin/godot.linuxbsd.editor.dev.x86_64 --path demos/pool --editor

# Headless, driven from a shell:
GODOT_AI_APPROVE_CLIENTS=1 GODOT_AI_POLICY="read_project=allow,read_runtime=allow,edit_files=allow,edit_scene=allow,run_project=allow" \
  bin/godot.linuxbsd.editor.dev.x86_64 --headless --path demos/pool --editor &
bin/godot-ai-relay --call Godot_PlayMainScene --project demos/pool
```

By hand: drag back from the striker and release, billiards style; hold space for the jet.

## What is not here

The brief's sections 4, 6 and 7 beyond the first playable: prestige and the Chlorine
meta, whirlpools, ring types that always bounce, the burst at maximum size, and the
living music bed. The music **system** is here — quantization to the grid, ring size to
pitch, chain milestones bringing layers in — but the bed it should sit under is not, and
this container has no audio device, so what is verified is the note schedule rather than
the sound. `music.gd` logs every scheduled note with the beat it lands on for exactly
that reason.
