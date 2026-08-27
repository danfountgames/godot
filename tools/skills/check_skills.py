#!/usr/bin/env python3
"""Do the shipped skills describe tools that exist?

A skill's `tools:` list is what a model reads before it decides how to work. A name in
that list that no tool answers to is worse than an omission: the model reaches for it,
gets "unknown tool", and either gives up on the skill or improvises around it. Neither is
visible to anyone until it happens in front of a user.

This caught `Godot_SendActionInput`, named by three shipped skills and implemented by
none of them - which is how it came to be implemented.

Needs no editor and no build: the tool names are read out of the C++ that registers them,
and the skills out of their own front matter. That is what makes it cheap enough to run in
the fast CI job on every push.

Run with:  python3 tools/skills/check_skills.py
"""
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SKILLS_DIR = os.path.join(REPO_ROOT, "misc", "godot_ai", "skills")
MODULE_DIR = os.path.join(REPO_ROOT, "modules", "godot_ai")

FAILURES = []


def check(condition, message):
    print(("PASS " if condition else "FAIL ") + message)
    if not condition:
        FAILURES.append(message)


def _names_in_tool_name_bodies(text):
    """Every `Godot_*` literal inside a `get_tool_name()` body.

    The whole body, not just a plain `return "X";`. Two tools in this module pick their
    name from a flag - `Godot_PlayMainScene` or `Godot_PlayCurrentScene`, `Godot_UndoLastAction`
    or `Godot_RedoLastAction` - and a pattern that only understood the simple form reported
    four real tools as missing on this check's very first run.
    """
    names = set()
    literal = re.compile(r'"(Godot_\w+)"')
    for match in re.finditer(r"get_tool_name\(\)\s*const\s*override\s*\{", text):
        depth = 1
        at = match.end()
        while at < len(text) and depth > 0:
            if text[at] == "{":
                depth += 1
            elif text[at] == "}":
                depth -= 1
            at += 1
        names.update(literal.findall(text[match.end():at]))
    return names


def registered_tool_names():
    """Every `Godot_*` name the module registers, however it registers it.

    Two spellings exist and both are read: a class overriding `get_tool_name`, and a
    `RuntimeCommandTool` constructed with its name inline. A check that knew only the
    first would quietly bless every runtime tool as missing.
    """
    names = set()
    by_construction = re.compile(r'RuntimeCommandTool\(\s*\n?\s*"(Godot_\w+)"')

    for directory, _subdirectories, files in os.walk(MODULE_DIR):
        for name in files:
            if not name.endswith(".cpp"):
                continue
            with open(os.path.join(directory, name)) as handle:
                text = handle.read()
            names.update(_names_in_tool_name_bodies(text))
            names.update(by_construction.findall(text))
    return names


def skill_front_matter(path):
    """The `name:` and the `tools:` list from a SKILL.md, without a YAML dependency."""
    with open(path) as handle:
        text = handle.read()
    if not text.startswith("---"):
        return None, []
    end = text.find("\n---", 3)
    if end < 0:
        return None, []
    front = text[3:end]

    name = None
    tools = []
    in_tools = False
    for line in front.splitlines():
        if line.startswith("name:"):
            name = line.split(":", 1)[1].strip()
        elif line.startswith("tools:"):
            in_tools = True
        elif in_tools and line.startswith("  - "):
            tools.append(line[4:].strip())
        elif line and not line.startswith(" "):
            in_tools = False
    return name, tools


def main():
    known = registered_tool_names()
    check(len(known) > 50,
          "the module's registered tool names were found (%d of them)" % len(known))
    if len(known) <= 50:
        # Nothing below can mean anything if the names could not be read at all, and a
        # green run in that state would be the worst outcome here.
        print("\nrefusing to check skills against a tool list this small")
        return 1

    if not os.path.isdir(SKILLS_DIR):
        check(False, "the shipped skills directory exists")
        return 1

    for entry in sorted(os.listdir(SKILLS_DIR)):
        skill_file = os.path.join(SKILLS_DIR, entry, "SKILL.md")
        if not os.path.exists(skill_file):
            continue
        name, tools = skill_front_matter(skill_file)
        check(name == entry,
              "%s: the skill's declared name matches its directory (%r)" % (entry, name))
        check(bool(tools), "%s: names the tools it uses" % entry)
        for tool in tools:
            check(tool in known, "%s: names a tool that exists (%s)" % (entry, tool))

    print()
    if FAILURES:
        print("skill check: %d problem(s)" % len(FAILURES))
        return 1
    print("skill check: every tool named by a shipped skill is registered")
    return 0


if __name__ == "__main__":
    sys.exit(main())
