#!/usr/bin/env python3
"""End-to-end test of the editor's own user interface.

`run_editor_e2e.py` drives the editor the way an AI client does: over the protocol.
This drives it the way a person does - keyboard and pointer, through the command
palette and the approvals dialog - and checks the results over the protocol.

That distinction matters for the approvals dialog in particular. Its whole purpose is
that a human decides what an AI client may do, so a test that reached past the UI and
called the approval code directly would be testing everything except the part that has
to work. Here the only way the client gets approved is that something clicks the
button.

Needs a display and `xdotool`; skips cleanly without them.

Usage:
  python3 tools/relay/tests/run_editor_ui_e2e.py [--editor <path-to-godot-binary>]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from relay_harness import REPO_ROOT, RelayProcess  # noqa: E402

sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))

import virtual_display  # noqa: E402
from run_editor_e2e import (  # noqa: E402
    DEFAULT_EDITOR,
    Failure,
    build_project,
    check,
    refusal_text,
    refused,
    visible_windows,
    wait_for_instance,
    window_geometry,
    xdotool,
)

CLIENT_NAME = "ui-e2e"

# Godot's own shortcut for the editor command palette.
PALETTE_SHORTCUT = "ctrl+shift+p"

# Where the palette's search field sits inside its window. The palette is Godot's, not
# ours, so this is the one offset here that another project's layout could move.
PALETTE_SEARCH_Y = 22

# The approvals dialog's first row, measured from the top of its window: a summary
# label, then the column headers, then the rows. Only the *first* row is ever clicked -
# each decision removes the row that prompted it, so whatever needs deciding next moves
# up to the top - which keeps this to a single offset rather than a layout model.
FIRST_ROW_Y = 62
ROW_PROBE_OFFSETS = (0, 6, -6, 12, -12, 18)
ACTION_COLUMN_INSET = 16


def require_tools():
    """Returns a usable display, or None when this machine cannot run these checks."""
    if not shutil.which("xdotool"):
        print("SKIP editor UI checks: xdotool is not installed "
              "(apt-get install -y xdotool)")
        return None
    display = virtual_display.ensure(width=1280, height=800, quiet=True)
    if not display.usable:
        print("SKIP editor UI checks: no display is available "
              "(apt-get install -y xvfb x11-utils libgl1-mesa-dri)")
        return None
    return display


def save_screenshot(display, path):
    """Saves what is on screen. A UI test that only says "the click missed" is a bad
    UI test; this is how the next person sees what was actually there."""
    if not (shutil.which("xwd") and shutil.which("convert")):
        return None
    environment = dict(os.environ)
    environment["DISPLAY"] = display
    raw = path + ".xwd"
    try:
        subprocess.run(["xwd", "-root", "-out", raw], env=environment, check=True, timeout=60)
        subprocess.run(["convert", "xwd:" + raw, path], env=environment, check=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    finally:
        if os.path.exists(raw):
            os.remove(raw)
    return path


def focus(display, window):
    """Points X input focus at `window`.

    There is no window manager on a virtual display, so nothing hands focus to a
    window when the one that had it is destroyed. After a popup closes, keystrokes go
    to a window that no longer exists and are silently lost - which looks exactly like
    a shortcut that stopped working.
    """
    xdotool(display, "windowfocus", "--sync", window)
    time.sleep(0.3)


def open_palette(display, editor_window):
    """Opens the command palette and returns its window.

    The shortcut is occasionally swallowed - the editor may be mid-layout, or a popup
    the previous step left behind is eating it - so this clears the way and asks again
    rather than failing on one missed keystroke.
    """
    for attempt in range(3):
        if attempt:
            xdotool(display, "key", "Escape")
            time.sleep(1.0)
        focus(display, editor_window)
        before = visible_windows(display)
        xdotool(display, "key", PALETTE_SHORTCUT)
        deadline = time.time() + 8
        while time.time() < deadline:
            appeared = visible_windows(display) - before
            if appeared:
                time.sleep(0.8)
                return appeared.pop()
            time.sleep(0.3)
    raise Failure("the command palette did not open")


def run_palette_command(display, editor_window, query):
    """Finds a command by typing part of its name, and runs it.

    Clicking the search field first is not optional: the palette opens with focus
    elsewhere, and characters go to whatever holds focus, so typing into thin air
    silently does nothing.
    """
    palette = open_palette(display, editor_window)
    focus(display, palette)
    box = window_geometry(display, palette)
    xdotool(display, "mousemove", str(box["X"] + box["WIDTH"] // 2),
            str(box["Y"] + PALETTE_SEARCH_Y), "click", "1")
    time.sleep(0.4)
    xdotool(display, "type", "--delay", "60", query)
    time.sleep(1.2)
    xdotool(display, "key", "Return")
    time.sleep(1.5)


def open_approvals_dialog(display, editor_window):
    """Runs the palette command that shows the approvals dialog, and returns it."""
    before = visible_windows(display)
    run_palette_command(display, editor_window, "Clients and Skills")
    deadline = time.time() + 15
    while time.time() < deadline:
        appeared = {w for w in visible_windows(display) - before
                    if window_geometry(display, w).get("WIDTH", 0) >= 560}
        if appeared:
            time.sleep(0.8)
            return appeared.pop()
        time.sleep(0.3)
    raise Failure("the approvals dialog did not open from the command palette")


def click_first_action(display, window, confirm):
    """Clicks the action button on the dialog's first row until `confirm()` agrees.

    `confirm` is what actually decides success - the point of this test is the effect
    of the click (a client that may now connect, a skill that may now be read), not the
    pixel it landed on.
    """
    box = window_geometry(display, window)
    x = box["X"] + box["WIDTH"] - ACTION_COLUMN_INSET
    for offset in ROW_PROBE_OFFSETS:
        xdotool(display, "mousemove", str(x), str(box["Y"] + FIRST_ROW_Y + offset), "click", "1")
        time.sleep(0.8)
        if confirm():
            return
    raise Failure("no click on the dialog's first row had any effect (geometry %r)" % box)


def close_dialog(display, window):
    xdotool(display, "key", "Escape")
    time.sleep(0.8)
    if window in visible_windows(display):
        # Escape is handled by the dialog itself; if it is somehow still up, say so
        # rather than letting the next step click through a window that is in the way.
        raise Failure("the approvals dialog did not close")


def connect(home, expect_approved):
    """Starts a relay and completes (or fails) the handshake, returning it."""
    relay = RelayProcess(args=["--client-name", CLIENT_NAME, "--approval-mode", "allow"],
                         home=home)
    relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                        "params": {"protocolVersion": "2025-06-18",
                                   # Offering sampling is what lets the editor's chat
                                   # panel borrow this client's model.
                                   "capabilities": {"sampling": {}},
                                   "clientInfo": {"name": CLIENT_NAME, "version": "1"}}})
    reply = relay.read_message(timeout=25)
    if reply is None:
        relay.cleanup()
        raise Failure("no reply to initialize")
    approved = "error" not in reply
    if approved != expect_approved:
        relay.cleanup()
        raise Failure("client approval state is wrong: expected approved=%s, got %r"
                      % (expect_approved, reply))
    if approved:
        relay.send_message({"jsonrpc": "2.0", "method": "notifications/initialized"})
    return relay


def run(editor_binary, display):
    if not os.path.exists(editor_binary):
        raise Failure("editor binary not found at %s" % editor_binary)

    workspace = tempfile.mkdtemp(prefix="godot-ai-ui-e2e-")
    project = os.path.join(workspace, "project")
    home = os.path.join(workspace, "home")
    settings = os.path.join(workspace, "editor-settings")
    os.makedirs(home)
    os.makedirs(settings)
    build_project(project)

    environment = display.environment()
    environment["GODOT_AI_HOME"] = home
    # No GODOT_AI_AUTO_APPROVE: the whole point is that approval happens by hand.
    environment.pop("GODOT_AI_AUTO_APPROVE", None)
    # Approvals are stored in the editor's settings, and this test writes them, so it
    # gets its own settings directory rather than editing the developer's.
    for variable in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
        environment[variable] = settings

    # To a file rather than a pipe: a UI test spends most of its time not reading, and
    # a full pipe buffer would wedge the editor it is trying to drive.
    log_path = os.path.join(workspace, "editor.log")
    log = open(log_path, "wb")
    editor = subprocess.Popen(
        [editor_binary, "--path", project, "--editor"] + display.godot_arguments(),
        stdout=log, stderr=subprocess.STDOUT,
        env=environment, cwd=workspace)

    relay = None
    d = display.display
    failure_shot = os.path.join(tempfile.gettempdir(), "godot-ai-ui-e2e-failure.png")

    def require_editor_alive(what):
        if editor.poll() is None:
            return
        with open(log_path, "rb") as handle:
            tail = handle.read().decode("utf-8", "replace").strip().splitlines()[-25:]
        raise Failure("the editor exited (status %s) before %s:\n%s"
                      % (editor.poll(), what, "\n".join(tail)))

    try:
        wait_for_instance(os.path.join(home, "instances"), editor)
        # The editor has to have drawn before it can be clicked.
        time.sleep(5)
        windows = visible_windows(d)
        check(len(windows) == 1,
              "expected exactly one window before anything is opened, saw %r" % windows)
        editor_window = windows.pop()

        # --- an unapproved client is refused ----------------------------------
        relay = connect(home, expect_approved=False)
        relay.cleanup()
        relay = None
        print("PASS an unapproved client is refused by the editor")

        # --- U1: the palette runs our command ---------------------------------
        dialog = open_approvals_dialog(d, editor_window)
        print("PASS the command palette opened and ran 'Clients and Skills'")

        # --- U2: approving the client happens by clicking -----------------------
        approved = {}

        def client_can_connect():
            try:
                session = connect(home, expect_approved=True)
            except Failure:
                return False
            approved["relay"] = session
            return True

        click_first_action(d, dialog, client_can_connect)
        relay = approved["relay"]
        print("PASS clicking the dialog approved the client, which could then connect")

        close_dialog(d, dialog)

        def call(message):
            relay.send_message(message)
            reply = relay.read_message(timeout=25)
            check(reply is not None, "no reply to %s" % message.get("method"))
            return reply

        # --- U2: a skill stays untrusted until the same dialog says otherwise ---
        reply = call({"jsonrpc": "2.0", "id": 10, "method": "tools/call",
                      "params": {"name": "Godot_ReadSkill",
                                 "arguments": {"name": "scene-cleanup"}}})
        check(refused(reply), "an unapproved skill was readable")
        print("PASS a discovered skill is not readable before it is allowed")

        dialog = open_approvals_dialog(d, editor_window)

        def skill_is_readable():
            probe = call({"jsonrpc": "2.0", "id": 11, "method": "tools/call",
                          "params": {"name": "Godot_ReadSkill",
                                     "arguments": {"name": "scene-cleanup"}}})
            return not refused(probe)

        click_first_action(d, dialog, skill_is_readable)
        print("PASS clicking the dialog allowed the skill, which could then be read")
        close_dialog(d, dialog)

        # --- O1: the chat panel borrows this client's model ---------------------
        # The editor has no model. It asks whichever client is attached to run one, so
        # a round trip here is the whole feature: the panel sends, this test answers as
        # the client would, and the answer has to become part of the conversation.
        run_palette_command(d, editor_window, "Godot AI: Chat")
        # The palette window is gone now, and nothing hands its input focus back, so
        # typing would go to a destroyed window. The panel has Godot focus; the editor
        # window needs the X focus.
        focus(d, editor_window)
        xdotool(d, "type", "--delay", "40", "what scenes are in this project")
        time.sleep(0.6)
        xdotool(d, "key", "Return")

        request = relay.read_message(timeout=30)
        check(request is not None, "the chat panel sent nothing to the client")
        check(request.get("method") == "sampling/createMessage",
              "expected a sampling request, got %r" % request.get("method"))
        conversation = request["params"]["messages"]
        check(conversation[-1]["content"]["text"] == "what scenes are in this project",
              "the sampling request does not carry the typed prompt: %r" % conversation[-1])
        check("systemPrompt" in request["params"], "the sampling request has no system prompt")
        print("PASS the chat panel asked the client to run a model")

        relay.send_message({"jsonrpc": "2.0", "id": request["id"],
                            "result": {"role": "assistant", "model": "test-model",
                                       "content": {"type": "text",
                                                   "text": "main.tscn, and nothing else"}}})
        time.sleep(2)

        # The answer is only real if the conversation kept it: send a second turn and
        # look at what the editor now considers the history.
        focus(d, editor_window)
        xdotool(d, "type", "--delay", "40", "and how many nodes")
        time.sleep(0.6)
        xdotool(d, "key", "Return")

        second = relay.read_message(timeout=30)
        check(second is not None and second.get("method") == "sampling/createMessage",
              "the second chat turn was not sent: %r" % second)
        texts = [entry["content"]["text"] for entry in second["params"]["messages"]]
        check("main.tscn, and nothing else" in texts,
              "the client's answer did not become part of the conversation: %r" % texts)
        check(texts[-1] == "and how many nodes", "the second prompt is not last: %r" % texts)
        print("PASS the client's answer became part of the conversation")

        # Leave nothing in flight for the steps below.
        relay.send_message({"jsonrpc": "2.0", "id": second["id"],
                            "result": {"role": "assistant", "model": "test-model",
                                       "content": {"type": "text", "text": "three"}}})
        time.sleep(1)

        # --- U1: the other palette commands ------------------------------------
        run_palette_command(d, editor_window, "Show Service Status")
        reply = call({"jsonrpc": "2.0", "id": 12, "method": "tools/call",
                      "params": {"name": "Godot_ReadOutputLog",
                                 "arguments": {"contains": "client(s) connected"}}})
        messages = reply["result"]["structuredContent"]["messages"]
        check(messages, "the status command wrote nothing to the output log")
        check("listening on" in messages[-1]["text"],
              "the status message does not report the service: %r" % messages[-1]["text"])
        print("PASS the palette's status command reported the running service")

        # Restarting drops every connection, so it goes last: what has to be true
        # afterwards is that the editor is serving again.
        relay.cleanup()
        relay = None
        require_editor_alive("the restart command could be run")
        run_palette_command(d, editor_window, "Restart Service")
        time.sleep(3)
        relay = connect(home, expect_approved=True)
        print("PASS the palette's restart command left the service usable")

        print("\neditor UI: all checks passed")
    except Failure:
        # While the editor is still up: cleanup below would leave a black screen.
        if save_screenshot(d, failure_shot):
            print("screen at the time of failure: %s" % failure_shot, file=sys.stderr)
        require_editor_alive("the failing step")
        raise
    finally:
        if relay is not None:
            relay.cleanup()
        editor.terminate()
        try:
            editor.wait(timeout=20)
        except subprocess.TimeoutExpired:
            editor.kill()
        log.close()
        shutil.copy(log_path, os.path.join(tempfile.gettempdir(), "godot-ai-ui-e2e-editor.log"))
        shutil.rmtree(workspace, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--editor", default=DEFAULT_EDITOR)
    args = parser.parse_args()

    display = require_tools()
    if display is None:
        return 0
    try:
        with display:
            run(args.editor, display)
    except Failure as failure:
        print("FAIL %s" % failure, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
