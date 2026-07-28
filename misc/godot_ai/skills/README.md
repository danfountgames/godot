# Example skills

Skills are reusable workflow instructions the editor discovers from the filesystem.
Copy a folder from here into one of the discovery roots to use it:

| Root | Location | Scope |
|---|---|---|
| project | `res://ai_skills/<skill>/SKILL.md` | Travels with the repository |
| plugin | `res://addons/<addon>/ai_skills/<skill>/SKILL.md` | Ships with an addon |
| user | `<editor data dir>/godot_ai/skills/<skill>/SKILL.md` | Personal, all projects |

Project skills take precedence over plugin skills, which take precedence over user
skills. A duplicate name in a lower-precedence root is reported rather than silently
shadowing the first.

A discovered skill is **not** trusted. It stays denied until you allow it by name in
*Editor Settings → Network → Godot AI → Allowed Skills*; until then
`Godot_ListSkills` shows it but `Godot_ReadSkill` refuses to return its instructions.
