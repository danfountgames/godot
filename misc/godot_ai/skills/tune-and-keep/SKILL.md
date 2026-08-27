---
name: tune-and-keep
description: Adjust a value while the game runs, judge it in play, and keep the one that felt right by promoting it into the scene.
enabled: true
required_editor_version: ">=4.3"
tools:
  - Godot_GetRuntimeProperty
  - Godot_SetRuntimeProperty
  - Godot_PromoteRuntimeValue
  - Godot_SaveScene
  - Godot_UndoLastAction
  - Godot_CaptureGame
  - Godot_AskUser
---

You are tuning a value that can only be judged by playing: a jump height, a movement
speed, a camera offset, a colour. The point of doing it in the running game is that the
answer is a feeling, not a number — and the point of this skill is that the feeling
survives into the project.

The game must already be running.

1. `Godot_GetRuntimeProperty` first, and say what the current value is. A tuning session
   that cannot report where it started cannot report what it changed.
2. `Godot_SetRuntimeProperty` to try a value. Change **one** thing at a time. Two
   changes at once produce a feeling that neither of them explains.
3. Play or look. `Godot_CaptureGame` if the difference is visible.
4. Repeat from 2 until it is right. Say what each value felt like as you go — that
   history is the useful part of the session, and it is gone the moment the game exits.
5. When one is right, **`Godot_PromoteRuntimeValue`**. It reads the value out of the
   running game and writes it into the same node in the edited scene. Without this step
   the whole session was theatre: the value dies with the process.
6. `Godot_SaveScene` to keep it. Until you do, the scene is changed but not written.

## Things worth knowing

- Promotion refuses when the running game is playing a different scene from the one the
  editor has open. That is not an obstacle to work around — it is the check that stops a
  value from one scene being written into another, where it would look entirely
  plausible.
- Promoting a value the scene already holds reports `promoted: false`. That is a
  confirmation, not a failure.
- A promotion goes through the editor's undo history, so `Godot_UndoLastAction` takes it
  back.
- Use `Godot_AskUser` when the judgement is genuinely the person's — "does this feel
  right?" is not a question you can answer for them, and offering two or three candidate
  values is more useful than asking for a number.

## When not to use this

If the value can be decided from a rule rather than a feeling — a size that must match
another size, a timing that must match an animation — set it in the scene directly and
say why. Tuning in play is for what only play can settle.
