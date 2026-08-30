#!/usr/bin/env python3
"""Does this benchmark measure anything?

A benchmark whose planted defect does not reproduce fails silently in the flattering
direction: every agent passes, and the number goes up. So before any of it is trusted,
each task is run against two projects that differ only in the defect - once as shipped,
once with the known fix applied - and the oracle must fail on the first and pass on the
second.

An oracle that passes on the broken project is a broken oracle. An oracle that fails on
the fixed one is testing something the fix does not do. Either way this says which, by
name, rather than reporting a number nobody can act on.
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import projects  # noqa: E402
import scoring  # noqa: E402
import tasks  # noqa: E402

FAILURES = []


def check(condition, message):
    print(("PASS " if condition else "FAIL ") + message)
    if not condition:
        FAILURES.append(message)


def main():
    workspace = tempfile.mkdtemp(prefix="godot-ai-benchmark-selfcheck-")
    try:
        # Every project builds, both ways, and the two actually differ.
        for name in projects.project_names():
            broken_root = os.path.join(workspace, name + "-broken")
            fixed_root = os.path.join(workspace, name + "-fixed")
            projects.build(name, broken_root, fixed=False)
            projects.build(name, fixed_root, fixed=True)

            broken = scoring.file_digests(broken_root)
            fixed = scoring.file_digests(fixed_root)
            differing = scoring.changed_files(broken, fixed)
            check(bool(differing),
                  "%s: the fixed project differs from the broken one" % name)

            # And differ *only* in the defect files. A project whose two builds differ
            # elsewhere would let an oracle pass for the wrong reason.
            expected = sorted(defect.relative_path for defect in projects.PROJECTS[name].defects)
            check(sorted(differing) == expected,
                  "%s: the two builds differ in exactly the planted defects (%s vs %s)"
                  % (name, sorted(differing), expected))

        # Every oracle can tell the two apart.
        for task in tasks.TASKS:
            broken_root = os.path.join(workspace, task.project + "-broken")
            fixed_root = os.path.join(workspace, task.project + "-fixed")

            held_broken, detail_broken = task.oracle(broken_root)
            check(not held_broken,
                  "%s: the oracle fails on the unfixed project (said: %s)"
                  % (task.identifier, detail_broken))

            held_fixed, detail_fixed = task.oracle(fixed_root)
            check(held_fixed,
                  "%s: the oracle passes on the fixed project (said: %s)"
                  % (task.identifier, detail_fixed))

        # Collateral detection notices a file the task did not license, and does not
        # complain about one it did.
        for task in tasks.TASKS:
            root = os.path.join(workspace, task.project + "-collateral-" + task.identifier.replace("/", "-"))
            projects.build(task.project, root, fixed=False)
            before = scoring.file_digests(root)

            # Something the task never licensed, taken from the project rather than
            # hardcoded. This used to name scripts/audio.gd, which every project
            # happened to have until one did not - and then the check failed for a
            # reason that had nothing to do with collateral detection.
            unlicensed = sorted(
                relative for relative in projects.PROJECTS[task.project].files
                if relative not in task.licensed_paths
                and not scoring.is_bookkeeping(relative)
                and relative.endswith(".gd"))
            check(unlicensed,
                  "%s: the project has no unlicensed script to touch, so collateral "
                  "detection cannot be tested against it" % task.identifier)
            bystander = unlicensed[0]
            with open(os.path.join(root, bystander), "a") as handle:
                handle.write("\n# touched by nobody's request\n")

            result = scoring.score_task(task, root, before)
            check(bystander in result["collateral"],
                  "%s: an unlicensed change to %s is reported as collateral"
                  % (task.identifier, bystander))
            check(not result["clean"],
                  "%s: a run with collateral is not clean, whatever the oracle said"
                  % task.identifier)

            # And the licensed file is not counted against it.
            licensed_root = root + "-licensed"
            projects.build(task.project, licensed_root, fixed=True)
            licensed_before = scoring.file_digests(os.path.join(workspace, task.project + "-broken"))
            licensed_result = scoring.score_task(task, licensed_root, licensed_before)
            check(not licensed_result["collateral"],
                  "%s: fixing only the licensed file leaves no collateral (%s)"
                  % (task.identifier, licensed_result["collateral"]))
            check(licensed_result["clean"],
                  "%s: the known fix scores clean" % task.identifier)

    finally:
        # Only the directory this script created, named explicitly.
        shutil.rmtree(workspace, ignore_errors=True)

    print()
    if FAILURES:
        print("benchmark self-check: %d check(s) failed" % len(FAILURES))
        return 1
    # Precisely what this proves, and no more. It compares files: every oracle fails on
    # the project as shipped and passes on the known fix, and the known fix leaves no
    # collateral. It does *not* run the game, so it cannot tell whether a defect
    # produces the symptom its prompt describes - and one of them did not, for as long
    # as nobody ran the benchmark for real. Claiming "every planted defect reproduces"
    # was the flattering reading of a textual check.
    print("benchmark self-check: every oracle fails on the shipped project and passes on "
          "the known fix. This compares files; whether a defect produces the symptom its "
          "prompt describes is only shown by running the task.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
