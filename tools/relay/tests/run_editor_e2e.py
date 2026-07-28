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


def refused(reply):
    """True when a tools/call was refused, either way the server can refuse.

    Argument-level mistakes come back as JSON-RPC errors (the client asked for
    something malformed); state-level refusals come back as a tool result with
    isError set (the request was well-formed but cannot be carried out).
    """
    if "error" in reply:
        return True
    return reply.get("result", {}).get("isError") is True


def install_example_skill(root):
    """Copies the shipped example skill into the project's discovery root.

    The skill that gets exercised here is the same file the repository ships, so
    "the example skill loads" is a fact about the artifact, not about a fixture.
    """
    source = os.path.join(REPO_ROOT, "misc", "godot_ai", "skills", "scene-cleanup")
    destination = os.path.join(root, "ai_skills", "scene-cleanup")
    shutil.copytree(source, destination)


def build_project(root):
    os.makedirs(os.path.join(root, "scenes"), exist_ok=True)
    with open(os.path.join(root, "project.godot"), "w") as handle:
        handle.write(PROJECT_GODOT)
    with open(os.path.join(root, "scenes", "main.tscn"), "w") as handle:
        handle.write(MAIN_SCENE)
    with open(os.path.join(root, "notes.txt"), "w") as handle:
        handle.write("hello from a project text file\n")
    install_example_skill(root)


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
                         "Godot_ReadTextFile", "Godot_WriteTextFile", "Godot_SearchProject",
                         "Godot_ManageNode", "Godot_UndoLastAction",
                         "Godot_ListSkills", "Godot_ReadSkill",
                         "Godot_ListCheckpoints", "Godot_RestoreCheckpoint",
                         "Godot_ReadOutputLog"):
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
        # Other project files legitimately mention the type, so assert on the match
        # that must be there rather than on the total.
        scene_matches = [m for m in matches if m["path"] == "res://scenes/main.tscn"]
        check(len(scene_matches) == 1, "search did not find the node type in the scene: %r" % matches)
        check(scene_matches[0]["line"] == 5, "search reported the wrong line: %r" % scene_matches[0])
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

        # --- structural scene editing, undo, and persistence ------------------
        def scene_node_names():
            tree = call({"jsonrpc": "2.0", "id": 100, "method": "tools/call",
                         "params": {"name": "Godot_GetEditedSceneTree"}})
            return [node["name"] for node in tree["result"]["structuredContent"]["nodes"]]

        reply = call({"jsonrpc": "2.0", "id": 20, "method": "tools/call",
                      "params": {"name": "Godot_ManageNode",
                                 "arguments": {"action": "create", "type": "Node2D",
                                               "name": "Spawner", "parent": "."}}})
        check(reply["result"]["isError"] is False,
              "create failed: %s" % reply["result"]["content"][0]["text"])
        check(reply["result"]["structuredContent"]["node"]["name"] == "Spawner",
              "created node has the wrong name")
        check("Spawner" in scene_node_names(), "created node is not in the scene tree")
        print("PASS Godot_ManageNode create")

        reply = call({"jsonrpc": "2.0", "id": 21, "method": "tools/call",
                      "params": {"name": "Godot_ManageNode",
                                 "arguments": {"action": "rename", "path": "Spawner",
                                               "name": "EnemySpawner"}}})
        check(reply["result"]["isError"] is False, "rename failed")
        names = scene_node_names()
        check("EnemySpawner" in names and "Spawner" not in names, "rename did not take effect")
        print("PASS Godot_ManageNode rename")

        reply = call({"jsonrpc": "2.0", "id": 22, "method": "tools/call",
                      "params": {"name": "Godot_ManageNode",
                                 "arguments": {"action": "reparent", "path": "EnemySpawner",
                                               "new_parent": "Player"}}})
        check(reply["result"]["isError"] is False, "reparent failed")
        tree = call({"jsonrpc": "2.0", "id": 23, "method": "tools/call",
                     "params": {"name": "Godot_GetEditedSceneTree"}})
        paths = {node["name"]: node["path"] for node in tree["result"]["structuredContent"]["nodes"]}
        check(paths.get("EnemySpawner") == "Player/EnemySpawner",
              "reparent left the node at %r" % paths.get("EnemySpawner"))
        print("PASS Godot_ManageNode reparent")

        # Undo must put the node back where it was, not merely remove it.
        reply = call({"jsonrpc": "2.0", "id": 24, "method": "tools/call",
                      "params": {"name": "Godot_UndoLastAction"}})
        check(reply["result"]["structuredContent"]["performed"] is True, "undo reported nothing to do")
        tree = call({"jsonrpc": "2.0", "id": 25, "method": "tools/call",
                     "params": {"name": "Godot_GetEditedSceneTree"}})
        paths = {node["name"]: node["path"] for node in tree["result"]["structuredContent"]["nodes"]}
        check(paths.get("EnemySpawner") == "EnemySpawner",
              "undo did not restore the original parent: %r" % paths.get("EnemySpawner"))
        print("PASS Godot_UndoLastAction restores the previous parent")

        # Saving is what makes an edit persistent; check the file on disk.
        reply = call({"jsonrpc": "2.0", "id": 26, "method": "tools/call",
                      "params": {"name": "Godot_SaveScene"}})
        check(reply["result"]["structuredContent"]["saved"] is True, "save failed")
        with open(os.path.join(project, "scenes", "main.tscn")) as handle:
            saved = handle.read()
        check("EnemySpawner" in saved, "the created node did not reach the saved scene")
        print("PASS Godot_SaveScene persisted the change")

        reply = call({"jsonrpc": "2.0", "id": 27, "method": "tools/call",
                      "params": {"name": "Godot_ManageNode",
                                 "arguments": {"action": "delete", "path": "EnemySpawner"}}})
        check(reply["result"]["isError"] is False, "delete failed")
        check("EnemySpawner" not in scene_node_names(), "delete did not remove the node")

        reply = call({"jsonrpc": "2.0", "id": 28, "method": "tools/call",
                      "params": {"name": "Godot_UndoLastAction"}})
        check(reply["result"]["structuredContent"]["performed"] is True, "undo after delete did nothing")
        check("EnemySpawner" in scene_node_names(), "undo did not bring the deleted node back")
        print("PASS Godot_ManageNode delete and undo")

        # Refusals that protect scene integrity.
        reply = call({"jsonrpc": "2.0", "id": 29, "method": "tools/call",
                      "params": {"name": "Godot_ManageNode",
                                 "arguments": {"action": "delete", "path": "."}}})
        check(refused(reply), "deleting the scene root was allowed")
        check("root" in reply["result"]["content"][0]["text"],
              "root-delete refusal does not explain itself")

        reply = call({"jsonrpc": "2.0", "id": 30, "method": "tools/call",
                      "params": {"name": "Godot_ManageNode",
                                 "arguments": {"action": "reparent", "path": "Player",
                                               "new_parent": "Player"}}})
        check(refused(reply), "reparenting a node into itself was allowed")

        reply = call({"jsonrpc": "2.0", "id": 31, "method": "tools/call",
                      "params": {"name": "Godot_ManageNode",
                                 "arguments": {"action": "create", "type": "NotARealClass"}}})
        check(refused(reply), "an unknown class was accepted")

        reply = call({"jsonrpc": "2.0", "id": 32, "method": "tools/call",
                      "params": {"name": "Godot_ManageNode",
                                 "arguments": {"action": "delete", "path": "NoSuchNode"}}})
        check(refused(reply), "deleting a node that does not exist was allowed")

        # The scene must be unchanged by everything that was refused.
        check(sorted(scene_node_names()) == ["EnemySpawner", "Hud", "Main", "Player"],
              "a refused operation still changed the scene: %r" % scene_node_names())
        print("PASS structural refusals leave the scene untouched")

        # --- skills -----------------------------------------------------------
        reply = call({"jsonrpc": "2.0", "id": 40, "method": "tools/call",
                      "params": {"name": "Godot_ListSkills"}})
        skills = {skill["name"]: skill for skill in reply["result"]["structuredContent"]["skills"]}
        check("scene-cleanup" in skills, "the shipped example skill was not discovered")
        found = skills["scene-cleanup"]
        check(found["source"] == "project", "skill reported the wrong root: %r" % found["source"])
        check("Godot_ManageNode" in found["tools"], "skill did not declare its tools")
        check(found.get("problem") is None, "skill reported a problem: %r" % found.get("problem"))
        print("PASS Godot_ListSkills found the shipped example skill")

        reply = call({"jsonrpc": "2.0", "id": 41, "method": "tools/call",
                      "params": {"name": "Godot_ReadSkill",
                                 "arguments": {"name": "scene-cleanup"}}})
        check(reply["result"]["isError"] is False,
              "reading the skill failed: %s" % reply["result"]["content"][0]["text"])
        text = reply["result"]["structuredContent"]["text"]
        check(text.startswith("You are a Godot scene-maintenance specialist."),
              "skill instructions were not returned with the frontmatter stripped")
        print("PASS Godot_ReadSkill returned the instructions")

        reply = call({"jsonrpc": "2.0", "id": 42, "method": "tools/call",
                      "params": {"name": "Godot_ReadSkill",
                                 "arguments": {"name": "scene-cleanup",
                                               "resource": "references/naming.md"}}})
        check("PascalCase" in reply["result"]["structuredContent"]["text"],
              "the supporting file was not loaded on demand")

        reply = call({"jsonrpc": "2.0", "id": 43, "method": "tools/call",
                      "params": {"name": "Godot_ReadSkill",
                                 "arguments": {"name": "scene-cleanup",
                                               "resource": "../../../etc/passwd"}}})
        check(refused(reply), "a skill resource escaped its own folder")

        reply = call({"jsonrpc": "2.0", "id": 44, "method": "tools/call",
                      "params": {"name": "Godot_ReadSkill",
                                 "arguments": {"name": "no-such-skill"}}})
        check(refused(reply), "an unknown skill was accepted")
        print("PASS skill resources load on demand and stay confined")

        reply = call({"jsonrpc": "2.0", "id": 7, "method": "tools/call",
                      "params": {"name": "Godot_WriteTextFile",
                                 "arguments": {"path": "res://written.txt",
                                               "text": "written through MCP"}}})
        check(reply["result"]["structuredContent"]["created"] is True, "file not reported created")
        # The effect is checked on disk, not taken from the tool's own report.
        with open(os.path.join(project, "written.txt")) as handle:
            check(handle.read() == "written through MCP", "file content does not match")
        # Held separately: later calls reuse `reply`, and the checkpoint check below
        # needs the metadata from this specific write.
        write_reply = reply
        print("PASS Godot_WriteTextFile")

        # --- output log -------------------------------------------------------
        # The AI service announces itself in the Output panel at startup, so that
        # message is a fact about this editor that the tool must be able to see.
        reply = call({"jsonrpc": "2.0", "id": 60, "method": "tools/call",
                      "params": {"name": "Godot_ReadOutputLog",
                                 "arguments": {"contains": "Godot AI service"}}})
        check(reply["result"]["isError"] is False,
              "reading the output log failed: %s" % reply["result"]["content"][0]["text"])
        messages = reply["result"]["structuredContent"]["messages"]
        check(any("listening on" in m["text"] for m in messages),
              "the service startup message was not in the output log: %r" % messages)
        check(messages[0]["type"] == "editor", "message type was not classified")
        print("PASS Godot_ReadOutputLog returned editor output")

        reply = call({"jsonrpc": "2.0", "id": 61, "method": "tools/call",
                      "params": {"name": "Godot_ReadOutputLog",
                                 "arguments": {"contains": "this string appears nowhere"}}})
        check(reply["result"]["structuredContent"]["messages"] == [],
              "a filter that matches nothing still returned messages")

        # --- checkpoints ------------------------------------------------------
        # The write above must have produced a checkpoint; restoring it has to remove
        # the file, because it did not exist beforehand.
        created_checkpoint = write_reply["result"].get("_meta", {}).get("checkpoint")
        check(created_checkpoint, "the mutating write did not report a checkpoint")

        reply = call({"jsonrpc": "2.0", "id": 50, "method": "tools/call",
                      "params": {"name": "Godot_ListCheckpoints"}})
        checkpoints = reply["result"]["structuredContent"]["checkpoints"]
        check(any(c["id"] == created_checkpoint for c in checkpoints),
              "the checkpoint is not listed")
        entry = [c for c in checkpoints if c["id"] == created_checkpoint][0]
        check(entry["tool"] == "Godot_WriteTextFile", "checkpoint names the wrong tool")
        print("PASS a mutating write created a listed checkpoint")

        # Change the file again, so the restore has something to undo.
        call({"jsonrpc": "2.0", "id": 51, "method": "tools/call",
              "params": {"name": "Godot_WriteTextFile",
                         "arguments": {"path": "res://written.txt", "text": "second write"}}})
        with open(os.path.join(project, "written.txt")) as handle:
            check(handle.read() == "second write", "the second write did not land")

        reply = call({"jsonrpc": "2.0", "id": 52, "method": "tools/call",
                      "params": {"name": "Godot_RestoreCheckpoint",
                                 "arguments": {"id": created_checkpoint}}})
        check(reply["result"]["isError"] is False,
              "restore failed: %s" % reply["result"]["content"][0]["text"])
        check(reply["result"]["structuredContent"]["files_removed"] == 1,
              "restore did not remove the file the tool had created")
        check(not os.path.exists(os.path.join(project, "written.txt")),
              "the created file survived the restore")
        print("PASS Godot_RestoreCheckpoint undid a file the tool created")

        reply = call({"jsonrpc": "2.0", "id": 53, "method": "tools/call",
                      "params": {"name": "Godot_RestoreCheckpoint",
                                 "arguments": {"id": "no-such-checkpoint"}}})
        check(refused(reply), "restoring an unknown checkpoint was accepted")

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
