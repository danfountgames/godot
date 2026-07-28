# NEXT

At most five ordered actions. The first must be immediately executable.

1. Implement `SKILL.md` discovery over project (`res://ai_skills/**`), user
   (`<editor data>/godot_ai/skills/**`) and plugin (`addons/*/ai_skills/**`) roots,
   with YAML-frontmatter parsing (name, description, enabled, editor-version gate,
   tools). — S1, S2 — verified by doctest over a fixture tree including malformed
   frontmatter, duplicate names and a version gate that excludes this build.
2. Add the allow/deny trust state: discovered skills are denied until the user
   allows them, persisted in editor settings, and exposed through the protocol.
   — S3, S5 — verified by a test that a denied skill is discoverable but not usable.
3. Ship the example `scene-cleanup` skill from the specification and assert it loads.
   — D3 — verified by the discovery test finding it with its declared tool list.
4. Implement checkpoints for mutating tools (git-backed when the project is a repo,
   snapshot otherwise) and prove restoration in a test. — F8 — verified by mutating,
   restoring, and comparing file contents.
5. Add `Godot_ReadOutputLog` and the runtime/persistent property split
   (`Godot_SetRuntimeProperty` vs `Godot_SetSceneProperty`). — T9, T15 — verified by
   e2e coverage that a runtime edit does not survive stopping the game.
