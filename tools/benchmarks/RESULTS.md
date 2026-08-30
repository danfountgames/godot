# Benchmark results

## Run 1 — 2026-08-30, all four tasks, first run against a real model

Until this run, every claim about the tooling rested on scripts we wrote. `NEXT.md` had
carried "no evaluation" as one of three under-covered risks since the specification, and
the benchmark harness had existed for days without once being pointed at a model. This
is that run.

**Driver:** the session model, driving the editor itself over the relay — `--call`, the
ordinary tools, the ordinary permission model. No shortcuts, no direct file edits outside
the tools, no reading of the oracles before answering.
**Editor:** `--headless`, no display, `GODOT_AI_POLICY` granting reads, edits and running.
(Both runs predate the viewport fix below, so they drove the game by action only.)
**Harness:** `run_task.py setup` / `run_task.py score`, written for this run because the
harness could set up and grade a task but had nothing to hand one over with — which is a
large part of why it had never been used.

### Scorecard

| Task | Category | Solved | Clean | Evidence |
|---|---|---|---|---|
| `jump-height/fix` | fix a wrong value | yes | yes | none |
| `renamed-method/repair` | repair a stale connection | yes | yes | none |
| `dead-button/find` | find an unwired control | yes | yes | none |
| `jump-height/no-collateral` | stay inside the task | yes | yes | none |

**4/4 solved, 4/4 without collateral, 0 of 4 left machine-checkable evidence.**

Read that last column as the finding it is. On all four tasks the fix was confirmed by
reading the *running* game — `reached_far_side` true, `waves` at 1 — which is the closed
loop working exactly as intended. But none of it was recorded through
`Godot_StartPlaytest` or `Godot_RecordSession`, so the scorecard finds nothing a person
could check afterwards, and it is right to say so. Four tasks this small do not each
warrant a playtest; the honest conclusion is that the evidence column measures a habit
the tools do not yet make automatic, not one the agent chose to skip.

Do not read 4/4 as a headline. Four tasks is not a benchmark, three of them are
single-value edits, and the agent that ran them is the one that built the tools. What
the run is worth is the four defects below, none of which any test had found.

### What the run exposed

**1. A headless editor launched a game that could not run — and crashed it.** *(fixed)*

The first attempt to verify a fix in the running game found `playing: false` and an
empty output log. `EditorRun::build_base_arguments` never told the child which display
driver to use, because upstream assumes an editor implies a screen. The game went
looking for X11, printed "Unable to create DisplayServer", and then segfaulted inside
`XGetSelectionOwner` on the way out. From the editor's side it simply vanished.

This broke the entire closed loop for a headless agent — run the game, observe it,
revise — which is the product. Ninety-six tools, three test suites and a
hundred-and-thirty-check end-to-end run had not noticed, because the end-to-end suite's
headless path *skips* the runtime checks with `SKIP runtime tree: the headless game did
not report one`. A skip that had been recording the bug as a fact of life.

**2. A benchmark task described a symptom that never happens.** *(fixed)*

`renamed-method/repair` told the agent "the game logs an error about a missing method as
soon as it starts". It does not. Godot 4.8 drops a scene connection to a missing method
without printing anything, so an agent doing exactly the right thing — run it, read the
output log — finds two lines, neither an error, and has to fall back to reading files.

**3. The self-check was overclaiming, and this is why the second one survived.** *(fixed)*

`run_selfcheck.py` ended by printing "every planted defect reproduces". It proves nothing
of the sort: it compares files, requiring each oracle to fail on the shipped project and
pass on the known fix. A defect whose *prompt* describes a runtime symptom can be
textually present and behaviourally absent, and that is precisely what had happened. The
check that existed to stop the benchmark failing in the flattering direction was itself
phrased in the flattering direction.

**4. There was no tool that connects a signal.** *(fixed, and it found two more)*

`dead-button/find` is about an unwired control, and completing it meant adding a
connection to a scene. Nothing in ninety-six tools did that, so the only route was
writing the `.tscn` as text — which the repository's own rules permit only because no
structured API existed, and which is exactly the kind of edit those rules exist to
prevent. Connecting a signal is one of the commonest things anyone does in this editor.

`Godot_ManageConnection` now does it, and writing it turned up two defects of its own,
both of which made it look like it worked:

- **A listing was useless.** The editor watches every node in the edited scene, so
  asking a button what it was connected to returned eleven `ScriptEditor` and
  `SceneTreeEditor` hooks and not one line of the scene's own wiring. Connections whose
  far end is outside the scene root are now counted and excluded rather than listed.
- **The connection did not survive the save.** `Object::connect` makes a *runtime*
  connection; `PackedScene::pack()` records only connections carrying `CONNECT_PERSIST`.
  Without it the editor behaved correctly for the rest of the session, `Godot_SaveScene`
  reported success, and the `.tscn` had no connection in it. This is the failure mode
  worth remembering from the whole exercise: everything reported success and nothing had
  happened.

And a third within that: a refusal for a misspelt signal listed `script_changed,
property_list_changed, ready, renamed…`, because asking a `Button` for its *own* signals
returns nothing — `pressed` belongs to `BaseButton`. It now climbs to the class that
actually declares them.

The task was re-run afterwards and passes through the tool, with no hand-edited scene
text.

**5. The collateral measurement reported three false positives.** *(fixed)*

The collateral measurement reported three changes on the first
task and all three were false positives — two `.uid` files the importer wrote and the
agent's own project-memory note, which the skills tell it to write. Counting those buries
the measurement in noise; excluding them silently puts a hole in the one number this
benchmark exists for. They are now classified and still shown, under `bookkeeping (not
counted)`.

### What is still missing

- **Timing and cost.** Still empty, and still honestly empty. The run took roughly twenty
  minutes of wall clock including two engine rebuilds, which is not a number worth
  recording as a benchmark result.
- **Scale.** Four tasks, three projects, one driver. Enough to find bugs, nowhere near
  enough to compare two models or two prompts.
- **A task that needs the game played rather than read.** Every task here is fixed by
  editing a file; the runtime was used to confirm, not to discover. The capability that
  makes this product different is not yet what any task requires. *(Addressed in run 2.)*

## Run 2 — 2026-08-30, `sticky-pause/reproduce`

Added because of the last line above. The prompt is a pure behavioural symptom — no file
named, nothing in the output log, nothing to grep for:

> Pressing cancel opens the pause menu, but pressing it again does not close it. The
> player gets stuck. Find out why and fix it.

Be precise about what that demands, because overclaiming is the failure this document
keeps finding. Reading `pause.gd` reveals the bug too; most bugs are findable by reading
everything. What is different is that **confirming the fix cannot be done by reading**.
A broken build and a fixed one differ, after two presses, only in `paused` and
`toggles`, and only a running game tells you which you have.

Driven headless, entirely through the tools:

```
reproduce   after press 1: toggles=1 paused=True
            after press 2: toggles=1 paused=True     <- stuck, as reported
fix         _toggle() sets `paused = not paused`
re-verify   after press 1: toggles=1 paused=True
            after press 2: toggles=2 paused=False    <- closes
```

**Solved, clean, and this time the loop did the work rather than confirming it.**
`Godot_SendActionInput` is the way in deliberately: `ui_cancel` goes through the input
map, so the task is solvable headless where coordinates are not.

It also found one more thing, in the benchmark rather than the product: `run_selfcheck.py`
had hardcoded `scripts/audio.gd` as the file to dirty when testing collateral detection.
Every project happened to have one until this one did not, and the check then failed for
a reason that had nothing to do with collateral. It now takes an unlicensed script from
the project definition.
