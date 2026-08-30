# Three agents, three editors, one interface they had never seen

Every measurement of this tooling before this one had a single driver: the session that
built it. That is the weakest thing about all of the evidence in this repository, and the
only way to fix it is to hand the interface to something that has not seen the inside.

Three agents, each with its own headless editor on its own copy of POOL (`demos/pool/`),
were given three different jobs and told nothing except how to reach the relay:

| | job | family |
|---|---|---|
| A | *"Merging feels wrong. The striker swallows everything it drifts into."* A planted defect, described only as a symptom. | fix |
| B | Measure the brief's two drag targets, and answer one it does not ask: how far does a shot travel, and how much in each regime? | measure |
| C | Build the brief's unbuilt "burst at maximum size" feature and prove it fires. | build |

All three finished, and all three were right about the work. **The reports are worth more
than the work**, so this document is mostly theirs.

## What they got right that no test had

**A** identified the planted defect (a merge threshold of 2 where the code's own comment
documented 50), and did not stop at reading it. It needed contacts at chosen low speeds,
found teleporting rings racy, and instead applied a uniform damping to the striker alone —
under linear damping speed falls linearly with distance, so sweeping launch power swept
impact speed with nothing teleported and the rule under test untouched. Before: 7 of 7
contacted shots absorbed, nothing bounced below 189 px/s. After: the gate sits between
41 px/s and 56 px/s where it belongs. It also noticed that `ring.gd`'s own
`merge_speed = 90.0` is dead — `main.gd` overwrites it on every ring — and called the
plausible-looking 90 a decoy.

**B** established that the two drag regimes differ by exactly 12.9×, that a clean
full-power shot runs 868.34 px over 5.05 s, and that **98.2 % of that distance is glide and
1.8 % is grab** — the grab phase is a fixed 15.3 px tail whatever speed the ring arrived
at. Then it went past the question: the cheapest upgrade in the game, *Slicker Water* at 40
likes, breaks the "at rest within 5–6 seconds" target on its second purchase (6.92 s), and
the jet can put a ring at 1246 px/s — 3.3× the maximum shot — needing 8.25 s to stop. Both
are shipped features. Nobody had measured either.

**C** built the burst, and put it on the game rather than the ring so the cap could be
tuned live; measured the maximum from six real shots rather than choosing it; verified the
radial wave per ring against its own falloff prediction (229/223/215/205/192 px/s measured
against 239/230/222/211/195 predicted); and caught that the merge tipping the ring over the
cap played a note at the same pitch, on the same grid line, as the burst — "two voices on
one note is a flam, not a hit" — by reading the note log rather than by hearing it.

It also drew the conclusion the whole interface implies: *"a burst destroys the node you
would otherwise interrogate, so in a read-a-property world it is invisible unless the game
leaves a record behind. Any event that removes its own subject has to be self-reporting."*

**A** found a real bug in POOL nobody had: `_begin_run()` did not clear `_shot_live`, so a
restart mid-shot consumed the next turn. It reported it and deliberately did not fix it,
being out of scope. Fixed since.

## What it cost the product

### The skills were not shipped at all

All three found it independently. `--list-prompts` returned `{"prompts": []}` and
`Godot_ListSkills` returned `{"skills": []}` — on a project with no `ai_skills/` folder,
which is every project anyone has just made.

Discovery reads a project's folder, an addon's, and the editor's user data directory. The
ten skills this fork writes live in `misc/godot_ai/skills/`, which is none of those. So the
design's central claim — *a named job with the primitives composed underneath, offered
ahead of ninety-nine tools* — was false out of the box, and the relay's own help
advertised a door onto an empty room.

> "The front door is a locked, well-signposted empty room, and I had to assemble the
> investigation out of 99 primitives myself. 'Run a behavioural regression sweep' is
> *exactly* the job I just did by hand." — A

> "I could not tell whether no skills exist, or skills exist and are not allowed."  — B

The end-to-end suite had passed throughout, because it *copies* a skill into its own test
project before checking. A test that arranges the condition it is testing for.

**Fixed:** the skills are compiled into the binary and travel with the editor the way the
tool descriptions do. They are trusted by default, and that exception is narrow and stated:
a skill that turned up in a project or an addon is text of unknown origin steering the
model and still needs consent; one that arrived inside the binary came by the same route as
the tool descriptions, and nobody can add one without shipping a build.

### Three tools that reported success and did nothing

Every one of these produces a confident wrong answer rather than an error, which is the
failure this repository keeps finding and keeps re-committing.

- **`Godot_RecordRuntimeSeries` recorded a window of zeros** for a property that does not
  exist: `numeric: true, samples: 5, values: [0,0,0,0,0]`, with `missing: 5` the only tell
  among nine fields. B's words: *"A zero-filled velocity series is a completely believable
  'the thing never moved'."* That tool was one day old.
- **`Godot_WaitForRuntimeCondition` could only compare equality**, and a condition already
  true is satisfied immediately. B waited for `rings_in_play == 1` after asking for a
  restart; it was already 1, the wait returned before the restart, the next call set a
  property on a node about to be freed, and the shot fired from the wrong place. Result:
  839.33 px instead of 868.34 px. *"Entirely plausible, no error, no warning."*
- **Vectors came back only as text, in two different encodings** — `text` as
  `"Vector2(143.84, 132.88)"` and `value` as `"(143.84, 132.88)"`. C's regex ate the `2`
  out of `Vector2` and built a complete table of positions six hundred pixels wrong, with
  derived speeds to match: *"It did not error. It produced confident nonsense that I nearly
  wrote into this report."*

All three fixed. The vector fix is the same rule the code already applied to dictionaries
and arrays, for a reason its own comment had already written down — it simply had not been
extended to the type most often read off a moving body.

### "No game is running" was a lie

`Godot_PlayMainScene` answered at launch, but the debugger connection every runtime tool
goes through takes a second or two longer, so the very next call refused with *"no game is
running"*. A said it plainly: *"which is flatly false; a game is running, the tool just
can't reach it yet. That message will send someone hunting for a crash."*

Fixed: play waits until the game is reachable, and where it still is not, the refusal
distinguishes "nothing was started" from "it started and its connection has not come up".

### Things they asked for that do not exist

Ranked by how often they cost a round trip, not by how hard they are.

1. ~~**No `--describe <tool>`.**~~ *(fixed)* All three lost calls to argument-name drift —
   `activity` not `now`, `label` not `name`, `limit` not `lines`, `class_name` not `class`,
   `body` not `summary`, `id` not `checkpoint`, `reached` not `pass`. Six to eight round
   trips each. The refusals are good (*"unknown argument 'now' (known arguments: goal,
   activity, clear)"* self-corrects in one retry) but the only way to read a schema is to
   fetch all ninety-nine and grep. `--describe <tool>` now prints one, and suggests near
   matches so a half-remembered "runtimeseries" finds `Godot_RecordRuntimeSeries`.
2. **No frame stepping for the game started by `Godot_PlayMainScene`.** Two of them wanted
   "advance N physics frames and return". Both ended up abusing something else as a
   barrier; A abandoned an entire measurement approach over it.
3. ~~**No way to arm a recording before the action it records.**~~ *(fixed)*
   `Godot_RecordRuntimeSeries` blocks for its window, so from one client it could only be
   placed *after* the thing — and missed the first two frames, measured as 12.55 px of an
   868 px shot. Its own description said *"start it, then do the thing you want to
   watch"*, which is precisely what a single client could not do. It now takes
   `start_on_change`: it waits at the property's current value and begins on the frame it
   moves, so the first sample is the first frame of the movement. Verified by arming it
   three seconds before a shot and getting back `378 376 373 371 …` — the launch, not a
   run of zeros. Giving up waiting is a refusal naming what it waited on, because "it
   never moved" and "I stopped waiting" are different answers.
4. **No way to call a method on a running node.** `apply_water_to_existing()` existed and
   was exactly what A wanted; it set four properties on each ring instead.
5. **No structured patch write.** `Godot_WriteTextFile` is whole-file, so a one-character
   constant change means read 18 KB, replace, write 18 KB, and hope nothing else touched
   the file. A called it *"the most-used tool and the riskiest one"*.
6. **`frame` is the frame of the read, not of the last write.** A stale property reads as
   current. B only caught a `settle_time` that was a whole shot out of date by poisoning
   the property with a sentinel first — which requires already suspecting the problem.
7. **No physics-contact timeline.** The brief's target is phrased relative to "the last
   event", and nothing reports events. B reconstructed contacts from discontinuities in
   the speed trace and called it *"forensics, not instrumentation"*.
8. **`Godot_GetActivity` permanently reads `deferred`**, including for calls that answered
   seconds earlier. *"An audit trail that permanently reads 'still working' is not an audit
   trail."*

### A design contradiction, and a measurement it invalidates

C ran its whole proof inside `Godot_StartPlaytest` and got back:

> verdict: **indeterminate** — "the goal was reported as reached, but this playtest
> injected no input at all, so nothing it did can account for reaching it" (calls: 391,
> inputs: 0)

The reconciler's model of "did something" was input events only. But this project's README
and `main.gd`'s own docstring both say, in as many words, that every verb is reachable as a
*property* because a drag is a bad unit of intent — and the tooling recommends that route.
So the intended way to drive a game scored zero with the tool built to check that the
driving happened.

**Fixed:** a write to the running game counts as acting on it. A *read* still does not, and
that distinction is the whole point — a report assembled from `Godot_GetRuntimeProperty`
alone still cannot account for anything.

C also found that its own malformed tool call was recorded permanently in the playtest
report's `problems` list, pooling "the agent typed the wrong enum" with "the game did
something bad" into one count a human reads as evidence about the build. A schema rejection
says nothing about the game and can flip a reached goal to indeterminate on the strength of
a misspelt argument name; those are now reported separately as `caller_mistakes`, still
shown, because hiding them would let an agent quietly fail half its calls.

## One report that did not survive checking

A reported that a malformed `--batch` entry *"printed to stderr and exited 0"*, which would
contradict the documented contract. Measured: a malformed entry exits **2**, a failing
entry exits **1**, bad JSON exits **2**. That was the agent's own wrapper swallowing the
status — the same mistake made three times while building POOL, and now the first thing the
demo's relay helper warns about.

It is recorded here because a report from an agent is evidence, not a finding, and the
difference is whether anybody checked.

## What this exercise says about the product

The closed loop works for someone who did not build it. Three agents with no prior exposure
each drove a headless game to a real, defended result, and every one of them reached for
the running game rather than reasoning from source — B measured a counterfactual on the
live integrator, A swept impact speed through a physics trick rather than a teleport, C
verified a radial wave ring by ring.

What they did *not* do is start from a skill, because there were none. Whether the skills
lead — the thing this design bets on — is therefore still unmeasured. It is measurable now,
and running these three again against a build that ships them is the obvious next
experiment.

Reproduce the setup with:

```sh
python3 tools/benchmarks/fanout.py start --project demos/pool --workers 3
python3 tools/benchmarks/fanout.py collect
python3 tools/benchmarks/fanout.py stop
```
