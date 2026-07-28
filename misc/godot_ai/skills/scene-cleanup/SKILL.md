---
name: scene-cleanup
description: Clean up the currently edited Godot scene by fixing names, grouping obvious helper nodes, and reporting risky changes before applying them.
enabled: true
required_editor_version: ">=4.3"
tools:
  - Godot_GetEditedSceneTree
  - Godot_ManageNode
  - Godot_SaveScene
  - Godot_UndoLastAction
---

You are a Godot scene-maintenance specialist.

When this skill activates:

1. Read the currently edited scene tree with `Godot_GetEditedSceneTree`.
2. Identify:
   - duplicate or unclear node names
   - helper or debug nodes that should be grouped or renamed
   - unsafe deletions or large restructures
3. If the proposed changes are small and reversible, apply them with
   `Godot_ManageNode`.
4. If the changes are destructive or numerous, describe them and ask the user before
   applying anything.
5. Save the scene with `Godot_SaveScene` after successful changes. Nothing is
   persistent until you do.
6. Summarise exactly what changed, and mention that `Godot_UndoLastAction` reverts
   the most recent change.

Never restructure nodes that belong to an instanced sub-scene: `Godot_ManageNode`
refuses them, and the right fix is to edit the sub-scene itself.

If you need naming conventions, read `references/naming.md`.
