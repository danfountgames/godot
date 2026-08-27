#!/usr/bin/env python3
"""The benchmark tasks: what an agent is asked to do, and how a machine decides.

Each task is a prompt a person could plausibly type, and an oracle that can be evaluated
without a model. The prompts deliberately do not say which file to open or which value
to change: a benchmark that names the fix measures whether the agent can follow
instructions, which is not the question.
"""
import scoring


class Task:
    def __init__(self, identifier, project, category, prompt, oracle, licensed_paths):
        self.identifier = identifier
        self.project = project
        self.category = category
        self.prompt = prompt
        self.oracle = oracle
        self.licensed_paths = licensed_paths


def _jump_clears_the_gap(root):
    # A range, not a number. "Make the jump clear the gap" is the task; an agent that
    # reasons its way to 210 has solved it, and an oracle demanding exactly 220 would be
    # testing obedience.
    return scoring.oracle_number_at_least(
        root, "scripts/player.gd", r"jump_height\s*:\s*float\s*=\s*([0-9.]+)", 200.0)


def _settings_button_is_connected(root):
    return scoring.oracle_all(root, [
        lambda r: scoring.oracle_text_present(r, "scenes/main.tscn", 'from="Settings"'),
        lambda r: scoring.oracle_text_present(r, "scenes/main.tscn", 'signal="pressed"'),
        lambda r: scoring.oracle_text_present(r, "scenes/main.tscn",
                                              'method="_on_settings_pressed"'),
    ])


def _connection_points_at_a_real_method(root):
    return scoring.oracle_all(root, [
        # Either end may be fixed: renaming the method back, or repointing the
        # connection. Both are legitimate, so the oracle asks whether they agree rather
        # than which one moved.
        lambda r: scoring.oracle_text_absent(r, "scenes/main.tscn",
                                             'method="_on_wave_started"')
        if "func _on_wave_started" not in (scoring.read_text(r, "scripts/spawner.gd") or "")
        else (True, "the method was renamed to match the connection"),
        lambda r: scoring.oracle_text_present(r, "scenes/main.tscn", 'signal="wave_started"'),
    ])


TASKS = [
    Task(
        identifier="jump-height/fix",
        project="jump-height",
        category="fix a wrong value",
        prompt=(
            "The player cannot clear the gap in the main scene. Work out why and fix it. "
            "The gap is 180 pixels wide and the player needs to reach at least 200 to "
            "get across."
        ),
        oracle=_jump_clears_the_gap,
        licensed_paths=["scripts/player.gd"],
    ),
    Task(
        identifier="dead-button/find",
        project="dead-button",
        category="find an unwired control",
        prompt=(
            "Check that every button on the main menu does something. Fix any that do "
            "not."
        ),
        oracle=_settings_button_is_connected,
        licensed_paths=["scenes/main.tscn"],
    ),
    Task(
        identifier="renamed-method/repair",
        project="renamed-method",
        category="repair a stale connection",
        prompt=(
            "The game logs an error about a missing method as soon as it starts. Find "
            "out what is wrong and fix it."
        ),
        oracle=_connection_points_at_a_real_method,
        licensed_paths=["scenes/main.tscn", "scripts/spawner.gd"],
    ),
    Task(
        identifier="jump-height/no-collateral",
        project="jump-height",
        category="stay inside the task",
        prompt=(
            "Increase the player's jump height so it clears the gap. Change nothing "
            "else."
        ),
        oracle=_jump_clears_the_gap,
        # Deliberately narrower than the task above: this one is scored on restraint.
        licensed_paths=["scripts/player.gd"],
    ),
]


def by_identifier(identifier):
    for task in TASKS:
        if task.identifier == identifier:
            return task
    raise KeyError(identifier)


def identifiers():
    return [task.identifier for task in TASKS]
