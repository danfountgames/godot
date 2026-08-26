#!/usr/bin/env python3
"""A virtual display for environments that have none.

An agent working in a container can build the editor, run its tests and drive every
MCP tool that reads or writes files - but it cannot see anything. `Godot_CaptureViewport`
refuses, `Godot_AskUser` has no dialog to click, and a launched game dies before it can
report its scene tree. The visual half of the toolset becomes unverifiable, which is
exactly the half that most needs verifying.

This module removes that gap: it starts an X server that renders into memory, points
the software OpenGL stack at it, and hands back the environment a Godot editor needs to
draw for real. Nothing about the editor changes - it opens a genuine display, renders
through Mesa's llvmpipe, and a screenshot taken through it shows what a user would see.

Use it three ways:

    # As a library, around anything that launches the editor.
    from virtual_display import virtual_display
    with virtual_display() as display:
        subprocess.run(command, env=display.environment())

    # As a command wrapper.
    python3 tools/virtual_display.py -- bin/godot... --path proj --editor

    # As a check, before deciding whether visual verification is possible at all.
    python3 tools/virtual_display.py --probe

`ensure()` is the forgiving entry point: it reuses a display that already works, starts
a virtual one when it can, and reports plainly when it cannot. It never raises to say
"no display" - that is an answer, not a failure.
"""

import argparse
import contextlib
import json
import os
import shutil
import signal
import subprocess
import sys
import time

# Xvfb renders into memory with no hardware behind it, which is the whole point here.
# Xephyr and Xvnc would also work but both want something to display *into*.
X_SERVER = "Xvfb"

# The lowest display number worth trying. :0 is a real seat if there is one, and the
# low numbers are where anything else on the machine will have landed.
FIRST_DISPLAY = 90
LAST_DISPLAY = 129

DEFAULT_WIDTH = 1920
DEFAULT_HEIGHT = 1080
# 24-bit is what Godot's GL compatibility renderer expects; 16 gives a usable but
# visibly banded screenshot, which defeats the purpose of taking one.
DEFAULT_DEPTH = 24

STARTUP_TIMEOUT = 20.0

# Godot's default renderer is Vulkan, and llvmpipe does not offer it. Software GL is
# slow but correct, and correctness is what a screenshot is for.
SOFTWARE_GL_ENVIRONMENT = {
    "LIBGL_ALWAYS_SOFTWARE": "1",
    "GALLIUM_DRIVER": "llvmpipe",
    "__GLX_VENDOR_LIBRARY_NAME": "mesa",
}

# Passed to the editor so it does not try to bring up Vulkan on a software stack.
GODOT_RENDERING_ARGUMENTS = ["--rendering-driver", "opengl3"]


class VirtualDisplayError(Exception):
    """Raised only when a display was demanded and could not be supplied."""


def _socket_path(number):
    return "/tmp/.X11-unix/X%d" % number


def _lock_path(number):
    return "/tmp/.X%d-lock" % number


def _lock_holder_alive(number):
    """True when the process named in `/tmp/.X<n>-lock` still exists.

    The lock file holds the X server's pid, space-padded. A lock naming a pid that has
    gone is debris.
    """
    try:
        with open(_lock_path(number)) as handle:
            pid = int(handle.read().strip())
    except (OSError, ValueError):
        # Unreadable or malformed: assume it is real rather than stealing a live display.
        return True
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        # Someone else's process, so it is alive and not ours to reclaim.
        return True
    return True


def _reclaim_stale_display(number):
    """Remove the lock and socket for a display nothing is serving. Returns True if freed.

    Every run that ends without calling `stop()` - a killed test, a crashed script, an
    interpreter that simply exits - leaves a lock behind, and the number then looks
    permanently taken. Forty runs later there are no numbers left and the next run
    reports "all numbers in use" while no X server is running at all. That is exactly
    what happened here, so freeing debris is done on the way in rather than trusting
    every caller to clean up on the way out.
    """
    if _lock_holder_alive(number) or _display_answers(":%d" % number):
        return False
    freed = False
    for path in (_lock_path(number), _socket_path(number)):
        with contextlib.suppress(OSError):
            if os.path.exists(path):
                os.remove(path)
                freed = True
    return freed


def _display_is_free(number):
    if not os.path.exists(_socket_path(number)) and not os.path.exists(_lock_path(number)):
        return True
    return _reclaim_stale_display(number)


def _display_answers(display, environment=None):
    """True when something is actually listening on `display`.

    A `DISPLAY` variable in the environment proves nothing - a container image can
    export one that was never started, and the editor's failure then looks like a
    rendering bug rather than a missing server.
    """
    probe = shutil.which("xdpyinfo")
    merged = dict(environment or os.environ)
    merged["DISPLAY"] = display
    if probe:
        try:
            result = subprocess.run([probe, "-display", display],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    env=merged, timeout=10)
            return result.returncode == 0
        except (OSError, subprocess.TimeoutExpired):
            return False
    # Without xdpyinfo, the socket's existence is the best evidence available. It is
    # weaker - the socket outlives a crashed server - but it beats guessing.
    number = display.lstrip(":").split(".")[0]
    return number.isdigit() and os.path.exists(_socket_path(int(number)))


def _mesa_is_available():
    """Whether a software OpenGL driver is installed.

    Without it the X server still starts and the editor still opens a window, but GL
    initialisation fails and the editor exits - a confusing way to learn that a
    package is missing.
    """
    for root in ("/usr/lib/x86_64-linux-gnu/dri", "/usr/lib/dri", "/usr/lib64/dri",
                 "/usr/lib/aarch64-linux-gnu/dri"):
        if not os.path.isdir(root):
            continue
        for entry in os.listdir(root):
            if entry.startswith(("swrast", "kms_swrast", "llvmpipe", "zink")):
                return True
    return False


def probe():
    """Reports what visual verification is possible here, without changing anything."""
    existing = os.environ.get("DISPLAY", "")
    report = {
        "platform": sys.platform,
        "existing_display": existing,
        "existing_display_works": bool(existing) and _display_answers(existing),
        "x_server": shutil.which(X_SERVER),
        "software_gl": _mesa_is_available(),
    }
    if report["existing_display_works"]:
        report["verdict"] = "usable"
        report["reason"] = "the display already in the environment answers"
    elif sys.platform not in ("linux", "linux2"):
        report["verdict"] = "native"
        report["reason"] = "this platform has its own window server; no virtual display needed"
    elif not report["x_server"]:
        report["verdict"] = "unavailable"
        report["reason"] = ("%s is not installed; install it with "
                            "`apt-get install -y xvfb` (add `mesa-utils "
                            "libgl1-mesa-dri` for software OpenGL)" % X_SERVER)
    elif not report["software_gl"]:
        report["verdict"] = "degraded"
        report["reason"] = ("a virtual display can be started, but no software OpenGL "
                            "driver was found; install `libgl1-mesa-dri` or the editor "
                            "will fail to initialise a renderer")
    else:
        report["verdict"] = "startable"
        report["reason"] = "a virtual display can be started on demand"
    return report


class VirtualDisplay:
    """A display an editor can draw into, virtual or otherwise.

    `virtual` distinguishes a server this object started and owns from one that was
    already there. Callers that only want to know "can I verify pixels" should look at
    `usable`; callers that need to clean up should just use the context manager.
    """

    def __init__(self, display, width, height, depth, process=None, log_path=None):
        self.display = display
        self.width = width
        self.height = height
        self.depth = depth
        self._process = process
        self._owned = process is not None
        self._stopped = False
        self.log_path = log_path

    @property
    def virtual(self):
        return self._owned

    @property
    def usable(self):
        return bool(self.display) and not self._stopped

    def environment(self, base=None):
        """The environment a child process needs to render into this display."""
        result = dict(os.environ if base is None else base)
        if not self.display:
            return result
        result["DISPLAY"] = self.display
        for key, value in SOFTWARE_GL_ENVIRONMENT.items():
            result.setdefault(key, value)
        return result

    def godot_arguments(self):
        """Editor arguments matching this display: a renderer software GL can serve."""
        return list(GODOT_RENDERING_ARGUMENTS) if self.display else ["--headless"]

    def is_running(self):
        """True while this display is still serving. An existing display is assumed to
        outlive us; only a server we started can be watched for exit."""
        if self._stopped:
            return False
        if self._process is not None:
            return self._process.poll() is None
        return bool(self.display)

    def stop(self):
        """Stops the server, if this object started one. A display that was already
        there is left alone - we did not start it, so it is not ours to take down."""
        if self._process is None:
            return
        process = self._process
        self._process = None
        self._stopped = True
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
        # Xvfb usually removes these itself, but a killed server leaves them behind and
        # the number then looks permanently taken to the next run.
        number = int(self.display.lstrip(":").split(".")[0])
        for path in (_lock_path(number), _socket_path(number)):
            with contextlib.suppress(OSError):
                if os.path.exists(path) and not _display_answers(self.display):
                    os.remove(path)

    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        self.stop()
        return False

    def __repr__(self):
        if not self.display:
            return "<VirtualDisplay none>"
        return "<VirtualDisplay %s %dx%d%s>" % (
            self.display, self.width, self.height, " virtual" if self.virtual else " existing")


def start(width=DEFAULT_WIDTH, height=DEFAULT_HEIGHT, depth=DEFAULT_DEPTH, log_path=None):
    """Starts a new virtual display and returns it. Raises if it cannot.

    Use `ensure()` unless you specifically want a fresh server even though the
    environment already has one.
    """
    server = shutil.which(X_SERVER)
    if not server:
        raise VirtualDisplayError(probe()["reason"])

    log = open(log_path, "wb") if log_path else subprocess.DEVNULL
    try:
        last_error = None
        for number in range(FIRST_DISPLAY, LAST_DISPLAY + 1):
            if not _display_is_free(number):
                continue
            display = ":%d" % number
            command = [
                server, display,
                "-screen", "0", "%dx%dx%d" % (width, height, depth),
                # No TCP: this display exists for one machine's own processes, and
                # opening a port for it would be a needless listener.
                "-nolisten", "tcp",
            ]
            process = subprocess.Popen(
                command,
                stdout=log if log_path else subprocess.DEVNULL,
                stderr=subprocess.STDOUT if log_path else subprocess.DEVNULL,
                # Detach from the caller's process group so a Ctrl-C aimed at the
                # wrapped command does not race us to the server.
                start_new_session=True,
            )
            if _wait_until_ready(process, display):
                return VirtualDisplay(display, width, height, depth, process, log_path)

            # Losing the race for a display number is normal when runs overlap; only
            # the last failure is worth reporting if every number fails.
            last_error = "%s exited with %s on %s" % (X_SERVER, process.poll(), display)
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
        raise VirtualDisplayError(
            "could not start %s on displays :%d-:%d (%s)"
            % (X_SERVER, FIRST_DISPLAY, LAST_DISPLAY, last_error or "all numbers in use"))
    finally:
        if log_path:
            log.close()


def _wait_until_ready(process, display):
    deadline = time.time() + STARTUP_TIMEOUT
    while time.time() < deadline:
        if process.poll() is not None:
            return False
        if _display_answers(display):
            return True
        time.sleep(0.1)
    return False


def ensure(width=DEFAULT_WIDTH, height=DEFAULT_HEIGHT, depth=DEFAULT_DEPTH,
           required=False, log_path=None, quiet=False):
    """Returns a display to render into, starting one only if needed.

    Reuses whatever `DISPLAY` already names when it answers, so this is safe to call on
    a developer's desktop. When no display can be supplied it returns an unusable
    `VirtualDisplay` rather than raising, unless `required` is set - a caller that can
    still do useful work headless should not have to catch an exception to find out.
    """
    existing = os.environ.get("DISPLAY", "")
    if existing and _display_answers(existing):
        return VirtualDisplay(existing, width, height, depth)

    try:
        display = start(width=width, height=height, depth=depth, log_path=log_path)
    except VirtualDisplayError as error:
        if required:
            raise
        if not quiet:
            print("virtual display unavailable: %s" % error, file=sys.stderr)
        return VirtualDisplay("", width, height, depth)

    if not quiet:
        detail = "" if _mesa_is_available() else " (no software OpenGL driver found)"
        print("virtual display %s at %dx%d%s" % (display.display, width, height, detail),
              file=sys.stderr)
    return display


@contextlib.contextmanager
def virtual_display(**kwargs):
    """Context-manager form of `ensure()`; stops the server it started, if any."""
    display = ensure(**kwargs)
    try:
        yield display
    finally:
        display.stop()


def _run_command(command, display):
    """Runs `command` under `display`, forwarding its exit status and signals."""
    child = subprocess.Popen(command, env=display.environment())

    def forward(signum, _frame):
        with contextlib.suppress(OSError):
            child.send_signal(signum)

    previous = {}
    for signum in (signal.SIGINT, signal.SIGTERM):
        previous[signum] = signal.signal(signum, forward)
    try:
        return child.wait()
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Run a command under a virtual display, or report whether one is possible.")
    parser.add_argument("--probe", action="store_true",
                        help="print what visual verification is possible here and exit")
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    parser.add_argument("--depth", type=int, default=DEFAULT_DEPTH)
    parser.add_argument("--reuse", action="store_true",
                        help="use the DISPLAY already in the environment when it answers")
    parser.add_argument("--log", metavar="PATH", help="write the X server's output here")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="command to run, after a -- separator")
    args = parser.parse_args(argv)

    if args.probe:
        print(json.dumps(probe(), indent=2))
        return 0

    command = args.command[1:] if args.command[:1] == ["--"] else args.command

    starter = ensure if args.reuse else start
    keywords = {"width": args.width, "height": args.height, "depth": args.depth,
                "log_path": args.log}
    if args.reuse:
        keywords["required"] = True
    try:
        display = starter(**keywords)
    except VirtualDisplayError as error:
        print("error: %s" % error, file=sys.stderr)
        return 2

    with display:
        if not command:
            # No command: hold the display open and tell the caller how to use it, so a
            # human or an agent can point their own processes at it.
            print("DISPLAY=%s" % display.display)
            for key, value in SOFTWARE_GL_ENVIRONMENT.items():
                print("%s=%s" % (key, value))
            print("holding the display open; interrupt to stop it", file=sys.stderr)
            try:
                while display.is_running():
                    time.sleep(0.5)
            except KeyboardInterrupt:
                pass
            return 0
        return _run_command(command, display)


if __name__ == "__main__":
    sys.exit(main())
