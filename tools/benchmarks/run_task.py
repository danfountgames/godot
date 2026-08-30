#!/usr/bin/env python3
"""Set up one benchmark task for a real agent, then score what it did.

The harness deliberately does not talk to a model - `README.md` says so, and it is the
right design, because "how the agent was driven" should not be the benchmark's business.
What was missing was the other half: something that hands a task over through the
ordinary product path and grades the result afterwards. Without it, running a benchmark
by hand meant reinventing the setup and the scoring each time, and so it never happened.

Two commands, and an agent in between:

    python3 tools/benchmarks/run_task.py setup jump-height/fix
    #   ... the agent works, through the relay, using the ordinary tools ...
    python3 tools/benchmarks/run_task.py score jump-height/fix

`setup` builds a fresh copy of the project, records the digest of every file as handed
over, prints the prompt, and prints the editor and relay commands to drive it with.
`score` evaluates the task's oracle, diffs the project against what was handed over, and
reports collateral damage and evidence alongside the pass.

The digests are written to disk between the two so the scoring is against the project as
it really was at handover, not as it is remembered.
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import projects  # noqa: E402
import scoring  # noqa: E402
import tasks  # noqa: E402

DEFAULT_WORKSPACE = os.path.join(REPO_ROOT, "bin", "benchmark-runs")


def find_task(identifier):
    for task in tasks.TASKS:
        if task.identifier == identifier:
            return task
    raise SystemExit("no task called %r. Known: %s"
                     % (identifier, ", ".join(t.identifier for t in tasks.TASKS)))


def run_paths(workspace, identifier):
    slug = identifier.replace("/", "_")
    return (os.path.join(workspace, slug),
            os.path.join(workspace, slug + ".handover.json"))


def command_setup(arguments):
    task = find_task(arguments.task)
    project_root, handover_path = run_paths(arguments.workspace, task.identifier)
    os.makedirs(arguments.workspace, exist_ok=True)

    projects.build(task.project, project_root)
    digests = scoring.file_digests(project_root)
    with open(handover_path, "w") as handle:
        json.dump({"task": task.identifier, "root": project_root, "digests": digests},
                  handle, indent=2, sort_keys=True)

    editor = os.path.join(REPO_ROOT, "bin", "godot.linuxbsd.editor.dev.x86_64")
    relay = os.path.join(REPO_ROOT, "bin", "godot-ai-relay")

    print("task:     %s  (%s)" % (task.identifier, task.category))
    print("project:  %s" % project_root)
    print("licensed: %s" % (", ".join(task.licensed_paths) or "(nothing)"))
    print("files:    %d recorded at handover" % len(digests))
    print()
    print("--- the prompt, verbatim ---")
    print(task.prompt)
    print("--- end ---")
    print()
    print("Drive it through the ordinary product path. Nothing here is a shortcut:")
    print()
    print("  GODOT_AI_APPROVE_CLIENTS=1 GODOT_AI_POLICY=%s \\" % arguments.policy)
    print("    %s --headless --path %s --editor &" % (editor, project_root))
    print("  %s --call <tool> --arguments '<json>' --project %s" % (relay, project_root))
    print()
    print("Then: python3 tools/benchmarks/run_task.py score %s" % task.identifier)


def command_score(arguments):
    task = find_task(arguments.task)
    project_root, handover_path = run_paths(arguments.workspace, task.identifier)
    if not os.path.exists(handover_path):
        raise SystemExit("no handover record at %s; run `setup %s` first"
                         % (handover_path, task.identifier))
    with open(handover_path) as handle:
        handover = json.load(handle)

    result = scoring.score_task(task, project_root, handover["digests"],
                                user_data_root=arguments.user_data)
    if arguments.seconds is not None:
        result["seconds"] = arguments.seconds

    summary = scoring.summarise([result])
    print(scoring.format_scorecard(summary))
    if arguments.json:
        with open(arguments.json, "w") as handle:
            json.dump(summary, handle, indent=2, sort_keys=True)
        print("\nwrote %s" % arguments.json)

    # Exit non-zero on an unsolved task so a scripted sweep notices. Collateral does not
    # fail the run by itself - it is a measurement, and one worth reading rather than
    # collapsing into pass or fail.
    return 0 if result["solved"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--workspace", default=DEFAULT_WORKSPACE,
                        help="where benchmark project copies live")
    sub = parser.add_subparsers(dest="command", required=True)

    setup = sub.add_parser("setup", help="build the project and print the prompt")
    setup.add_argument("task")
    setup.add_argument("--policy",
                       default="read_project=allow,edit_files=allow,edit_scene=allow,"
                               "run_project=allow,simulate_input=allow,read_runtime=allow",
                       help="GODOT_AI_POLICY for the run")

    score = sub.add_parser("score", help="grade what the agent left behind")
    score.add_argument("task")
    score.add_argument("--user-data", default=None,
                       help="the project's user:// directory, for the evidence check")
    score.add_argument("--seconds", type=float, default=None,
                       help="wall-clock the run took, if you measured it")
    score.add_argument("--json", default=None, help="also write the scorecard here")

    arguments = parser.parse_args()
    if arguments.command == "setup":
        command_setup(arguments)
        return 0
    return command_score(arguments)


if __name__ == "__main__":
    sys.exit(main())
