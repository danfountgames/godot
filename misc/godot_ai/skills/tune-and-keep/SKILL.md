---
name: tune-and-keep
description: Adjust a value while the game runs, judge it in play, and keep the one that felt right by promoting it into the scene.
enabled: true
required_editor_version: ">=4.3"
tools:
  - Godot_GetRuntimeProperty
  - Godot_SetRuntimeProperty
  - Godot_OfferVariants
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

**If the brief states a number, use `measure-a-feel-target` instead.** "It should feel
weighty" is this skill; "it should cross the screen in about three seconds" is a
measurement, and arguing about it by eye when the game can be asked is wasted time.

## When you have candidates in mind, offer them as a set

`Godot_OfferVariants` is the workspace for exactly this. `offer` captures the current
value and takes a list of candidates; `switch` puts one into the running game; `note`
records what you saw; `keep` names the winner; `discard` puts the original back. Prefer
it over a sequence of bare `Godot_SetRuntimeProperty` calls whenever you have more than
one value in mind, because it keeps three things the bare calls lose:

- **The original**, captured before anything changed and always available to switch back
  to. Comparing a candidate against what the game already had is the comparison that gets
  forgotten, and often the one that settles it.
- **Your notes, attached to the value they are about.** "260 overshoots the platform" is
  the session; a list of numbers is not.
- **How long each value was actually live**, which the reply reports back to you. If you
  flipped through four numbers in a second it will say the set recorded a choice rather
  than a comparison, and it will be right. Play each candidate properly before judging
  it.

Two rules it enforces, both worth understanding rather than working around:

- A candidate that was never switched to cannot be kept. Keeping a value nobody played is
  editing the scene by a longer route; if that is what you want, say so and use
  `Godot_SetSceneProperty`.
- `keep` writes nothing to the project. It names the winner and hands you to
  `Godot_PromoteRuntimeValue`, which is the tool that holds the authority to change a
  scene. Do that while the game is still running and still holding the kept value.

Then continue at step 5 below.

## When you are feeling your way to a value

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
  values is more useful than asking for a number. `Godot_OfferVariants` and `Godot_AskUser`
  go together well: offer the set, switch through it while they watch, then ask which one
  they want kept.
- `discard` puts the original back into the *running game*. It writes nothing to the
  project either way, so a session you abandon leaves no trace — which is the point of
  tuning live.

## When not to use this

If the value can be decided from a rule rather than a feeling — a size that must match
another size, a timing that must match an animation — set it in the scene directly and
say why. Tuning in play is for what only play can settle.
