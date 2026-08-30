#!/usr/bin/env python3
"""Benchmark Godot projects, with their defects deliberately planted.

Each project is small and each defect is one a real game has: a value that is wrong
rather than absent, a button wired to nothing, a signal connected to a method that was
renamed. Defects that make the project fail to load would be found by the engine and
would measure nothing.

Every defect carries its own fix, so `run_selfcheck.py` can build the project both ways
and prove the oracle can tell them apart. A benchmark whose defect does not reproduce
fails silently in the flattering direction - every agent passes - so the fix is not a
convenience, it is what makes the measurement trustworthy.
"""
import os
import shutil

PROJECT_GODOT = """\
config_version=5

[application]

config/name="%(name)s"
run/main_scene="res://scenes/main.tscn"

[rendering]

renderer/rendering_method="gl_compatibility"
"""


# --- jump-height: a tuned value that is simply wrong -------------------------
#
# The most ordinary bug there is. Nothing errors, nothing crashes; the jump is too weak
# to clear the gap, and the only way to know is to play it or to read the number and
# compare it with the gap.

JUMP_PLAYER = """\
extends Node2D

# The height the player reaches, in pixels. The gap in main.tscn is 180 wide and the
# player needs at least 200 to clear it.
@export var jump_height: float = %(jump_height)s

var reached_far_side: bool = false

func _ready() -> void:
	set_process(true)

func _process(_delta: float) -> void:
	reached_far_side = jump_height >= 200.0
"""

JUMP_MAIN = """\
[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://scripts/player.gd" id="1"]

[node name="Main" type="Node2D"]

[node name="Player" type="Node2D" parent="."]
script = ExtResource("1")

[node name="Gap" type="Node2D" parent="."]
"""

# Something else in the project, so "did it change what it was not asked to" has
# something to catch. A real project is full of these.
JUMP_BYSTANDER = """\
extends Node

# Nothing in any task touches this. It is here so that changing it can be noticed.
@export var volume: float = 0.8
"""


# --- sticky-pause: a toggle that only works once -----------------------------
#
# The first task whose prompt is a pure behavioural symptom, with no hint about which
# file to open and nothing wrong in the output log.
#
# Be precise about what that does and does not demand, because the lesson of the first
# benchmark run was that a benchmark overclaims easily. Reading pause.gd also reveals
# this bug - most bugs are findable by reading everything. What is different here is
# that *confirming the fix* cannot be done by reading: `paused` and `toggles` after two
# presses are the same in a file either way, and only a running game distinguishes them.
# That is the capability this product has and others do not, and until now no task
# needed it.

PAUSE_BYSTANDER = """\
extends Node

# Nothing in any task touches this. It is here so that changing it can be noticed.
@export var fade_seconds: float = 0.25
"""

PAUSE_SCRIPT = """\
extends Node

## Set while the pause menu is up.
var paused: bool = false

## How many times the menu has been toggled. A player who opens and closes it once
## should leave this at 2.
var toggles: int = 0

func _unhandled_input(event: InputEvent) -> void:
	if event.is_action_pressed("ui_cancel"):
		_toggle()

func _toggle() -> void:
%(body)s
"""

PAUSE_BROKEN_BODY = """\
	if paused:
		return
	paused = true
	toggles += 1"""

PAUSE_FIXED_BODY = """\
	paused = not paused
	toggles += 1"""

PAUSE_MAIN = """\
[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://scripts/pause.gd" id="1"]

[node name="Main" type="Node"]
script = ExtResource("1")
"""


# --- dead-button: a control wired to nothing ---------------------------------
#
# The bug traverse-the-menus exists to find. The button is there, it is enabled, it
# looks right, and pressing it does nothing because the signal was never connected.

MENU_SCRIPT = """\
extends Control

var opened_settings: bool = false

func _on_settings_pressed() -> void:
	opened_settings = true
"""

MENU_SCENE_BROKEN = """\
[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://scripts/menu.gd" id="1"]

[node name="Main" type="Control"]
script = ExtResource("1")

[node name="Play" type="Button" parent="."]
text = "Play"

[node name="Settings" type="Button" parent="."]
text = "Settings"
"""

MENU_SCENE_FIXED = MENU_SCENE_BROKEN + """
[connection signal="pressed" from="Settings" to="." method="_on_settings_pressed"]
"""


# --- renamed-method: a connection left pointing at a method that moved -------
#
# The bug that only appears at run time, as an error in the log, long after the rename
# that caused it. Reading the script does not show it; reading the scene does.

SPAWNER_SCRIPT = """\
extends Node

signal wave_started

var waves: int = 0

func _ready() -> void:
	wave_started.emit()

func on_wave_started() -> void:
	waves += 1
"""

SPAWNER_SCENE_BROKEN = """\
[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://scripts/spawner.gd" id="1"]

[node name="Main" type="Node"]
script = ExtResource("1")

[connection signal="wave_started" from="." to="." method="_on_wave_started"]
"""

SPAWNER_SCENE_FIXED = SPAWNER_SCENE_BROKEN.replace(
    'method="_on_wave_started"', 'method="on_wave_started"')


class Defect:
    """One planted defect: how it is written, and how it would be fixed.

    `broken` and `fixed` are the two contents of the same file. Nothing else differs, so
    an oracle that cannot tell them apart is an oracle that measures nothing.
    """

    def __init__(self, relative_path, broken, fixed, describe):
        self.relative_path = relative_path
        self.broken = broken
        self.fixed = fixed
        self.describe = describe


class BenchmarkProject:
    def __init__(self, name, files, defects, licensed_paths):
        self.name = name
        # Files that are the same either way.
        self.files = files
        self.defects = defects
        # The files a task is licensed to change. Anything else the agent writes is
        # collateral, which is the measurement worth having.
        self.licensed_paths = licensed_paths


PROJECTS = {
    "jump-height": BenchmarkProject(
        name="jump-height",
        files={
            "scripts/audio.gd": JUMP_BYSTANDER,
            "scenes/main.tscn": JUMP_MAIN,
        },
        defects=[
            Defect(
                "scripts/player.gd",
                JUMP_PLAYER % {"jump_height": "120.0"},
                JUMP_PLAYER % {"jump_height": "220.0"},
                "the player's jump_height is 120 and the gap needs 200",
            )
        ],
        licensed_paths=["scripts/player.gd", "scenes/main.tscn"],
    ),
    "sticky-pause": BenchmarkProject(
        name="sticky-pause",
        files={
            "scripts/fade.gd": PAUSE_BYSTANDER,
            "scenes/main.tscn": PAUSE_MAIN,
        },
        defects=[
            Defect(
                "scripts/pause.gd",
                PAUSE_SCRIPT % {"body": PAUSE_BROKEN_BODY},
                PAUSE_SCRIPT % {"body": PAUSE_FIXED_BODY},
                "_toggle() returns early when already paused, so the menu never closes",
            )
        ],
        licensed_paths=["scripts/pause.gd"],
    ),
    "dead-button": BenchmarkProject(
        name="dead-button",
        files={
            "scripts/menu.gd": MENU_SCRIPT,
            "scripts/audio.gd": JUMP_BYSTANDER,
        },
        defects=[
            Defect(
                "scenes/main.tscn",
                MENU_SCENE_BROKEN,
                MENU_SCENE_FIXED,
                "the Settings button's pressed signal is connected to nothing",
            )
        ],
        licensed_paths=["scenes/main.tscn", "scripts/menu.gd"],
    ),
    "renamed-method": BenchmarkProject(
        name="renamed-method",
        files={
            "scripts/spawner.gd": SPAWNER_SCRIPT,
            "scripts/audio.gd": JUMP_BYSTANDER,
        },
        defects=[
            Defect(
                "scenes/main.tscn",
                SPAWNER_SCENE_BROKEN,
                SPAWNER_SCENE_FIXED,
                "a signal is connected to _on_wave_started, which no longer exists",
            )
        ],
        licensed_paths=["scenes/main.tscn", "scripts/spawner.gd"],
    ),
}


def build(name, root, fixed=False):
    """Writes benchmark project `name` into `root`. Returns the project definition.

    With `fixed`, every planted defect is written in its repaired form instead. That is
    what the self-check builds to prove an oracle can tell the two apart.
    """
    project = PROJECTS[name]
    if os.path.exists(root):
        # Only ever a directory this function was asked to build, and only after it has
        # been named explicitly by the caller. Never a recursive delete of anything
        # discovered.
        shutil.rmtree(root)
    os.makedirs(root)

    contents = dict(project.files)
    for defect in project.defects:
        contents[defect.relative_path] = defect.fixed if fixed else defect.broken
    contents["project.godot"] = PROJECT_GODOT % {"name": project.name}

    for relative, text in contents.items():
        path = os.path.join(root, relative)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as handle:
            handle.write(text)

    # Godot writes here on first open; making it now keeps a benchmark run from being
    # the thing that creates it.
    os.makedirs(os.path.join(root, ".godot"), exist_ok=True)
    return project


def project_names():
    return sorted(PROJECTS)
