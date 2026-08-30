#!/usr/bin/env python3
"""Give N agents an editor each, on their own copy of a project.

Every measurement of this tooling so far has had one driver: the session that built it.
That is the weakest thing about all of it, and the only way to fix it is to hand the
interface to something that has not seen the inside.

This provisions the isolated worlds. Each agent gets:

  * its own copy of the project, so one agent's edits cannot be another's
  * its own headless editor on its own port
  * its own GODOT_AI_HOME, so instance discovery and checkpoints do not cross

and nothing else - no helper script, no worked example. Whether the interface is usable
is the question, so anything supplied here that is not part of the product is a thumb on
the scale.

    python3 tools/benchmarks/fanout.py start --project demos/pool --workers 3
    python3 tools/benchmarks/fanout.py status
    python3 tools/benchmarks/fanout.py collect   # what each world looks like now
    python3 tools/benchmarks/fanout.py stop
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EDITOR = os.path.join(REPO, "bin/godot.linuxbsd.editor.dev.x86_64")
RELAY = os.path.join(REPO, "bin/godot-ai-relay")
DEFAULT_ROOT = "/tmp/godot_ai_fanout"

# What a worker is allowed to do. Deliberately not blanket approval: the point of the
# exercise includes finding out whether an agent handles a refusal well, and
# dangerous_exec cannot be granted at all.
POLICY = ("read_project=allow,read_runtime=allow,edit_files=allow,edit_scene=allow,"
          "run_project=allow,simulate_input=allow")


def world_dir(root, index):
    return os.path.join(root, "worker%d" % index)


def state_path(root):
    return os.path.join(root, "fanout.json")


def load_state(root):
    try:
        with open(state_path(root)) as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return {"workers": []}


def save_state(root, state):
    os.makedirs(root, exist_ok=True)
    with open(state_path(root), "w") as handle:
        json.dump(state, handle, indent=1)


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def start(args):
    source = os.path.abspath(os.path.join(REPO, args.project))
    if not os.path.isdir(source):
        sys.exit("no project at %s" % source)
    if not os.path.exists(EDITOR):
        sys.exit("no editor binary at %s - build it first" % EDITOR)

    stop(args, quiet=True)
    os.makedirs(args.root, exist_ok=True)
    state = {"source": source, "workers": []}

    for index in range(args.workers):
        world = world_dir(args.root, index)
        if os.path.exists(world):
            shutil.rmtree(world)
        project = os.path.join(world, "project")
        # The editor's import cache is per-copy and regenerated on first open; copying it
        # would hand every worker the same stale one.
        shutil.copytree(source, project, ignore=shutil.ignore_patterns(".godot"))
        home = os.path.join(world, "home")
        os.makedirs(home, exist_ok=True)

        environment = dict(os.environ)
        environment["GODOT_AI_HOME"] = home
        environment["GODOT_AI_APPROVE_CLIENTS"] = "1"
        environment["GODOT_AI_POLICY"] = POLICY
        log = open(os.path.join(world, "editor.log"), "w")
        process = subprocess.Popen(
            [EDITOR, "--headless", "--path", project, "--editor"],
            stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
            env=environment, start_new_session=True)
        state["workers"].append({
            "index": index, "pid": process.pid, "project": project,
            "home": home, "log": os.path.join(world, "editor.log"),
        })
        print("worker %d: pid %d, project %s" % (index, process.pid, project))

    save_state(args.root, state)
    print("\nwaiting for each editor to answer...")
    for worker in state["workers"]:
        ready = False
        for _ in range(30):
            time.sleep(2)
            if probe(worker):
                ready = True
                break
        print("worker %d: %s" % (worker["index"], "serving" if ready else "NOT ANSWERING"))
    print("\nEach worker's brief is the same two lines:")
    for worker in state["workers"]:
        print("  GODOT_AI_HOME=%s %s --call <tool> --project %s"
              % (worker["home"], RELAY, worker["project"]))


def probe(worker):
    environment = dict(os.environ)
    environment["GODOT_AI_HOME"] = worker["home"]
    environment["GODOT_AI_APPROVE_CLIENTS"] = "1"
    try:
        result = subprocess.run(
            [RELAY, "--call", "Godot_GetEditorStatus", "--project", worker["project"]],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=environment, timeout=30)
    except subprocess.TimeoutExpired:
        return False
    return result.returncode == 0 and b"project_root" in result.stdout


def status(args):
    state = load_state(args.root)
    if not state["workers"]:
        print("no workers")
        return
    for worker in state["workers"]:
        running = alive(worker["pid"])
        print("worker %d  pid %-7d %-10s %s"
              % (worker["index"], worker["pid"],
                 "running" if running else "gone",
                 "serving" if running and probe(worker) else ""))
        print("          project %s" % worker["project"])


def collect(args):
    """What each world looks like now, against the copy it started from.

    Diffed rather than asked: what an agent reports it did and what its project contains
    are different things, and only one of them is evidence.
    """
    state = load_state(args.root)
    for worker in state["workers"]:
        print("=" * 70)
        print("worker %d" % worker["index"])
        print("=" * 70)
        result = subprocess.run(
            ["diff", "-rq", "--exclude=.godot", state["source"], worker["project"]],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        text = result.stdout.decode()
        print(text if text.strip() else "  (no change)")


def stop(args, quiet=False):
    state = load_state(args.root)
    for worker in state.get("workers", []):
        if alive(worker["pid"]):
            try:
                os.killpg(os.getpgid(worker["pid"]), signal.SIGTERM)
            except OSError:
                try:
                    os.kill(worker["pid"], signal.SIGTERM)
                except OSError:
                    pass
            if not quiet:
                print("stopped worker %d (pid %d)" % (worker["index"], worker["pid"]))
    save_state(args.root, {"workers": []})


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["start", "status", "collect", "stop"])
    parser.add_argument("--project", default="demos/pool",
                        help="Project to copy, relative to the repository root.")
    parser.add_argument("--workers", type=int, default=3)
    parser.add_argument("--root", default=DEFAULT_ROOT)
    args = parser.parse_args()
    {"start": start, "status": status, "collect": collect, "stop": stop}[args.command](args)


if __name__ == "__main__":
    main()
