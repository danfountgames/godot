---
name: scene-cleanup
description: Tidy an edited scene's names and structure by proposing the whole plan first, so the reversible renames are one decision and anything destructive is asked about on its own.
enabled: true
required_editor_version: ">=4.3"
tools:
  - Godot_GetEditedSceneTree
  - Godot_ProposeChange
  - Godot_ManageNode
  - Godot_SearchProject
  - Godot_SaveScene
  - Godot_UndoLastAction
---

You are tidying a scene someone else has to keep working in. Cleanup is the job where an
agent most easily does more harm than good: every change is individually defensible, the
whole is a scene nobody recognises, and a rename that broke a `NodePath` in a script does
not show up until the game runs.

So the shape of this work is **propose the whole plan, then make only what was agreed**.

## Read before you plan

`Godot_GetEditedSceneTree`. Then look for, in roughly this order of value:

- **Names the engine chose.** `Sprite2D2`, `Node3`, `Timer4` — an auto-generated suffix
  means the node was never named at all. These are the highest-value renames and the
  safest, because nothing was ever written against a name nobody picked.
- **Names that say the type instead of the role.** `ProgressBar2` wants to be `HealthBar`.
- **Duplicates and near-duplicates** at the same level, which make every `NodePath`
  through them ambiguous to a reader.
- **Debug and helper nodes** left in the tree, which want grouping under one parent rather
  than deleting — you do not know they are unused.

Read `references/naming.md` for the conventions this project follows.

## Check what a rename would break, before proposing it

`Godot_SearchProject` for the current name. A `NodePath` written against it in a script, an
animation track or a signal connection will not follow the rename, and the failure appears
at run time far from the change. If a name is referenced anywhere, either leave it alone
and say why, or include the fix for the reference in the same plan.

This is the step that makes the difference between a cleanup and a breakage, and it is the
one that is tempting to skip because the tree looks so obviously improvable.

## Propose the whole thing at once

`Godot_ProposeChange` with a `title` saying what the cleanup is for, and one entry per
change: a `description` a person can judge, and the exact `tool` and `arguments` that
would make it.

It groups the plan by what it costs to be wrong. Reversible narrow changes — which is what
almost every rename is — become a single decision however many there are. Anything
destructive is offered separately. That is the whole point: forty renames should be one
question, and the one node you want to delete should be its own.

The reply hands back the approved calls. **Make exactly those, in that order, and nothing
else.** An item that was not approved was declined; do not do it, do not do a smaller
version of it, and do not come back to it in the same session.

If nothing was approved, say what was declined and stop. That is a legitimate outcome and
it is not a failure of the plan.

## Then apply, and say what happened

Make the approved calls with `Godot_ManageNode`. Then `Godot_SaveScene` — nothing is
persistent until you do, and a session that renames twenty nodes and never saves has
achieved nothing.

Report what changed, what you left alone and why, and that `Godot_UndoLastAction` takes
back the most recent change.

## Things worth knowing

- **Never restructure a node belonging to an instanced sub-scene.** `Godot_ManageNode`
  refuses it, and the refusal is right: the fix is to edit the sub-scene, where the change
  is visible to everything that instances it.
- A plan whose changes the schema would reject is refused while it is still a plan, which
  is the cheap place to find out. But the check is schema-level: a rule a tool only applies
  when it runs can still fail an approved call. If one does, stop and report it rather than
  carrying on down the list.
- Deleting is almost never the right cleanup. You are reading a tree, not the game; a node
  that looks unused is a node whose use you have not found. Group it, rename it to say what
  it is, or leave it and mention it.
