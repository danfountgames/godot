#!/usr/bin/env python3
"""Record a session in one editor, replay it in another.

This exists because the experience ledger's own rule says so, and the rule is right:
recording and replaying inside one process proves nothing about the *trace format*. Every
absolute path, every in-memory assumption, every field that happens to still be sitting in
a singleton survives a round trip that never leaves the process. S4 sat at IMPLEMENTED for
that reason alone.

So: editor A records a session against the running game and exits. Editor B starts on the
same project, finds the session on disk, and replays it. Nothing is shared between them
except the files.

    python3 tools/relay/tests/run_replay_two_editors.py [--editor <binary>] [--headless]

Skips rather than fails where there is no display: replay needs a game, and a game needs
somewhere to draw.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# tools/relay/tests -> tools/relay -> tools -> the repository root.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))

import virtual_display  # noqa: E402
from relay_harness import RelayProcess  # noqa: E402

# The fixture project, the editor launch and the failure type all already exist for the
# single-process run. Importing them keeps the two scripts describing the same project
# rather than two projects that drift apart.
import run_editor_e2e as e2e  # noqa: E402

DEFAULT_EDITOR = e2e.DEFAULT_EDITOR
SESSION_NAME = "Two Editor Handover"
SESSION_SLUG = "two-editor-handover"


class Editor:
    """One editor process, with a relay talking to it.

    Everything is torn down on exit, and the *editor* is what has to be gone before the
    second one starts: two editors on one project would share the session store while both
    were alive, which is exactly the thing this test must not accidentally rely on.
    """

    def __init__(self, binary, project, workspace, display, label):
        self.label = label
        self.home = tempfile.mkdtemp(prefix="godot-ai-two-editor-%s-" % label)
        environment = display.environment()
        environment["GODOT_AI_HOME"] = self.home
        environment["GODOT_AI_AUTO_APPROVE"] = "1"
        self.process = subprocess.Popen(
            [binary, "--path", project, "--editor"] + display.godot_arguments(),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env=environment, cwd=workspace)
        self.output = e2e.EditorOutput(self.process)
        self.relay = None

    def __enter__(self):
        descriptor = e2e.wait_for_instance(os.path.join(self.home, "instances"), self.process)
        e2e.check(descriptor["port"] > 0, "%s advertised no port" % self.label)
        self.relay = RelayProcess(args=["--client-name", "two-editor", "--approval-mode", "allow"],
                                  home=self.home)
        reply = self.call({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                           "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                                      "clientInfo": {"name": "two-editor", "version": "1"}}})
        e2e.check(reply["result"]["protocolVersion"] == "2025-06-18",
                  "%s did not negotiate the protocol" % self.label)
        return self

    def __exit__(self, kind, value, traceback):
        if kind is not None:
            tail = self.output.tail()
            if tail:
                print("\n--- %s's last %d line(s) ---\n%s\n--- end ---"
                      % (self.label, len(self.output.lines), tail), file=sys.stderr)
        if self.relay is not None:
            self.relay.cleanup()
        self.process.terminate()
        try:
            self.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=10)
        shutil.rmtree(self.home, ignore_errors=True)
        return False

    def call(self, message, timeout=150):
        self.relay.send_message(message)
        reply = self.relay.read_message(timeout=timeout)
        e2e.check(reply is not None, "%s did not answer %s" % (self.label, message.get("method")))
        return reply

    def tool(self, identifier, name, arguments=None):
        return self.call({"jsonrpc": "2.0", "id": identifier, "method": "tools/call",
                          "params": {"name": name, "arguments": arguments or {}}})


def play_and_settle(editor, identifier):
    """Starts the game and waits until it answers, so nothing below races the launch."""
    reply = editor.tool(identifier, "Godot_PlayMainScene")
    e2e.check(not e2e.refused(reply), "%s could not start the game: %s"
              % (editor.label, e2e.refusal_text(reply)))
    deadline = time.time() + 60
    while time.time() < deadline:
        probe = editor.tool(identifier + 1, "Godot_GetRuntimeSceneTree")
        if not e2e.refused(probe):
            return
        time.sleep(1.0)
    raise e2e.Failure("%s: the game never reported its scene tree" % editor.label)


def record_in_the_first(editor):
    play_and_settle(editor, 10)

    reply = editor.tool(20, "Godot_RecordSession", {"action": "start", "name": SESSION_NAME})
    e2e.check(not e2e.refused(reply), "recording did not start: %s" % e2e.refusal_text(reply))
    e2e.check(reply["result"]["structuredContent"]["session"] == SESSION_SLUG,
              "the session opened under an unexpected slug: %r"
              % reply["result"]["structuredContent"])

    # Something worth replaying: a click the fixture counts, so the assertion below is
    # about a value the game actually changed.
    for index in range(3):
        pressed = editor.tool(30 + index, "Godot_SendPointerInput",
                              {"x": 200, "y": 130, "action": "click"})
        e2e.check(not e2e.refused(pressed), "clicking failed: %s" % e2e.refusal_text(pressed))

    reply = editor.tool(40, "Godot_AssertRuntimeState",
                        {"node_path": "/root/Main/Hud/Target", "property": "press_count"})
    e2e.check(not e2e.refused(reply), "capturing an assertion failed: %s" % e2e.refusal_text(reply))
    recorded_presses = reply["result"]["structuredContent"]["value"]

    reply = editor.tool(50, "Godot_RecordSession", {"action": "stop"})
    e2e.check(not e2e.refused(reply), "stopping the recording failed: %s" % e2e.refusal_text(reply))
    stopped = reply["result"]["structuredContent"]
    e2e.check(stopped["event_count"] >= 3,
              "the trace did not capture the clicks: %r" % stopped)
    e2e.check(stopped["assertion_count"] == 1,
              "the assertion did not survive into the session: %r" % stopped)

    editor.tool(60, "Godot_StopPlaying")
    print("PASS the first editor recorded %d event(s) and an assertion"
          % stopped["event_count"])
    return recorded_presses


def replay_in_the_second(editor, recorded_presses):
    # Before anything is played: the session has to be *findable* by a process that never
    # saw it recorded. This is the half that a single-process run cannot check at all.
    reply = editor.tool(110, "Godot_ListSessions")
    e2e.check(not e2e.refused(reply), "listing sessions failed: %s" % e2e.refusal_text(reply))
    listed = {entry["slug"]: entry for entry in reply["result"]["structuredContent"]["sessions"]}
    e2e.check(SESSION_SLUG in listed,
              "the second editor cannot see the session the first recorded: %r" % sorted(listed))
    found = listed[SESSION_SLUG]
    e2e.check(found.get("event_count", 0) >= 3,
              "the trace did not survive the handover: %r" % found)
    e2e.check(found.get("assertion_count", 0) == 1,
              "the assertion did not survive the handover: %r" % found)
    print("PASS the second editor found the session on disk, with its trace and assertion")

    play_and_settle(editor, 120)

    reply = editor.tool(130, "Godot_ReplaySession",
                        {"name": SESSION_NAME, "timeout_seconds": 90})
    e2e.check(not e2e.refused(reply), "replaying failed: %s" % e2e.refusal_text(reply))
    replayed = reply["result"]["structuredContent"]
    e2e.check(replayed["verdict"] in ("passed", "failed", "indeterminate"),
              "the replay produced no usable verdict: %r" % replayed)
    e2e.check(replayed["events_injected"] >= 3,
              "the replay did not re-inject the recorded trace: %r" % replayed)
    e2e.check(replayed["assertions_checked"] == 1,
              "the replay did not check the recorded assertion: %r" % replayed)

    # This project's counter only goes up, and the second game started from zero, so the
    # assertion recorded in the first editor cannot match. That is the *right* answer, and
    # a divergence naming the property with both values is exactly the regression report
    # the feature exists to produce - so it is asserted rather than tolerated.
    e2e.check(replayed["verdict"] == "failed",
              "a counter recorded at %r in another process should not match here: %r"
              % (recorded_presses, replayed))
    divergence = replayed["first_divergence"]
    e2e.check(divergence["property"] == "press_count",
              "the divergence names the wrong property: %r" % divergence)
    e2e.check(divergence["expected"] == recorded_presses,
              "the divergence lost the value recorded by the other editor: %r" % divergence)
    print("PASS the second editor replayed it and reported the divergence with both values")

    editor.tool(140, "Godot_StopPlaying")


def run(binary, display):
    if not os.path.exists(binary):
        raise e2e.Failure("editor binary not found at %s" % binary)

    workspace = tempfile.mkdtemp(prefix="godot-ai-two-editor-")
    project = os.path.join(workspace, "project")
    e2e.build_project(project)
    print("project at %s" % project)

    try:
        with Editor(binary, project, workspace, display, "editor A") as first:
            recorded_presses = record_in_the_first(first)
        # The first editor is gone by here, and with it every singleton that might have
        # been holding the trace. What follows can only be reading files.
        print("the first editor has exited")

        with Editor(binary, project, workspace, display, "editor B") as second:
            replay_in_the_second(second, recorded_presses)
    finally:
        shutil.rmtree(workspace, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--editor", default=DEFAULT_EDITOR)
    parser.add_argument("--headless", action="store_true")
    arguments = parser.parse_args()

    if arguments.headless:
        print("SKIP replay across two editors: replay needs a running game, and a game "
              "needs somewhere to draw")
        return 0

    display = virtual_display.ensure(width=1280, height=800)
    if not display.usable:
        print("SKIP replay across two editors: no display is available (%s)" % display.display)
        return 0

    try:
        with display:
            run(arguments.editor, display)
    except e2e.Failure as failure:
        print("FAIL %s" % failure, file=sys.stderr)
        return 1
    print("\nreplay across two editors: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
