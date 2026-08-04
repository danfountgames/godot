#!/usr/bin/env python3
"""Tests for the repository's standalone tooling (currently the virtual display).

These start real X servers and run real processes against them; nothing is faked,
because the point of the module under test is that a display genuinely exists.

Where no X server is installed the tests that need one skip rather than fail - the
module's contract is that it degrades honestly, and a machine without Xvfb is exactly
where that contract matters.

Run with:  python3 tools/tests/run_tests.py [-k <name-substring>]
"""

import argparse
import os
import subprocess
import sys
import traceback

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))

import virtual_display  # noqa: E402

TESTS = []


class Skip(Exception):
    pass


def test(func):
    TESTS.append(func)
    return func


def assert_eq(actual, expected, what="value"):
    if actual != expected:
        raise AssertionError("%s: expected %r, got %r" % (what, expected, actual))


def assert_true(condition, what):
    if not condition:
        raise AssertionError(what)


def require_x_server():
    if not virtual_display.probe()["x_server"]:
        raise Skip("%s is not installed" % virtual_display.X_SERVER)


def without_display(environment=None):
    """A copy of the environment with any inherited DISPLAY removed."""
    result = dict(os.environ if environment is None else environment)
    result.pop("DISPLAY", None)
    return result


@test
def probe_reports_a_verdict_without_changing_anything():
    before = os.environ.get("DISPLAY")
    report = virtual_display.probe()
    assert_true(report["verdict"] in ("usable", "startable", "degraded", "unavailable", "native"),
                "unexpected verdict: %r" % report["verdict"])
    assert_true(bool(report["reason"]), "the probe gave no reason")
    assert_eq(os.environ.get("DISPLAY"), before, "probe modified DISPLAY")


@test
def start_yields_a_display_that_answers():
    require_x_server()
    with virtual_display.start(width=320, height=240) as display:
        assert_true(display.virtual, "a started display did not report itself as virtual")
        assert_true(display.usable, "a started display is not usable")
        assert_true(virtual_display._display_answers(display.display),
                    "nothing is listening on %s" % display.display)


@test
def a_stopped_display_stops_answering():
    require_x_server()
    display = virtual_display.start(width=320, height=240)
    number = display.display
    display.stop()
    assert_true(not virtual_display._display_answers(number),
                "%s still answers after stop" % number)
    assert_true(not display.is_running(), "a stopped display still reports as running")


@test
def stopping_twice_is_harmless():
    require_x_server()
    display = virtual_display.start(width=320, height=240)
    display.stop()
    display.stop()


@test
def two_displays_do_not_collide():
    require_x_server()
    with virtual_display.start(width=320, height=240) as first:
        with virtual_display.start(width=320, height=240) as second:
            assert_true(first.display != second.display,
                        "both servers claimed %s" % first.display)
            assert_true(virtual_display._display_answers(first.display),
                        "the first display stopped answering")
            assert_true(virtual_display._display_answers(second.display),
                        "the second display never answered")


@test
def the_environment_points_children_at_the_display():
    require_x_server()
    with virtual_display.start(width=320, height=240) as display:
        environment = display.environment(base=without_display())
        assert_eq(environment["DISPLAY"], display.display, "DISPLAY")
        assert_eq(environment["LIBGL_ALWAYS_SOFTWARE"], "1", "LIBGL_ALWAYS_SOFTWARE")
        # A caller that has already chosen a GL configuration keeps it; these are
        # defaults for an environment that has expressed no preference.
        chosen = display.environment(base=dict(without_display(), LIBGL_ALWAYS_SOFTWARE="0"))
        assert_eq(chosen["LIBGL_ALWAYS_SOFTWARE"], "0", "an explicit GL choice was overridden")


@test
def a_child_process_can_see_the_display():
    require_x_server()
    if not virtual_display.shutil.which("xdpyinfo"):
        raise Skip("xdpyinfo is not installed")
    with virtual_display.start(width=640, height=480) as display:
        result = subprocess.run(["xdpyinfo"], env=display.environment(base=without_display()),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        assert_eq(result.returncode, 0, "xdpyinfo failed: %s" % result.stdout[-400:])
        assert_true(b"640x480" in result.stdout,
                    "the display is not the requested size:\n%s" % result.stdout[-400:].decode())


@test
def ensure_reuses_a_display_that_already_works():
    require_x_server()
    with virtual_display.start(width=320, height=240) as existing:
        previous = os.environ.get("DISPLAY")
        os.environ["DISPLAY"] = existing.display
        try:
            reused = virtual_display.ensure(quiet=True)
            assert_eq(reused.display, existing.display, "ensure did not reuse the display")
            assert_true(not reused.virtual, "a reused display claimed to be virtual")
            # Stopping a display it does not own must not take the server down.
            reused.stop()
            assert_true(virtual_display._display_answers(existing.display),
                        "ensure stopped a display it did not start")
        finally:
            if previous is None:
                os.environ.pop("DISPLAY", None)
            else:
                os.environ["DISPLAY"] = previous


@test
def ensure_ignores_a_display_variable_that_names_nothing():
    require_x_server()
    previous = os.environ.get("DISPLAY")
    # A container image can export a DISPLAY that was never started; trusting it makes
    # the editor fail in a way that looks like a rendering bug.
    os.environ["DISPLAY"] = ":991"
    try:
        display = virtual_display.ensure(width=320, height=240, quiet=True)
        try:
            assert_true(display.usable, "ensure gave up on a dead DISPLAY instead of replacing it")
            assert_true(display.display != ":991", "ensure trusted a display that answers nothing")
            assert_true(display.virtual, "ensure did not start its own display")
        finally:
            display.stop()
    finally:
        if previous is None:
            os.environ.pop("DISPLAY", None)
        else:
            os.environ["DISPLAY"] = previous


@test
def ensure_degrades_instead_of_raising_when_it_cannot_help():
    if sys.platform not in ("linux", "linux2"):
        raise Skip("the host has a native window server and does not need Xvfb")
    previous = os.environ.get("DISPLAY")
    os.environ.pop("DISPLAY", None)
    real_which = virtual_display.shutil.which
    virtual_display.shutil.which = lambda name: None if name == virtual_display.X_SERVER \
        else real_which(name)
    try:
        display = virtual_display.ensure(quiet=True)
        assert_true(not display.usable, "a display appeared without an X server")
        assert_eq(display.godot_arguments(), ["--headless"], "arguments without a display")
        # The caller's environment must come back unpoisoned: no DISPLAY pointing at
        # something that does not exist.
        assert_true("DISPLAY" not in display.environment(base=without_display()),
                    "an unusable display still set DISPLAY")
        try:
            virtual_display.ensure(required=True, quiet=True)
            raise AssertionError("required=True did not raise")
        except virtual_display.VirtualDisplayError as error:
            assert_true("install" in str(error), "the error does not say how to fix it: %s" % error)
    finally:
        virtual_display.shutil.which = real_which
        if previous is not None:
            os.environ["DISPLAY"] = previous


@test
def godot_arguments_match_the_display():
    require_x_server()
    with virtual_display.start(width=320, height=240) as display:
        assert_eq(display.godot_arguments(), ["--rendering-driver", "opengl3"],
                  "arguments for a software-rendered display")


@test
def the_command_line_runs_a_command_under_a_display():
    require_x_server()
    script = "import os, sys; sys.exit(0 if os.environ.get('DISPLAY') else 3)"
    status = virtual_display.main(["--width", "320", "--height", "240",
                                   "--", sys.executable, "-c", script])
    assert_eq(status, 0, "the wrapped command did not see a display")


@test
def the_command_line_forwards_the_exit_status():
    require_x_server()
    status = virtual_display.main(["--width", "320", "--height", "240",
                                   "--", sys.executable, "-c", "import sys; sys.exit(7)"])
    assert_eq(status, 7, "exit status")


@test
def the_command_line_probe_prints_json():
    output = subprocess.run(
        [sys.executable, os.path.join(REPO_ROOT, "tools", "virtual_display.py"), "--probe"],
        stdout=subprocess.PIPE, timeout=60, check=True).stdout
    import json
    report = json.loads(output)
    assert_true("verdict" in report, "the probe output has no verdict: %r" % report)


# --- the export decision (O3) ------------------------------------------------
# The decision is that exported games get none of this: the tooling drives an editor,
# and a shipped game has no editor to drive. These check the decision holds, because
# "editor-only" is a claim about a build, not a comment.

def load_module_config():
    import importlib.util
    path = os.path.join(REPO_ROOT, "modules", "godot_ai", "config.py")
    spec = importlib.util.spec_from_file_location("godot_ai_config", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeEnv:
    def __init__(self, editor_build):
        self.editor_build = editor_build


@test
def the_module_refuses_to_build_into_anything_but_an_editor():
    config = load_module_config()
    # The platform matrix: the answer must not depend on which one is building.
    for platform_name in ("linuxbsd", "windows", "macos", "android", "ios", "web"):
        if config.can_build(FakeEnv(editor_build=False), platform_name):
            raise AssertionError("the module would build into a %s template" % platform_name)
        if not config.can_build(FakeEnv(editor_build=True), platform_name):
            raise AssertionError("the module refuses to build into the %s editor" % platform_name)


@test
def an_export_template_contains_none_of_the_tooling():
    binaries = []
    binary_dir = os.path.join(REPO_ROOT, "bin")
    if os.path.isdir(binary_dir):
        binaries = [os.path.join(binary_dir, name) for name in os.listdir(binary_dir)
                    if "template" in name and os.path.isfile(os.path.join(binary_dir, name))]
    if not binaries:
        raise Skip("no export template has been built (scons target=template_release)")

    # Reading the whole binary is cheaper than shelling out to `strings`, and works
    # the same on a machine that does not have it.
    for path in binaries:
        with open(path, "rb") as handle:
            contents = handle.read()
        for marker in (b"Godot_ManageNode", b"Godot_CaptureViewport", b"MCPService",
                       b"godot_ai"):
            if marker in contents:
                raise AssertionError("%s leaked into %s" % (marker.decode(), os.path.basename(path)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-k", "--filter", help="only run tests whose name contains this")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    selected = [t for t in TESTS if not args.filter or args.filter in t.__name__]
    failures = []
    skipped = 0
    for func in selected:
        name = func.__name__
        try:
            func()
            print("PASS %s" % name)
        except Skip as reason:
            skipped += 1
            print("SKIP %s: %s" % (name, reason))
        except Exception as error:  # noqa: BLE001 - test runner boundary
            failures.append((name, error, traceback.format_exc()))
            print("FAIL %s: %s" % (name, error))
            if args.verbose:
                print(failures[-1][2])

    print("\n%d passed, %d failed, %d skipped, %d total" % (
        len(selected) - len(failures) - skipped, len(failures), skipped, len(selected)))
    if failures:
        print("\nFailures:")
        for name, error, _ in failures:
            print("- %s: %s" % (name, error))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
