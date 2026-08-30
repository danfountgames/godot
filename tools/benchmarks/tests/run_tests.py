#!/usr/bin/env python3
"""Unit tests for the benchmark scoring logic.

The scoring is what turns a run into a number somebody will quote, so it is the part
that must not be quietly wrong. None of it needs an editor, a model or a network, which
is exactly why it is worth testing here rather than only through a full run.
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import projects  # noqa: E402
import scoring  # noqa: E402
import tasks  # noqa: E402

TESTS = []
FAILURES = []


def test(function):
    TESTS.append(function)
    return function


def assert_true(condition, what):
    if not condition:
        raise AssertionError(what)


def assert_eq(actual, expected, what="value"):
    if actual != expected:
        raise AssertionError("%s: expected %r, got %r" % (what, expected, actual))


class Scratch:
    def __enter__(self):
        self.root = tempfile.mkdtemp(prefix="godot-ai-benchmark-test-")
        return self.root

    def __exit__(self, *_):
        shutil.rmtree(self.root, ignore_errors=True)
        return False


def write(root, relative, text):
    path = os.path.join(root, relative)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as handle:
        handle.write(text)


@test
def digests_notice_a_change_and_ignore_the_engines_own_directory():
    with Scratch() as root:
        write(root, "a.gd", "one")
        write(root, ".godot/cache", "whatever")
        before = scoring.file_digests(before_root := root)
        assert_true("a.gd" in before, "the real file was not seen")
        # .godot is rewritten on every editor open and no agent is responsible for it.
        assert_true(not any(path.startswith(".godot") for path in before),
                    "the engine's own directory was counted: %r" % sorted(before))

        write(before_root, "a.gd", "two")
        after = scoring.file_digests(before_root)
        assert_eq(scoring.changed_files(before, after), ["a.gd"], "changed files")


@test
def an_added_or_removed_file_counts_as_a_change():
    with Scratch() as root:
        write(root, "a.gd", "one")
        before = scoring.file_digests(root)

        write(root, "b.gd", "new")
        os.remove(os.path.join(root, "a.gd"))
        after = scoring.file_digests(root)

        assert_eq(scoring.changed_files(before, after), ["a.gd", "b.gd"],
                  "an added and a removed file")


@test
def collateral_is_what_the_task_did_not_licence():
    with Scratch() as root:
        write(root, "allowed.gd", "one")
        write(root, "other.gd", "one")
        before = scoring.file_digests(root)

        write(root, "allowed.gd", "two")
        write(root, "other.gd", "two")
        after = scoring.file_digests(root)

        assert_eq(scoring.collateral_changes(before, after, ["allowed.gd"]), ["other.gd"],
                  "collateral")
        # And a task that licensed both has none.
        assert_eq(scoring.collateral_changes(before, after, ["allowed.gd", "other.gd"]), [],
                  "no collateral when both are licensed")


@test
def bookkeeping_is_reported_separately_rather_than_counted_or_hidden():
    # The first real benchmark run reported three collateral changes and every one was
    # a false positive: two .uid files the importer wrote, and the agent's own project
    # memory note - which the skills tell it to write. Counting those buries the
    # measurement in noise; excluding them silently puts a hole in it. So they are
    # classified and still shown.
    with Scratch() as root:
        write(root, "scripts/player.gd", "one")
        write(root, "scripts/other.gd", "one")
        before = scoring.file_digests(root)

        write(root, "scripts/player.gd", "two")
        write(root, "scripts/player.gd.uid", "uid://abc")
        write(root, ".godot_ai/memory/jump.md", "what I learned")
        write(root, "scripts/other.gd", "two")
        after = scoring.file_digests(root)

        licensed = ["scripts/player.gd"]
        assert_eq(scoring.collateral_changes(before, after, licensed), ["scripts/other.gd"],
                  "only the unlicensed game file is collateral")
        assert_eq(scoring.bookkeeping_changes(before, after, licensed),
                  [".godot_ai/memory/jump.md", "scripts/player.gd.uid"],
                  "the importer's and the tooling's files are reported, in their own bucket")

        # Nothing is lost by the split: everything still appears in `changed`.
        for path in (".godot_ai/memory/jump.md", "scripts/player.gd.uid",
                     "scripts/other.gd", "scripts/player.gd"):
            assert_true(path in scoring.changed_files(before, after),
                        "%s is still reported as changed" % path)


@test
def a_deleted_script_is_collateral_even_though_its_uid_is_bookkeeping():
    # The reason the split loses no signal: a real change always shows up as the real
    # file, with the bookkeeping merely following it.
    with Scratch() as root:
        write(root, "scripts/gone.gd", "one")
        write(root, "scripts/gone.gd.uid", "uid://abc")
        before = scoring.file_digests(root)
        os.remove(os.path.join(root, "scripts/gone.gd"))
        os.remove(os.path.join(root, "scripts/gone.gd.uid"))
        after = scoring.file_digests(root)

        assert_eq(scoring.collateral_changes(before, after, []), ["scripts/gone.gd"],
                  "the deleted script is still collateral")


@test
def a_licence_is_exact_paths_not_a_folder():
    # "It was allowed to touch the scripts folder" is how an agent ends up allowed to
    # touch everything, so a licence names files.
    with Scratch() as root:
        write(root, "scripts/allowed.gd", "one")
        write(root, "scripts/other.gd", "one")
        before = scoring.file_digests(root)
        write(root, "scripts/other.gd", "two")
        after = scoring.file_digests(root)
        assert_eq(scoring.collateral_changes(before, after, ["scripts/allowed.gd"]),
                  ["scripts/other.gd"], "a sibling in the same folder is still collateral")


@test
def the_number_oracle_accepts_a_range_and_says_what_it_found():
    with Scratch() as root:
        write(root, "player.gd", "@export var jump_height: float = 120.0\n")
        held, detail = scoring.oracle_number_at_least(
            root, "player.gd", r"jump_height\s*:\s*float\s*=\s*([0-9.]+)", 200.0)
        assert_true(not held, "120 should not satisfy a minimum of 200")
        assert_true("120" in detail and "200" in detail,
                    "the detail must say what was found and what was needed: %r" % detail)

        # Anything at or above the minimum, not one exact number: the task is "clear the
        # gap", and an agent that reasons its way to 210 has solved it.
        for value in ("200.0", "210", "999.5"):
            write(root, "player.gd", "@export var jump_height: float = %s\n" % value)
            held, detail = scoring.oracle_number_at_least(
                root, "player.gd", r"jump_height\s*:\s*float\s*=\s*([0-9.]+)", 200.0)
            assert_true(held, "%s should satisfy a minimum of 200: %s" % (value, detail))


@test
def an_oracle_on_a_missing_file_says_so_rather_than_crashing():
    with Scratch() as root:
        held, detail = scoring.oracle_number_at_least(root, "gone.gd", r"x = (\d+)", 1)
        assert_true(not held, "a missing file cannot satisfy an oracle")
        assert_true("does not exist" in detail, "the detail should say why: %r" % detail)

        held, detail = scoring.oracle_text_present(root, "gone.gd", "anything")
        assert_true(not held and "does not exist" in detail, detail)


@test
def oracle_all_reports_the_first_failure_only():
    # The later failures usually follow from the first, and a list of five reads as five
    # problems.
    with Scratch() as root:
        write(root, "a.tscn", "nothing useful")
        held, detail = scoring.oracle_all(root, [
            lambda r: scoring.oracle_text_present(r, "a.tscn", "first"),
            lambda r: scoring.oracle_text_present(r, "a.tscn", "second"),
        ])
        assert_true(not held, "neither check held")
        assert_true("first" in detail and "second" not in detail,
                    "only the first failure should be reported: %r" % detail)


@test
def evidence_reads_what_the_tools_actually_wrote():
    with Scratch() as root:
        write(root, "godot_ai_playtests/reach-the-room/report.json",
              '{"verdict": "reached", "goal": "reach the room"}')
        write(root, "godot_ai_sessions/a-crash/meta.json", "{}")
        os.makedirs(os.path.join(root, "godot_ai_checkpoints", "one"))

        evidence = scoring.evidence_found(root)
        assert_eq(len(evidence["playtests"]), 1, "playtests found")
        assert_eq(evidence["playtests"][0]["verdict"], "reached", "the verdict was read")
        assert_eq(evidence["sessions"], ["a-crash"], "sessions found")
        assert_eq(evidence["checkpoints"], 1, "checkpoints counted")


@test
def evidence_survives_a_report_that_does_not_parse():
    with Scratch() as root:
        write(root, "godot_ai_playtests/broken/report.json", "{not json")
        evidence = scoring.evidence_found(root)
        assert_eq(evidence["playtests"], [], "an unreadable report is skipped, not fatal")


@test
def missing_evidence_is_reported_as_absent_rather_than_as_an_error():
    assert_eq(scoring.evidence_found(None)["playtests"], [], "no root at all")
    assert_eq(scoring.evidence_found("/nowhere/at/all")["sessions"], [], "a root that is not there")


@test
def a_solved_task_with_collateral_is_not_clean():
    # The distinction the whole scorecard exists for: an agent that fixes the bug and
    # rewrites three other scripts has not done the job, and one number would hide it.
    with Scratch() as workspace:
        root = os.path.join(workspace, "project")
        task = tasks.by_identifier("jump-height/fix")
        projects.build(task.project, root, fixed=False)
        before = scoring.file_digests(root)

        projects.build(task.project, root, fixed=True)
        with open(os.path.join(root, "scripts", "audio.gd"), "a") as handle:
            handle.write("\n# unrelated\n")

        result = scoring.score_task(task, root, before)
        assert_true(result["solved"], "the fix should satisfy the oracle: %s" % result["detail"])
        assert_true(not result["clean"], "a solved task with collateral is not clean")
        assert_true("scripts/audio.gd" in result["collateral"], result["collateral"])


@test
def the_summary_keeps_solved_and_clean_apart():
    results = [
        {"task": "a", "category": "x", "solved": True, "clean": True,
         "evidence": {"playtests": [{"verdict": "reached"}], "sessions": [], "checkpoints": 0},
         "detail": "", "collateral": []},
        {"task": "b", "category": "x", "solved": True, "clean": False,
         "evidence": {"playtests": [], "sessions": [], "checkpoints": 0},
         "detail": "", "collateral": ["other.gd"]},
        {"task": "c", "category": "y", "solved": False, "clean": False,
         "evidence": {"playtests": [], "sessions": ["s"], "checkpoints": 0},
         "detail": "", "collateral": []},
    ]
    summary = scoring.summarise(results)
    assert_eq(summary["solved"], 2, "solved")
    assert_eq(summary["clean"], 1, "clean")
    assert_eq(summary["with_evidence"], 2, "with evidence")
    assert_eq(summary["by_category"]["x"], {"total": 2, "solved": 1 + 1}, "category x")
    assert_eq(summary["by_category"]["y"], {"total": 1, "solved": 0}, "category y")

    text = scoring.format_scorecard(summary)
    assert_true("2/3 solved" in text, text)
    assert_true("other.gd" in text, "collateral should be named in the scorecard: %s" % text)


@test
def timing_and_cost_are_absent_rather_than_zero():
    # A zero would read as "free and instant", which is a claim about a run that has not
    # happened.
    with Scratch() as workspace:
        root = os.path.join(workspace, "project")
        task = tasks.by_identifier("jump-height/fix")
        projects.build(task.project, root)
        result = scoring.score_task(task, root, scoring.file_digests(root))
        assert_true(result["seconds"] is None, "seconds should be absent")
        assert_true(result["cost"] is None, "cost should be absent")


@test
def every_task_names_a_project_that_exists_and_licences_files_that_do():
    with Scratch() as workspace:
        for task in tasks.TASKS:
            assert_true(task.project in projects.PROJECTS,
                        "%s names an unknown project %r" % (task.identifier, task.project))
            root = os.path.join(workspace, task.identifier.replace("/", "-"))
            projects.build(task.project, root)
            for relative in task.licensed_paths:
                assert_true(os.path.exists(os.path.join(root, relative)),
                            "%s licenses %r, which the project does not contain"
                            % (task.identifier, relative))
            assert_true(len(task.prompt.split()) >= 8,
                        "%s's prompt is too terse to be a real request" % task.identifier)


def main():
    for function in TESTS:
        try:
            function()
        except Exception as error:  # noqa: BLE001 - a failing test is the point
            print("FAIL %s: %s" % (function.__name__, error))
            FAILURES.append(function.__name__)
        else:
            print("PASS %s" % function.__name__)

    print()
    print("%d passed, %d failed, %d total"
          % (len(TESTS) - len(FAILURES), len(FAILURES), len(TESTS)))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
