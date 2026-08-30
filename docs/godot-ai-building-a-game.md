# Building a real game with this, and what it cost

Everything else in this repository tests the tooling against tasks the tooling's authors
chose. This is the first time it was pointed at a game somebody else specified, with no
screen, from a shell — a brief arrived, and POOL (`demos/pool/`) was built against it
entirely through the MCP tools. No file in that project was written by touching it
directly.

The game is described in `demos/pool/README.md`. This document is about the tooling: what
worked, what did not, and what the exercise changed.

## The short version

The closed loop is real, and it is the only reason the game works.

Four defects in the game were found by running it and reading it back, and **not one of
them was visible in the source**. Each is a case where every file said the right thing:

| What the code said | What the running game did |
|---|---|
| A ring bounces off another ring | 534 px/s in, 23 px/s out, in a single frame — a dead stop |
| A hard contact merges, a graze bounces | Merging never fired, at any speed |
| The board holds twelve rings | 283 rings after three shots |
| `settle_time` is how long the board took to still | A flat 0.13s however violent the shot |

The second is the one worth dwelling on, because it is the sort of bug that survives
review. The merge rule reads the impact speed inside a `body_entered` callback — which
is not the approach speed. `body_entered` fires around the solver rather than reliably
before it, so **the two bodies in one contact disagree about how fast it was**. Measured:
the target reported 203.9 px/s and the striker 66.1, against a threshold of 90. The
striker's reading loses, the merge is declined, and the central rule of the game silently
never happens. Reading the source, it is correct. Reading each ring's own account of its
last contact, it is obvious in about four seconds.

## What the tooling made easy

**Building a scene without a scene editor.** `Godot_CreateScene`, `Godot_ManageNode`,
`Godot_SetSceneProperty` and `Godot_SaveScene` built the whole tree headless.
`Godot_CheckScript` after every write caught every GDScript error before it could become
a runtime mystery, including two warnings that would have failed a strict build.

**Wiring the upgrade page.** `Godot_ManageConnection` connected four buttons and the
connections were in the `.tscn` afterwards. That tool exists because the *previous*
exercise proved it missing; this one used it for real and it held.

**Tuning by measurement instead of by taste.** Four candidate settings were tried against
the running game with `Godot_SetRuntimeProperty` and a property that rebuilds the board,
three shots each, and the numbers picked the winner. It took about ninety seconds and
produced a result nobody would have guessed: **lowering the merge threshold makes chains
shorter, not longer.** A slow graze merges, so the ring is heavy before it is through the
rack. Chains went 4,3,3 at a threshold of fifty and 1,3,2 at thirty-five. The brief lists
"merge-everything might be too easy" as an open question; this is that question answered
with a measurement rather than an opinion.

**Headless was not a compromise.** No screen, and the only thing missed was looking at
it. Every claim in the game's README is a number read off a running process.

## What the tooling made hard

### 1. A running game cannot be sampled from outside

This is the big one, and it is not a bug that can be fixed with a patch.

Each `Godot_GetRuntimeProperty` is a round trip. Through `--call` it is also a process
launch and a handshake: about half a second. A six-property "snapshot" of the running
game therefore spans **three seconds**, and the first shot in POOL lasted 0.6.

The first attempt to measure that shot produced a table of positions and velocities that
looked like a physics bug and was actually an artefact of the sampling. Hours could go
into that. `--batch` collapses six reads to 0.24s, which is a twelvefold improvement and
still not a snapshot — the six replies came back stamped with frames 18872 through 18887.

The answer is that **the sampling has to happen inside the game**. POOL solved it for
itself by keeping a `PackedFloat32Array` of the active ring's speed per physics frame, and
once that existed everything became legible at once: the glide decay, the exact frame of
each merge, the wallow, and the grab.

But every game worth measuring would have to write that, and it is not the game's job.
**`Godot_RecordRuntimeSeries` now does it for any of them.** Name a node, a property, a
frame count and a clock; the game records it and the whole window comes back in one reply.
The same shot, with no game-side instrumentation at all:

```
$ Godot_RecordRuntimeSeries path=/root/Main/Rings/Ring13 property=linear_velocity
                            component=length frames=160 clock=physics

{clock: physics, first_frame: 351, last_frame: 510, samples: 160, missing: 0, numeric: true}

  f+ 32     0    0    0    0    0    0    0    0    0  378  376  373  371  369  367  365
  f+ 64   330  328  326  324  323  321  319  317  315  313  311  310  308   47  164  163
  f+ 96   147  147  146  145  144  143  142  142  141  140   66   92   91   91   90   90
  f+112    89   89   88   88   87   87   86   84   73   67   62   58   53   49   46   42
  f+128    39   36   33   31   29   26   24   23   21   19   18   17   15   14   13   12
  f+144    11   10   10    9    8    8    7    7    6    6    5    0    0    0    0    0
```

Four details are there because of what this exercise cost:

- **The clock is a choice, defaulting to physics.** A shot, a jump arc and a collision
  chain all happen on the physics step; sampling them from process frames adds a beat of
  jitter to every reading. Anything visual is the other way round.
- **`component` picks one number out of a vector.** Without it a `Vector2` window came
  back as a hundred and sixty strings, and "how fast is it going" is the commonest
  question anyone asks of a moving body.
- **Unreadable frames are counted, not dropped.** A node that has not spawned yet, or has
  been freed mid-window, leaves `missing: 5` rather than a hole — because a hole in a
  series is invisible and a count is not.
- **The reply carries the first frame, the last, and the interval,** so the timeline is
  reconstructable rather than being a bag of numbers.

`Godot_GetRuntimeProperty` also now says in its own description that reads are not
snapshots, and `--batch`'s help says what batching is actually for.

### 2. Nothing in the game was addressable

Rings spawned at runtime came back from `Godot_GetRuntimeSceneTree` as
`@RigidBody2D@270`. The number is an instance counter: it changes every run, so nothing
outside can name the same ring twice. Naming them in `_add_ring` fixed it for this game,
but a tool driving *someone else's* game has no such recourse.

**`Godot_FindRuntimeNodes` now addresses them by what does survive a restart** — engine
class, a fragment of the name, or where they are:

```
$ Godot_FindRuntimeNodes class_name=RigidBody2D near_x=512 near_y=490 within=400

{"path": "/root/Main/Rings/Ring2", "type": "RigidBody2D",
 "position": "(668, 250.96)", "distance": 285.44}
```

Base classes match their subclasses, so `CollisionObject2D` finds a `RigidBody2D` and the
caller does not have to know the exact leaf class. With `near_x`/`near_y` the results sort
by distance, so the thing closest to where something just happened is entry zero. A class
name that does not exist is a **refusal**, not an empty list: "there are none" and "you
misspelt it" are different answers, and only one of them is worth acting on.

`Godot_GetRuntimeSceneTree` now also carries each node's path, which it did not — it gave
a name and a depth, and left the caller to rebuild tree walking before it could use any
other runtime tool. The path in both is absolute, which is a deliberate second pass: the
first version returned one relative to the scene root, so the field called `path` was the
one string in the reply that would not work anywhere else.

### 3. Rewriting an attached script stranded the editor's copy

`Godot_WriteTextFile` over a script the open scene uses left the editor's in-memory node
with the old property set. `Godot_PromoteRuntimeValue` then refused with *"the running
node has 'merge_speed' but the edited 'Node2D' does not; the two have drifted apart"* —
accurate, and pointing at the wrong cause. The scene had not drifted; the editor was
holding a stale script, and the whole live-tuning loop dead-ended there: a value tuned in
the running game could not be promoted back into a script the same session had edited.

**Fixed, and it took three attempts, which is the interesting part.** The editor holds a
script as a loaded `Resource`, and every node using it holds that same reference.

1. Reloading with `CACHE_MODE_REPLACE` swaps the resource's contents in place. Not
   enough: instances keep the member layout they were compiled with.
2. Adding `Script::reload(true)` to recompile them. Still not enough.
3. `ScriptLanguage::reload_scripts()` — GDScript keeps its own parse cache, and only the
   language clears it. That is also the call the editor makes for "reload scripts", and
   it lives on `ScriptLanguage` rather than in the GDScript module, so the fix stays
   language-agnostic.

Attempts one and two are worth recording because both *looked* like they worked: the
write reported `reloaded_in_editor: true` and the node still did not have the property.
The measurement that settled it was blunt — write a script with a new export, then ask
the edited node to set it — and it is now an end-to-end check, because a fix that reports
success without doing anything is precisely the failure this whole exercise keeps finding.

Closing and reopening the scene did not help either, so there was no workaround to fall
back on. That is why this was worth chasing past the second dead end.

### 4. A control's wiring was buried in the engine's own

Asking a `Button` inside a `VBoxContainer` what it was connected to returned six
connections: one the author wrote, and five `Container::_child_minsize_changed` and
friends. The existing filter drops connections whose far end is outside the scene root,
and every one of these is inside it. Fixed: connections bound to an engine method
(`Class::method`) are counted with the editor's own observers rather than listed. The
same button now answers with one line and `editor_connections_hidden: 16`.

## Three mistakes that were mine, and are worth more than the fixes

The harness driving all of this was forty lines of Python. It got three things wrong, and
each one imitated a product defect convincingly enough to send me looking in the wrong
place. They are recorded because an agent writing its own harness will make them too.

**Reading the wrong error shape.** The relay reports a protocol rejection as a bare
JSON-RPC object — `{"code": -32602, "message": "..."}` at the top level, not nested under
`"error"`. The harness looked for the nested shape, found none, and returned the error
object *as the result*. A batch of five rejected calls printed five success lines. The
scene had no script attached, no connections, and nothing had happened — which is exactly
the failure this repository's own benchmark write-up calls the one worth remembering.

**Then reading it too eagerly.** The fix checked the exit status first and reached for
`payload["message"]`. But a *tool-level* refusal is a successful MCP result carrying
`isError`, with the reason in `content[].text` and nothing in `message` at all. Every one
of those then printed as an empty refusal. I concluded three tools were refusing without
saying why, and started reading their C++. They were answering perfectly well.

**Parsing a `PackedVector2Array` as if it printed like a `Vector2`.** It does not:
`Vector2` prints `(x, y)`, the packed array prints its components flat. The regex found
nothing, the board read as empty, and the empty board looked like a spawning bug.

The common thread: **every one of them turned a working thing into a broken-looking
thing, and none of them failed loudly.** Check the exit status, not the shape of what came
back — this repository learned that once before, in a different place, and it did not
transfer.

## What changed in the product

- `Godot_ManageConnection` no longer lists the engine's own bindings.
- `Godot_GetRuntimeProperty` says in its description that reads are not snapshots, and
  what to do instead.
- The relay's `--batch` help text is no longer interleaved with `--continue-on-error`'s,
  and says what batching is actually for.
- **`Godot_RecordRuntimeSeries` is new**, and is the largest of these: it removes the
  reason every game driven this way would have had to instrument itself.
- **`Godot_FindRuntimeNodes` is new**, and `Godot_GetRuntimeSceneTree` now carries paths,
  so a node a script created without naming it can be addressed at all.
- **`Godot_WriteTextFile` reloads a script the editor has open**, so an export added to
  an attached script exists on the edited node immediately, and can be tuned live and
  promoted back.
- **A new skill, `measure-a-feel-target`**, so the next agent does not have to rediscover
  the loop: record from inside, read the shape, try candidates as a set, solve for the
  number where arithmetic exists, and watch for two stated targets that fight each other.

## What should change next, in order

1. **Address a runtime node without a stable name.** By class and index, or by position.
2. **Reload a script the editor has open**, so promotion works after a rewrite.
2. **A worked example in the skills** that says the game must instrument itself, because
   an agent that has not hit this will spend its first hour sampling from outside.
