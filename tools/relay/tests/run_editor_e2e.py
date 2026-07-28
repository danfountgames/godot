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
import base64
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from relay_harness import REPO_ROOT, RELAY_BINARY, RelayProcess  # noqa: E402

sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))

import virtual_display  # noqa: E402

DEFAULT_EDITOR = os.path.join(REPO_ROOT, "bin", "godot.linuxbsd.editor.dev.x86_64")

PROJECT_GODOT = """config_version=5

[application]

config/name="AI E2E Project"
run/main_scene="res://scenes/main.tscn"
config/features=PackedStringArray("4.3")

[rendering]

; The *game* process inherits the project's renderer, not the editor's command line.
; Software rendering under a virtual display cannot do Vulkan, so the project has to
; ask for GL compatibility or the game dies before the debugger ever connects.
renderer/rendering_method="gl_compatibility"
renderer/rendering_method.mobile="gl_compatibility"
"""

MAIN_SCENE = """[gd_scene load_steps=2 format=3 uid="uid://bqxaie2e001"]

[ext_resource type="Script" path="res://scripts/target.gd" id="1"]

[node name="Main" type="Node2D"]

[node name="Player" type="Sprite2D" parent="."]

[node name="Hud" type="CanvasLayer" parent="."]

[node name="Target" type="Button" parent="Hud"]
offset_left = 100.0
offset_top = 100.0
offset_right = 300.0
offset_bottom = 160.0
text = "Target"
script = ExtResource("1")
"""

# The button records two different things on purpose. `pressed` fires however the
# button was activated; `saw_input_event` only becomes true if a real InputEvent
# reached _gui_input. An implementation of Godot_SendPointerInput that took a shortcut
# - calling the handler, emitting the signal - would satisfy the first and fail the
# second, which is the only reason this test is worth anything.
TARGET_SCRIPT = """extends Button

var saw_input_event := false
var press_count := 0

func _ready() -> void:
	pressed.connect(_on_pressed)

func _on_pressed() -> void:
	press_count += 1
	print("E2E_CLICK press_count=%d saw_input_event=%s" % [press_count, saw_input_event])

func _gui_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		saw_input_event = true
"""


class Failure(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


def refusal_text(reply):
    """The human-readable reason a call was refused, whichever shape it came in."""
    if "error" in reply:
        return reply["error"]["message"]
    # A successful call may answer with an image block and no text at all, and this
    # runs eagerly to build every check's failure message - so never index blindly.
    for block in reply.get("result", {}).get("content", []):
        if block.get("type") == "text":
            return block["text"]
    return ""


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


def xdotool(display, *args):
    """Runs xdotool against `display`, returning (status, output)."""
    environment = dict(os.environ)
    environment["DISPLAY"] = display
    result = subprocess.run(["xdotool"] + list(args), env=environment,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    return result.returncode, result.stdout.decode().strip()


def visible_windows(display):
    status, output = xdotool(display, "search", "--onlyvisible", "--name", ".")
    return set(output.split()) if status == 0 else set()


def window_geometry(display, window):
    _, output = xdotool(display, "getwindowgeometry", "--shell", window)
    fields = dict(line.split("=", 1) for line in output.splitlines() if "=" in line)
    return {key: int(value) for key, value in fields.items() if value.lstrip("-").isdigit()}


def open_question(relay, display, identifier, arguments):
    """Asks a question and waits for its dialog to appear on screen."""
    before = visible_windows(display)
    relay.send_message({"jsonrpc": "2.0", "id": identifier, "method": "tools/call",
                        "params": {"name": "Godot_AskUser", "arguments": arguments}})
    deadline = time.time() + 20
    while time.time() < deadline:
        appeared = visible_windows(display) - before
        if appeared:
            # Let the window finish laying out before anything is aimed at it.
            time.sleep(1.0)
            return appeared.pop()
        time.sleep(0.3)
    raise Failure("no dialog window appeared for the question")


def click_an_answer(relay, display, choices):
    """Clicks a choice button and returns the answer that came back.

    The button's exact offset depends on how the dialog lays out its text, which is
    not something a test should be pinned to - so this walks down the dialog until a
    click lands. A single choice keeps the result unambiguous: whatever answer arrives
    can only have come from that button.
    """
    window = open_question(relay, display, 92,
                           {"question": "Which planet?", "choices": list(choices),
                            "timeout_seconds": 60})
    box = window_geometry(display, window)
    for fraction in (0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8):
        x = box["X"] + box["WIDTH"] // 2
        y = box["Y"] + int(box["HEIGHT"] * fraction)
        xdotool(display, "mousemove", str(x), str(y), "click", "1")
        reply = relay.read_message(timeout=3)
        if reply is not None:
            check(reply["result"]["isError"] is False,
                  "clicking the dialog produced an error: %s" % refusal_text(reply))
            return reply["result"]["structuredContent"]
    raise Failure("no click on the dialog produced an answer (geometry %r)" % box)


def dismiss_a_question(relay, display):
    """Presses Escape on the dialog and returns the answer that came back."""
    open_question(relay, display, 93,
                  {"question": "Never mind?", "timeout_seconds": 60})
    xdotool(display, "key", "Escape")
    reply = relay.read_message(timeout=20)
    check(reply is not None, "dismissing the dialog produced no response at all")
    check(reply["result"]["isError"] is False,
          "dismissing the dialog produced an error: %s" % refusal_text(reply))
    return reply["result"]["structuredContent"]


def build_project(root):
    os.makedirs(os.path.join(root, "scenes"), exist_ok=True)
    os.makedirs(os.path.join(root, "scripts"), exist_ok=True)
    with open(os.path.join(root, "scripts", "target.gd"), "w") as handle:
        handle.write(TARGET_SCRIPT)
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


def run(editor_binary, display):
    if not os.path.exists(editor_binary):
        raise Failure("editor binary not found at %s" % editor_binary)

    workspace = tempfile.mkdtemp(prefix="godot-ai-e2e-")
    project = os.path.join(workspace, "project")
    home = os.path.join(workspace, "home")
    os.makedirs(home)
    build_project(project)

    environment = display.environment()
    environment["GODOT_AI_HOME"] = home
    # Automation opt-in: skips the first-connection approval a human would give.
    environment["GODOT_AI_AUTO_APPROVE"] = "1"

    # With a display we can verify what a headless editor genuinely cannot show: that a
    # screenshot is a real image, and that the running game reports its tree. The
    # display is virtual unless the machine has a real one; either way the editor draws
    # for real, and the checks below are the same checks a human would make.
    has_display = display.usable
    command = [editor_binary, "--path", project, "--editor"] + display.godot_arguments()
    print("running %s" % ("with display %s" % display.display if has_display else "headless"))

    editor = subprocess.Popen(
        command,
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
                         "Godot_ReadOutputLog", "Godot_SetSceneProperty",
                         "Godot_SetRuntimeProperty", "Godot_GetRuntimeSceneTree",
                         "Godot_CaptureViewport", "Godot_AskUser"):
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
        # Derived from the fixture rather than hard-coded: the scene grows whenever a
        # new check needs a node in it, and a literal line number turns that into an
        # unrelated failure in this assertion.
        expected_line = next(index for index, text in enumerate(MAIN_SCENE.splitlines(), start=1)
                             if "Sprite2D" in text)
        check(scene_matches[0]["line"] == expected_line,
              "search reported line %r, expected %d: %r"
              % (scene_matches[0]["line"], expected_line, scene_matches[0]))
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
              "create failed: %s" % refusal_text(reply))
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
        check("root" in refusal_text(reply),
              "root-delete refusal does not explain itself")

        names_before_refusals = scene_node_names()

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

        # The scene must be unchanged by everything that was refused. Compared against
        # what it was a moment ago rather than a literal list, so adding a node to the
        # fixture for some other check does not fail this one.
        check(sorted(scene_node_names()) == sorted(names_before_refusals),
              "a refused operation still changed the scene: %r vs %r"
              % (scene_node_names(), names_before_refusals))
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
              "reading the skill failed: %s" % refusal_text(reply))
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

        # --- persistent vs runtime property edits ------------------------------
        reply = call({"jsonrpc": "2.0", "id": 70, "method": "tools/call",
                      "params": {"name": "Godot_SetSceneProperty",
                                 "arguments": {"path": "Player", "property": "position",
                                               "value": [128, 64]}}})
        check(reply["result"]["isError"] is False,
              "setting a scene property failed: %s" % refusal_text(reply))
        check(reply["result"]["structuredContent"]["persistent"] is True,
              "a scene property edit did not report itself as persistent")

        # It must survive a save/reopen, which is what "persistent" has to mean.
        call({"jsonrpc": "2.0", "id": 71, "method": "tools/call",
              "params": {"name": "Godot_SaveScene"}})
        with open(os.path.join(project, "scenes", "main.tscn")) as handle:
            check("128" in handle.read(), "the scene property did not reach the saved file")
        print("PASS Godot_SetSceneProperty is persistent")

        reply = call({"jsonrpc": "2.0", "id": 72, "method": "tools/call",
                      "params": {"name": "Godot_SetSceneProperty",
                                 "arguments": {"path": "Player", "property": "not_a_property",
                                               "value": 1}}})
        check(refused(reply), "setting an unknown property was accepted")

        # The runtime tools must refuse clearly while nothing is running, rather than
        # pretending to have changed something.
        reply = call({"jsonrpc": "2.0", "id": 73, "method": "tools/call",
                      "params": {"name": "Godot_SetRuntimeProperty",
                                 "arguments": {"path": "/root/Main/Player",
                                               "property": "position", "value": [1, 2]}}})
        check(refused(reply), "a runtime property edit was accepted with no game running")
        check("running" in refusal_text(reply),
              "the refusal does not explain that the game is not running")

        reply = call({"jsonrpc": "2.0", "id": 74, "method": "tools/call",
                      "params": {"name": "Godot_GetRuntimeSceneTree"}})
        check(refused(reply), "the runtime scene tree was returned with no game running")

        reply = call({"jsonrpc": "2.0", "id": 75, "method": "tools/call",
                      "params": {"name": "Godot_SendPointerInput",
                                 "arguments": {"x": 10, "y": 10}}})
        check(refused(reply), "pointer input was accepted with no game running")
        check("no game is running" in refusal_text(reply),
              "the input refusal does not explain itself: %r" % refusal_text(reply))
        print("PASS runtime tools refuse cleanly while nothing is running")

        # --- deferred responses -----------------------------------------------
        # Nobody is here to answer, so a short timeout proves the whole deferred path
        # end to end: the request is held, the editor keeps working, and exactly one
        # response arrives later.
        before = time.time()
        reply = call({"jsonrpc": "2.0", "id": 90, "method": "tools/call",
                      "params": {"name": "Godot_AskUser",
                                 "arguments": {"question": "Is anyone there?",
                                               "timeout_seconds": 2}}})
        elapsed = time.time() - before
        check(refused(reply), "an unanswered question did not fail")
        check("timed out" in refusal_text(reply),
              "the unanswered question did not report a timeout: %r" % refusal_text(reply))
        check(elapsed >= 1.5, "the response came back too fast to have been deferred (%.2fs)" % elapsed)

        # The editor must still be serving other calls while a question is pending.
        reply = call({"jsonrpc": "2.0", "id": 91, "method": "tools/call",
                      "params": {"name": "Godot_GetEditorStatus"}})
        check(reply["result"]["isError"] is False, "the editor stopped serving after a deferred call")
        print("PASS Godot_AskUser deferred the response and timed out cleanly")

        # --- answering the question for real ----------------------------------
        # A timeout only proves the plumbing. This drives the dialog the way a person
        # would: a real pointer, on a real window, hitting a real button.
        if has_display and shutil.which("xdotool"):
            answer = click_an_answer(relay, display.display, ["mercury"])
            check(answer == {"answer": "mercury", "cancelled": False},
                  "clicking a choice did not return it: %r" % answer)
            print("PASS Godot_AskUser returned the choice that was clicked")

            answer = dismiss_a_question(relay, display.display)
            check(answer["cancelled"] is True,
                  "dismissing the dialog was not reported as a cancellation: %r" % answer)
            print("PASS Godot_AskUser reports a dismissed question as cancelled")
        elif has_display:
            print("SKIP clicking the dialog: xdotool is not installed")

        # --- screenshots ------------------------------------------------------
        # This editor is headless, so the honest answer is a refusal that names the
        # reason - not a blank image presented as if it were the editor.
        reply = call({"jsonrpc": "2.0", "id": 80, "method": "tools/call",
                      "params": {"name": "Godot_CaptureViewport"}})
        if not has_display:
            check(refused(reply), "a headless editor produced a screenshot")
            check("headless" in refusal_text(reply),
                  "the capture refusal does not name the reason: %r" % refusal_text(reply))
            print("PASS Godot_CaptureViewport refuses cleanly when headless")
        else:
            check(reply["result"]["isError"] is False,
                  "capture failed: %s" % refusal_text(reply))
            shot = reply["result"]["structuredContent"]
            check(shot["width"] > 200 and shot["height"] > 200,
                  "the capture is implausibly small: %r" % shot)
            on_disk = os.path.join(project, shot["path"].replace("res://", ""))
            with open(on_disk, "rb") as handle:
                header = handle.read(8)
            check(header == b"\x89PNG\r\n\x1a\n", "the saved capture is not a PNG")
            check(os.path.getsize(on_disk) > 5000,
                  "the capture is too small to be a rendered editor: %d bytes"
                  % os.path.getsize(on_disk))
            images = [c for c in reply["result"]["content"] if c["type"] == "image"]
            check(images and base64.b64decode(images[0]["data"])[:8] == b"\x89PNG\r\n\x1a\n",
                  "the inline image block is not a PNG")
            print("PASS Godot_CaptureViewport produced a real %dx%d image"
                  % (shot["width"], shot["height"]))

        # --- output log -------------------------------------------------------
        # The AI service announces itself in the Output panel at startup, so that
        # message is a fact about this editor that the tool must be able to see.
        reply = call({"jsonrpc": "2.0", "id": 60, "method": "tools/call",
                      "params": {"name": "Godot_ReadOutputLog",
                                 "arguments": {"contains": "Godot AI service"}}})
        check(reply["result"]["isError"] is False,
              "reading the output log failed: %s" % refusal_text(reply))
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

        # --- play lifecycle ---------------------------------------------------
        # Deliberately after the output-log checks: starting the game clears the
        # editor's Output panel, which would wipe the messages those checks read.
        reply = call({"jsonrpc": "2.0", "id": 85, "method": "tools/call",
                      "params": {"name": "Godot_PlayCurrentScene"}})
        if has_display:
            check(reply.get("result", {}).get("isError") is False,
                  "play failed even with a display: %s" % refusal_text(reply))
        if reply.get("result", {}).get("isError") is False:
            playing = reply["result"]["structuredContent"]["playing"]
            check(playing is True, "the editor reported the game as not running after play")

            # Runtime inspection needs the game to attach a debugger session and
            # report its tree. A headless game may exit before it ever does, so this is
            # probed there and asserted where a display exists.
            # Wait for the tree to contain the *scene*, not merely for the call to
            # succeed. The debugger answers as soon as the game is up, which can be
            # before the main scene has been instantiated - so accepting the first
            # successful reply is a race that reports a bare `root` and blames the
            # scene for it.
            deadline = time.time() + 15
            tree = None
            while time.time() < deadline:
                probe = call({"jsonrpc": "2.0", "id": 86, "method": "tools/call",
                              "params": {"name": "Godot_GetRuntimeSceneTree"}})
                if probe.get("result", {}).get("isError") is False:
                    candidate = probe["result"]["structuredContent"]
                    if any(node["name"] == "Main" for node in candidate["nodes"]):
                        tree = candidate
                        break
                time.sleep(0.5)
            if tree is not None:
                names = [n["name"] for n in tree["nodes"]]
                check("Main" in names and "Player" in names,
                      "the runtime tree does not match the scene: %r" % names)
                print("PASS Godot_GetRuntimeSceneTree saw the running game: %r" % names)

                # The whole point of the runtime/persistent split: this edit must not
                # reach the file on disk.
                before = open(os.path.join(project, "scenes", "main.tscn")).read()
                reply = call({"jsonrpc": "2.0", "id": 88, "method": "tools/call",
                              "params": {"name": "Godot_SetRuntimeProperty",
                                         "arguments": {"path": "/root/Main/Player",
                                                       "property": "position",
                                                       "value": [64, 32]}}})
                check(reply["result"]["isError"] is False,
                      "runtime property edit failed: %s" % refusal_text(reply))
                check(reply["result"]["structuredContent"]["persistent"] is False,
                      "a runtime edit claimed to be persistent")
                after = open(os.path.join(project, "scenes", "main.tscn")).read()
                check(before == after, "a runtime edit changed the scene file on disk")
                print("PASS Godot_SetRuntimeProperty applied without touching the project")

                # --- real input into the running game -------------------------
                # The button is at (100,100)-(300,160) inside a CanvasLayer, so its
                # centre is a point a player could hit.
                reply = call({"jsonrpc": "2.0", "id": 89, "method": "tools/call",
                              "params": {"name": "Godot_SendPointerInput",
                                         "arguments": {"x": 200, "y": 130,
                                                       "action": "click"}}})
                check(reply["result"]["isError"] is False,
                      "sending pointer input failed: %s" % refusal_text(reply))
                check(reply["result"]["structuredContent"]["events"] == 3,
                      "a click should be a move, a press and a release: %r"
                      % reply["result"]["structuredContent"])

                deadline = time.time() + 10
                proof = []
                while time.time() < deadline and not proof:
                    probe = call({"jsonrpc": "2.0", "id": 90, "method": "tools/call",
                                  "params": {"name": "Godot_ReadOutputLog",
                                             "arguments": {"contains": "E2E_CLICK"}}})
                    proof = probe["result"]["structuredContent"]["messages"]
                    if not proof:
                        time.sleep(0.5)
                check(proof, "the click never reached the button in the running game")
                check("press_count=1" in proof[-1]["text"],
                      "the button was not pressed exactly once: %r" % proof[-1]["text"])
                # The assertion that separates real input from a convincing imitation.
                check("saw_input_event=true" in proof[-1]["text"],
                      "the button was activated without an InputEvent reaching it, so "
                      "this was not real input: %r" % proof[-1]["text"])
                print("PASS Godot_SendPointerInput delivered a real click to the game")

                reply = call({"jsonrpc": "2.0", "id": 91, "method": "tools/call",
                              "params": {"name": "Godot_SendPointerInput",
                                         "arguments": {"x": 9000, "y": 9000}}})
                check(refused(reply), "a click outside the window was accepted")
                check("outside the game window" in refusal_text(reply),
                      "the refusal does not explain itself: %r" % refusal_text(reply))
                print("PASS Godot_SendPointerInput refuses coordinates off the window")
            elif has_display:
                raise Failure("the running game never reported its scene tree")
            else:
                print("SKIP runtime tree: the headless game did not report one")

            reply = call({"jsonrpc": "2.0", "id": 87, "method": "tools/call",
                          "params": {"name": "Godot_StopPlaying"}})
            # A headless game with an empty scene may already have exited by now, so
            # `was_playing` can only be pinned down where the game actually stayed up.
            check(reply["result"]["structuredContent"]["playing"] is False,
                  "the game was still running after stop")
            if has_display:
                check(reply["result"]["structuredContent"]["was_playing"] is True,
                      "stop did not report that a game had been running")
            print("PASS play reported a running game and stop left nothing running")
        else:
            # Running a game needs more than a headless editor on some systems; say so
            # rather than pretending the path was exercised.
            print("SKIP play lifecycle: %s" % refusal_text(reply)[:80])

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
              "restore failed: %s" % refusal_text(reply))
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
        check("outside the project" in refusal_text(reply),
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

        # --- headless one-shot execution --------------------------------------
        # The scripted path: no MCP client, no editor UI, just a tool and a result.
        one_shot = subprocess.run(
            [RELAY_BINARY, "--call", "Godot_GetEditorStatus"],
            input=b"", stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=dict(environment), timeout=60)
        check(one_shot.returncode == 0,
              "one-shot call failed (%d): %s" % (one_shot.returncode, one_shot.stderr.decode()))
        status = json.loads(one_shot.stdout.decode())["structuredContent"]
        check(status["has_editor"] is True, "one-shot did not reach a live editor")
        check(os.path.realpath(status["project_root"]) == os.path.realpath(project),
              "one-shot reported the wrong project")
        print("PASS headless one-shot tool call against the live editor")

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
    parser.add_argument("--headless", action="store_true",
                        help="skip the display checks even where a display is available")
    args = parser.parse_args()

    # Start a display rather than assume one: on a container this is the difference
    # between verifying the visual tools and merely verifying that they refuse.
    display = (virtual_display.VirtualDisplay("", 0, 0, 0) if args.headless
               else virtual_display.ensure(width=1280, height=800))
    try:
        with display:
            run(args.editor, display)
    except Failure as failure:
        print("FAIL %s" % failure, file=sys.stderr)
        return 1
    print("\nend-to-end: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
