# Example skills

Skills are reusable workflow instructions the editor discovers from the filesystem.

They exist because the tool surface is large. There are more than eighty `Godot_*`
tools, and a model choosing among eighty similar names picks the wrong one, or builds a
sequence that never made sense, more often than one following a named workflow. A skill
is that named workflow: "run a playtest", "investigate a crash", "tune this and keep
it" - with the primitives underneath, available for the unusual cases rather than
equally prominent in every interaction. Reaching for a skill first is the normal way to
work here, not an advanced option.

| Skill | What it is for |
|---|---|
| `playtest-a-goal` | Play towards a stated goal and produce a report whose verdict is checked against what was actually pressed |
| `investigate-a-crash` | Turn "it crashed" into a reproducible, minimised sequence |
| `traverse-the-menus` | Walk every reachable screen and report what is unreachable, what has no way back, and what does nothing |
| `tune-and-keep` | Adjust a value in play, judge it, and promote the one that felt right into the scene |
| `performance-profiling` | Capture a profiler window and read it |
| `scene-cleanup` | Tidy an edited scene's names and structure |

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
