#!/usr/bin/env python3
"""Verifies the relay builds and works from a clean checkout.

Local builds accumulate state - a stale object file, a binary from three commits ago,
an untracked header. This exports the tracked tree to a temporary directory, builds
the relay there with nothing else present, and runs its test suite against that
binary, so "it builds from a fresh clone" is a checked fact rather than an assumption.
"""

import os
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def run(command, cwd, env=None):
    result = subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=900)
    return result.returncode, result.stdout.decode()


def main():
    workspace = tempfile.mkdtemp(prefix="godot-ai-clean-")
    checkout = os.path.join(workspace, "checkout")
    try:
        # git archive gives exactly the tracked tree: no build output, no local files.
        os.makedirs(checkout)
        archive = os.path.join(workspace, "tree.tar")
        code, output = run(["git", "archive", "-o", archive, "HEAD"], REPO_ROOT)
        if code != 0:
            print("FAIL could not export the tracked tree:\n%s" % output, file=sys.stderr)
            return 1
        code, output = run(["tar", "-xf", archive, "-C", checkout], workspace)
        if code != 0:
            print("FAIL could not unpack the tree:\n%s" % output, file=sys.stderr)
            return 1

        if os.path.exists(os.path.join(checkout, "bin")):
            print("FAIL the tracked tree contains a bin/ directory", file=sys.stderr)
            return 1
        print("PASS exported a clean tracked tree")

        code, output = run(["tools/relay/build.sh"], checkout)
        if code != 0:
            print("FAIL the relay does not build from a clean checkout:\n%s" % output,
                  file=sys.stderr)
            return 1
        binary = os.path.join(checkout, "bin", "godot-ai-relay")
        if not os.path.exists(binary):
            print("FAIL the build produced no binary", file=sys.stderr)
            return 1
        print("PASS the relay builds from a clean checkout")

        # Run that binary's own suite, from that checkout, so the test harness is the
        # one that shipped rather than the one in the working tree.
        code, output = run([sys.executable, "tools/relay/tests/run_tests.py"], checkout)
        if code != 0:
            print("FAIL the relay suite fails from a clean checkout:\n%s" % output,
                  file=sys.stderr)
            return 1
        print("PASS the relay suite passes from a clean checkout")

        # The virtual display is what lets a machine with no screen verify the visual
        # tools, so a checkout that cannot start one is missing something that matters.
        code, output = run([sys.executable, "tools/tests/run_tests.py"], checkout)
        if code != 0:
            print("FAIL the tooling suite fails from a clean checkout:\n%s" % output,
                  file=sys.stderr)
            return 1
        print("PASS the tooling suite passes from a clean checkout")

        code, output = run(["tools/relay/package.sh", "--output",
                            os.path.join(workspace, "package")], checkout)
        if code != 0:
            print("FAIL packaging does not work from a clean checkout:\n%s" % output,
                  file=sys.stderr)
            return 1
        for required in ("bin/godot-ai-relay", "LICENSE.txt", "COPYRIGHT.txt",
                         "INSTALL.md", "README.md", "skills/scene-cleanup/SKILL.md"):
            path = os.path.join(workspace, "package", required)
            if not os.path.exists(path):
                print("FAIL the package is missing %s" % required, file=sys.stderr)
                return 1
        print("PASS the package contains the binary, the notices and the example skill")

    finally:
        shutil.rmtree(workspace, ignore_errors=True)

    print("\nclean checkout: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
