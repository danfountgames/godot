#!/usr/bin/env python3
"""Phase-zero spike: can the editor embed two game processes at once?

Reuses the end-to-end harness to build a project, start an editor on a virtual
display, and drive it over the real relay. The only thing under test is whether two
launched instances both embed, which the throwaway patch in editor/run/ makes the
editor attempt.

Evidence collected:
  * the editor's own output, which the patch prints to when it embeds the second pid
  * a screenshot of the editor window, so a human can see two games or one
  * the number of live child processes the editor reports
"""
import base64
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = "/home/user/godot"
sys.path.insert(0, os.path.join(REPO, "tools", "relay", "tests"))
sys.path.insert(0, os.path.join(REPO, "tools"))

import virtual_display  # noqa: E402
import run_editor_e2e as e2e  # noqa: E402

OUT = HERE


def main():
    editor_binary = os.path.join(REPO, "bin", "godot.linuxbsd.editor.dev.x86_64")
    display = virtual_display.ensure(width=1600, height=900)
    if not display.usable:
        print("SPIKE INCONCLUSIVE: no display available")
        return 2

    workspace = tempfile.mkdtemp(prefix="godot-ai-spike-")
    project = os.path.join(workspace, "project")
    home = os.path.join(workspace, "home")
    os.makedirs(home)
    e2e.build_project(project)

    # Ask the editor for two run instances. This is the same project metadata the
    # Run Instances dialog writes.
    meta_dir = os.path.join(project, ".godot", "editor")
    os.makedirs(meta_dir, exist_ok=True)
    with open(os.path.join(meta_dir, "project_metadata.cfg"), "w") as handle:
        handle.write(
            "[debug_options]\n\n"
            'multiple_instances_enabled=true\n'
            "run_instance_count=2\n\n"
            "[game_view]\n\n"
            "embed_on_play=true\n"
        )

    environment = display.environment()
    environment["GODOT_AI_HOME"] = home
    environment["GODOT_AI_AUTO_APPROVE"] = "1"

    editor = subprocess.Popen(
        [editor_binary, "--path", project, "--editor"] + display.godot_arguments(),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env=environment, cwd=workspace,
    )

    relay = None
    verdict = 1
    try:
        descriptor = e2e.wait_for_instance(os.path.join(home, "instances"), editor)
        print("editor up on port %d" % descriptor["port"])
        relay = e2e.RelayProcess(
            args=["--client-name", "spike", "--approval-mode", "allow"], home=home)

        def call(message):
            relay.send_message(message)
            return relay.read_message(timeout=60)

        call({"jsonrpc": "2.0", "id": 1, "method": "initialize",
              "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                         "clientInfo": {"name": "spike", "version": "1"}}})
        relay.send_message({"jsonrpc": "2.0", "method": "notifications/initialized"})

        print("pressing play...")
        reply = call({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                      "params": {"name": "Godot_PlayMainScene", "arguments": {}}})
        print("play reply isError=%s" % reply["result"].get("isError"))

        time.sleep(12)

        # How many game processes are actually alive?
        children = subprocess.run(
            ["pgrep", "-fc", "godot.linuxbsd.editor.dev.x86_64 --path"],
            capture_output=True, text=True).stdout.strip()
        print("godot processes matching the project: %s" % children)

        shot = call({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                     "params": {"name": "Godot_CaptureEditorWindow", "arguments": {}}})
        wrote = None
        for item in shot["result"].get("content", []):
            if item.get("type") == "image":
                wrote = os.path.join(OUT, "spike_two_embeds.png")
                with open(wrote, "wb") as handle:
                    handle.write(base64.b64decode(item["data"]))
        print("screenshot: %s" % wrote)

        log = call({"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                    "params": {"name": "Godot_ReadOutputLog",
                               "arguments": {"contains": "SPIKE"}}})
        messages = log["result"]["structuredContent"]["messages"]
        for entry in messages:
            print("EDITOR: %s" % entry["text"])
        verdict = 0 if messages else 3

        call({"jsonrpc": "2.0", "id": 5, "method": "tools/call",
              "params": {"name": "Godot_StopPlaying", "arguments": {}}})
    finally:
        if relay:
            relay.close()
        editor.terminate()
        try:
            editor.wait(timeout=20)
        except subprocess.TimeoutExpired:
            editor.kill()
    return verdict


if __name__ == "__main__":
    sys.exit(main())
