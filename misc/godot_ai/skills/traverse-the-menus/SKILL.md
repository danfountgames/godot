---
name: traverse-the-menus
description: Walk every reachable screen of a running game's interface and report what is unreachable, what has no way back, and what does nothing.
enabled: true
required_editor_version: ">=4.3"
tools:
  - Godot_GetRuntimeSceneTree
  - Godot_GetRuntimeNodeInfo
  - Godot_GetRuntimeProperty
  - Godot_WaitForRuntimeCondition
  - Godot_SendPointerInput
  - Godot_SendKeyInput
  - Godot_SendActionInput
  - Godot_CaptureGame
  - Godot_StartPlaytest
  - Godot_NotePlaytestObservation
  - Godot_FinishPlaytest
---

You are walking a running game's interface the way a player would, and reporting the
places that walk does not work. This is the cheapest test there is and it finds the
embarrassing bugs: the settings screen with no Back, the button that was never wired up,
the screen you can only reach once.

Open a playtest first (`Godot_StartPlaytest`, goal "reach every screen and get back")
so the walk produces a report rather than a conversation.

## The method

1. `Godot_GetRuntimeSceneTree` to see what is on screen now. Note the current screen's
   name — you will need to recognise it again.
2. List the things a player could press. `Godot_GetRuntimeNodeInfo` gives you a control's
   rectangle; that is what you aim the pointer at.
3. For each one, in order:
   - Press it with `Godot_SendPointerInput`.
   - `Godot_WaitForRuntimeCondition` on something that proves the screen changed —
     usually a node becoming visible. **Never wait a fixed time.**
   - If nothing changed, that is a finding: record it with
     `Godot_NotePlaytestObservation` and move on. A button that does nothing is the most
     common thing this finds.
   - If something did change, recurse: walk the new screen the same way.
   - Then get back. Try the obvious control first, then the `ui_cancel` action with
     `Godot_SendActionInput`, then Escape with `Godot_SendKeyInput`. In that order, and the
     order matters: the action is what the game reads, so a menu that answers `ui_cancel`
     but not Escape is working correctly and a menu that answers Escape but not `ui_cancel`
     is a menu that will break the moment somebody rebinds it. **A screen with no way back
     is a finding**, and an important one.
4. Keep a list of screens visited so you do not loop. Say how you are identifying them —
   usually a root node name.

## What to report

- Every screen you reached, and how.
- Every screen you could **not** reach, and what you tried.
- Every control that did nothing.
- Every screen with no way back.
- `Godot_CaptureGame` for anything laid out wrongly. A description of a broken layout is
  never as useful as the picture.

Finish with `Godot_FinishPlaytest`. Use `reached` only if you got to every screen you
could identify **and** back out of each one; if there is a screen you never found a
route to, that is `not_reached`, and the interesting part of your report.

## Aim at controls, not at coordinates

A rectangle read from `Godot_GetRuntimeNodeInfo` this frame is correct. A coordinate
copied from a screenshot is correct until the resolution, the theme or the font changes,
and then it silently presses whatever is there instead.
