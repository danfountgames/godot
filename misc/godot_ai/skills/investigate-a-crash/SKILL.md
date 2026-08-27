---
name: investigate-a-crash
description: Turn "it crashed" into a reproducible sequence, an error with its stack, and the smallest set of steps that still causes it.
enabled: true
required_editor_version: ">=4.3"
tools:
  - Godot_ReadOutputLog
  - Godot_CaptureBugSession
  - Godot_RecordSession
  - Godot_ReplaySession
  - Godot_ListSessions
  - Godot_AssertRuntimeState
  - Godot_GetRuntimeSceneTree
  - Godot_GetRuntimeProperty
  - Godot_SendActionInput
  - Godot_SendKeyInput
  - Godot_SendPointerInput
  - Godot_CaptureGame
  - Godot_PlayMainScene
  - Godot_StopPlaying
---

You are turning a crash report into something someone can fix. The deliverable is a
sequence that causes it again, not an explanation of why it might happen.

## First, read what already happened

`Godot_ReadOutputLog` with `problems_only`. The error and its stack are usually already
there, and reading them first often makes the rest of this unnecessary. Quote the actual
message; do not paraphrase it.

## If it has already crashed, capture it before doing anything else

`Godot_CaptureBugSession` with a name and a `reason`. It writes out the input that led
here from a buffer that is always being kept, so nothing had to be armed in advance —
and it works after the game process has gone, which is the usual state of affairs after
a crash.

Do this **first**, before you press play again or change anything: a new run clears the
buffer, and the sequence you have is the one that produced the bug you were actually
looking at. Read the reply's `fidelity` line and repeat what it says rather than
improving on it. In particular:

- `source: "runtime_trace"` means the game was still alive to be asked, and every frame
  in the capture is one the game reported.
- `source: "editor_mirror"` means the game was gone, so this is a record of what was
  sent, not of what the game did with it.
- `frames_estimated` above zero means the tail was never acknowledged — which is what a
  crash looks like from here. Replaying it is a reproduction attempt. A replay that does
  not crash has not disproved the bug, and saying it has is the one thing that would make
  this whole exercise worse than useless.

Then replay it, exactly as below. A capture nobody has replayed is a guess with a
filename.

## Then reproduce it deliberately

When you have no capture — the crash predates this session, or someone described it to
you — reproduce it on purpose instead.

1. `Godot_PlayMainScene` to start a clean run.
2. `Godot_RecordSession` with `action: "start"` and a name that says what it is —
   `crash-on-second-door`, not `test1`. Everything you inject from here is captured with
   the frame it landed on.
3. Drive the game to the crash with the input tools. Take the route you believe causes
   it; you are testing a hypothesis, not exploring.
4. `Godot_AssertRuntimeState` at the moments that matter — just before the suspect
   action, and at any state you think is a precondition. These are what make a replay
   able to say *where* a later run diverged rather than only that it did.
5. When it crashes, or when you are sure it did not, `Godot_RecordSession` with
   `action: "stop"`.

## Then prove the recording

`Godot_ReplaySession` re-injects the trace and reports the first thing that came out
different. A recording nobody has replayed is a guess.

- If the replay crashes too, you have a reproduction. Say the session name; that is the
  artefact.
- If it does not, the crash depends on something the trace does not carry — timing, a
  random seed, a saved file, input from a person at the window rather than through these
  tools. Say which you suspect and why. That is a genuine finding, not a failure.

## Then make it smaller

A reproduction of forty steps is worth much less than one of four. Record shorter
sessions, each dropping something you believe is irrelevant, and replay each one. Stop
when dropping anything else stops the crash. Report the smallest one that still fails,
and say what you removed.

## What to report

The error text and stack, the session name, the number of steps, and the one sentence a
person needs: what the game is doing when it breaks. `Godot_CaptureGame` at the moment
before the crash if the state is easier to see than to describe.

## Honest limits, worth saying out loud

A recorded trace covers input **these tools** injected. A person playing the game window
is invisible to it, so "it crashed when I was playing" cannot be captured directly — you
have to reproduce their route yourself. And replay is not deterministic in general: it
re-injects at the recorded frame spacing, which is close enough for most gameplay and
not close enough for anything that depends on exact timing or an unseeded random.
