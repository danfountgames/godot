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
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

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

SURFACE_SCRIPT = """extends Control

# Counts only what a real drag and a real wheel produce. A drag implemented as a press
# at one point and a release at another - which is what "drag" most easily degrades into
# - leaves drag_events at zero, because the motion between the ends never happened. And
# motion that does not carry the held button is a hover, not a drag, so button_mask is
# checked rather than assumed.
var drag_distance := 0.0
var drag_events := 0
var drag_had_button := false
var scroll_up := 0
var scroll_down := 0

# A cancelled touch is not a release. The engine models it as pressed = false *and*
# canceled = true, so `is_released()` is false for it - a game that collapses the two
# fires the button the player was dragging away from when a notification arrives.
var touch_released := 0
var touch_canceled := 0

func _input(event: InputEvent) -> void:
	if event is InputEventScreenTouch and not event.pressed:
		if event.canceled:
			touch_canceled += 1
		else:
			touch_released += 1

func _gui_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion:
		if event.button_mask != 0:
			drag_had_button = true
			drag_distance += event.relative.length()
			drag_events += 1
	elif event is InputEventMouseButton and event.pressed:
		if event.button_index == MOUSE_BUTTON_WHEEL_UP:
			scroll_up += 1
		elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			scroll_down += 1
"""

CHIMES_SCRIPT = """extends Node

# Two players, one stream. Setting play_count to 2 makes the same sound play twice at
# once - which no bus peak can distinguish from one loud playback, and which is the
# audio bug an agent working without ears is most likely to ship.
var play_count := 0:
	set(value):
		play_count = value
		if value >= 1:
			$One.play()
		if value >= 2:
			$Two.play()
"""

SCENE_TEST_SCRIPT = """extends Node

# The contract a test scene declares on its root node. Nothing in the project reads
# these; the editor's runtime agent does, once a frame, until test_finished is true.
var test_finished := false
var test_results: Array = []

@export var include_failure := false

func _ready() -> void:
	var started := Time.get_ticks_msec()
	test_results.append({
		"name": "the scene tree is built",
		"passed": get_tree() != null,
		"message": "",
		"duration_ms": Time.get_ticks_msec() - started,
	})
	if include_failure:
		test_results.append({
			"name": "a case that is meant to fail",
			"passed": false,
			"message": "expected 5, got 4",
			"duration_ms": 2,
		})
	# Finish a couple of frames later, so the watcher has to actually watch rather than
	# finding the answer already sitting there on its first look.
	await get_tree().process_frame
	await get_tree().process_frame
	test_finished = true
"""

TEST_SCENE_TEMPLATE = """[gd_scene load_steps=2 format=3 uid="uid://bqxaie2et%02d"]

[ext_resource type="Script" path="res://tests/scene_test.gd" id="1"]

[node name="SceneTest" type="Node"]
script = ExtResource("1")
include_failure = %s
"""

# A scene under tests/ that declares none of the contract. Running it has to be an
# error that says what is missing, not a pass with nothing in it.
NOT_A_TEST_SCENE = """[gd_scene format=3 uid="uid://bqxaie2et09"]

[node name="NotATest" type="Node2D"]
"""

MAIN_SCENE = """[gd_scene load_steps=5 format=3 uid="uid://bqxaie2e001"]

[ext_resource type="Script" path="res://scripts/target.gd" id="1"]
[ext_resource type="Script" path="res://scripts/chimes.gd" id="2"]
[ext_resource type="AudioStream" path="res://audio/chime.wav" id="3"]
[ext_resource type="Script" path="res://scripts/surface.gd" id="4"]

[node name="Main" type="Node2D"]

[node name="Player" type="Sprite2D" parent="."]

[node name="Hud" type="CanvasLayer" parent="."]

[node name="Field" type="LineEdit" parent="Hud"]
offset_left = 100.0
offset_top = 200.0
offset_right = 400.0
offset_bottom = 240.0

[node name="Target" type="Button" parent="Hud"]
offset_left = 100.0
offset_top = 100.0
offset_right = 300.0
offset_bottom = 160.0
text = "Target"
script = ExtResource("1")

[node name="Surface" type="Control" parent="Hud"]
offset_left = 500.0
offset_top = 100.0
offset_right = 900.0
offset_bottom = 500.0
script = ExtResource("4")

[node name="Chimes" type="Node" parent="."]
script = ExtResource("2")

[node name="One" type="AudioStreamPlayer" parent="Chimes"]
stream = ExtResource("3")

[node name="Two" type="AudioStreamPlayer" parent="Chimes"]
stream = ExtResource("3")
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

# Setting this makes the game report a real engine error, so the structured error
# reader can be checked against something it must find rather than against an empty
# list - which passes whether the feature works or not.
var trigger_error := false:
	set(value):
		trigger_error = value
		if value:
			push_error("E2E_DELIBERATE_ERROR")
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


def check_capture_metadata(shot, expected_source):
    """Every capture has to say what it is a picture of.

    An image on its own is not evidence, and once one has been saved and quoted there is
    nothing left to say whether it was the editor, the whole screen or the game.
    """
    check(shot.get("source") == expected_source,
          "the capture does not say it is a %s: %r" % (expected_source, shot.get("source")))
    check(shot.get("subject"), "the capture does not say what exactly it photographed: %r" % shot)
    stamp = shot.get("captured_at", "")
    check(len(stamp) >= 19 and stamp[4] == "-" and stamp[10] == "T",
          "the capture timestamp is not a datetime: %r" % stamp)


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


def write_png(path, width=2, height=2, gradient=False):
    """Writes a small but genuinely valid PNG.

    The import-pipeline checks need an asset the editor's importer will actually
    consume; a file merely named `.png` would be reported as a broken import and every
    assertion below would then be about the wrong thing.

    `gradient` gives every pixel a different colour. A flat image is the wrong thing to
    compare imports of: the importer compresses it, and a 16x16 block of one colour
    comes out very nearly the same size as a 2x2 block of it, so a check on the
    importer's output would pass whether the reimport happened or not.
    """
    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    def pixel(x, y):
        if gradient:
            return bytes(((x * 37) % 256, (y * 91) % 256, ((x * y) * 13) % 256))
        return b"\xff\x00\x00"

    scanlines = b"".join(
        b"\x00" + b"".join(pixel(x, y) for x in range(width)) for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", header)
                     + chunk(b"IDAT", zlib.compress(scanlines))
                     + chunk(b"IEND", b""))


def imported_output_of(project, source_relative):
    """The file the importer produced for an asset, read out of its `.import` sidecar."""
    sidecar = os.path.join(project, source_relative + ".import")
    if not os.path.exists(sidecar):
        return None
    with open(sidecar) as handle:
        for line in handle:
            if line.startswith("path="):
                res_path = line.split("=", 1)[1].strip().strip('"')
                return os.path.join(project, res_path[len("res://"):])
    return None


def write_wav(path, seconds=30, rate=8000):
    """A valid 16-bit PCM mono WAV.

    The stacking check needs a stream with a res:// path: two players sounding the same
    *file* is what makes them the same sound, and a stream generated in script has no
    identity to compare. Long enough that it cannot finish during the checks.
    """
    frames = seconds * rate
    samples = bytearray()
    for i in range(frames):
        value = int(8000 * math.sin(2 * math.pi * 440 * i / rate))
        samples += struct.pack("<h", value)
    with open(path, "wb") as handle:
        handle.write(b"RIFF" + struct.pack("<I", 36 + len(samples)) + b"WAVE")
        handle.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        handle.write(b"data" + struct.pack("<I", len(samples)) + bytes(samples))


def build_project(root):
    os.makedirs(os.path.join(root, "scenes"), exist_ok=True)
    os.makedirs(os.path.join(root, "scripts"), exist_ok=True)
    with open(os.path.join(root, "scripts", "target.gd"), "w") as handle:
        handle.write(TARGET_SCRIPT)
    with open(os.path.join(root, "scripts", "chimes.gd"), "w") as handle:
        handle.write(CHIMES_SCRIPT)
    with open(os.path.join(root, "scripts", "surface.gd"), "w") as handle:
        handle.write(SURFACE_SCRIPT)
    os.makedirs(os.path.join(root, "audio"), exist_ok=True)
    write_wav(os.path.join(root, "audio", "chime.wav"))
    with open(os.path.join(root, "project.godot"), "w") as handle:
        handle.write(PROJECT_GODOT)
    with open(os.path.join(root, "scenes", "main.tscn"), "w") as handle:
        handle.write(MAIN_SCENE)
    with open(os.path.join(root, "notes.txt"), "w") as handle:
        handle.write("hello from a project text file\n")
    write_png(os.path.join(root, "sprite.png"))

    os.makedirs(os.path.join(root, "tests"), exist_ok=True)
    with open(os.path.join(root, "tests", "scene_test.gd"), "w") as handle:
        handle.write(SCENE_TEST_SCRIPT)
    with open(os.path.join(root, "tests", "test_green.tscn"), "w") as handle:
        handle.write(TEST_SCENE_TEMPLATE % (7, "false"))
    with open(os.path.join(root, "tests", "test_red.tscn"), "w") as handle:
        handle.write(TEST_SCENE_TEMPLATE % (8, "true"))
    with open(os.path.join(root, "tests", "test_not_a_test.tscn"), "w") as handle:
        handle.write(NOT_A_TEST_SCENE)

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
        # Derived from what the fixture writes rather than hard-coded: this list has
        # already broken once for no reason but the fixture gaining a scene.
        expected_scenes = sorted(
            "res://" + os.path.relpath(os.path.join(base, name), project).replace(os.sep, "/")
            for base, _, files in os.walk(project)
            for name in files if name.endswith(".tscn"))
        check(sorted(scenes) == expected_scenes,
              "unexpected scene list: %r, expected %r" % (sorted(scenes), expected_scenes))
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

        for identifier, name in ((76, "Godot_SendKeyInput"), (77, "Godot_CaptureGame"),
                                 (78, "Godot_GetRuntimeProperty")):
            arguments = {"path": "/root/Main", "property": "name"} \
                if name == "Godot_GetRuntimeProperty" else {}
            reply = call({"jsonrpc": "2.0", "id": identifier, "method": "tools/call",
                          "params": {"name": name, "arguments": arguments}})
            check(refused(reply), "%s was accepted with no game running" % name)
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

        # The timeout answered the client. The dialog has to go with it: left up, it
        # invites an answer, accepts the click, and does nothing, because the token it
        # would complete is already gone. This used to be exactly what happened.
        deadline = time.time() + 10
        while time.time() < deadline:
            reply = call({"jsonrpc": "2.0", "id": 94, "method": "tools/call",
                          "params": {"name": "Godot_ListWindows"}})
            stale = [window for window in reply["result"]["structuredContent"]["windows"]
                     if window["title"] == "Godot AI asks"]
            if not stale:
                break
            time.sleep(0.5)
        check(not stale, "the timed-out question left its dialog on screen: %r" % stale)
        print("PASS a timed-out question closes its own dialog")

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
            check_capture_metadata(shot, "editor_viewport")
            check("not the running game" in shot.get("note", ""),
                  "the viewport capture does not warn that it is not the game: %r" % shot)
            print("PASS Godot_CaptureViewport produced a real %dx%d image, saying what it is"
                  % (shot["width"], shot["height"]))

            # The whole screen, which is the only capture that contains a dialog -
            # every dialog is a separate OS window and so invisible to the viewport.
            reply = call({"jsonrpc": "2.0", "id": 81, "method": "tools/call",
                          "params": {"name": "Godot_CaptureEditorWindow"}})
            check(reply["result"]["isError"] is False,
                  "capturing the editor window failed: %s" % refusal_text(reply))
            window_shot = reply["result"]["structuredContent"]
            check(window_shot["width"] >= shot["width"],
                  "the screen capture is smaller than the viewport capture: %r vs %r"
                  % (window_shot, shot))
            on_disk = os.path.join(project, window_shot["path"].replace("res://", ""))
            with open(on_disk, "rb") as handle:
                check(handle.read(8) == b"\x89PNG\r\n\x1a\n",
                      "the editor window capture is not a PNG")
            check_capture_metadata(window_shot, "editor_screen")
            check(window_shot["source"] != shot["source"],
                  "two captures of different things claim the same source")
            print("PASS Godot_CaptureEditorWindow captured the whole screen (%dx%d), saying so"
                  % (window_shot["width"], window_shot["height"]))

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

                # --- drag and scroll ---------------------------------------------
                # The motion *between* the ends is the whole content of a drag: a
                # slider, a camera or a swipe reads deltas, not the two endpoints. The
                # fixture counts only motion that carries a held button, so a drag
                # degraded into a press at one point and a release at another scores
                # zero here.
                def surface(field):
                    probe = call({"jsonrpc": "2.0", "id": 190, "method": "tools/call",
                                  "params": {"name": "Godot_GetRuntimeProperty",
                                             "arguments": {"path": "/root/Main/Hud/Surface",
                                                           "property": field}}})
                    check(not refused(probe),
                          "reading Surface.%s failed: %s" % (field, refusal_text(probe)))
                    return probe["result"]["structuredContent"]["value"]

                check(surface("drag_events") == 0, "the surface saw a drag before one was sent")
                reply = call({"jsonrpc": "2.0", "id": 191, "method": "tools/call",
                              "params": {"name": "Godot_SendPointerInput",
                                         "arguments": {"action": "drag",
                                                       "x": 550, "y": 150,
                                                       "to_x": 850, "to_y": 150,
                                                       "steps": 10}}})
                check(not refused(reply), "dragging failed: %s" % refusal_text(reply))
                dragged = reply["result"]["structuredContent"]
                check(dragged["events"] == 13,
                      "a 10-step drag should be a move, a press, 10 motions and a release: %r"
                      % dragged)
                check(surface("drag_events") == 10,
                      "the surface did not see 10 motion events: %r" % surface("drag_events"))
                check(surface("drag_had_button") is True,
                      "the drag's motion did not carry the held button, so a game asking "
                      "button_mask would read it as a hover")
                distance = surface("drag_distance")
                check(abs(distance - 300.0) < 2.0,
                      "the drag covered %r pixels, not the 300 between its ends" % distance)

                reply = call({"jsonrpc": "2.0", "id": 192, "method": "tools/call",
                              "params": {"name": "Godot_SendPointerInput",
                                         "arguments": {"action": "drag",
                                                       "x": 550, "y": 150,
                                                       "to_x": 99999, "to_y": 150}}})
                check(refused(reply), "a drag ending off the window was accepted")
                check("ends outside" in refusal_text(reply),
                      "the refusal does not say the *end* is the problem: %r" % refusal_text(reply))
                print("PASS Godot_SendPointerInput drags with real motion between the ends")

                reply = call({"jsonrpc": "2.0", "id": 193, "method": "tools/call",
                              "params": {"name": "Godot_SendPointerInput",
                                         "arguments": {"action": "scroll", "x": 700, "y": 300,
                                                       "direction": "down", "amount": 4}}})
                check(not refused(reply), "scrolling failed: %s" % refusal_text(reply))
                check(surface("scroll_down") == 4,
                      "the surface saw %r wheel-down notches, not 4" % surface("scroll_down"))
                check(surface("scroll_up") == 0, "scrolling down produced wheel-up events")

                call({"jsonrpc": "2.0", "id": 194, "method": "tools/call",
                      "params": {"name": "Godot_SendPointerInput",
                                 "arguments": {"action": "scroll", "x": 700, "y": 300,
                                               "direction": "up", "amount": 2}}})
                check(surface("scroll_up") == 2,
                      "the surface saw %r wheel-up notches, not 2" % surface("scroll_up"))

                reply = call({"jsonrpc": "2.0", "id": 195, "method": "tools/call",
                              "params": {"name": "Godot_SendPointerInput",
                                         "arguments": {"action": "scroll", "x": 700, "y": 300,
                                                       "direction": "sideways"}}})
                check(refused(reply), "an unknown scroll direction was accepted")
                print("PASS Godot_SendPointerInput scrolls with real wheel events")

                # --- reading runtime state back --------------------------------
                # Godot_SetRuntimeProperty set the player's position earlier. Until now
                # nothing could confirm it from the game's own state rather than from
                # the tool's own report, which is not evidence of anything.
                reply = call({"jsonrpc": "2.0", "id": 92, "method": "tools/call",
                              "params": {"name": "Godot_GetRuntimeProperty",
                                         "arguments": {"path": "/root/Main/Player",
                                                       "property": "position"}}})
                check(reply["result"]["isError"] is False,
                      "reading a runtime property failed: %s" % refusal_text(reply))
                value = reply["result"]["structuredContent"]
                check(value["type"] == "Vector2",
                      "the property came back as the wrong type: %r" % value)
                check("64" in value["text"] and "32" in value["text"],
                      "the runtime property does not hold what was written to it: %r" % value)
                print("PASS Godot_GetRuntimeProperty read the value back out of the game")

                reply = call({"jsonrpc": "2.0", "id": 93, "method": "tools/call",
                              "params": {"name": "Godot_GetRuntimeProperty",
                                         "arguments": {"path": "/root/Main/Nope",
                                                       "property": "position"}}})
                check(refused(reply), "reading a property of a node that does not exist succeeded")

                reply = call({"jsonrpc": "2.0", "id": 94, "method": "tools/call",
                              "params": {"name": "Godot_GetRuntimeProperty",
                                         "arguments": {"path": "/root/Main/Player",
                                                       "property": "not_a_property"}}})
                check(refused(reply), "reading a property that does not exist succeeded")
                print("PASS Godot_GetRuntimeProperty refuses unknown nodes and properties")

                # --- typing into the running game ------------------------------
                # Click the field first, then type: characters go to whatever holds
                # focus, exactly as they would for a player.
                reply = call({"jsonrpc": "2.0", "id": 95, "method": "tools/call",
                              "params": {"name": "Godot_SendPointerInput",
                                         "arguments": {"x": 250, "y": 220}}})
                check(reply["result"]["isError"] is False,
                      "clicking the text field failed: %s" % refusal_text(reply))

                reply = call({"jsonrpc": "2.0", "id": 96, "method": "tools/call",
                              "params": {"name": "Godot_SendKeyInput",
                                         "arguments": {"action": "type", "text": "hello"}}})
                check(reply["result"]["isError"] is False,
                      "typing failed: %s" % refusal_text(reply))
                check(reply["result"]["structuredContent"]["events"] == 10,
                      "five characters should be five presses and five releases: %r"
                      % reply["result"]["structuredContent"])

                # The proof: the field's own text. Nothing here set it - the characters
                # had to travel the input pipeline into the focused control.
                reply = call({"jsonrpc": "2.0", "id": 97, "method": "tools/call",
                              "params": {"name": "Godot_GetRuntimeProperty",
                                         "arguments": {"path": "/root/Main/Hud/Field",
                                                       "property": "text"}}})
                check(reply["result"]["isError"] is False,
                      "reading the typed text failed: %s" % refusal_text(reply))
                check(reply["result"]["structuredContent"]["value"] == "hello",
                      "the typed characters did not reach the field: %r"
                      % reply["result"]["structuredContent"])
                print("PASS Godot_SendKeyInput typed into the running game")

                reply = call({"jsonrpc": "2.0", "id": 98, "method": "tools/call",
                              "params": {"name": "Godot_SendKeyInput",
                                         "arguments": {"action": "tap", "key": "NotAKey"}}})
                check(refused(reply), "an unknown key name was accepted")
                print("PASS Godot_SendKeyInput refuses a key name it does not know")

                # --- seeing the running game -----------------------------------
                reply = call({"jsonrpc": "2.0", "id": 99, "method": "tools/call",
                              "params": {"name": "Godot_CaptureGame"}})
                check(reply["result"]["isError"] is False,
                      "capturing the game failed: %s" % refusal_text(reply))
                shot = reply["result"]["structuredContent"]
                check(shot["width"] > 100 and shot["height"] > 100,
                      "the game capture is implausibly small: %r" % shot)
                with open(shot["path"], "rb") as handle:
                    check(handle.read(8) == b"\x89PNG\r\n\x1a\n",
                          "the game capture is not a PNG")
                images = [c for c in reply["result"]["content"] if c["type"] == "image"]
                check(images and base64.b64decode(images[0]["data"])[:8] == b"\x89PNG\r\n\x1a\n",
                      "the game capture was not returned inline")
                check_capture_metadata(shot, "game_window")
                check(shot["frame"] > 0, "the game capture has no frame number: %r" % shot)
                check(abs(shot["time_scale"] - 1.0) < 0.001,
                      "the game is not running at normal speed: %r" % shot["time_scale"])
                check("note" not in shot or "speed" not in shot["note"],
                      "a capture at normal speed carries a speed warning: %r" % shot.get("note"))
                check(shot["window_width"] > 100 and shot["scene"].endswith("main.tscn"),
                      "the game capture does not describe the window and scene: %r" % shot)
                print("PASS Godot_CaptureGame photographed the running game (%dx%d) at frame %d"
                      % (shot["width"], shot["height"], shot["frame"]))

                # --- describing a node, and aiming at it by name ----------------
                reply = call({"jsonrpc": "2.0", "id": 100, "method": "tools/call",
                              "params": {"name": "Godot_GetRuntimeNodeInfo",
                                         "arguments": {"path": "/root/Main/Hud/Target"}}})
                check(reply["result"]["isError"] is False,
                      "describing a node failed: %s" % refusal_text(reply))
                info = reply["result"]["structuredContent"]
                check(info["type"] == "Button", "wrong class: %r" % info)
                check(info["script"].endswith("target.gd"), "script not reported: %r" % info)
                check("rect" in info, "a Control reported no geometry: %r" % info)
                print("PASS Godot_GetRuntimeNodeInfo described the node and where it is")

                # Aim at the centre the game itself reported, rather than at a
                # coordinate copied from the fixture. The click is still real.
                centre = (int(info["rect"]["center_x"]), int(info["rect"]["center_y"]))
                reply = call({"jsonrpc": "2.0", "id": 101, "method": "tools/call",
                              "params": {"name": "Godot_SendPointerInput",
                                         "arguments": {"x": centre[0], "y": centre[1]}}})
                check(reply["result"]["isError"] is False,
                      "clicking the reported centre failed: %s" % refusal_text(reply))

                # --- waiting for the game to reach a state ----------------------
                # press_count is now 2: the earlier click plus this one. Waiting for it
                # asserts the click landed without anything sleeping.
                reply = call({"jsonrpc": "2.0", "id": 102, "method": "tools/call",
                              "params": {"name": "Godot_WaitForRuntimeCondition",
                                         "arguments": {"path": "/root/Main/Hud/Target",
                                                       "property": "press_count",
                                                       "equals": 2,
                                                       "timeout_seconds": 10}}})
                check(reply["result"]["isError"] is False,
                      "waiting for the press count failed: %s" % refusal_text(reply))
                check(reply["result"]["structuredContent"]["satisfied"] is True,
                      "the wait did not report satisfaction: %r"
                      % reply["result"]["structuredContent"])
                print("PASS Godot_WaitForRuntimeCondition saw the game reach the state")

                before = time.time()
                reply = call({"jsonrpc": "2.0", "id": 103, "method": "tools/call",
                              "params": {"name": "Godot_WaitForRuntimeCondition",
                                         "arguments": {"path": "/root/Main/Hud/Target",
                                                       "property": "press_count",
                                                       "equals": 999,
                                                       "timeout_seconds": 2}}})
                check(refused(reply), "waiting for something untrue eventually succeeded")
                # The failure has to say what it found, or the reader goes back to the
                # game to work out which half was wrong.
                check("it is 2" in refusal_text(reply),
                      "the timeout does not report the actual value: %r" % refusal_text(reply))
                check(time.time() - before >= 1.5,
                      "the wait returned too fast to have waited")
                print("PASS Godot_WaitForRuntimeCondition times out and says what it saw")

                # --- performance and window ------------------------------------
                reply = call({"jsonrpc": "2.0", "id": 104, "method": "tools/call",
                              "params": {"name": "Godot_GetPerformanceMetrics"}})
                check(reply["result"]["isError"] is False,
                      "sampling performance failed: %s" % refusal_text(reply))
                metrics = reply["result"]["structuredContent"]
                check(metrics["node_count"] > 0, "no nodes reported: %r" % metrics)
                check("sample" in metrics["note"], "the sample is not qualified: %r" % metrics)
                print("PASS Godot_GetPerformanceMetrics sampled the running game")

                reply = call({"jsonrpc": "2.0", "id": 105, "method": "tools/call",
                              "params": {"name": "Godot_GetGameWindowInfo"}})
                check(reply["result"]["isError"] is False,
                      "reading the window failed: %s" % refusal_text(reply))
                window = reply["result"]["structuredContent"]
                check(window["width"] == shot["width"] and window["height"] == shot["height"],
                      "the window and the capture disagree about size: %r vs %r" % (window, shot))
                print("PASS Godot_GetGameWindowInfo agrees with the capture")

                # --- touch and gamepad ------------------------------------------
                reply = call({"jsonrpc": "2.0", "id": 106, "method": "tools/call",
                              "params": {"name": "Godot_SendTouchInput",
                                         "arguments": {"x": centre[0], "y": centre[1],
                                                       "action": "tap"}}})
                check(reply["result"]["isError"] is False,
                      "sending touch failed: %s" % refusal_text(reply))
                check(reply["result"]["structuredContent"]["events"] == 2,
                      "a tap should be a touch down and up: %r"
                      % reply["result"]["structuredContent"])

                reply = call({"jsonrpc": "2.0", "id": 107, "method": "tools/call",
                              "params": {"name": "Godot_SendTouchInput",
                                         "arguments": {"x": 9000, "y": 9000}}})
                check(refused(reply), "a touch outside the window was accepted")

                for arguments in ({"action": "tap", "button": 0},
                                  {"action": "axis", "axis": 0, "value": 1},
                                  {"action": "disconnect", "device": 0}):
                    reply = call({"jsonrpc": "2.0", "id": 108, "method": "tools/call",
                                  "params": {"name": "Godot_SendGamepadInput",
                                             "arguments": arguments}})
                    check(reply["result"]["isError"] is False,
                          "gamepad %r failed: %s" % (arguments, refusal_text(reply)))

                reply = call({"jsonrpc": "2.0", "id": 109, "method": "tools/call",
                              "params": {"name": "Godot_SendGamepadInput",
                                         "arguments": {"action": "nonsense"}}})
                check(refused(reply), "an unknown gamepad action was accepted")
                print("PASS Godot_SendTouchInput and Godot_SendGamepadInput deliver events")

                # A cancelled touch is not a release, and the difference is the whole
                # reason this exists: a game that collapses the two fires the button the
                # player was dragging away from when a notification arrives. Both counts
                # are asserted, so a tool that sent an ordinary release and called it a
                # cancellation fails.
                before_released = surface("touch_released")
                before_canceled = surface("touch_canceled")
                call({"jsonrpc": "2.0", "id": 196, "method": "tools/call",
                      "params": {"name": "Godot_SendTouchInput",
                                 "arguments": {"action": "down", "x": 700, "y": 300}}})
                reply = call({"jsonrpc": "2.0", "id": 197, "method": "tools/call",
                              "params": {"name": "Godot_SendTouchInput",
                                         "arguments": {"action": "cancel", "x": 700, "y": 300}}})
                check(not refused(reply), "cancelling a touch failed: %s" % refusal_text(reply))
                check(surface("touch_canceled") == before_canceled + 1,
                      "the game did not see a cancelled touch")
                check(surface("touch_released") == before_released,
                      "the cancellation was delivered as an ordinary release, which is the "
                      "one thing it must not be")

                # And the other direction, so the check cannot pass by calling
                # everything a cancellation.
                call({"jsonrpc": "2.0", "id": 198, "method": "tools/call",
                      "params": {"name": "Godot_SendTouchInput",
                                 "arguments": {"action": "tap", "x": 700, "y": 300}}})
                check(surface("touch_released") == before_released + 1,
                      "a normal tap did not produce a release")
                check(surface("touch_canceled") == before_canceled + 1,
                      "a normal tap was reported as a cancellation")
                print("PASS Godot_SendTouchInput cancels a touch as a cancellation, not a release")

                # --- the trace ---------------------------------------------------
                # Everything above was sent through this editor, so the game's own
                # record of what arrived is the check on all of it.
                reply = call({"jsonrpc": "2.0", "id": 118, "method": "tools/call",
                              "params": {"name": "Godot_GetInputTrace"}})
                check(reply["result"]["isError"] is False,
                      "reading the input trace failed: %s" % refusal_text(reply))
                trace = reply["result"]["structuredContent"]["events"]
                kinds = {entry["kind"] for entry in trace}
                check({"pointer", "key", "touch", "gamepad"} <= kinds,
                      "the trace is missing input kinds that were sent: %r" % kinds)
                check(all("frame" in entry and "msec" in entry for entry in trace),
                      "trace entries do not say when they happened: %r" % trace[:2])
                print("PASS Godot_GetInputTrace recorded every kind of input sent (%d events)"
                      % len(trace))

                # --- structured errors -------------------------------------------
                # Make the game report a real error, so this is checked against
                # something it must find. An assertion over an empty list passes
                # whether the feature works or not.
                reply = call({"jsonrpc": "2.0", "id": 119, "method": "tools/call",
                              "params": {"name": "Godot_SetRuntimeProperty",
                                         "arguments": {"path": "/root/Main/Hud/Target",
                                                       "property": "trigger_error",
                                                       "value": True}}})
                check(reply["result"]["isError"] is False,
                      "triggering an error failed: %s" % refusal_text(reply))

                reply = call({"jsonrpc": "2.0", "id": 122, "method": "tools/call",
                              "params": {"name": "Godot_GetRuntimeErrors"}})
                check(reply["result"]["isError"] is False,
                      "reading runtime errors failed: %s" % refusal_text(reply))
                errors = reply["result"]["structuredContent"]["errors"]
                deliberate = [e for e in errors if "E2E_DELIBERATE_ERROR" in e.get("message", "")]
                check(deliberate, "the error the game reported was not captured: %r" % errors)
                entry = deliberate[0]
                check({"file", "line", "function", "message", "kind"} <= set(entry),
                      "the error entry is missing structure: %r" % entry)
                check(entry["kind"] == "error", "the error was not classified: %r" % entry)
                check(entry["line"] > 0, "the error has no line number: %r" % entry)
                # The file is the engine's own push_error, not the script that called
                # it: Godot's error handler reports the C++ call site, and only a
                # genuine script fault carries a .gd path. Asserting otherwise would be
                # asserting a bug that is not there.
                check(entry["file"].endswith(".cpp") or entry["file"].endswith(".gd"),
                      "the error names no source at all: %r" % entry)
                print("PASS Godot_GetRuntimeErrors captured a real error with file and line")

                # --- resolution matrix -------------------------------------------
                reply = call({"jsonrpc": "2.0", "id": 120, "method": "tools/call",
                              "params": {"name": "Godot_SetGameWindowSize",
                                         "arguments": {"width": 640, "height": 480}}})
                check(reply["result"]["isError"] is False,
                      "resizing the game failed: %s" % refusal_text(reply))
                sized = reply["result"]["structuredContent"]
                check(sized["requested_width"] == 640,
                      "the resize did not echo the request: %r" % sized)

                reply = call({"jsonrpc": "2.0", "id": 121, "method": "tools/call",
                              "params": {"name": "Godot_SetGameWindowSize",
                                         "arguments": {"width": 8, "height": 8}}})
                check(refused(reply), "an absurd window size was accepted")
                print("PASS Godot_SetGameWindowSize resized the game and reports what applied")

                # --- frame sequences, profiling, audio, time scale ---------------
                reply = call({"jsonrpc": "2.0", "id": 123, "method": "tools/call",
                              "params": {"name": "Godot_CaptureFrameSequence",
                                         "arguments": {"frames": 4}}})
                check(reply["result"]["isError"] is False,
                      "capturing a frame sequence failed: %s" % refusal_text(reply))
                frames = reply["result"]["structuredContent"]["frames"]
                check(len(frames) == 4, "wrong number of frames: %r" % frames)
                numbers = [f["frame"] for f in frames]
                check(numbers == sorted(numbers) and len(set(numbers)) == 4,
                      "the frames are not distinct and in order: %r" % numbers)
                for entry in frames:
                    with open(entry["path"], "rb") as handle:
                        check(handle.read(8) == b"\x89PNG\r\n\x1a\n",
                              "a sequence frame is not a PNG: %r" % entry)
                sequence = reply["result"]["structuredContent"]
                check_capture_metadata(sequence, "game_window")
                print("PASS Godot_CaptureFrameSequence captured %d distinct frames" % len(frames))

                reply = call({"jsonrpc": "2.0", "id": 124, "method": "tools/call",
                              "params": {"name": "Godot_ProfileWindow",
                                         "arguments": {"frames": 20, "budget_frame_ms": 1000}}})
                check(reply["result"]["isError"] is False,
                      "profiling failed: %s" % refusal_text(reply))
                profile = reply["result"]["structuredContent"]
                check(profile["frames"] == 20, "wrong sample count: %r" % profile)
                check(profile["worst_frame_ms"] >= profile["mean_frame_ms"],
                      "the worst frame is below the mean: %r" % profile)
                check(profile["within_budget"] is True,
                      "a 1000ms budget was somehow exceeded: %r" % profile)
                check("verdict" in profile, "a budget produced no verdict: %r" % profile)

                # And a budget nothing can meet, so the verdict is exercised both ways.
                reply = call({"jsonrpc": "2.0", "id": 125, "method": "tools/call",
                              "params": {"name": "Godot_ProfileWindow",
                                         "arguments": {"frames": 5, "budget_frame_ms": 0.0001}}})
                strict = reply["result"]["structuredContent"]
                check(strict["within_budget"] is False,
                      "an impossible budget was reported as met: %r" % strict)
                check("worst frame" in strict["verdict"],
                      "the failing verdict does not name the worst frame: %r" % strict)
                print("PASS Godot_ProfileWindow judges the worst frame against a budget")

                reply = call({"jsonrpc": "2.0", "id": 126, "method": "tools/call",
                              "params": {"name": "Godot_GetAudioState"}})
                check(reply["result"]["isError"] is False,
                      "reading audio state failed: %s" % refusal_text(reply))
                audio = reply["result"]["structuredContent"]
                check(audio["buses"], "no audio buses reported: %r" % audio)
                check(audio["buses"][0]["name"] == "Master",
                      "the first bus is not Master: %r" % audio["buses"][0])
                check("cannot hear" in audio["note"],
                      "the audio state does not say what it cannot tell you: %r" % audio)
                players = {entry["node_path"]: entry for entry in audio["players"]}
                check("/root/Main/Chimes/One" in players and "/root/Main/Chimes/Two" in players,
                      "the audio players in the scene were not found: %r" % sorted(players))
                check(all(not entry["playing"] for entry in players.values()),
                      "a sound is playing before anything asked for one: %r" % players)
                check(audio["stacked"] == [],
                      "nothing is playing, yet something is reported as stacked: %r" % audio)
                print("PASS Godot_GetAudioState reported %d buses and %d silent players"
                      % (len(audio["buses"]), len(players)))

                # One sound playing is not stacking. This half matters as much as the
                # other: a check that only ever saw the stacked case would pass just as
                # well against a tool that called everything stacked.
                call({"jsonrpc": "2.0", "id": 180, "method": "tools/call",
                      "params": {"name": "Godot_SetRuntimeProperty",
                                 "arguments": {"path": "/root/Main/Chimes",
                                               "property": "play_count", "value": 1}}})
                reply = call({"jsonrpc": "2.0", "id": 181, "method": "tools/call",
                              "params": {"name": "Godot_GetAudioState"}})
                audio = reply["result"]["structuredContent"]
                sounding = [entry for entry in audio["players"] if entry["playing"]]
                check(len(sounding) == 1,
                      "expected exactly one sounding player: %r" % audio["players"])
                check(audio["stacked"] == [],
                      "one playback was reported as stacked: %r" % audio["stacked"])

                # Now the same sound on both players. A bus peak cannot tell this from
                # one loud playback; this is the only thing here that can.
                call({"jsonrpc": "2.0", "id": 182, "method": "tools/call",
                      "params": {"name": "Godot_SetRuntimeProperty",
                                 "arguments": {"path": "/root/Main/Chimes",
                                               "property": "play_count", "value": 2}}})
                reply = call({"jsonrpc": "2.0", "id": 183, "method": "tools/call",
                              "params": {"name": "Godot_GetAudioState"}})
                audio = reply["result"]["structuredContent"]
                check(len(audio["stacked"]) == 1,
                      "the same sound playing twice was not reported as stacked: %r" % audio)
                stack = audio["stacked"][0]
                check(stack["stream"] == "res://audio/chime.wav",
                      "the stacked entry names the wrong stream: %r" % stack)
                check(stack["count"] == 2 and sorted(stack["node_paths"]) ==
                      ["/root/Main/Chimes/One", "/root/Main/Chimes/Two"],
                      "the stacked entry does not name both players: %r" % stack)
                check("stacked" in audio["note"] and "more than one player" in audio["note"],
                      "the note does not mention the stacking it found: %r" % audio["note"])
                print("PASS Godot_GetAudioState found one sound playing on two players at once")

                reply = call({"jsonrpc": "2.0", "id": 127, "method": "tools/call",
                              "params": {"name": "Godot_SetTimeScale",
                                         "arguments": {"scale": 2}}})
                check(reply["result"]["isError"] is False,
                      "setting the time scale failed: %s" % refusal_text(reply))
                check(reply["result"]["structuredContent"]["scale"] == 2,
                      "the time scale did not take: %r" % reply["result"]["structuredContent"])
                # The discriminating half of capture provenance: a frame taken while the
                # game runs at 2x has to say so on the image's own record, because
                # nothing in the picture ever will, and a fast-forwarded frame quoted as
                # a playtest result is a wrong conclusion, not a missing caveat.
                reply = call({"jsonrpc": "2.0", "id": 130, "method": "tools/call",
                              "params": {"name": "Godot_CaptureGame",
                                         "arguments": {"inline_image": False}}})
                fast = reply["result"]["structuredContent"]
                check(abs(fast["time_scale"] - 2.0) < 0.001,
                      "the capture did not record the time scale it was taken at: %r" % fast)
                check("2.00x speed" in fast.get("note", ""),
                      "a capture taken at 2x speed does not say so: %r" % fast.get("note"))
                print("PASS a capture taken at 2x speed says so, on the image's own record")

                reply = call({"jsonrpc": "2.0", "id": 128, "method": "tools/call",
                              "params": {"name": "Godot_SetTimeScale",
                                         "arguments": {"scale": 0}}})
                check(refused(reply), "a time scale of zero was accepted")
                call({"jsonrpc": "2.0", "id": 129, "method": "tools/call",
                      "params": {"name": "Godot_SetTimeScale", "arguments": {"scale": 1}}})
                print("PASS Godot_SetTimeScale changes and restores the pace of the game")
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

            # --- scene tests --------------------------------------------------
            # A test here is a scene the engine plays, not a shell command: nothing in
            # this interface executes one, and a test runner is exactly where that rule
            # would be quietly broken.
            if has_display:
                reply = call({"jsonrpc": "2.0", "id": 170, "method": "tools/call",
                              "params": {"name": "Godot_RunSceneTest",
                                         "arguments": {"path": "res://tests/test_green.tscn",
                                                       "timeout_seconds": 30}}})
                check(not refused(reply),
                      "running the passing test scene failed: %s" % refusal_text(reply))
                run = reply["result"]["structuredContent"]
                check(run["succeeded"] is True and run["passed"] == 1 and run["failed"] == 0,
                      "the passing scene did not report a clean run: %r" % run)
                check(run["cases"][0]["name"] == "the scene tree is built",
                      "the case came back without its name: %r" % run["cases"])
                check(run["wall_duration_ms"] > 0, "the run reports no elapsed time: %r" % run)

                # The failing scene has to come back as a *named case with a message*.
                # A count of failures is not something anyone can act on.
                reply = call({"jsonrpc": "2.0", "id": 171, "method": "tools/call",
                              "params": {"name": "Godot_RunSceneTest",
                                         "arguments": {"path": "res://tests/test_red.tscn",
                                                       "timeout_seconds": 30}}})
                check(not refused(reply),
                      "running the failing test scene failed: %s" % refusal_text(reply))
                run = reply["result"]["structuredContent"]
                check(run["succeeded"] is False and run["failed"] == 1 and run["passed"] == 1,
                      "the failing scene did not report one pass and one failure: %r" % run)
                failures = [case for case in run["cases"] if not case["passed"]]
                check(len(failures) == 1, "expected exactly one failing case: %r" % run["cases"])
                check(failures[0]["name"] == "a case that is meant to fail",
                      "the failing case lost its name: %r" % failures[0])
                check(failures[0]["message"] == "expected 5, got 4",
                      "the failing case lost its message: %r" % failures[0])
                print("PASS Godot_RunSceneTest reports per-case results, pass and fail alike")

                # A scene that declares nothing is not a test, and saying "0 failed"
                # about it would read as a pass.
                reply = call({"jsonrpc": "2.0", "id": 172, "method": "tools/call",
                              "params": {"name": "Godot_RunSceneTest",
                                         "arguments": {"path": "res://tests/test_not_a_test.tscn",
                                                       "timeout_seconds": 15}}})
                check(refused(reply), "a scene that declares no results was reported as passing")
                check("test_finished" in refusal_text(reply),
                      "the refusal does not say what the scene is missing: %r"
                      % refusal_text(reply))
                print("PASS a scene without the test contract is an error, not an empty pass")

            reply = call({"jsonrpc": "2.0", "id": 173, "method": "tools/call",
                          "params": {"name": "Godot_RunSceneTest",
                                     "arguments": {"path": "res://notes.txt"}}})
            check(refused(reply), "a text file was accepted as a test scene")
        else:
            # Running a game needs more than a headless editor on some systems; say so
            # rather than pretending the path was exercised.
            print("SKIP play lifecycle: %s" % refusal_text(reply)[:80])

        # --- listing the project's test scenes ---------------------------------
        reply = call({"jsonrpc": "2.0", "id": 174, "method": "tools/call",
                      "params": {"name": "Godot_ListSceneTests"}})
        check(not refused(reply), "listing test scenes failed: %s" % refusal_text(reply))
        listed = reply["result"]["structuredContent"]
        paths = sorted(scene["path"] for scene in listed["scenes"])
        check(paths == ["res://tests/test_green.tscn", "res://tests/test_not_a_test.tscn",
                        "res://tests/test_red.tscn"],
              "the wrong scenes were listed as tests: %r" % paths)
        check(listed["count"] == 3, "the count disagrees with the list: %r" % listed)

        # The convention is the whole discovery rule, so a different prefix must find
        # nothing rather than quietly falling back to the default.
        reply = call({"jsonrpc": "2.0", "id": 175, "method": "tools/call",
                      "params": {"name": "Godot_ListSceneTests",
                                 "arguments": {"prefix": "spec_"}}})
        check(reply["result"]["structuredContent"]["count"] == 0,
              "a prefix nothing uses matched scenes anyway")

        reply = call({"jsonrpc": "2.0", "id": 176, "method": "tools/call",
                      "params": {"name": "Godot_ListSceneTests",
                                 "arguments": {"directory": "res://scenes"}}})
        check(reply["result"]["structuredContent"]["count"] == 0,
              "a directory with no test scenes reported some")

        reply = call({"jsonrpc": "2.0", "id": 177, "method": "tools/call",
                      "params": {"name": "Godot_ListSceneTests",
                                 "arguments": {"directory": "res://notes.txt"}}})
        check(refused(reply), "a file was accepted as a directory to search")
        print("PASS Godot_ListSceneTests finds test scenes by convention")

        # --- user data --------------------------------------------------------
        # Saves live outside the project, which is why the project tools cannot see
        # them and why these exist. The round trip below is the one the game-production
        # template asks for and could not previously be performed at all.
        reply = call({"jsonrpc": "2.0", "id": 110, "method": "tools/call",
                      "params": {"name": "Godot_ListUserFiles"}})
        check(reply["result"]["isError"] is False,
              "listing user files failed: %s" % refusal_text(reply))
        user_root = reply["result"]["structuredContent"]["root"]
        check(os.path.isabs(user_root), "the user root is not an absolute path: %r" % user_root)

        reply = call({"jsonrpc": "2.0", "id": 111, "method": "tools/call",
                      "params": {"name": "Godot_WriteUserFile",
                                 "arguments": {"path": "user://saves/slot1.json",
                                               "content": '{"level": 3}'}}})
        check(reply["result"]["isError"] is False,
              "writing a user file failed: %s" % refusal_text(reply))
        check(reply["result"]["structuredContent"]["replaced"] is False,
              "a new file reported itself as a replacement")

        reply = call({"jsonrpc": "2.0", "id": 112, "method": "tools/call",
                      "params": {"name": "Godot_ReadUserFile",
                                 "arguments": {"path": "user://saves/slot1.json"}}})
        check(reply["result"]["structuredContent"]["content"] == '{"level": 3}',
              "the user file did not round-trip: %r" % reply["result"]["structuredContent"])

        reply = call({"jsonrpc": "2.0", "id": 113, "method": "tools/call",
                      "params": {"name": "Godot_ListUserFiles"}})
        listed = [f["path"] for f in reply["result"]["structuredContent"]["files"]]
        check("user://saves/slot1.json" in listed,
              "the written save was not listed: %r" % listed)
        print("PASS user data round-trips through write, read and list")

        # The boundary matters more here than in the project: this is the one place
        # these tools can reach that no version control is watching.
        for escape in ("user://../../etc/passwd", "res://project.godot", "/etc/passwd"):
            reply = call({"jsonrpc": "2.0", "id": 114, "method": "tools/call",
                          "params": {"name": "Godot_ReadUserFile",
                                     "arguments": {"path": escape}}})
            check(refused(reply), "reading '%s' from the user tools was allowed" % escape)
        print("PASS user data tools stay inside the user directory")

        reply = call({"jsonrpc": "2.0", "id": 115, "method": "tools/call",
                      "params": {"name": "Godot_DeleteUserFile",
                                 "arguments": {"path": "user://saves/slot1.json"}}})
        check(refused(reply), "a delete without confirmation was accepted")
        check("confirm=true" in refusal_text(reply),
              "the delete refusal does not say what is missing: %r" % refusal_text(reply))

        reply = call({"jsonrpc": "2.0", "id": 116, "method": "tools/call",
                      "params": {"name": "Godot_DeleteUserFile",
                                 "arguments": {"path": "user://saves/slot1.json",
                                               "confirm": True}}})
        check(reply["result"]["isError"] is False,
              "a confirmed delete failed: %s" % refusal_text(reply))
        reply = call({"jsonrpc": "2.0", "id": 117, "method": "tools/call",
                      "params": {"name": "Godot_ReadUserFile",
                                 "arguments": {"path": "user://saves/slot1.json"}}})
        check(refused(reply), "the deleted save was still readable")
        print("PASS Godot_DeleteUserFile demands confirmation, then deletes")

        # --- project settings and on-demand checkpoints ------------------------
        reply = call({"jsonrpc": "2.0", "id": 130, "method": "tools/call",
                      "params": {"name": "Godot_GetProjectSetting",
                                 "arguments": {"name": "application/run/main_scene"}}})
        check(reply["result"]["isError"] is False,
              "reading a project setting failed: %s" % refusal_text(reply))
        check(reply["result"]["structuredContent"]["value"] == "res://scenes/main.tscn",
              "the main scene setting is wrong: %r" % reply["result"]["structuredContent"])

        reply = call({"jsonrpc": "2.0", "id": 131, "method": "tools/call",
                      "params": {"name": "Godot_GetProjectSetting"}})
        names = reply["result"]["structuredContent"]["settings"]
        check("application/run/main_scene" in names,
              "listing settings omitted one the project sets: %r" % names)

        reply = call({"jsonrpc": "2.0", "id": 132, "method": "tools/call",
                      "params": {"name": "Godot_GetProjectSetting",
                                 "arguments": {"name": "nope/not/a/setting"}}})
        check(refused(reply), "reading a setting that does not exist succeeded")

        reply = call({"jsonrpc": "2.0", "id": 133, "method": "tools/call",
                      "params": {"name": "Godot_SetProjectSetting",
                                 "arguments": {"name": "display/window/size/viewport_width",
                                               "value": 800}}})
        check(reply["result"]["isError"] is False,
              "writing a project setting failed: %s" % refusal_text(reply))
        # Read back from the file, not from the tool's own report.
        with open(os.path.join(project, "project.godot")) as handle:
            check("viewport_width=800" in handle.read().replace(" ", ""),
                  "the setting did not reach project.godot")
        check(reply["result"].get("_meta", {}).get("checkpoint"),
              "changing a project setting was not checkpointed")
        print("PASS project settings read, list, write and checkpoint")

        reply = call({"jsonrpc": "2.0", "id": 134, "method": "tools/call",
                      "params": {"name": "Godot_CreateCheckpoint",
                                 "arguments": {"label": "before the risky bit",
                                               "paths": ["res://notes.txt"]}}})
        check(reply["result"]["isError"] is False,
              "creating a checkpoint failed: %s" % refusal_text(reply))
        manual_checkpoint = reply["result"]["structuredContent"]["checkpoint"]

        # Change the file, then put it back through the named point. The clobber is
        # asserted, not assumed: this call used to pass the wrong argument name, so the
        # write was rejected and the restore below had nothing to undo - a check that
        # could not fail is not a check.
        reply = call({"jsonrpc": "2.0", "id": 135, "method": "tools/call",
                      "params": {"name": "Godot_WriteTextFile",
                                 "arguments": {"path": "res://notes.txt", "text": "clobbered"}}})
        check(not refused(reply), "clobbering the file failed: %s" % refusal_text(reply))
        with open(os.path.join(project, "notes.txt")) as handle:
            check(handle.read() == "clobbered", "the clobbering write did not land")

        reply = call({"jsonrpc": "2.0", "id": 136, "method": "tools/call",
                      "params": {"name": "Godot_RestoreCheckpoint",
                                 "arguments": {"id": manual_checkpoint}}})
        check(not refused(reply),
              "restoring the manual checkpoint failed: %s" % refusal_text(reply))
        with open(os.path.join(project, "notes.txt")) as handle:
            check(handle.read().startswith("hello"),
                  "the manual checkpoint did not restore the original file")

        reply = call({"jsonrpc": "2.0", "id": 137, "method": "tools/call",
                      "params": {"name": "Godot_CreateCheckpoint",
                                 "arguments": {"paths": ["res://nope.txt"]}}})
        check(refused(reply), "a checkpoint of a file that does not exist was accepted")
        print("PASS Godot_CreateCheckpoint marks a point that Godot_RestoreCheckpoint returns to")

        # --- import pipeline --------------------------------------------------
        # Editing an asset on disk is not the same as the editor having imported it.
        # These tools exist so that gap is answerable rather than guessed at from the
        # output log, and the checks below are about the importer's real output.
        reply = call({"jsonrpc": "2.0", "id": 140, "method": "tools/call",
                      "params": {"name": "Godot_WaitForImportQueue",
                                 "arguments": {"timeout_seconds": 90}}})
        check(not refused(reply), "waiting for the import queue failed: %s" % refusal_text(reply))
        check(reply["result"]["structuredContent"]["idle"] is True,
              "the import queue reported itself as still busy after succeeding")

        reply = call({"jsonrpc": "2.0", "id": 141, "method": "tools/call",
                      "params": {"name": "Godot_GetImportStatus"}})
        check(not refused(reply), "reading import status failed: %s" % refusal_text(reply))
        import_status = reply["result"]["structuredContent"]
        check(import_status["scanning"] is False,
              "the pipeline says it is scanning immediately after reporting itself idle")
        broken = [entry["path"] for entry in import_status["broken"]]
        check("res://sprite.png" not in broken,
              "a valid PNG was reported as a broken import: %r" % broken)

        produced = imported_output_of(project, "sprite.png")
        check(produced and os.path.exists(produced),
              "the editor never imported the fixture PNG (%r)" % produced)
        with open(produced, "rb") as handle:
            before_bytes = handle.read()

        # Replace the asset behind the editor's back, then make it notice. A reimport
        # that was merely accepted would leave the old texture in place, so the check is
        # on the importer's output, not on the tool's own report.
        write_png(os.path.join(project, "sprite.png"), width=16, height=16, gradient=True)
        reply = call({"jsonrpc": "2.0", "id": 142, "method": "tools/call",
                      "params": {"name": "Godot_ReimportAsset",
                                 "arguments": {"paths": ["res://sprite.png"]}}})
        check(not refused(reply), "reimporting an asset failed: %s" % refusal_text(reply))
        reimported = reply["result"]["structuredContent"]
        check(reimported["reimported"] == ["res://sprite.png"],
              "reimport named the wrong files: %r" % reimported["reimported"])
        check(reimported["scanned"] is False,
              "reimporting one file reported a whole-project rescan")
        produced = imported_output_of(project, "sprite.png")
        with open(produced, "rb") as handle:
            after_bytes = handle.read()
        check(after_bytes != before_bytes,
              "the replacement asset produced a byte-identical texture (%d bytes), so the "
              "reimport was accepted but never happened" % len(after_bytes))
        print("PASS Godot_ReimportAsset rebuilt the importer's output for a changed asset")

        # No `paths` at all. This is also the regression check for the const-Dictionary
        # trap: reading a missing optional key by subscript inserts a null, and schema
        # validation then rejects the call the tool itself corrupted.
        reply = call({"jsonrpc": "2.0", "id": 143, "method": "tools/call",
                      "params": {"name": "Godot_ReimportAsset", "arguments": {}}})
        check(not refused(reply), "a whole-project rescan was refused: %s" % refusal_text(reply))
        check(reply["result"]["structuredContent"]["scanned"] is True,
              "omitting paths did not rescan the project")

        reply = call({"jsonrpc": "2.0", "id": 144, "method": "tools/call",
                      "params": {"name": "Godot_ReimportAsset",
                                 "arguments": {"paths": ["res://not-an-asset.png"]}}})
        check(refused(reply), "reimporting a file that does not exist was accepted")
        print("PASS Godot_GetImportStatus and Godot_WaitForImportQueue answer for the pipeline")

        # --- checkpoint diff --------------------------------------------------
        # The restore above put notes.txt back, so the checkpoint and the project agree.
        reply = call({"jsonrpc": "2.0", "id": 145, "method": "tools/call",
                      "params": {"name": "Godot_DiffCheckpoint",
                                 "arguments": {"id": manual_checkpoint}}})
        check(not refused(reply), "diffing a checkpoint failed: %s" % refusal_text(reply))
        diff = reply["result"]["structuredContent"]
        check("res://notes.txt" in diff["unchanged"],
              "a restored file was not reported as unchanged: %r" % diff)
        check(diff["changed"] == [], "an untouched project reported changes: %r" % diff)

        call({"jsonrpc": "2.0", "id": 146, "method": "tools/call",
              "params": {"name": "Godot_WriteTextFile",
                         "arguments": {"path": "res://notes.txt", "text": "diverged"}}})
        reply = call({"jsonrpc": "2.0", "id": 147, "method": "tools/call",
                      "params": {"name": "Godot_DiffCheckpoint",
                                 "arguments": {"id": manual_checkpoint}}})
        diff = reply["result"]["structuredContent"]
        check("res://notes.txt" in diff["changed"],
              "an edited file was not reported as changed: %r" % diff)
        check("res://notes.txt" not in diff["unchanged"],
              "the same file was reported both changed and unchanged: %r" % diff)

        os.remove(os.path.join(project, "notes.txt"))
        reply = call({"jsonrpc": "2.0", "id": 148, "method": "tools/call",
                      "params": {"name": "Godot_DiffCheckpoint",
                                 "arguments": {"id": manual_checkpoint}}})
        diff = reply["result"]["structuredContent"]
        check("res://notes.txt" in diff["deleted"],
              "a deleted file was not reported as deleted: %r" % diff)

        # Put the project back, so nothing downstream inherits the divergence.
        call({"jsonrpc": "2.0", "id": 149, "method": "tools/call",
              "params": {"name": "Godot_RestoreCheckpoint",
                         "arguments": {"id": manual_checkpoint}}})
        with open(os.path.join(project, "notes.txt")) as handle:
            check(handle.read().startswith("hello"), "the restore did not bring notes.txt back")

        reply = call({"jsonrpc": "2.0", "id": 150, "method": "tools/call",
                      "params": {"name": "Godot_DiffCheckpoint",
                                 "arguments": {"id": "no-such-checkpoint"}}})
        check(refused(reply), "diffing an unknown checkpoint was accepted")
        print("PASS Godot_DiffCheckpoint separates changed, unchanged and deleted files")

        # --- windows ----------------------------------------------------------
        reply = call({"jsonrpc": "2.0", "id": 151, "method": "tools/call",
                      "params": {"name": "Godot_ListWindows"}})
        check(not refused(reply), "listing windows failed: %s" % refusal_text(reply))
        windows = reply["result"]["structuredContent"]["windows"]
        main_windows = [window for window in windows if window["main"]]
        check(len(main_windows) == 1, "expected exactly one main window, got %r" % windows)
        check(main_windows[0]["width"] > 0 and main_windows[0]["height"] > 0,
              "the main window has no size: %r" % main_windows[0])
        check(all(window["visible"] for window in windows),
              "the default listing included a hidden window")

        reply = call({"jsonrpc": "2.0", "id": 152, "method": "tools/call",
                      "params": {"name": "Godot_ListWindows",
                                 "arguments": {"visible_only": False}}})
        all_windows = reply["result"]["structuredContent"]["windows"]
        check(len(all_windows) > len(windows),
              "including hidden windows found no more than the visible ones (%d vs %d) - "
              "the editor always has closed dialogs parented in its tree"
              % (len(all_windows), len(windows)))
        print("PASS Godot_ListWindows reports the editor's windows, hidden ones on request")

        # The point of the tool: knowing a dialog is open without looking at a screen.
        # Godot_AskUser is deferred, so its dialog stays up while other calls are served.
        relay.send_message({"jsonrpc": "2.0", "id": 153, "method": "tools/call",
                            "params": {"name": "Godot_AskUser",
                                       "arguments": {"question": "Is this window listed?",
                                                     "timeout_seconds": 10}}})
        # --- finding things in the editor's interface -------------------------
        reply = call({"jsonrpc": "2.0", "id": 155, "method": "tools/call",
                      "params": {"name": "Godot_FindControl", "arguments": {}}})
        check(refused(reply), "a search with no criteria was accepted")
        check("text" in refusal_text(reply),
              "the refusal does not say what to give instead: %r" % refusal_text(reply))

        reply = call({"jsonrpc": "2.0", "id": 156, "method": "tools/call",
                      "params": {"name": "Godot_FindControl",
                                 "arguments": {"class": "Button", "limit": 5}}})
        check(not refused(reply), "searching by class failed: %s" % refusal_text(reply))
        found = reply["result"]["structuredContent"]
        check(found["count"] == 5 and found["truncated"] is True,
              "limit did not cut the editor's buttons down to 5: %r" % found["count"])
        for match in found["matches"]:
            check(match["kind"] == "control", "a class search returned a non-control: %r" % match)
            check(match["width"] > 0 and match["height"] > 0,
                  "a visible button has no size: %r" % match)
            check(match["center_x"] == match["x"] + match["width"] // 2 or
                  abs(match["center_x"] - (match["x"] + match["width"] / 2)) <= 1,
                  "the reported centre is not the centre of the reported rect: %r" % match)

        # A class that matches nothing is an empty answer, not a failure - and it must
        # not be confused with the refusal above, which is about the request.
        reply = call({"jsonrpc": "2.0", "id": 157, "method": "tools/call",
                      "params": {"name": "Godot_FindControl",
                                 "arguments": {"name": "NoControlIsCalledThis"}}})
        check(not refused(reply), "a search that matches nothing was reported as an error")
        check(reply["result"]["structuredContent"]["count"] == 0,
              "a name nothing uses matched something")

        # Criteria combine: a class that exists, in a window that does not, finds nothing.
        reply = call({"jsonrpc": "2.0", "id": 158, "method": "tools/call",
                      "params": {"name": "Godot_FindControl",
                                 "arguments": {"class": "Button", "window": "No Such Window"}}})
        check(reply["result"]["structuredContent"]["count"] == 0,
              "the window filter was ignored")
        print("PASS Godot_FindControl locates editor controls and combines its criteria")

        reply = call({"jsonrpc": "2.0", "id": 159, "method": "tools/call",
                      "params": {"name": "Godot_SendEditorInput",
                                 "arguments": {"action": "nonsense"}}})
        check(refused(reply), "an unknown editor input action was accepted")

        if not has_display:
            # A headless editor has no display-server dispatch, so an event would vanish
            # silently. The refusal is the honest answer; reporting a delivery would not be.
            reply = call({"jsonrpc": "2.0", "id": 160, "method": "tools/call",
                          "params": {"name": "Godot_SendEditorInput",
                                     "arguments": {"action": "click", "x": 10, "y": 10}}})
            check(refused(reply), "a headless editor claimed to have received a click")
            check("headless" in refusal_text(reply),
                  "the refusal does not name the reason: %r" % refusal_text(reply))
            print("PASS Godot_SendEditorInput refuses cleanly when headless")
        else:
            # Aimed at nothing in particular, and asserted only on where it went: this
            # runs against the live editor, so it must not press anything.
            reply = call({"jsonrpc": "2.0", "id": 161, "method": "tools/call",
                          "params": {"name": "Godot_SendEditorInput",
                                     "arguments": {"action": "move", "x": 5, "y": 5,
                                                   "window": "No Such Window"}}})
            check(refused(reply), "the window filter did not restrict where input could go")
            print("PASS Godot_SendEditorInput honours its window filter")

        # The question's own reply can arrive in the middle of this, so replies are
        # matched by id rather than taken in order.
        question_reply = [None]

        def call_by_id(identifier, name):
            relay.send_message({"jsonrpc": "2.0", "id": identifier, "method": "tools/call",
                                "params": {"name": name}})
            while True:
                reply = relay.read_message(timeout=20)
                check(reply is not None, "no reply to %s" % name)
                if reply.get("id") == identifier:
                    return reply
                if reply.get("id") == 153:
                    question_reply[0] = reply

        titles = set()
        deadline = time.time() + 15
        while time.time() < deadline:
            reply = call_by_id(154, "Godot_ListWindows")
            titles = {window["title"] for window in
                      reply["result"]["structuredContent"]["windows"]}
            if "Godot AI asks" in titles:
                break
            time.sleep(0.5)
        check("Godot AI asks" in titles,
              "an open dialog was not among the listed windows: %r" % sorted(titles))
        print("PASS an open dialog is visible through Godot_ListWindows, with no screen")

        # Let the question time out so the deferred reply is drained before moving on.
        deadline = time.time() + 40
        while question_reply[0] is None and time.time() < deadline:
            reply = relay.read_message(timeout=30)
            if reply is not None and reply.get("id") == 153:
                question_reply[0] = reply
        check(question_reply[0] is not None,
              "the pending question never produced its reply")

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
