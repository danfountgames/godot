---
name: check-a-visual-change
description: Prove an edit changed what it was meant to change on screen, and nothing else - by capturing before and after and comparing them.
enabled: true
required_editor_version: ">=4.3"
tools:
  - Godot_CaptureViewport
  - Godot_CaptureGame
  - Godot_CompareCaptures
  - Godot_GetEditorStatus
  - Godot_RecallProjectMemory
  - Godot_UpdateProjectMemory
  - Godot_ListCheckpoints
  - Godot_RestoreCheckpoint
  - Godot_SetIntent
  - Godot_PlayMainScene
  - Godot_StopPlaying
---

You are proving that a change did what it was meant to do *and nothing else*. This is
the cheapest way to catch the second half, which is the half that ships: the shader
tweak that also darkened the UI, the layout fix that moved something offscreen, the
"harmless" rename that unhooked a texture.

Describing a screen in prose is not evidence about a screen. Two captures and a
comparison are.

## Before you start

`Godot_SetIntent` with a goal, so every checkpoint taken from here belongs to one task
and `Godot_RestoreCheckpoint` can undo the whole attempt in one call if it goes wrong.

`Godot_RecallProjectMemory` — a previous session may already have recorded that this
scene renders differently on this machine, or which region is expected to be noisy.

## The method

1. **Capture the before.** `Godot_CaptureViewport` for an editor change,
   `Godot_CaptureGame` for a running one. Save it somewhere you will recognise:
   `res://.godot_ai/shots/<what>-before.png`.
2. **Make exactly one change.** More than one and the comparison cannot attribute what
   it finds.
3. **Capture the after, the same way, at the same size.** `Godot_CompareCaptures`
   refuses two images of different sizes rather than comparing their overlap, because a
   resized window is a different question. If the window moved or resized, start again.
4. **Compare**, passing an `output` so there is a picture to look at:

   ```
   Godot_CompareCaptures before=…-before.png after=…-after.png output=…-diff.png
   ```

5. **Read the verdict against what you expected**, which is the whole point:

   | Result | What it means |
   |---|---|
   | `identical` and you expected a change | The change did not reach the screen. Check it applied at all before assuming the capture is wrong. |
   | `identical` and you expected none | What you wanted. Say so with the number, not just the word. |
   | `minor`/`substantial` where you expected it | Read `changed_bounds` and confirm the box is where the change should be. |
   | changed **outside** the box you expected | The finding. This is what the skill is for. |

## Reading the numbers honestly

- `max_channel_delta` is reported even when the verdict is `identical`. A delta just
  under the tolerance means the two are not really the same and the next run may tip
  over; say so rather than reporting a clean pass.
- The default tolerance of 8 exists because two captures of an unchanged scene are
  never bit-identical. Do not drop it to 0 to "be strict" — you will get a change on
  every call and learn nothing. Drop it only when comparing two images you generated.
- `changed_fraction` is of the whole frame. Two percent of a 1080p frame is a large
  object; two percent of a 64-pixel icon is a couple of pixels.

## When it finds something you did not intend

Undo the whole attempt with `Godot_RestoreCheckpoint` and its `task`, rather than
picking checkpoints off the list one at a time. Then make the change again in smaller
pieces, comparing after each, until you know which piece caused it.

## Finish by recording it

If you learned something that will still be true next month — this scene always shows a
few pixels of noise around the timer, that panel is expected to repaint on every frame,
this effect only differs under the software renderer — record it with
`Godot_UpdateProjectMemory`. The next session will otherwise investigate it again from
scratch.

Report the numbers and the difference image, not an adjective. "Substantial change:
4,182 of 2,073,600 pixels (0.20%) differ, all within 210x64 at (1130, 12)" is a
finding. "It looks a bit different" is not.
