#!/usr/bin/env python3
"""End-to-end test: a real MCP client session against a real Godot editor.

Launches a headless editor on a scratch project, waits for it to advertise itself
through the instance registry, then drives initialize / tools/list / tools/call
through the real relay binary and checks the results against the project on disk.

This is the only test that exercises the whole stack; the relay suite and the
engine doctest cases each cover one side of the bridge.

Usage:
  python3 tools/relay/tests/run_editor_e2e.py [--editor <path-to-godot-binary>]
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from relay_harness import REPO_ROOT, RelayProcess  # noqa: E402

DEFAULT_EDITOR = os.path.join(REPO_ROOT, "bin", "godot.linuxbsd.editor.dev.x86_64")

PROJECT_GODOT = """config_version=5

[application]

config/name="AI E2E Project"
run/main_scene="res://scenes/main.tscn"
config/features=PackedStringArray("4.3")
"""

MAIN_SCENE = """[gd_scene format=3 uid="uid://bqxaie2e001"]

[node name="Main" type="Node2D"]

[node name="Player" type="Sprite2D" parent="."]

[node name="Hud" type="CanvasLayer" parent="."]
"""


class Failure(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


def build_project(root):
    os.makedirs(os.path.join(root, "scenes"), exist_ok=True)
    with open(os.path.join(root, "project.godot"), "w") as handle:
        handle.write(PROJECT_GODOT)
    with open(os.path.join(root, "scenes", "main.tscn"), "w") as handle:
        handle.write(MAIN_SCENE)
    with open(os.path.join(root, "notes.txt"), "w") as handle:
        handle.write("hello from a project text file\n")


def wait_for_instance(instances_dir, process, timeout=120.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if process.poll() is not None:
            raise Failure("the editor exited before advertising itself")
        entries = [e for e in os.listdir(instances_dir)] if os.path.isdir(instances_dir) else []
        if entries:
            with open(os.path.join(instances_dir, entries[0])) as handle:
                return json.load(handle)
        time.sleep(0.5)
    raise Failure("the editor never wrote an instance descriptor")


def run(editor_binary):
    if not os.path.exists(editor_binary):
        raise Failure("editor binary not found at %s" % editor_binary)

    workspace = tempfile.mkdtemp(prefix="godot-ai-e2e-")
    project = os.path.join(workspace, "project")
    home = os.path.join(workspace, "home")
    os.makedirs(home)
    build_project(project)

    environment = dict(os.environ)
    environment["GODOT_AI_HOME"] = home
    # Automation opt-in: skips the first-connection approval a human would give.
    environment["GODOT_AI_AUTO_APPROVE"] = "1"

    editor = subprocess.Popen(
        [editor_binary, "--headless", "--path", project, "--editor"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=environment,
        cwd=workspace,
    )

    relay = None
    try:
        descriptor = wait_for_instance(os.path.join(home, "instances"), editor)
        check(descriptor["port"] > 0, "descriptor has no port")
        check(os.path.realpath(descriptor["project_path"]) == os.path.realpath(project),
              "descriptor names the wrong project")
        print("PASS editor advertised itself on port %d" % descriptor["port"])

        relay = RelayProcess(
            args=["--client-name", "e2e", "--approval-mode", "allow"], home=home
        )

        def call(message, expect_reply=True):
            relay.send_message(message)
            if not expect_reply:
                return None
            reply = relay.read_message(timeout=20)
            check(reply is not None, "no reply to %s" % message.get("method"))
            return reply

        reply = call({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                      "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                                 "clientInfo": {"name": "e2e", "version": "1"}}})
        check(reply["result"]["protocolVersion"] == "2025-06-18", "protocol not negotiated")
        check(reply["result"]["capabilities"]["tools"]["listChanged"] is True,
              "listChanged capability missing")
        print("PASS initialize")

        call({"jsonrpc": "2.0", "method": "notifications/initialized"}, expect_reply=False)

        reply = call({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        names = [tool["name"] for tool in reply["result"]["tools"]]
        for expected in ("Godot_ListScenes", "Godot_OpenScene", "Godot_GetEditedSceneTree",
                         "Godot_ReadTextFile", "Godot_WriteTextFile", "Godot_SearchProject"):
            check(expected in names, "tools/list is missing %s" % expected)
        print("PASS tools/list (%d tools)" % len(names))

        reply = call({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                      "params": {"name": "Godot_ListScenes"}})
        scenes = reply["result"]["structuredContent"]["scenes"]
        check(scenes == ["res://scenes/main.tscn"], "unexpected scene list: %r" % scenes)
        print("PASS Godot_ListScenes")

        reply = call({"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                      "params": {"name": "Godot_SearchProject",
                                 "arguments": {"query": "Sprite2D"}}})
        matches = reply["result"]["structuredContent"]["matches"]
        check(len(matches) == 1 and matches[0]["path"] == "res://scenes/main.tscn",
              "search did not find the node type: %r" % matches)
        print("PASS Godot_SearchProject")

        reply = call({"jsonrpc": "2.0", "id": 5, "method": "tools/call",
                      "params": {"name": "Godot_OpenScene",
                                 "arguments": {"path": "res://scenes/main.tscn"}}})
        check(reply["result"]["structuredContent"]["root_name"] == "Main", "wrong scene root")
        print("PASS Godot_OpenScene")

        reply = call({"jsonrpc": "2.0", "id": 6, "method": "tools/call",
                      "params": {"name": "Godot_GetEditedSceneTree"}})
        nodes = reply["result"]["structuredContent"]["nodes"]
        types = {node["name"]: node["type"] for node in nodes}
        check(types.get("Player") == "Sprite2D" and types.get("Hud") == "CanvasLayer",
              "scene tree does not match the scene on disk: %r" % types)
        print("PASS Godot_GetEditedSceneTree")

        reply = call({"jsonrpc": "2.0", "id": 7, "method": "tools/call",
                      "params": {"name": "Godot_WriteTextFile",
                                 "arguments": {"path": "res://written.txt",
                                               "text": "written through MCP"}}})
        check(reply["result"]["structuredContent"]["created"] is True, "file not reported created")
        # The effect is checked on disk, not taken from the tool's own report.
        with open(os.path.join(project, "written.txt")) as handle:
            check(handle.read() == "written through MCP", "file content does not match")
        print("PASS Godot_WriteTextFile")

        reply = call({"jsonrpc": "2.0", "id": 8, "method": "tools/call",
                      "params": {"name": "Godot_ReadTextFile",
                                 "arguments": {"path": "res://../../etc/passwd"}}})
        check(reply["result"]["isError"] is True, "project escape was not refused")
        check("outside the project" in reply["result"]["content"][0]["text"],
              "escape refusal does not explain itself")
        print("PASS project escape refused")

        reply = call({"jsonrpc": "2.0", "id": 9, "method": "tools/call",
                      "params": {"name": "Godot_NoSuchTool"}})
        check(reply["error"]["code"] == -32601, "unknown tool is not method-not-found")
        print("PASS unknown tool rejected")

        reply = call({"jsonrpc": "2.0", "id": 10, "method": "tools/call",
                      "params": {"name": "Godot_ReadTextFile", "arguments": {"nope": 1}}})
        check(reply["error"]["code"] == -32602, "bad arguments are not invalid-params")
        print("PASS invalid arguments rejected")

        exit_code, stray_stdout, _ = relay.finish()
        check(exit_code == 0, "relay exited with %d" % exit_code)
        check(stray_stdout == "", "relay wrote non-protocol output to stdout")
        print("PASS relay shut down cleanly with a clean stdout")
        relay = None

    finally:
        if relay is not None:
            relay.cleanup()
        editor.terminate()
        try:
            editor.wait(timeout=20)
        except subprocess.TimeoutExpired:
            editor.kill()
        shutil.rmtree(workspace, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--editor", default=DEFAULT_EDITOR)
    args = parser.parse_args()
    try:
        run(args.editor)
    except Failure as failure:
        print("FAIL %s" % failure, file=sys.stderr)
        return 1
    print("\nend-to-end: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
