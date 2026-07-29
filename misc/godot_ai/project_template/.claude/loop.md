Continue building this game. This is one iteration of a loop, not a fresh start.

**Do not re-bootstrap.** Skip the "Begin now" sequence in `AGENTS.md`; it is for the
first run only. Start here instead:

1. Read `.agent/STATE.md` and `.agent/NEXT.md`. They are your memory — your context
   window is not.
2. Reconcile them against reality: `git status --short`, `git log --oneline -5`. The
   repository wins over any written claim. Correct stale notes before acting on them.
3. Health-check the interface with one `Godot_GetEditorStatus`. If the editor is gone —
   long runs lose it — restart it per `ENVIRONMENT.md` before anything else. A dead
   editor makes every tool call fail in a way that looks like a broken game.
4. Take the first concrete unblocked item from `.agent/NEXT.md` and finish it: implement,
   run, drive it with real input, capture and *look at* the screenshots, test, fix.
5. Update `.agent/` and commit. Then print the status block below.

**One slice per iteration.** Prefer finishing one thing to starting three. An iteration
that ends with a committed, verified slice is worth more than one that ends mid-edit.

**End every iteration in a resumable state**, because the next iteration may begin after
a crash, a compaction, or a week:

- working tree committed, or deliberately and explicitly left dirty in `STATE.md`
- no game left playing (`Godot_StopPlaying`)
- no unsaved edited scene (`Godot_SaveScene`)
- `.agent/NEXT.md` naming the *next* concrete action, not a vague direction

If nothing in `NEXT.md` is actionable, do not invent work. Run a verification pass
instead — rerun the real-input routes, reread the newest screenshots, recheck the
Definition of Done — and record what you confirmed.

Finish by printing the status block that `AGENTS.md` specifies under *Running under
/goal and /loop*. Print it every iteration, even when little changed: it is the only
thing a `/goal` evaluator can see.
