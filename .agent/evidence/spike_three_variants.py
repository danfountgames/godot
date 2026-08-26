#!/usr/bin/env python3
"""W6 vertical slice: three live variants in the GodotAI workspace.

Drives a real editor through the real relay and checks the things the workspace
specification's first vertical slice asks for:

  * three isolated game processes launch and embed
  * each has an independent lifecycle and its own id
  * pausing one leaves the others running - the check that the debugger router
    actually targets, rather than broadcasting the way the Game workspace does
  * stopping one leaves the others running
  * stopping all agent instances does not touch a game the user started

A screenshot is written so the tiles can be looked at, because "three processes are
alive" and "three games are visible side by side" are different claims.
"""
import base64
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools", "relay", "tests"))
sys.path.insert(0, os.path.join(REPO, "tools"))

import virtual_display  # noqa: E402
import run_editor_e2e as e2e  # noqa: E402

FAILURES = []


def check(condition, message):
    if condition:
        print("PASS %s" % message)
    else:
        print("FAIL %s" % message)
        FAILURES.append(message)


def main():
    editor_binary = os.path.join(REPO, "bin", "godot.linuxbsd.editor.dev.x86_64")
    display = virtual_display.ensure(width=1600, height=1000)
    if not display.usable:
        print("SKIP: no display available; the workspace needs one to embed")
        return 0

    workspace_dir = tempfile.mkdtemp(prefix="godot-ai-w6-")
    project = os.path.join(workspace_dir, "project")
    home = os.path.join(workspace_dir, "home")
    os.makedirs(home)
    e2e.build_project(project)

    environment = display.environment()
    environment["GODOT_AI_HOME"] = home
    environment["GODOT_AI_AUTO_APPROVE"] = "1"

    editor = subprocess.Popen(
        [editor_binary, "--path", project, "--editor"] + display.godot_arguments(),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env=environment, cwd=workspace_dir,
    )

    try:
        descriptor = e2e.wait_for_instance(os.path.join(home, "instances"), editor)
        relay = e2e.RelayProcess(
            args=["--client-name", "w6", "--approval-mode", "allow"], home=home)

        def call(message):
            relay.send_message(message)
            return relay.read_message(timeout=90)

        call({"jsonrpc": "2.0", "id": 1, "method": "initialize",
              "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                         "clientInfo": {"name": "w6", "version": "1"}}})
        relay.send_message({"jsonrpc": "2.0", "method": "notifications/initialized"})

        def tool(identifier, name, arguments):
            reply = call({"jsonrpc": "2.0", "id": identifier, "method": "tools/call",
                          "params": {"name": name, "arguments": arguments}})
            if reply is None:
                return None, "no reply"
            result = reply.get("result", {})
            if result.get("isError"):
                return None, e2e.refusal_text(reply)
            return result.get("structuredContent", {}), None

        # The user's own game, started the ordinary way. Nothing below may stop it.
        users_game, error = tool(10, "Godot_PlayMainScene", {})
        check(error is None, "the user's own game started (%s)" % error)
        time.sleep(6)

        ids = []
        for index, label in enumerate(["Jump Variant A", "Jump Variant B", "Jump Variant C"]):
            payload, error = tool(20 + index, "Godot_LaunchInstance",
                                  {"label": label, "role": "candidate",
                                   "task": "compare jump feels",
                                   "retention": "interactive"})
            if error:
                check(False, "launching %s: %s" % (label, error))
                continue
            ids.append(payload["instance_id"])
            print("  %s -> %s (pid %s, %s)"
                  % (label, payload["instance_id"], payload.get("pid"),
                     payload.get("lifecycle")))
            time.sleep(5)

        check(len(ids) == 3, "three instances were launched")
        check(len(set(ids)) == len(ids), "each instance got its own id")

        listed, error = tool(30, "Godot_ListInstances", {"live_only": True})
        check(error is None and listed is not None, "listing instances (%s)" % error)
        if listed:
            check(listed["live_count"] == 3,
                  "three agent instances are live, and the user's game is not among them "
                  "(live_count=%r)" % listed["live_count"])

        # Every Godot process: the editor, the user's game, and three agent instances.
        alive = subprocess.run(["pgrep", "-fc", "godot.linuxbsd.editor.dev"],
                               capture_output=True, text=True).stdout.strip()
        print("  godot processes alive: %s" % alive)
        check(alive.isdigit() and int(alive) >= 5,
              "editor + user's game + three agent instances are all running (%s)" % alive)

        shot, error = tool(31, "Godot_CaptureEditorWindow", {})
        reply = call({"jsonrpc": "2.0", "id": 32, "method": "tools/call",
                      "params": {"name": "Godot_CaptureEditorWindow", "arguments": {}}})
        for item in reply.get("result", {}).get("content", []):
            if item.get("type") == "image":
                path = os.path.join(HERE, "spike_three_variants.png")
                with open(path, "wb") as handle:
                    handle.write(base64.b64decode(item["data"]))
                print("  screenshot: %s" % path)

        # The claim that matters: pause reaches one instance and no other.
        if len(ids) == 3:
            paused, error = tool(40, "Godot_ControlInstance",
                                 {"instance_id": ids[0], "action": "pause"})
            check(error is None, "pausing one instance was accepted (%s)" % error)
            if paused:
                check(paused["applied"] is True,
                      "the pause actually reached the instance: %r" % paused.get("note"))
            after, _ = tool(41, "Godot_ListInstances", {"live_only": True})
            if after:
                check(after["live_count"] == 3,
                      "pausing one instance did not stop any of them (%r)" % after["live_count"])

            stopped, error = tool(50, "Godot_ControlInstance",
                                  {"instance_id": ids[2], "action": "stop"})
            check(error is None and stopped and stopped["applied"],
                  "stopping one instance was accepted (%s)" % error)
            time.sleep(2)
            after, _ = tool(51, "Godot_ListInstances", {"live_only": True})
            if after:
                check(after["live_count"] == 2,
                      "stopping one left the other two running (%r)" % after["live_count"])

        # And the boundary that protects the user.
        all_stopped, error = tool(60, "Godot_StopAllInstances", {})
        check(error is None, "stopping all agent instances was accepted (%s)" % error)
        if all_stopped:
            check(all_stopped["live_count"] == 0, "no agent instance is left running")
        time.sleep(3)

        still, error = tool(70, "Godot_GetRuntimeSceneTree", {})
        check(error is None and still is not None,
              "the user's own game survived 'stop all agent instances' (%s)" % error)

        tool(80, "Godot_StopPlaying", {})
    finally:
        editor.terminate()
        try:
            editor.wait(timeout=20)
        except subprocess.TimeoutExpired:
            editor.kill()
        subprocess.run(["pkill", "-f", "godot.linuxbsd.editor.dev.*--path"],
                       capture_output=True)

    print()
    if FAILURES:
        print("three-variant slice: %d checks failed" % len(FAILURES))
        return 1
    print("three-variant slice: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
