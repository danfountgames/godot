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
import collections
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from relay_harness import REPO_ROOT, RELAY_BINARY, RelayProcess  # noqa: E402

sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))

import virtual_display  # noqa: E402

def default_editor():
    if sys.platform == "darwin":
        machine = os.uname().machine
        arch = "arm64" if machine == "arm64" else "x86_64"
        return os.path.join(REPO_ROOT, "bin", "godot.macos.editor.dev.%s" % arch)
    if sys.platform.startswith("win"):
        return os.path.join(REPO_ROOT, "bin", "godot.windows.editor.dev.x86_64.exe")
    return os.path.join(REPO_ROOT, "bin", "godot.linuxbsd.editor.dev.x86_64")


DEFAULT_EDITOR = default_editor()

PROJECT_GODOT = """config_version=5

[application]

config/name="AI E2E Project"
run/main_scene="res://scenes/main.tscn"
config/features=PackedStringArray("4.3")

[input]

; A project-defined action, so Godot_SendActionInput can be tested against the thing it
; is actually for: a game reads the action, not the key, and a test bound to the key
; passes until somebody rebinds it.
e2e_fire={
"deadzone": 0.5,
"events": [Object(InputEventKey,"resource_local_to_scene":false,"resource_name":"","device":-1,"window_id":0,"alt_pressed":false,"shift_pressed":false,"ctrl_pressed":false,"meta_pressed":false,"pressed":false,"keycode":0,"physical_keycode":70,"key_label":0,"unicode":0,"location":0,"echo":false,"script":null)
]
}

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

var action_pressed := 0
var action_released := 0
var action_is_held := false
var action_strength := 0.0

# A cancelled touch is not a release. The engine models it as pressed = false *and*
# canceled = true, so `is_released()` is false for it - a game that collapses the two
# fires the button the player was dragging away from when a notification arrives.
var touch_released := 0
var touch_canceled := 0

# An error raised three calls deep. The handler's own file and line give the call site;
# only a real stack shows the route that reached it, which is what tells you which of a
# helper's callers passed the bad value.
var trigger_deep_error := false:
	set(value):
		trigger_deep_error = value
		if value:
			_level_one()

func _level_one() -> void:
	_level_two()

func _level_two() -> void:
	_level_three()

func _level_three() -> void:
	push_error("E2E_DEEP_ERROR")

func _input(event: InputEvent) -> void:
	if event is InputEventScreenTouch and not event.pressed:
		if event.canceled:
			touch_canceled += 1
		else:
			touch_released += 1
	# Read as an *action*, which is what a game does, so a test of Godot_SendActionInput
	# proves the input reached the path the player's key would take rather than proving
	# that some event arrived.
	if event.is_action_pressed("e2e_fire"):
		action_pressed += 1
		action_strength = event.get_action_strength("e2e_fire")
	elif event.is_action_released("e2e_fire"):
		action_released += 1

func _process(_delta: float) -> void:
	# The held state, not the event. A press with no release has to be visible as a
	# *state*, or "held" and "tapped" are indistinguishable from the outside.
	action_is_held = Input.is_action_pressed("e2e_fire")

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

SAVES_SCRIPT = """extends Node

# A save system with a recovery path, so that "the tool can write a malformed save" can
# become "a malformed save is survived". Without a game that has saves there is nothing
# for a corruption fixture to be a fixture *of*.
const SAVE_PATH := "user://save.json"

var slot := {"level": 1, "score": 0}
# Mirrored as a plain int: Godot_GetRuntimeProperty returns anything that is not a
# scalar in Godot's own text form, so reading `slot` back means parsing a string.
var level := 1
var load_result := ""

var save_now := 0:
	set(value):
		save_now = value
		if value > 0:
			_write()

var load_now := 0:
	set(value):
		load_now = value
		if value > 0:
			_read()

func _write() -> void:
	var file := FileAccess.open(SAVE_PATH, FileAccess.WRITE)
	file.store_string(JSON.stringify(slot))
	file.close()

func _read() -> void:
	if not FileAccess.file_exists(SAVE_PATH):
		load_result = "missing"
		return
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(SAVE_PATH))
	# The recovery path. A save that will not parse must not take the game down, and
	# must not be quietly treated as a fresh start either - the player is told.
	if typeof(parsed) != TYPE_DICTIONARY or not parsed.has("level"):
		slot = {"level": 1, "score": 0}
		level = 1
		load_result = "recovered"
		return
	slot = parsed
	level = int(slot["level"])
	load_result = "loaded"
"""

CHIMES_SCRIPT = """extends Node

# Two players, one stream. Setting play_count to 2 makes the same sound play twice at
# once - which no bus peak can distinguish from one loud playback, and which is the
# audio bug an agent working without ears is most likely to ship.
var play_count := 0:
	set(value):
		play_count = value
		if value == 0:
			$One.stop()
			$Two.stop()
		if value >= 1:
			$One.play()
		if value >= 2:
			$Two.play()

# A burst: both players start and both stop again a fraction of a second later. This is
# the case a snapshot cannot catch - by the time anything asks, the stacking is over -
# and it is exactly what a sound triggered twice by one button press looks like.
var flash := 0:
	set(value):
		flash = value
		if value > 0:
			_flash()

# A stream built in script has no resource path. Skipping such streams made every
# procedurally generated sound invisible - a game whose audio is entirely generated
# reported nothing playing while it was audible.
func _ready() -> void:
	var generated := AudioStreamWAV.new()
	var frames := PackedByteArray()
	for i in range(8000):
		frames.append(128 + int(60.0 * sin(float(i) * 0.05)))
	generated.data = frames
	generated.format = AudioStreamWAV.FORMAT_8_BITS
	generated.mix_rate = 8000
	$Made.stream = generated

var play_generated := 0:
	set(value):
		play_generated = value
		if value > 0:
			$Made.play()
		else:
			$Made.stop()

func _flash() -> void:
	$One.play()
	$Two.play()
	await get_tree().create_timer(0.25).timeout
	$One.stop()
	$Two.stop()
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

INSPECTOR_RESOURCE = """[gd_resource type="GradientTexture2D" load_steps=2 format=3]

[sub_resource type="Gradient" id="Gradient_docs"]
offsets = PackedFloat32Array(0, 0.35, 1)
colors = PackedColorArray(0.1, 0.2, 0.8, 1, 0.9, 0.4, 0.1, 1, 0.95, 0.9, 0.2, 1)

[resource]
gradient = SubResource("Gradient_docs")
width = 512
height = 128
"""

DOCUMENTATION_SCENE = """[gd_scene format=3]

[node name="DocumentationScene" type="Node"]

[node name="Section" type="Node" parent="."]

[node name="Target" type="Button" parent="Section"]
text = "Documented button"
"""

MAIN_SCENE = """[gd_scene load_steps=6 format=3 uid="uid://bqxaie2e001"]

[ext_resource type="Script" path="res://scripts/target.gd" id="1"]
[ext_resource type="Script" path="res://scripts/chimes.gd" id="2"]
[ext_resource type="AudioStream" path="res://audio/chime.wav" id="3"]
[ext_resource type="Script" path="res://scripts/surface.gd" id="4"]
[ext_resource type="Script" path="res://scripts/saves.gd" id="5"]

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

[node name="Saves" type="Node" parent="."]
script = ExtResource("5")

[node name="Chimes" type="Node" parent="."]
script = ExtResource("2")

[node name="One" type="AudioStreamPlayer" parent="Chimes"]
stream = ExtResource("3")

[node name="Two" type="AudioStreamPlayer" parent="Chimes"]
stream = ExtResource("3")

[node name="Made" type="AudioStreamPlayer" parent="Chimes"]
"""

# The button records two different things on purpose. `pressed` fires however the
# button was activated; `saw_input_event` only becomes true if a real InputEvent
# reached _gui_input. An implementation of Godot_SendPointerInput that took a shortcut
# - calling the handler, emitting the signal - would satisfy the first and fail the
# second, which is the only reason this test is worth anything.
PATROL_ROUTE_SCRIPT = '''class_name PatrolRoute
extends Node2D

## A closed loop of waypoints an enemy walks.
##
## Nothing in the engine has a class like this, which is the point: an agent that
## can only recall Godot's own API cannot know it exists.

## How long to pause at each waypoint, in seconds.
@export var dwell_seconds: float = 1.5

## Emitted when the walker gets back to where it started.
signal loop_completed(laps: int)

## Returns the waypoint after [param index], wrapping at the end of the route.
func next_waypoint(index: int) -> Vector2:
\treturn Vector2.ZERO
'''

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


class EditorOutput:
    """Drains the editor's output and keeps the tail of it.

    Two reasons, both learned here. A pipe nobody reads fills at 64 KiB and blocks the
    editor mid-write, which looks like a hang and is not one. And when the editor dies -
    which is how a real crash reaches this script, as `editor disconnected` - its own last
    words are the only evidence of why, and they were being thrown away.
    """

    KEPT_LINES = 200

    def __init__(self, process):
        self.lines = collections.deque(maxlen=self.KEPT_LINES)
        self._thread = threading.Thread(target=self._drain, args=(process,), daemon=True)
        self._thread.start()

    def _drain(self, process):
        try:
            for line in iter(process.stdout.readline, b""):
                self.lines.append(line.decode("utf-8", "replace").rstrip("\n"))
        except (ValueError, OSError):
            # The pipe closed under us while the editor was being torn down.
            pass

    def tail(self):
        return "\n".join(self.lines)


class NativeMacOSDisplay:
    """The macOS window server is already the display; it does not use DISPLAY/Xvfb."""

    usable = True
    display = "native macOS"

    def environment(self):
        return dict(os.environ)

    def godot_arguments(self):
        return []

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        return False


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
    with open(os.path.join(root, "scripts", "saves.gd"), "w") as handle:
        handle.write(SAVES_SCRIPT)
    # A documented class of the project's own. The class reference is generated from
    # this too, which is what makes Godot_LookupClass a way to explore *this project*
    # rather than only a way to look up Godot.
    with open(os.path.join(root, "scripts", "patrol_route.gd"), "w") as handle:
        handle.write(PATROL_ROUTE_SCRIPT)
    os.makedirs(os.path.join(root, "audio"), exist_ok=True)
    write_wav(os.path.join(root, "audio", "chime.wav"))
    with open(os.path.join(root, "project.godot"), "w") as handle:
        handle.write(PROJECT_GODOT)
    with open(os.path.join(root, "scenes", "main.tscn"), "w") as handle:
        handle.write(MAIN_SCENE)
    with open(os.path.join(root, "notes.txt"), "w") as handle:
        handle.write("hello from a project text file\n")
    os.makedirs(os.path.join(root, "docs"), exist_ok=True)
    with open(os.path.join(root, "docs", "inspector_fixture.tres"), "w") as handle:
        handle.write(INSPECTOR_RESOURCE)
    with open(os.path.join(root, "docs", "capture_scene.tscn"), "w") as handle:
        handle.write(DOCUMENTATION_SCENE)
    write_png(os.path.join(root, "sprite.png"))
    # A script that does not parse. Nothing else in the interface can see this: the
    # scene silently fails to instantiate and every other tool points elsewhere.
    with open(os.path.join(root, "scripts", "broken.gd"), "w") as handle:
        handle.write("extends Node\n\nfunc broken(:\n\tpass\n")

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
    editor_output = EditorOutput(editor)

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
            # Long enough for the slowest legitimate answer, not for a hang. The capture
            # tools advance on drawn frames and give themselves 90 seconds on a
            # software-rendered editor; a client ceiling below that turns patience into a
            # failure. Only one call is ever outstanding, so a genuine hang still fails -
            # it just takes longer to say so.
            reply = relay.read_message(timeout=150)
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
                         "Godot_CaptureViewport", "Godot_CaptureInspectorProperty",
                         "Godot_CaptureSceneTreeNode", "Godot_AskUser"):
            check(expected in names, "tools/list is missing %s" % expected)
        print("PASS tools/list (%d tools)" % len(names))

        # A native editor can advertise the MCP service before its first filesystem
        # scan has imported the just-created fixture. Opening a scene during that
        # window is legitimately deferred by the editor, so synchronize on the
        # product's import-queue tool instead of sleeping and hoping.
        reply = call({"jsonrpc": "2.0", "id": 2000, "method": "tools/call",
                      "params": {"name": "Godot_WaitForImportQueue",
                                 "arguments": {"timeout_seconds": 60}}})
        check(not refused(reply),
              "initial import queue did not become idle: %s" % refusal_text(reply))
        print("PASS initial project import completed")

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
        check(not refused(reply), "opening the fixture scene failed: %s" % refusal_text(reply))
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
        # What matters is that the front matter is gone and the instructions are there -
        # not the skill's opening sentence, which is prose and is rewritten whenever the
        # workflow improves. Pinning the sentence made editing a skill fail this check.
        check(not text.lstrip().startswith("---"),
              "skill instructions were not returned with the frontmatter stripped")
        check("name: scene-cleanup" not in text,
              "the skill's front matter leaked into its instructions")
        check("Godot_ManageNode" in text,
              "the skill instructions do not mention the tool they are about")
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
        #
        # This needs a display, and not for the reason it looks like. Godot_AskUser is
        # only the vehicle; what is being tested is the deferral. With no display there
        # is nobody to ask at all, so the tool now answers immediately by design and
        # cannot carry this test - see the unattended check below, and the deferred
        # path is exercised headless by the polled tools later in this run.
        if has_display:
            before = time.time()
            reply = call({"jsonrpc": "2.0", "id": 90, "method": "tools/call",
                          "params": {"name": "Godot_AskUser",
                                     "arguments": {"question": "Is anyone there?",
                                                   "timeout_seconds": 2}}})
            elapsed = time.time() - before
            check(refused(reply), "an unanswered question did not fail")
            check("timed out" in refusal_text(reply),
                  "the unanswered question did not report a timeout: %r" % refusal_text(reply))
            check(elapsed >= 1.5,
                  "the response came back too fast to have been deferred (%.2fs)" % elapsed)

            # The editor must still be serving other calls while a question is pending.
            reply = call({"jsonrpc": "2.0", "id": 91, "method": "tools/call",
                          "params": {"name": "Godot_GetEditorStatus"}})
            check(reply["result"]["isError"] is False,
                  "the editor stopped serving after a deferred call")
            print("PASS Godot_AskUser deferred the response and timed out cleanly")

            # The timeout answered the client. The dialog has to go with it: left up,
            # it invites an answer, accepts the click, and does nothing, because the
            # token it would complete is already gone. This used to be exactly what
            # happened. Inside the display branch because with no display no dialog
            # was ever opened, and a check that passes because nothing happened is
            # not a check.
            stale = []
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
        else:
            # The headless path, which nothing used to check because every dialog test
            # was behind `has_display`. An EditorNode is not a person: `--headless
            # --editor` has a complete one and nobody looking at it, so this used to
            # open a dialog on the dummy display server and wait out the whole timeout.
            started = time.time()
            reply = call({"jsonrpc": "2.0", "id": 95, "method": "tools/call",
                          "params": {"name": "Godot_AskUser",
                                     "arguments": {"question": "is anybody there?",
                                                   "timeout_seconds": 120}}})
            elapsed = time.time() - started
            check(refused(reply), "an unattended editor accepted a question for a human")
            check(elapsed < 15,
                  "the question was refused but took %.1fs; it waited rather than answering"
                  % elapsed)
            check("nobody" in refusal_text(reply),
                  "the refusal does not say why: %s" % refusal_text(reply))
            print("PASS an unattended editor refuses a question at once (%.1fs)" % elapsed)

        # --- proposing a plan before doing any of it ---------------------------
        #
        # The claim worth checking is the grouping. "Not 40 separate approvals" is the
        # requirement; a plan that turned every change into its own tick would satisfy
        # the letter of it and miss the point entirely.
        plan_changes = [
            {"description": "rename the player node", "tool": "Godot_ManageNode",
             "arguments": {"action": "rename", "path": "Player", "name": "Hero"}},
            {"description": "rename the enemy node", "tool": "Godot_ManageNode",
             "arguments": {"action": "rename", "path": "Enemy", "name": "Foe"}},
            {"description": "drop the unused spawner", "tool": "Godot_ManageNode",
             "arguments": {"action": "delete", "path": "OldSpawner"}},
            {"description": "throw away the old save", "tool": "Godot_DeleteUserFile",
             "arguments": {"path": "user://saves/slot1.json", "confirm": True}},
        ]

        reply = call({"jsonrpc": "2.0", "id": 760, "method": "tools/call",
                      "params": {"name": "Godot_ProposeChange",
                                 "arguments": {"title": "tidy the scene",
                                               "changes": plan_changes,
                                               "dry_run": True}}})
        check(not refused(reply), "building a plan failed: %s" % refusal_text(reply))
        plan = reply["result"]["structuredContent"]
        check(plan["item_count"] == 4, "the plan lost changes: %r" % plan)
        check(plan["decided"] is False, "a dry run reported a decision nobody made: %r" % plan)

        risks = {item["index"]: item["risk"] for item in plan["items"]}
        # Two renames: scene edits, held in the undo history, so reversible and narrow.
        check(risks[0] == "mechanical" and risks[1] == "mechanical",
              "a rename was not treated as reversible: %r" % risks)
        # Deleting a node is undoable but worth seeing on its own.
        check(risks[2] == "substantial", "a node delete was mis-ranked: %r" % risks)
        # Deleting a user file is not undoable by anything here.
        check(risks[3] == "irreversible", "a file delete was mis-ranked: %r" % risks)

        groups = plan["groups"]
        check(len(groups) == 3,
              "four changes should be three decisions, not %d: %r"
              % (len(groups), [g["key"] for g in groups]))
        batched = [g for g in groups if g["risk"] == "mechanical"]
        check(len(batched) == 1 and len(batched[0]["items"]) == 2,
              "the two reversible changes were not offered as one decision: %r" % groups)
        print("PASS Godot_ProposeChange turned 4 changes into 3 decisions, ranked by risk")

        # A plan naming a tool that does not exist is refused now, not discovered after
        # three of its changes have already happened.
        reply = call({"jsonrpc": "2.0", "id": 761, "method": "tools/call",
                      "params": {"name": "Godot_ProposeChange",
                                 "arguments": {"title": "bad plan", "dry_run": True,
                                               "changes": [{"description": "do a thing",
                                                            "tool": "Godot_NoSuchTool"}]}}})
        check(refused(reply), "a plan naming an unknown tool was accepted")
        check("not a tool" in refusal_text(reply),
              "the refusal does not say what was wrong: %r" % refusal_text(reply))

        # And one whose arguments the tool's own schema would reject is rejected while it
        # is still a plan, by that same schema. Schema-level only, and the tool says so:
        # a rule a tool enforces when it runs - Godot_ManageNode wanting a `path` for a
        # rename but not for every action - is not in the schema and cannot be caught here.
        reply = call({"jsonrpc": "2.0", "id": 762, "method": "tools/call",
                      "params": {"name": "Godot_ProposeChange",
                                 "arguments": {"title": "bad arguments", "dry_run": True,
                                               "changes": [{"description": "do something vague",
                                                            "tool": "Godot_ManageNode",
                                                            "arguments": {"name": "Hero"}}]}}})
        check(refused(reply), "a plan missing a required argument was accepted")
        check("action" in refusal_text(reply),
              "the refusal does not name the missing argument: %r" % refusal_text(reply))

        # A call nobody described is not a proposal.
        reply = call({"jsonrpc": "2.0", "id": 763, "method": "tools/call",
                      "params": {"name": "Godot_ProposeChange",
                                 "arguments": {"title": "unlabelled", "dry_run": True,
                                               "changes": [{"tool": "Godot_ListScenes"}]}}})
        check(refused(reply), "a change with no description was accepted")
        check("description" in refusal_text(reply),
              "the refusal does not name the missing field: %r" % refusal_text(reply))
        print("PASS a plan that could not be carried out is refused while it is still a plan")

        if has_display and shutil.which("xdotool"):
            # Now the decision itself: ask the editor where its own button is, then click
            # there with a real pointer.
            #
            # Both halves are needed, and finding that out cost two diagnoses. A key press
            # is no good: there is no window manager on a bare Xvfb, so X input focus is
            # PointerRoot and `xdotool key` goes wherever the pointer happens to be, which
            # meant Escape landing in the editor behind the dialog while the test waited
            # out the whole deferred timeout. And Godot_SendEditorInput is no good either -
            # it reaches controls in the editor's main window, which is what the Activity
            # dock checks use it for, but this dialog is a separate native window and the
            # injected event never arrives. Godot_FindControl still reports the button's
            # screen rectangle correctly, so the coordinates come from the editor and the
            # click comes from X.
            def read_for(identifier, timeout=90):
                """Reads until the reply to `identifier`, ignoring anything else."""
                deadline = time.time() + timeout
                while time.time() < deadline:
                    message = relay.read_message(timeout=5)
                    if message is not None and float(message.get("id", -1)) == float(identifier):
                        return message
                return None

            def propose(identifier, arguments):
                relay.send_message({"jsonrpc": "2.0", "id": identifier, "method": "tools/call",
                                    "params": {"name": "Godot_ProposeChange",
                                               "arguments": arguments}})
                # Wait for the dialog to exist before looking for anything inside it.
                deadline = time.time() + 20
                while time.time() < deadline:
                    relay.send_message({"jsonrpc": "2.0", "id": 900, "method": "tools/call",
                                        "params": {"name": "Godot_ListWindows",
                                                   "arguments": {}}})
                    listed = read_for(900, timeout=20)
                    titles = [w["title"] for w
                              in listed["result"]["structuredContent"]["windows"]]
                    if "Godot AI proposes a change" in titles:
                        return
                    time.sleep(0.5)
                raise Failure("no dialog appeared for the proposal: %r" % titles)

            def press(identifier, label):
                relay.send_message({"jsonrpc": "2.0", "id": identifier, "method": "tools/call",
                                    "params": {"name": "Godot_FindControl",
                                               "arguments": {"text": label, "class": "Button",
                                                             "window": "Godot AI proposes a change"}}})
                found = read_for(identifier, timeout=30)
                check(found is not None, "looking for the %r button produced no reply" % label)
                matches = found["result"]["structuredContent"]["matches"]
                check(matches, "the plan dialog has no %r button" % label)
                check(matches[0]["window"] == "Godot AI proposes a change",
                      "the %r button found is not the plan dialog's: %r" % (label, matches[0]))
                xdotool(display.display, "mousemove", str(matches[0]["center_x"]),
                        str(matches[0]["center_y"]), "click", "1")

            propose(764, {"title": "tidy the scene", "changes": plan_changes,
                          "timeout_seconds": 90})
            press(770, "Leave It")
            reply = read_for(764)
            check(reply is not None, "declining the plan produced no response")
            check(not refused(reply), "declining the plan errored: %s" % refusal_text(reply))
            dismissed = reply["result"]["structuredContent"]
            check(dismissed["cancelled"] is True,
                  "declining the plan was not reported as a cancellation: %r" % dismissed)
            check(dismissed["approved_items"] == [],
                  "a declined plan approved something: %r" % dismissed)
            check(dismissed["calls"] == [],
                  "a declined plan handed back calls to make: %r" % dismissed)
            print("PASS declining a plan approves nothing and hands back no calls")

            propose(780, {"title": "tidy the scene", "changes": plan_changes,
                          "timeout_seconds": 90})
            # Only the reversible group starts ticked, so pressing Apply straight away is
            # the check that a default Apply cannot delete anything.
            press(790, "Apply Ticked")
            reply = read_for(780)
            check(reply is not None, "accepting the plan produced no response")
            check(not refused(reply), "accepting the plan errored: %s" % refusal_text(reply))
            decided = reply["result"]["structuredContent"]
            check(decided["decided"] is True, "the plan was not decided: %r" % decided)
            check(sorted(decided["approved_items"]) == [0, 1],
                  "accepting the default ticks approved the wrong changes: %r" % decided)
            check(3 in decided["rejected_items"],
                  "the irreversible change was approved by default: %r" % decided)
            check([c["index"] for c in decided["calls"]] == [0, 1],
                  "the calls handed back are not the approved ones: %r" % decided["calls"])
            # And they carry the real arguments, because that is what was agreed to.
            check(decided["calls"][0]["arguments"]["name"] == "Hero",
                  "the approved call lost its arguments: %r" % decided["calls"][0])
            print("PASS applying a plan's defaults approves the reversible changes and no others")
        elif has_display:
            print("SKIP deciding a plan: xdotool is not installed")
        else:
            # Unattended, a plan degrades to the dry run it already knew how to produce
            # rather than to an error: the grouping and validation are still worth
            # having, and the caller is told plainly that nobody approved anything.
            started = time.time()
            reply = call({"jsonrpc": "2.0", "id": 796, "method": "tools/call",
                          "params": {"name": "Godot_ProposeChange",
                                     "arguments": {"title": "tidy the scene",
                                                   "changes": plan_changes,
                                                   "timeout_seconds": 120}}})
            elapsed = time.time() - started
            check(not refused(reply), "an unattended plan errored: %s" % refusal_text(reply))
            check(elapsed < 15,
                  "the plan came back but took %.1fs; it waited for nobody" % elapsed)
            unattended_plan = reply["result"]["structuredContent"]
            check(unattended_plan["unattended"] is True,
                  "the plan does not say it was unattended: %r" % unattended_plan)
            check(unattended_plan["decided"] is False,
                  "an unattended plan claimed a decision: %r" % unattended_plan)
            # The point of degrading rather than erroring: the work is still there.
            check(len(unattended_plan["groups"]) > 1,
                  "the unattended plan lost its grouping: %r" % unattended_plan)
            print("PASS an unattended plan degrades to a dry run at once (%.1fs)" % elapsed)

        # --- screenshots ------------------------------------------------------
        inspector_resource_args = {
            "resource": "res://docs/inspector_fixture.tres",
            "property_chain": ["gradient", "offsets"],
            "context_properties": ["colors"],
            "path": "res://docs/inspector-gradient-offsets.png",
            "context_above": 90,
            "context_below": 110,
        }
        inspector_node_args = {
            "scene": "res://docs/capture_scene.tscn",
            "node_path": "Section/Target",
            "property_chain": ["text"],
            "path": "res://docs/inspector-target-text.png",
            "context_above": 80,
            "context_below": 100,
        }
        scene_tree_args = {
            "scene": "res://docs/capture_scene.tscn",
            "node_path": "Section/Target",
            "path": "res://docs/scene-tree-target.png",
            "context_above": 90,
            "context_below": 110,
        }

        semantic_captures = [
            (82, "Godot_CaptureInspectorProperty", inspector_resource_args),
            (83, "Godot_CaptureInspectorProperty", inspector_node_args),
            (84, "Godot_CaptureSceneTreeNode", scene_tree_args),
        ]
        # Timed, and the timing printed whether or not it passes. These advance on drawn
        # frames against a 90-second budget, and the failure mode is not a wrong image -
        # it is creeping back up towards that ceiling until CI goes red intermittently. A
        # number in the log makes the drift visible while it is still only drift.
        semantic_replies = []
        for identifier, tool_name, arguments in semantic_captures:
            began = time.monotonic()
            semantic_replies.append((tool_name, arguments, call({
                "jsonrpc": "2.0", "id": identifier, "method": "tools/call",
                "params": {"name": tool_name, "arguments": arguments},
            })))
            print("      %s answered in %.1fs" % (tool_name, time.monotonic() - began))

        # This editor is headless, so the honest answer is a refusal that names the
        # reason - not a blank image presented as if it were the editor.
        reply = call({"jsonrpc": "2.0", "id": 80, "method": "tools/call",
                      "params": {"name": "Godot_CaptureViewport"}})
        if not has_display:
            check(refused(reply), "a headless editor produced a screenshot")
            check("headless" in refusal_text(reply),
                  "the capture refusal does not name the reason: %r" % refusal_text(reply))
            print("PASS Godot_CaptureViewport refuses cleanly when headless")
            for tool_name, arguments, semantic_reply in semantic_replies:
                check(refused(semantic_reply), "%s produced a screenshot while headless" % tool_name)
                check("headless" in refusal_text(semantic_reply),
                      "%s refusal does not name headless rendering" % tool_name)
            print("PASS semantic Inspector and Scene tree captures refuse cleanly when headless")
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

            for tool_name, arguments, semantic_reply in semantic_replies:
                check(semantic_reply["result"]["isError"] is False,
                      "%s failed for %r: %s" % (tool_name, arguments, refusal_text(semantic_reply)))
                crop = semantic_reply["result"]["structuredContent"]
                check(120 < crop["width"] < shot["width"],
                      "%s did not crop to a plausible dock width: %r" % (tool_name, crop))
                check(60 < crop["height"] < shot["height"],
                      "%s did not crop vertically around its target: %r" % (tool_name, crop))
                check(0 <= crop["target_y"] < crop["height"],
                      "%s target is outside its crop: %r" % (tool_name, crop))
                check(crop["target_y"] + crop["target_height"] <= crop["height"] + 2,
                      "%s target extends outside its crop: %r" % (tool_name, crop))
                on_disk = os.path.join(project, crop["path"].replace("res://", ""))
                with open(on_disk, "rb") as handle:
                    check(handle.read(8) == b"\x89PNG\r\n\x1a\n",
                          "%s output is not a PNG" % tool_name)
                images = [c for c in semantic_reply["result"]["content"]
                          if c["type"] == "image"]
                check(images and base64.b64decode(images[0]["data"])[:8] == b"\x89PNG\r\n\x1a\n",
                      "%s did not return its crop inline" % tool_name)
                check_capture_metadata(crop, "editor_viewport")
            resource_crop = semantic_replies[0][2]["result"]["structuredContent"]
            check(resource_crop["property_chain"] == ["gradient", "offsets"],
                  "nested resource property chain was not preserved: %r" % resource_crop)
            check(resource_crop["context_properties"] == ["colors"],
                  "Inspector context properties were not preserved: %r" % resource_crop)
            check(resource_crop["target_property"] == "offsets",
                  "wrong nested Inspector target: %r" % resource_crop)
            node_crop = semantic_replies[1][2]["result"]["structuredContent"]
            check(node_crop["scene"] == "res://docs/capture_scene.tscn"
                  and node_crop["node_path"] == "Section/Target"
                  and node_crop["target_property"] == "text",
                  "scene node Inspector target was not reported exactly: %r" % node_crop)
            tree_crop = semantic_replies[2][2]["result"]["structuredContent"]
            check(tree_crop["node_path"] == "Section/Target" and tree_crop["node_name"] == "Target",
                  "Scene tree capture selected the wrong node: %r" % tree_crop)
            check(tree_crop["ancestor_depth"] >= 1 and tree_crop["expanded_ancestors"] >= 0,
                  "Scene tree capture did not traverse the target's ancestors: %r" % tree_crop)
            print("PASS semantic Inspector captures covered nested Resource and scene-node properties")
            print("PASS semantic Scene tree capture expanded, selected, highlighted and cropped its node")

            restored = call({"jsonrpc": "2.0", "id": 85, "method": "tools/call",
                             "params": {"name": "Godot_GetEditedSceneTree"}})
            restored_nodes = restored["result"]["structuredContent"]["nodes"]
            check(restored_nodes and restored_nodes[0]["name"] == "Main",
                  "semantic captures did not restore the original scene tab: %r" % restored_nodes)
            print("PASS semantic captures restored the original scene tab")

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

        # --- the activity stream -------------------------------------------
        # By now this session has made dozens of calls. The stream should show them,
        # in order, with the concrete things each one touched - that is what the dock
        # will render, and asserting it here is what keeps it regression-testable.
        reply = call({"jsonrpc": "2.0", "id": 250, "method": "tools/call",
                      "params": {"name": "Godot_GetActivity", "arguments": {"limit": 200}}})
        check(not refused(reply), "reading activity failed: %s" % refusal_text(reply))
        activity = reply["result"]["structuredContent"]
        records = activity["records"]
        check(len(records) > 5, "the activity stream is suspiciously short: %r" % len(records))
        check(activity["latest_sequence"] >= len(records),
              "the sequence counter disagrees with the buffer: %r" % activity)

        tools_seen = {record["tool"] for record in records}
        check("Godot_OpenScene" in tools_seen,
              "the stream did not record a call this run definitely made: %r" % sorted(tools_seen))
        for record in records:
            check(record["outcome"] in ("running", "ok", "failed", "refused", "deferred"),
                  "unknown outcome in the stream: %r" % record)
            check("capability" in record and "summary" in record,
                  "a record is missing its capability or summary: %r" % record)

        # E3: a record names what it touched, not just which tool ran.
        with_subjects = [r for r in records if r["subjects"]]
        check(with_subjects, "no record named any node or file it touched")
        subject_kinds = {s["kind"] for r in with_subjects for s in r["subjects"]}
        check("file" in subject_kinds,
              "no record named a file, so the dock would have nothing to reveal: %r"
              % subject_kinds)

        # Refusals are in the stream too - the trail has to show what was attempted.
        sequence_before = activity["latest_sequence"]
        call({"jsonrpc": "2.0", "id": 251, "method": "tools/call",
              "params": {"name": "Godot_ReadTextFile",
                         "arguments": {"path": "res://definitely_not_here.txt"}}})
        reply = call({"jsonrpc": "2.0", "id": 252, "method": "tools/call",
                      "params": {"name": "Godot_GetActivity",
                                 "arguments": {"after_sequence": sequence_before}}})
        fresh = reply["result"]["structuredContent"]["records"]
        check(fresh, "polling by sequence returned nothing after two more calls")
        check(all(r["sequence"] > sequence_before for r in fresh),
              "polling by sequence returned records it should have skipped: %r" % fresh)
        print("PASS Godot_GetActivity streams %d records, with the files they touched"
              % len(records))

        # --- declared intent --------------------------------------------------
        reply = call({"jsonrpc": "2.0", "id": 260, "method": "tools/call",
                      "params": {"name": "Godot_SetIntent",
                                 "arguments": {"goal": "Prove the activity stream",
                                               "activity": "Declaring an intent"}}})
        check(not refused(reply), "setting intent failed: %s" % refusal_text(reply))
        check(reply["result"]["structuredContent"]["goal"] == "Prove the activity stream",
              "the goal did not stick")

        # Poll from here, not from the start: snapshot() returns the *oldest* records
        # after a sequence, so a bare limit would hand back the beginning of the run and
        # find an earlier Godot_ListScenes that predates the intent.
        reply = call({"jsonrpc": "2.0", "id": 261, "method": "tools/call",
                      "params": {"name": "Godot_GetActivity", "arguments": {"limit": 1}}})
        before_intent_calls = reply["result"]["structuredContent"]["latest_sequence"]

        call({"jsonrpc": "2.0", "id": 262, "method": "tools/call",
              "params": {"name": "Godot_ListScenes", "arguments": {}}})
        reply = call({"jsonrpc": "2.0", "id": 263, "method": "tools/call",
                      "params": {"name": "Godot_GetActivity",
                                 "arguments": {"after_sequence": before_intent_calls}}})
        stamped = [r for r in reply["result"]["structuredContent"]["records"]
                   if r["tool"] == "Godot_ListScenes"]
        check(stamped and stamped[-1]["goal"] == "Prove the activity stream",
              "a call made after the intent was set did not carry it: %r" % stamped)
        print("PASS Godot_SetIntent stamps every call made after it")

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
                def surface_press_count():
                    probe = call({"jsonrpc": "2.0", "id": 234, "method": "tools/call",
                                  "params": {"name": "Godot_GetRuntimeProperty",
                                             "arguments": {"path": "/root/Main/Hud/Target",
                                                           "property": "press_count"}}})
                    check(not refused(probe),
                          "reading press_count failed: %s" % refusal_text(probe))
                    return probe["result"]["structuredContent"]["value"]

                def surface(field):
                    probe = call({"jsonrpc": "2.0", "id": 190, "method": "tools/call",
                                  "params": {"name": "Godot_GetRuntimeProperty",
                                             "arguments": {"path": "/root/Main/Hud/Surface",
                                                           "property": field}}})
                    check(not refused(probe),
                          "reading Surface.%s failed: %s" % (field, refusal_text(probe)))
                    return probe["result"]["structuredContent"]["value"]

                check(surface("drag_events") == 0, "the surface saw a drag before one was sent")
                # The Surface fixture spans (500,100)-(900,500) in viewport space, and
                # input maps 1:1 to the viewport here. This used to drag a fixed
                # 550->850, which is fine on a large screen and refused under a virtual
                # display, where the game gets 846x475 - so the drag ended one pixel
                # outside the window and the test failed for a reason that had nothing to
                # do with dragging. That is every Linux run, CI's included. Keep the
                # documented intent - a horizontal drag across the Surface - and clamp the
                # far end into whatever window the game actually got.
                window = call({"jsonrpc": "2.0", "id": 189, "method": "tools/call",
                               "params": {"name": "Godot_GetGameWindowInfo",
                                          "arguments": {}}})
                check(not refused(window),
                      "reading the game window failed: %s" % refusal_text(window))
                game_window = window["result"]["structuredContent"]
                drag_to_x = min(850, int(game_window["width"]) - 8)
                check(drag_to_x > 600,
                      "the game window is too narrow to drag across the Surface: %r"
                      % game_window)
                reply = call({"jsonrpc": "2.0", "id": 191, "method": "tools/call",
                              "params": {"name": "Godot_SendPointerInput",
                                         "arguments": {"action": "drag",
                                                       "x": 550, "y": 150,
                                                       "to_x": drag_to_x, "to_y": 150,
                                                       "steps": 10}}})
                check(not refused(reply), "dragging failed: %s" % refusal_text(reply))
                dragged = reply["result"]["structuredContent"]
                check(dragged["events"] == 13,
                      "a 10-step drag should be a move, a press, 10 motions and a release: %r"
                      % dragged)
                # Godot 4.8 may synthesize one zero-distance motion while updating the
                # pressed mouse state. The ten interpolated motions must all arrive;
                # the harmless state-sync event is allowed but an arbitrary stream is
                # not.
                delivered_drag_events = surface("drag_events")
                check(10 <= delivered_drag_events <= 11,
                      "the surface did not see the 10 drag motions: %r"
                      % delivered_drag_events)
                check(surface("drag_had_button") is True,
                      "the drag's motion did not carry the held button, so a game asking "
                      "button_mask would read it as a hover")
                distance = surface("drag_distance")
                expected_distance = float(drag_to_x - 550)
                check(abs(distance - expected_distance) < 2.0,
                      "the drag covered %r pixels, not the %r between its ends"
                      % (distance, expected_distance))

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

                # --- a whole playtest, start to report --------------------------
                #
                # The workflow the primitives above exist for: a stated goal, a window of
                # play, and a report that is checked against what actually happened rather
                # than against what the caller says happened.
                reply = call({"jsonrpc": "2.0", "id": 700, "method": "tools/call",
                              "params": {"name": "Godot_StartPlaytest",
                                         "arguments": {"goal": "type a word into the field",
                                                       "name": "e2e typing",
                                                       "budget_seconds": 60,
                                                       "oracle": "the field's text property reads 'played'"}}})
                check(reply["result"]["isError"] is False,
                      "starting a playtest failed: %s" % refusal_text(reply))
                check(reply["result"]["structuredContent"]["playtest"] == "e2e-typing",
                      "the playtest slug is wrong: %r" % reply["result"]["structuredContent"])
                print("PASS Godot_StartPlaytest opened a playtest against the running game")

                # A second one must be refused: two overlapping windows would each claim
                # the same activity and neither report would be true.
                reply = call({"jsonrpc": "2.0", "id": 701, "method": "tools/call",
                              "params": {"name": "Godot_StartPlaytest",
                                         "arguments": {"goal": "something else"}}})
                check(refused(reply), "a second overlapping playtest was allowed")
                print("PASS a second playtest is refused while one is open")

                # Play it. This is the same input tool used above; the playtest is only
                # watching.
                reply = call({"jsonrpc": "2.0", "id": 702, "method": "tools/call",
                              "params": {"name": "Godot_SendPointerInput",
                                         "arguments": {"x": 250, "y": 220}}})
                check(reply["result"]["isError"] is False,
                      "clicking during the playtest failed: %s" % refusal_text(reply))
                reply = call({"jsonrpc": "2.0", "id": 703, "method": "tools/call",
                              "params": {"name": "Godot_SendKeyInput",
                                         "arguments": {"action": "type", "text": "played"}}})
                check(reply["result"]["isError"] is False,
                      "typing during the playtest failed: %s" % refusal_text(reply))

                reply = call({"jsonrpc": "2.0", "id": 704, "method": "tools/call",
                              "params": {"name": "Godot_NotePlaytestObservation",
                                         "arguments": {"note": "the field took the characters",
                                                       "kind": "progress"}}})
                check(reply["result"]["isError"] is False,
                      "recording an observation failed: %s" % refusal_text(reply))

                reply = call({"jsonrpc": "2.0", "id": 705, "method": "tools/call",
                              "params": {"name": "Godot_FinishPlaytest",
                                         "arguments": {"verdict": "reached",
                                                       "summary": "typed the word and the field took it"}}})
                check(reply["result"]["isError"] is False,
                      "finishing the playtest failed: %s" % refusal_text(reply))
                report = reply["result"]["structuredContent"]["report"]
                check(report["goal"] == "type a word into the field",
                      "the report lost its goal: %r" % report)
                # The claim is only kept if the evidence supports it. Input was injected,
                # so 'reached' survives - unless the game logged something, in which case
                # the report says indeterminate and says why, which is also correct.
                check(report["verdict"] in ("reached", "indeterminate"),
                      "unexpected verdict: %r" % report.get("verdict"))
                if report["verdict"] == "indeterminate":
                    check(bool(report.get("verdict_reason")),
                          "an indeterminate verdict must say why: %r" % report)
                check(report["counts"]["inputs"] >= 2,
                      "the report did not see the input this playtest injected: %r"
                      % report["counts"])
                check(report["counts"]["observations"] == 1,
                      "the observation is missing: %r" % report["counts"])
                check(report["claimed_verdict"] == "reached",
                      "the report must keep what was claimed beside what it concluded: %r" % report)

                # The frame times came from the game, over this window. Until they were
                # wired in, every report said "no spikes" because nothing was measuring -
                # which reads exactly like "nothing spiked", and is the reason the report
                # now says which of the two it means.
                coverage = report["frame_coverage"]
                check(coverage["measured"] is True,
                      "the playtest did not measure the game's frame times: %r" % coverage)
                check(coverage["samples"] >= 3,
                      "too few frame samples to conclude anything: %r" % coverage)
                check("note" not in coverage,
                      "a measured window should not carry the not-measured note: %r" % coverage)
                # Whether this run spiked is the game's business; that the list is a list
                # and the count agrees with it is this report's.
                check(report["counts"]["spikes"] == len(report["spikes"]),
                      "the spike count disagrees with the spike list: %r" % report["counts"])
                print("PASS Godot_FinishPlaytest assembled a report from what actually happened, "
                      "over %d measured frames" % coverage["samples"])

                # A claimed success with no input at all is the check that catches a
                # report written from the source rather than from the game.
                reply = call({"jsonrpc": "2.0", "id": 706, "method": "tools/call",
                              "params": {"name": "Godot_StartPlaytest",
                                         "arguments": {"goal": "claim without playing",
                                                       "budget_seconds": 30}}})
                check(reply["result"]["isError"] is False,
                      "starting the second playtest failed: %s" % refusal_text(reply))
                reply = call({"jsonrpc": "2.0", "id": 707, "method": "tools/call",
                              "params": {"name": "Godot_FinishPlaytest",
                                         "arguments": {"verdict": "reached",
                                                       "summary": "it worked, honestly"}}})
                check(reply["result"]["isError"] is False,
                      "finishing the second playtest failed: %s" % refusal_text(reply))
                unsupported = reply["result"]["structuredContent"]["report"]
                check(unsupported["verdict"] == "indeterminate",
                      "a success claimed with no input was taken at face value: %r" % unsupported)
                check("no input" in unsupported.get("verdict_reason", ""),
                      "the reason should name the missing input: %r" % unsupported.get("verdict_reason"))
                print("PASS a success claimed without playing is reported as indeterminate")

                reply = call({"jsonrpc": "2.0", "id": 708, "method": "tools/call",
                              "params": {"name": "Godot_GetPlaytestReport",
                                         "arguments": {"playtest": "e2e-typing"}}})
                check(reply["result"]["isError"] is False,
                      "reading the report back failed: %s" % refusal_text(reply))
                check(reply["result"]["structuredContent"]["report"]["goal"]
                      == "type a word into the field",
                      "the report on disk is not the one that was written")
                print("PASS Godot_GetPlaytestReport reads a finished report back")

                # --- keeping a value that was tuned while playing ---------------
                #
                # The last act of the loop. Everything above can change a value in the
                # running game and read it back; until this, carrying it into the project
                # meant reading a number off the screen and typing it in.
                reply = call({"jsonrpc": "2.0", "id": 720, "method": "tools/call",
                              "params": {"name": "Godot_SetRuntimeProperty",
                                         "arguments": {"path": "/root/Main/Player",
                                                       "property": "position",
                                                       "value": [321, 123]}}})
                check(reply["result"]["isError"] is False,
                      "setting the runtime value failed: %s" % refusal_text(reply))

                reply = call({"jsonrpc": "2.0", "id": 721, "method": "tools/call",
                              "params": {"name": "Godot_PromoteRuntimeValue",
                                         "arguments": {"path": "/root/Main/Player",
                                                       "property": "position"}}})
                check(reply["result"]["isError"] is False,
                      "promoting the tuned value failed: %s" % refusal_text(reply))
                promoted = reply["result"]["structuredContent"]
                check(promoted["promoted"] is True,
                      "the promotion reported no change: %r" % promoted)
                check(promoted["scene_path"] == "Player",
                      "the runtime path did not translate to the scene path: %r" % promoted)
                # A Vector2 survives the trip. Through JSON alone it would have arrived as
                # an array and stopped being a Vector2, which is why the value is read
                # from Godot's own text form.
                check("321" in promoted["text"] and "123" in promoted["text"],
                      "the promoted value is not the tuned one: %r" % promoted)
                print("PASS Godot_PromoteRuntimeValue carried a tuned value into the scene")

                # The proof is in the edited scene, read through the tool that knows
                # nothing about promotion.
                reply = call({"jsonrpc": "2.0", "id": 722, "method": "tools/call",
                              "params": {"name": "Godot_GetEditedSceneTree", "arguments": {}}})
                check(reply["result"]["isError"] is False,
                      "reading the edited scene failed: %s" % refusal_text(reply))

                # Promoting again changes nothing, and says so rather than making a second
                # undo step for a write with no effect.
                reply = call({"jsonrpc": "2.0", "id": 723, "method": "tools/call",
                              "params": {"name": "Godot_PromoteRuntimeValue",
                                         "arguments": {"path": "/root/Main/Player",
                                                       "property": "position"}}})
                check(reply["result"]["isError"] is False,
                      "the second promotion failed: %s" % refusal_text(reply))
                check(reply["result"]["structuredContent"]["promoted"] is False,
                      "promoting an unchanged value reported a change: %r"
                      % reply["result"]["structuredContent"])
                print("PASS promoting a value the scene already holds changes nothing")

                # And the undo path: the promotion went through EditorUndoRedoManager, so
                # a person can take it back.
                reply = call({"jsonrpc": "2.0", "id": 724, "method": "tools/call",
                              "params": {"name": "Godot_UndoLastAction", "arguments": {}}})
                check(reply["result"]["isError"] is False,
                      "undoing the promotion failed: %s" % refusal_text(reply))
                print("PASS a promotion is undoable like any other scene edit")

                # A node the editor does not have open is refused rather than written to
                # the wrong scene.
                reply = call({"jsonrpc": "2.0", "id": 725, "method": "tools/call",
                              "params": {"name": "Godot_PromoteRuntimeValue",
                                         "arguments": {"path": "/root/SomeOtherScene/Player",
                                                       "property": "position"}}})
                check(refused(reply), "promoting out of a scene the editor has not open was allowed")
                print("PASS promoting out of a scene the editor does not have open is refused")

                # --- the live tuning workspace ----------------------------------
                #
                # Promotion keeps one value. This is the gesture around it: try several
                # against the same running game, flip between them, and keep the one that
                # felt right. Run against the whole loop rather than the store, because
                # the interesting failures are all at the join with the game.
                def tune(identifier, arguments):
                    return call({"jsonrpc": "2.0", "id": identifier, "method": "tools/call",
                                 "params": {"name": "Godot_OfferVariants",
                                            "arguments": arguments}})

                reply = tune(730, {"action": "offer", "path": "/root/Main/Player",
                                   "property": "position",
                                   "values": [{"name": "left", "value": [64, 123]},
                                              {"name": "right", "value": [512, 123]}]})
                check(not refused(reply), "offering variants failed: %s" % refusal_text(reply))
                offered = reply["result"]["structuredContent"]
                check(offered["tuning"] is True, "the set did not open: %r" % offered)
                # Three candidates: the two offered, plus the value the game already had.
                # Comparing against that is the comparison a designer forgets to make.
                names = [entry["name"] for entry in offered["candidates"]]
                check(names == ["original", "left", "right"],
                      "the set does not hold the original alongside the candidates: %r" % names)
                check(offered["current"] == "original",
                      "offering changed the running game before anything was chosen: %r" % offered)

                # The original must be what the game is actually holding, read through the
                # tool that knows nothing about tuning sets. Not a fixed number: whatever
                # the game happened to be at is the thing discard has to restore.
                reply = call({"jsonrpc": "2.0", "id": 729, "method": "tools/call",
                              "params": {"name": "Godot_GetRuntimeProperty",
                                         "arguments": {"path": "/root/Main/Player",
                                                       "property": "position"}}})
                check(not refused(reply), "reading the live value failed: %s" % refusal_text(reply))
                check(offered["original"] == reply["result"]["structuredContent"]["text"],
                      "the set's original is not what the game holds: %r vs %r"
                      % (offered["original"], reply["result"]["structuredContent"]["text"]))
                print("PASS Godot_OfferVariants captured the original and opened a set")

                # A candidate that was never live cannot be kept: that is editing the scene
                # by a longer route, and calling it a comparison would be a claim about
                # something that did not happen.
                reply = tune(731, {"action": "keep", "name": "left"})
                check(refused(reply), "a value that was never applied was allowed to be kept")
                check("never applied" in refusal_text(reply),
                      "the refusal does not say why: %r" % refusal_text(reply))
                print("PASS a value nobody played cannot be kept")

                reply = tune(732, {"action": "switch", "name": "left"})
                check(not refused(reply), "switching failed: %s" % refusal_text(reply))
                switched = reply["result"]["structuredContent"]
                check(switched["current"] == "left", "the switch did not take: %r" % switched)
                check("64" in switched["text"], "the game did not take the value: %r" % switched)

                # Read it back through the tool that knows nothing about tuning sets.
                reply = call({"jsonrpc": "2.0", "id": 733, "method": "tools/call",
                              "params": {"name": "Godot_GetRuntimeProperty",
                                         "arguments": {"path": "/root/Main/Player",
                                                       "property": "position"}}})
                check(not refused(reply), "reading the tuned value back failed: %s" % refusal_text(reply))
                live = reply["result"]["structuredContent"]
                check("64" in live["text"],
                      "the running game is not holding the switched value: %r" % live)
                print("PASS switching put the candidate into the running game")

                reply = tune(734, {"action": "note", "note": "lands short of the platform"})
                check(not refused(reply), "noting failed: %s" % refusal_text(reply))
                reply = tune(735, {"action": "switch", "name": "right"})
                check(not refused(reply), "switching to the second candidate failed: %s"
                      % refusal_text(reply))
                reply = tune(736, {"action": "note", "note": "clears it with room to spare"})
                check(not refused(reply), "noting the second candidate failed: %s" % refusal_text(reply))
                noted = reply["result"]["structuredContent"]
                by_name = {entry["name"]: entry for entry in noted["candidates"]}
                check(by_name["left"]["note"] == "lands short of the platform",
                      "the first note did not stay with its candidate: %r" % by_name["left"])
                check(by_name["right"]["note"] == "clears it with room to spare",
                      "the second note landed on the wrong candidate: %r" % by_name["right"])
                # Flipped through in well under a second, so the reply must not present this
                # as a comparison anybody made.
                check("choice rather than a comparison" in noted["comparison"],
                      "a set flipped through in a moment claimed to be a comparison: %r"
                      % noted["comparison"])
                print("PASS notes stay with their candidate, and a hurried set says so")

                reply = tune(737, {"action": "keep", "name": "right"})
                check(not refused(reply), "keeping failed: %s" % refusal_text(reply))
                kept = reply["result"]["structuredContent"]
                check(kept["kept"] == "right", "the wrong candidate was kept: %r" % kept)
                check(kept["tuning"] is False, "keeping did not close the set: %r" % kept)
                # Keeping writes nothing to the project: this tool is read_runtime, and a
                # tool holding more authority than it declares is the thing the capability
                # model exists to prevent.
                check("Godot_PromoteRuntimeValue" in kept.get("next", ""),
                      "keeping does not point at the tool that makes it stick: %r" % kept)
                print("PASS keeping names the winner and hands off to promotion")

                # And the handoff works: the game is still holding the kept value, so
                # promoting now writes that one.
                reply = call({"jsonrpc": "2.0", "id": 738, "method": "tools/call",
                              "params": {"name": "Godot_PromoteRuntimeValue",
                                         "arguments": {"path": "/root/Main/Player",
                                                       "property": "position"}}})
                check(not refused(reply), "promoting the kept value failed: %s" % refusal_text(reply))
                promoted_variant = reply["result"]["structuredContent"]
                check("512" in promoted_variant["text"],
                      "the promoted value is not the one that was kept: %r" % promoted_variant)
                print("PASS the value kept from a tuning set is the one promotion writes")
                call({"jsonrpc": "2.0", "id": 739, "method": "tools/call",
                      "params": {"name": "Godot_UndoLastAction", "arguments": {}}})

                # Discarding puts the original back in the running game. Otherwise
                # "discard" leaves the last thing tried in place and looks like it worked.
                reply = tune(740, {"action": "offer", "path": "/root/Main/Player",
                                   "property": "position", "values": [[10, 20], [30, 40]]})
                check(not refused(reply), "opening a second set failed: %s" % refusal_text(reply))
                second_set = reply["result"]["structuredContent"]
                before_discard = second_set["original"]
                # Unnamed candidates are named after their own printed value, and the reply
                # is where a caller learns what that came out as - so read it rather than
                # guess at the formatting.
                unnamed = [entry["name"] for entry in second_set["candidates"]
                           if entry["name"] != "original"]
                check(len(unnamed) == 2, "the second set did not take both values: %r" % second_set)
                reply = tune(741, {"action": "switch", "name": unnamed[0]})
                check(not refused(reply), "switching in the second set failed: %s" % refusal_text(reply))
                reply = tune(742, {"action": "discard"})
                check(not refused(reply), "discarding failed: %s" % refusal_text(reply))
                discarded = reply["result"]["structuredContent"]
                check(discarded["restored"] is True,
                      "discarding did not put the original back: %r" % discarded)

                reply = call({"jsonrpc": "2.0", "id": 743, "method": "tools/call",
                              "params": {"name": "Godot_GetRuntimeProperty",
                                         "arguments": {"path": "/root/Main/Player",
                                                       "property": "position"}}})
                check(reply["result"]["structuredContent"]["text"] == before_discard,
                      "the running game did not go back to the value it started with: %r vs %r"
                      % (reply["result"]["structuredContent"]["text"], before_discard))
                print("PASS discarding a set puts the original back in the running game")

                # One value is not a choice, and the refusal says which tool is.
                reply = tune(744, {"action": "offer", "path": "/root/Main/Player",
                                   "property": "position", "values": [[1, 2]]})
                check(refused(reply), "a set with one candidate was accepted")
                check("Godot_SetRuntimeProperty" in refusal_text(reply),
                      "the refusal does not point at the tool for setting one value: %r"
                      % refusal_text(reply))
                print("PASS a tuning set with one value is refused, and says what to use instead")

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
                check(shot["inlined"] is True,
                      "the default capture did not inline the image: %r" % shot)
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

                # --- input by action, not by key ---------------------------------
                #
                # The game reads an action; a test bound to the key passes until somebody
                # rebinds it. The fixture counts `is_action_pressed`, so this proves the
                # input took the same route the player's key takes rather than proving
                # that some event arrived.
                before_pressed = surface("action_pressed")
                reply = call({"jsonrpc": "2.0", "id": 110, "method": "tools/call",
                              "params": {"name": "Godot_SendActionInput",
                                         "arguments": {"action": "e2e_fire"}}})
                check(not refused(reply),
                      "sending an action failed: %s" % refusal_text(reply))
                check(reply["result"]["structuredContent"]["events"] == 2,
                      "a tap should be a press and a release: %r"
                      % reply["result"]["structuredContent"])
                check(surface("action_pressed") == before_pressed + 1,
                      "the game did not see the action as pressed")
                check(surface("action_released") >= 1,
                      "the game did not see the action released")
                check(surface("action_is_held") is False,
                      "a tapped action was left held down")

                # Held is a different test from tapped, and the reply says so rather than
                # leaving a run to walk into a wall for the rest of the session.
                reply = call({"jsonrpc": "2.0", "id": 111, "method": "tools/call",
                              "params": {"name": "Godot_SendActionInput",
                                         "arguments": {"action": "e2e_fire",
                                                       "press": "press", "strength": 0.5}}})
                check(not refused(reply), "holding an action failed: %s" % refusal_text(reply))
                check("held down" in reply["result"]["structuredContent"].get("note", ""),
                      "holding an action does not say it is still held: %r"
                      % reply["result"]["structuredContent"])
                check(surface("action_is_held") is True,
                      "the game does not report the action as held")
                check(abs(surface("action_strength") - 0.5) < 0.01,
                      "the strength did not reach the game: %r" % surface("action_strength"))

                reply = call({"jsonrpc": "2.0", "id": 112, "method": "tools/call",
                              "params": {"name": "Godot_SendActionInput",
                                         "arguments": {"action": "e2e_fire",
                                                       "press": "release"}}})
                check(not refused(reply), "releasing an action failed: %s" % refusal_text(reply))
                check(surface("action_is_held") is False, "the action stayed held after release")

                # An action nobody defined is refused, and the refusal names what the
                # project does define. Input the game never saw must not be reported as
                # input it received.
                reply = call({"jsonrpc": "2.0", "id": 113, "method": "tools/call",
                              "params": {"name": "Godot_SendActionInput",
                                         "arguments": {"action": "no_such_action"}}})
                check(refused(reply), "an action the project does not define was accepted")
                check("e2e_fire" in refusal_text(reply),
                      "the refusal does not name the actions that do exist: %r"
                      % refusal_text(reply))
                print("PASS Godot_SendActionInput drives the action path, held and tapped")

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

                # --- record, assert, replay --------------------------------------
                # The whole point of a session: play the game, keep what happened, and
                # replay it after a change to see what came out different. This runs the
                # loop end to end against the live game rather than trusting the store
                # and the scheduler, which are unit-tested separately.
                reply = call({"jsonrpc": "2.0", "id": 240, "method": "tools/call",
                              "params": {"name": "Godot_RecordSession",
                                         "arguments": {"action": "start",
                                                       "name": "E2E Press The Target"}}})
                check(not refused(reply), "starting a recording failed: %s" % refusal_text(reply))
                started = reply["result"]["structuredContent"]
                check(started["recording"] is True and started["session"] == "e2e-press-the-target",
                      "the recording did not open as named: %r" % started)
                check("does not observe a person playing" in started.get("input_source_note", ""),
                      "the reply does not say what the trace actually sees: %r" % started)

                # A second start must be refused: there is one game, so one recording.
                reply = call({"jsonrpc": "2.0", "id": 241, "method": "tools/call",
                              "params": {"name": "Godot_RecordSession",
                                         "arguments": {"action": "start", "name": "another"}}})
                check(refused(reply), "a second concurrent recording was accepted")

                # Something for the trace to hold, on a control the fixture counts.
                call({"jsonrpc": "2.0", "id": 242, "method": "tools/call",
                      "params": {"name": "Godot_SendPointerInput",
                                 "arguments": {"x": 200, "y": 130, "action": "click"}}})
                reply = call({"jsonrpc": "2.0", "id": 243, "method": "tools/call",
                              "params": {"name": "Godot_AssertRuntimeState",
                                         "arguments": {"node_path": "/root/Main/Hud/Target",
                                                       "property": "press_count"}}})
                check(not refused(reply), "capturing an assertion failed: %s" % refusal_text(reply))
                captured = reply["result"]["structuredContent"]
                check(captured.get("assertion_count") == 1,
                      "the assertion was not stored: %r" % captured)
                recorded_presses = captured["value"]

                reply = call({"jsonrpc": "2.0", "id": 244, "method": "tools/call",
                              "params": {"name": "Godot_RecordSession",
                                         "arguments": {"action": "stop"}}})
                check(not refused(reply), "stopping the recording failed: %s" % refusal_text(reply))
                stopped = reply["result"]["structuredContent"]
                check(stopped["recording"] is False, "the recording did not close: %r" % stopped)
                check(stopped["event_count"] >= 1,
                      "the trace captured no input: %r" % stopped)
                check(stopped["assertion_count"] == 1,
                      "the assertion did not survive into the session: %r" % stopped)
                print("PASS Godot_RecordSession captured %d events and an assertion"
                      % stopped["event_count"])

                reply = call({"jsonrpc": "2.0", "id": 245, "method": "tools/call",
                              "params": {"name": "Godot_ListSessions", "arguments": {}}})
                check(not refused(reply), "listing sessions failed: %s" % refusal_text(reply))
                listed = reply["result"]["structuredContent"]["sessions"]
                slugs = [entry["slug"] for entry in listed]
                check("e2e-press-the-target" in slugs,
                      "the recorded session is not listed: %r" % slugs)
                print("PASS Godot_ListSessions reports the recording")

                # An assertion recorded against a counter that only ever goes up will not
                # match on replay, so this must come back `failed` with both values named -
                # which is exactly the regression report the feature exists to produce.
                reply = call({"jsonrpc": "2.0", "id": 246, "method": "tools/call",
                              "params": {"name": "Godot_ReplaySession",
                                         "arguments": {"name": "E2E Press The Target",
                                                       "timeout_seconds": 60}}})
                check(not refused(reply), "replaying failed: %s" % refusal_text(reply))
                replayed = reply["result"]["structuredContent"]
                check(replayed["verdict"] in ("passed", "failed", "indeterminate"),
                      "the replay produced no usable verdict: %r" % replayed)
                check(replayed["events_injected"] >= 1,
                      "the replay injected nothing: %r" % replayed)
                if replayed["verdict"] == "failed":
                    divergence = replayed["first_divergence"]
                    check(divergence["property"] == "press_count",
                          "the divergence names the wrong property: %r" % divergence)
                    check(divergence["expected"] == recorded_presses,
                          "the divergence does not carry the recorded value: %r" % divergence)
                print("PASS Godot_ReplaySession re-ran the session and returned '%s'"
                      % replayed["verdict"])

                # Replaying something that was never recorded is a clear refusal, not a
                # pass over an empty trace.
                reply = call({"jsonrpc": "2.0", "id": 247, "method": "tools/call",
                              "params": {"name": "Godot_ReplaySession",
                                         "arguments": {"name": "never recorded at all"}}})
                check(refused(reply), "replaying an unknown session was accepted")
                print("PASS Godot_ReplaySession refuses a session that was never recorded")

                # --- capturing a bug that has already happened -------------------
                # The other direction: nothing was armed, the input has already been
                # sent, and the capture reaches backwards for it. Everything injected
                # earlier in this run is still in the buffer, so this must come back
                # with events and with the game's own frames on them.
                reply = call({"jsonrpc": "2.0", "id": 248, "method": "tools/call",
                              "params": {"name": "Godot_CaptureBugSession",
                                         "arguments": {"name": "E2E Retroactive Capture",
                                                       "reason": "the target stopped responding",
                                                       "last_events": 5}}})
                check(not refused(reply), "capturing a bug failed: %s" % refusal_text(reply))
                captured_bug = reply["result"]["structuredContent"]
                check(captured_bug["source"] == "runtime_trace",
                      "a live game should have been asked for its own trace: %r" % captured_bug)
                check(captured_bug["event_count"] >= 1,
                      "the capture found no input to write: %r" % captured_bug)
                check(captured_bug["event_count"] <= 5,
                      "last_events did not bound the window: %r" % captured_bug)
                # Nothing here crashed, so every event was acknowledged and no frame
                # should have needed extrapolating.
                check(captured_bug["frames_estimated"] == 0,
                      "a capture from a healthy game estimated frames: %r" % captured_bug)
                check("the game processed it on" in captured_bug.get("fidelity", ""),
                      "the capture does not say what it can prove: %r" % captured_bug)
                print("PASS Godot_CaptureBugSession wrote %d past event(s) with no arming"
                      % captured_bug["event_count"])

                # And what it wrote is an ordinary session, which is the whole claim:
                # a bug report and a regression test are the same file.
                reply = call({"jsonrpc": "2.0", "id": 249, "method": "tools/call",
                              "params": {"name": "Godot_ReplaySession",
                                         "arguments": {"name": "E2E Retroactive Capture",
                                                       "timeout_seconds": 60}}})
                check(not refused(reply),
                      "a captured session did not replay: %s" % refusal_text(reply))
                replayed_capture = reply["result"]["structuredContent"]
                check(replayed_capture["events_injected"] >= 1,
                      "replaying the capture injected nothing: %r" % replayed_capture)
                print("PASS a retroactive capture replays like any other session")

                # S6, against a live game rather than only in a unit test: a run whose
                # events arrive later than the caller is willing to accept must come back
                # indeterminate, never passed. Zero tolerance is the caller saying no
                # lateness at all is acceptable.
                #
                # What is asserted here is the *consistency* of the verdict with the
                # drift that was actually measured, not that drift happened. Demanding
                # drift made this check depend on the machine being slow enough to
                # produce some, and it duly passed for weeks and then failed on a run
                # that was merely fast enough (0 frames, verdict "passed"). The rule
                # itself - drift past tolerance is never a pass - is pinned
                # deterministically with injected drift in test_mcp_replay.h; what only
                # a live run can show is that real measured drift reaches the verdict.
                reply = call({"jsonrpc": "2.0", "id": 251, "method": "tools/call",
                              "params": {"name": "Godot_ReplaySession",
                                         "arguments": {"name": "E2E Retroactive Capture",
                                                       "drift_tolerance_frames": 0,
                                                       "timeout_seconds": 60}}})
                check(not refused(reply), "the zero-tolerance replay failed: %s"
                      % refusal_text(reply))
                strict = reply["result"]["structuredContent"]
                check(strict["drift_tolerance_frames"] == 0,
                      "the replay did not honour a zero drift tolerance: %r" % strict)
                # This session carries no assertions, so nothing can outrank drift.
                # Replaying the *recorded* session here came back "failed" instead,
                # because a divergence outranks drift and that is correct.
                if strict["max_drift_frames"] > 0:
                    check(strict["verdict"] == "indeterminate",
                          "%d frames of drift past a zero tolerance was still called %r: %r"
                          % (strict["max_drift_frames"], strict["verdict"], strict))
                    check("must not be counted as a pass" in strict.get("note", ""),
                          "an indeterminate verdict does not say what it means: %r" % strict)
                    print("PASS a live replay that drifts past its tolerance is "
                          "indeterminate, not a pass (%d frames of drift)"
                          % strict["max_drift_frames"])
                else:
                    check(strict["verdict"] == "passed",
                          "a replay with no measured drift was not a pass: %r" % strict)
                    print("PASS a live replay with no measured drift is a pass "
                          "(the indeterminate path is pinned by unit test instead)")

                # S5: the speed multiplier, and the disclaimer it carries.
                reply = call({"jsonrpc": "2.0", "id": 252, "method": "tools/call",
                              "params": {"name": "Godot_ReplaySession",
                                         "arguments": {"name": "E2E Retroactive Capture",
                                                       "speed": 4,
                                                       "timeout_seconds": 60}}})
                check(not refused(reply), "the sped-up replay failed: %s" % refusal_text(reply))
                fast = reply["result"]["structuredContent"]
                check(abs(fast["speed"] - 4.0) < 0.01,
                      "the replay did not report the speed it ran at: %r" % fast)
                check("nobody performed" in fast.get("speed_note", ""),
                      "a sped-up replay does not say what it fails to prove: %r" % fast)
                check(fast["events_injected"] >= 1,
                      "the sped-up replay injected nothing: %r" % fast)
                print("PASS Godot_ReplaySession replays faster and says what that costs")


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

                # The call site is one frame. The route that reached it is what tells
                # you which of a helper's callers passed the bad value, and the stack
                # has unwound by the time anyone asks - so it has to be captured at the
                # moment the error is raised.
                call({"jsonrpc": "2.0", "id": 199, "method": "tools/call",
                      "params": {"name": "Godot_SetRuntimeProperty",
                                 "arguments": {"path": "/root/Main/Hud/Surface",
                                               "property": "trigger_deep_error", "value": True}}})
                reply = call({"jsonrpc": "2.0", "id": 200, "method": "tools/call",
                              "params": {"name": "Godot_GetRuntimeErrors"}})
                deep = [e for e in reply["result"]["structuredContent"]["errors"]
                        if "E2E_DEEP_ERROR" in e.get("message", "")]
                check(deep, "the deliberate deep error was not captured")
                stack = deep[0].get("stack", [])
                functions = [frame["function"] for frame in stack]
                check("_level_three" in functions and "_level_two" in functions
                      and "_level_one" in functions,
                      "the stack does not show the route to the error, only its call "
                      "site: %r" % functions)
                check(functions.index("_level_three") < functions.index("_level_one"),
                      "the stack is not innermost-first: %r" % functions)
                for frame in stack:
                    check(frame["source"].endswith(".gd") and frame["line"] > 0,
                          "a stack frame names no script and line: %r" % frame)
                print("PASS Godot_GetRuntimeErrors carries the script call stack (%d frames)"
                      % len(stack))

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

                # The coordinate spaces have to compose. Godot_GetRuntimeNodeInfo used to
                # answer in *viewport* coordinates while Godot_SendPointerInput takes
                # *window* pixels. At the design size those agree, so everything worked
                # and nothing revealed the difference; at any other window size the click
                # was reported delivered, appeared in the input trace, and landed on empty
                # space. A false "verified at 1080p" is worse than a refusal, so this
                # resizes the window first and then aims purely by what the tool reports.
                reply = call({"jsonrpc": "2.0", "id": 230, "method": "tools/call",
                              "params": {"name": "Godot_SetGameWindowSize",
                                         "arguments": {"width": 960, "height": 540}}})
                check(not refused(reply), "resizing failed: %s" % refusal_text(reply))
                time.sleep(1.0)

                before_press = surface_press_count()
                reply = call({"jsonrpc": "2.0", "id": 231, "method": "tools/call",
                              "params": {"name": "Godot_GetRuntimeNodeInfo",
                                         "arguments": {"path": "/root/Main/Hud/Target"}}})
                info = reply["result"]["structuredContent"]
                check(info["rect"]["space"] == "window_pixels",
                      "the rect does not say which space it is in: %r" % info["rect"])
                check("viewport_rect" in info,
                      "the viewport figures were dropped rather than renamed: %r" % info)
                aimed = call({"jsonrpc": "2.0", "id": 232, "method": "tools/call",
                              "params": {"name": "Godot_SendPointerInput",
                                         "arguments": {"action": "click",
                                                       "x": int(info["rect"]["center_x"]),
                                                       "y": int(info["rect"]["center_y"])}}})
                check(not refused(aimed), "the aimed click was refused: %s" % refusal_text(aimed))
                time.sleep(0.6)
                check(surface_press_count() == before_press + 1,
                      "a click aimed at the rectangle Godot_GetRuntimeNodeInfo reported did "
                      "not reach the button at 960x540, so the two tools do not compose")
                print("PASS node rects are window pixels, so aiming works at any window size")

                call({"jsonrpc": "2.0", "id": 233, "method": "tools/call",
                      "params": {"name": "Godot_SetGameWindowSize",
                                 "arguments": {"width": 1152, "height": 648}}})
                time.sleep(1.0)

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

                # --- the full profiler: start, drive, stop, read the export -------
                # Godot_ProfileWindow answers "how slow"; the capture answers "why".
                # The window is explicit because the point is to drive the game while
                # it records; the bulk lands in a file because a capture is megabytes.
                reply = call({"jsonrpc": "2.0", "id": 940, "method": "tools/call",
                              "params": {"name": "Godot_GetProfilerStatus",
                                         "arguments": {}}})
                check(reply["result"]["structuredContent"]["state"] == "idle",
                      "the profiler is not idle before the capture: %r"
                      % reply["result"]["structuredContent"])

                reply = call({"jsonrpc": "2.0", "id": 941, "method": "tools/call",
                              "params": {"name": "Godot_StopProfiler", "arguments": {}}})
                check(refused(reply), "stopping with nothing recording did not refuse")
                check("Godot_StartProfiler" in refusal_text(reply),
                      "the refusal does not say which tool starts a capture: %s"
                      % refusal_text(reply))

                reply = call({"jsonrpc": "2.0", "id": 942, "method": "tools/call",
                              "params": {"name": "Godot_StartProfiler",
                                         "arguments": {"max_seconds": 60,
                                                       "label": "e2e"}}})
                check(not refused(reply), "starting the profiler failed: %s"
                      % refusal_text(reply))
                started = reply["result"]["structuredContent"]
                check(started["export_path"].startswith("user://godot_ai_profiles/"),
                      "the export is not in the shared user directory: %r" % started)
                check("jq" in started["reading_guide"],
                      "the start reply carries no reading guide: %r"
                      % sorted(started.keys()))

                reply = call({"jsonrpc": "2.0", "id": 943, "method": "tools/call",
                              "params": {"name": "Godot_StartProfiler",
                                         "arguments": {}}})
                check(refused(reply), "a second start did not refuse while recording")

                # The window has to contain something worth profiling: real frames,
                # at least one monitor sample (they arrive once a second), and a
                # script that actually runs.
                time.sleep(2.5)

                reply = call({"jsonrpc": "2.0", "id": 944, "method": "tools/call",
                              "params": {"name": "Godot_GetProfilerStatus",
                                         "arguments": {}}})
                mid = reply["result"]["structuredContent"]
                check(mid["state"] == "recording", "not recording mid-window: %r" % mid)
                check(mid["frames"] > 10,
                      "the capture is not receiving profiler frames: %r" % mid)

                reply = call({"jsonrpc": "2.0", "id": 945, "method": "tools/call",
                              "params": {"name": "Godot_StopProfiler", "arguments": {}}})
                check(not refused(reply), "stopping the profiler failed: %s"
                      % refusal_text(reply))
                capture = reply["result"]["structuredContent"]
                check(capture["end_reason"] == "stopped",
                      "an explicit stop reported end_reason %r" % capture["end_reason"])
                check(capture["partial"] is False,
                      "a stop with the game alive was partial: %r" % capture)
                check("jq" in capture["reading_guide"],
                      "the stop reply carries no reading guide")
                summary = capture["summary"]
                check(summary["window"]["frames"] > 10,
                      "the summary window is empty: %r" % summary["window"])
                check(summary["top_functions"]["source"] == "accumulated_total",
                      "the function totals did not come from the whole-window "
                      "accumulation: %r" % summary["top_functions"])
                check(summary["frame_ms"]["worst_ms"] >= summary["frame_ms"]["mean_ms"],
                      "the worst frame is below the mean: %r" % summary["frame_ms"])

                # The export itself: every line parses, the header opens it, the
                # summary closes it, and the streams the summary claims are there
                # actually are.
                export_records = []
                with open(capture["export_absolute_path"], "r", encoding="utf-8") as handle:
                    for line in handle:
                        line = line.strip()
                        if line:
                            export_records.append(json.loads(line))
                types = [record["type"] for record in export_records]
                check(types[0] == "header", "the export does not open with a header")
                check(types[-1] == "summary", "the export does not close with a summary")
                for expected in ("frame", "mon", "total", "vram", "mem"):
                    check(expected in types,
                          "the export has no '%s' record; streams present: %r"
                          % (expected, sorted(set(types))))
                frame_record = next(r for r in export_records if r["type"] == "frame")
                check(frame_record["frame_ms"] > 0,
                      "a frame record has no frame time: %r" % frame_record)
                # GPU records exist exactly when the summary says they do, so a
                # headless run stays honest rather than silently thin.
                if summary["gpu"].get("frames", 0) > 0:
                    check("gpu" in types, "the summary counts GPU frames the file lacks")
                else:
                    check("note" in summary["gpu"],
                          "no GPU data and no explanation why: %r" % summary["gpu"])
                print("PASS the profiler capture exported %d records (%d bytes) and "
                      "summarized them" % (len(export_records), capture["export_bytes"]))

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
                # A Dictionary has to come back as a *structure*. Returned as text, a
                # caller iterating it silently walks its characters and nothing anywhere
                # reports an error.
                reply = call({"jsonrpc": "2.0", "id": 210, "method": "tools/call",
                              "params": {"name": "Godot_GetRuntimeProperty",
                                         "arguments": {"path": "/root/Main/Saves",
                                                       "property": "slot"}}})
                slot = reply["result"]["structuredContent"]
                check(slot["type"] == "Dictionary", "the type is not reported: %r" % slot)
                check(isinstance(slot["value"], dict),
                      "a Dictionary came back as %s, which iterates as characters: %r"
                      % (type(slot["value"]).__name__, slot["value"]))
                check(slot["value"]["level"] == 1, "the dictionary lost its contents: %r" % slot)

                reply = call({"jsonrpc": "2.0", "id": 211, "method": "tools/call",
                              "params": {"name": "Godot_GetRuntimeProperty",
                                         "arguments": {"path": "/root/Main/Hud/Surface",
                                                       "property": "test_array_probe"}}})
                # (no such property - the refusal path stays a refusal)
                check(refused(reply), "an unknown property was accepted")
                print("PASS Godot_GetRuntimeProperty returns a Dictionary as a real object")

                # A stream generated in script has no resource path. It must still be
                # visible, or a game whose audio is entirely generated looks silent.
                call({"jsonrpc": "2.0", "id": 212, "method": "tools/call",
                      "params": {"name": "Godot_SetRuntimeProperty",
                                 "arguments": {"path": "/root/Main/Chimes",
                                               "property": "play_generated", "value": 1}}})
                reply = call({"jsonrpc": "2.0", "id": 213, "method": "tools/call",
                              "params": {"name": "Godot_GetAudioState"}})
                generated = reply["result"]["structuredContent"]
                made = [entry for entry in generated["players"]
                        if entry["node_path"].endswith("/Made")]
                check(made, "the generated-stream player was not listed")
                check(made[0]["playing"] is True, "the generated stream is not playing: %r" % made[0])
                check(made[0]["stream"].startswith("generated:"),
                      "an unsaved stream has no identity, so it would be skipped: %r" % made[0])
                check(made[0]["stream_saved"] is False,
                      "a generated stream is reported as saved: %r" % made[0])
                check(generated["playing_count"] >= 1,
                      "playing_count does not see the generated sound: %r"
                      % generated["playing_count"])
                call({"jsonrpc": "2.0", "id": 214, "method": "tools/call",
                      "params": {"name": "Godot_SetRuntimeProperty",
                                 "arguments": {"path": "/root/Main/Chimes",
                                               "property": "play_generated", "value": 0}}})
                print("PASS a stream generated in script is visible to the audio tools")

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

                # --- stacking across a burst -------------------------------------
                # Everything above was a snapshot of a sound left playing. The real bug
                # is a sound triggered twice by one button press: it doubles and is over
                # before the next call arrives, so no snapshot can ever see it.
                call({"jsonrpc": "2.0", "id": 184, "method": "tools/call",
                      "params": {"name": "Godot_SetRuntimeProperty",
                                 "arguments": {"path": "/root/Main/Chimes",
                                               "property": "play_count", "value": 0}}})
                reply = call({"jsonrpc": "2.0", "id": 185, "method": "tools/call",
                              "params": {"name": "Godot_GetAudioState"}})
                check(reply["result"]["structuredContent"]["stacked"] == [],
                      "the sounds did not stop before the burst test")

                # Sampling starts first and the burst happens inside it. The two replies
                # come back in whichever order they finish, so they are matched by id.
                relay.send_message({"jsonrpc": "2.0", "id": 186, "method": "tools/call",
                                    "params": {"name": "Godot_DetectAudioStacking",
                                               "arguments": {"frames": 60}}})
                relay.send_message({"jsonrpc": "2.0", "id": 187, "method": "tools/call",
                                    "params": {"name": "Godot_SetRuntimeProperty",
                                               "arguments": {"path": "/root/Main/Chimes",
                                                             "property": "flash", "value": 1}}})
                window = None
                deadline = time.time() + 60
                while window is None and time.time() < deadline:
                    message = relay.read_message(timeout=30)
                    if message is None:
                        break
                    if message.get("id") == 186:
                        window = message
                check(window is not None, "the audio window never answered")
                check(not refused(window), "sampling audio failed: %s" % refusal_text(window))
                sampled = window["result"]["structuredContent"]
                check(sampled["frames_sampled"] == 60,
                      "the window did not sample 60 frames: %r" % sampled)
                check(len(sampled["stacked"]) == 1,
                      "the burst doubled a sound and the window missed it: %r" % sampled)
                burst = sampled["stacked"][0]
                check(burst["stream"] == "res://audio/chime.wav" and burst["peak_count"] == 2,
                      "the window named the wrong sound or count: %r" % burst)
                check(burst["at_frame"] > 0, "the window does not say when it happened: %r" % burst)

                # And the proof that this catches what a snapshot cannot: the burst is
                # transient, so a snapshot taken once it is over finds nothing at all.
                # Waited for rather than assumed - an unthrottled game can run 60 frames
                # in less time than the burst lasts, and that is a fact about this
                # machine, not about the tool.
                deadline = time.time() + 15
                while time.time() < deadline:
                    reply = call({"jsonrpc": "2.0", "id": 188, "method": "tools/call",
                                  "params": {"name": "Godot_GetAudioState"}})
                    if reply["result"]["structuredContent"]["stacked"] == []:
                        break
                    time.sleep(0.5)
                check(reply["result"]["structuredContent"]["stacked"] == [],
                      "the burst never ended, so it proves nothing about windows: %r"
                      % reply["result"]["structuredContent"]["stacked"])

                # Pointed at an idle game it must report nothing, or it would call
                # everything stacked and the check above would be meaningless.
                reply = call({"jsonrpc": "2.0", "id": 189, "method": "tools/call",
                              "params": {"name": "Godot_DetectAudioStacking",
                                         "arguments": {"frames": 10}}})
                idle = reply["result"]["structuredContent"]
                check(idle["stacked"] == [] and idle["max_simultaneous"] == 0,
                      "an idle game was reported as stacking: %r" % idle)
                check("not the same as nothing ever stacking" in idle["note"],
                      "the empty result does not say what it does not prove: %r" % idle["note"])
                print("PASS Godot_DetectAudioStacking caught a burst no snapshot could see")

                # --- G3: a deliberately corrupted save ---------------------------
                # The user-data tools are the mechanism for writing a malformed save.
                # That only becomes a *verified* capability against a game that has a
                # save system and a recovery path, so the fixture has one.
                def saves(field):
                    probe = call({"jsonrpc": "2.0", "id": 201, "method": "tools/call",
                                  "params": {"name": "Godot_GetRuntimeProperty",
                                             "arguments": {"path": "/root/Main/Saves",
                                                           "property": field}}})
                    check(not refused(probe),
                          "reading Saves.%s failed: %s" % (field, refusal_text(probe)))
                    return probe["result"]["structuredContent"]["value"]

                def set_saves(field, value, identifier):
                    reply = call({"jsonrpc": "2.0", "id": identifier, "method": "tools/call",
                                  "params": {"name": "Godot_SetRuntimeProperty",
                                             "arguments": {"path": "/root/Main/Saves",
                                                           "property": field, "value": value}}})
                    check(not refused(reply),
                          "setting Saves.%s failed: %s" % (field, refusal_text(reply)))

                # A good save first: the editor's tools have to be looking at the same
                # user:// the game writes to, or nothing below means anything.
                set_saves("save_now", 1, 202)
                reply = call({"jsonrpc": "2.0", "id": 203, "method": "tools/call",
                              "params": {"name": "Godot_ListUserFiles"}})
                names = [entry["path"] for entry
                         in reply["result"]["structuredContent"]["files"]]
                check("user://save.json" in names,
                      "the game's save is not visible to the user-data tools: %r" % names)

                # A save the tool wrote must load as a save, or "recovered" below would
                # only prove that the game cannot read anything at all.
                reply = call({"jsonrpc": "2.0", "id": 204, "method": "tools/call",
                              "params": {"name": "Godot_WriteUserFile",
                                         "arguments": {"path": "user://save.json",
                                                       "content": '{"level": 7, "score": 42}'}}})
                check(not refused(reply), "writing a save failed: %s" % refusal_text(reply))
                set_saves("load_now", 1, 205)
                check(saves("load_result") == "loaded",
                      "a valid save written by the tool did not load: %r" % saves("load_result"))
                check(saves("level") == 7,
                      "the loaded save has the wrong contents: %r" % saves("level"))

                # Now the corruption fixture: truncated JSON, which is what a save
                # interrupted by a crash or a full disk actually looks like.
                reply = call({"jsonrpc": "2.0", "id": 206, "method": "tools/call",
                              "params": {"name": "Godot_WriteUserFile",
                                         "arguments": {"path": "user://save.json",
                                                       "content": '{"level": 7, "sc'}}})
                check(not refused(reply),
                      "writing a malformed save failed: %s" % refusal_text(reply))
                set_saves("load_now", 2, 207)
                check(saves("load_result") == "recovered",
                      "the game did not report recovering from a corrupt save: %r"
                      % saves("load_result"))
                check(saves("level") == 1,
                      "the recovery did not fall back to a fresh slot: %r" % saves("level"))

                # And it is still running. A recovery path that takes the game down with
                # it is not a recovery path, and every check above would still pass if
                # the game had died immediately afterwards.
                reply = call({"jsonrpc": "2.0", "id": 208, "method": "tools/call",
                              "params": {"name": "Godot_GetPerformanceMetrics"}})
                check(not refused(reply),
                      "the game did not survive loading a corrupt save: %s"
                      % refusal_text(reply))
                print("PASS a save corrupted through Godot_WriteUserFile is survived and reported")

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
                # The flag used to be declared and ignored, so every capture came back
                # with megabytes of base64 whether or not the caller wanted it.
                check(fast["inlined"] is False,
                      "inline_image: false was ignored and the image came back anyway")
                check(not [c for c in reply["result"]["content"] if c["type"] == "image"],
                      "the response carries an image block despite inline_image: false")
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

            # The case the editor-side mirror exists for. The game is gone and its own
            # trace went with it, so a capture taken now has to come from the mirror -
            # and has to say so, because a mirror knows what was sent and not what the
            # game did with it.
            if has_display:
                reply = call({"jsonrpc": "2.0", "id": 250, "method": "tools/call",
                              "params": {"name": "Godot_CaptureBugSession",
                                         "arguments": {"name": "E2E Capture After Exit",
                                                       "reason": "the game died"}}})
                check(not refused(reply),
                      "capturing after the game exited was refused: %s" % refusal_text(reply))
                after_exit = reply["result"]["structuredContent"]
                check(after_exit["source"] == "editor_mirror",
                      "a capture with no game did not fall back to the mirror: %r" % after_exit)
                check(after_exit["event_count"] >= 1,
                      "the mirror kept nothing from a run that injected input: %r" % after_exit)
                check("what was sent" in after_exit.get("fidelity", ""),
                      "the capture claims more than a mirror can know: %r" % after_exit)
                check("press play" in after_exit.get("fidelity", ""),
                      "the capture does not say what replaying it needs: %r" % after_exit)
                print("PASS Godot_CaptureBugSession still has the sequence after the game exits")

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

        # --- errors are answerable with no game running -------------------------
        # "Restart the game, then assert it logged nothing" needs this. Refusing made the
        # whole pattern inexpressible: you had to read the *previous* instance's buffer.
        reply = call({"jsonrpc": "2.0", "id": 224, "method": "tools/call",
                      "params": {"name": "Godot_GetRuntimeErrors"}})
        check(not refused(reply),
              "reading errors with no game running was refused: %s" % refusal_text(reply))
        quiet = reply["result"]["structuredContent"]
        check(quiet["count"] == 0 and quiet["errors"] == [],
              "no game is running, yet errors were reported: %r" % quiet)
        check("no game is running" in quiet["note"],
              "the empty answer does not say why it is empty: %r" % quiet.get("note"))
        print("PASS Godot_GetRuntimeErrors answers rather than refusing with no game")

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

        # --- a resource property set by path ----------------------------------
        # A res:// string written into an Object-typed property used to be stored as a
        # bare String: the scene then ran with no script, no error, and nothing in the
        # output log. A black screen and a clean console is the worst failure this
        # interface can produce, because there is nothing to search for.
        call({"jsonrpc": "2.0", "id": 220, "method": "tools/call",
              "params": {"name": "Godot_OpenScene",
                         "arguments": {"path": "res://scenes/main.tscn"}}})
        reply = call({"jsonrpc": "2.0", "id": 221, "method": "tools/call",
                      "params": {"name": "Godot_SetSceneProperty",
                                 "arguments": {"path": "Hud/Target", "property": "script",
                                               "value": "res://scripts/target.gd"}}})
        check(not refused(reply), "setting a script by path failed: %s" % refusal_text(reply))
        written = reply["result"]["structuredContent"]["value_written"]
        check("Resource(" in written and "target.gd" in written,
              "the script was stored as %r rather than as a loaded resource" % written)

        reply = call({"jsonrpc": "2.0", "id": 222, "method": "tools/call",
                      "params": {"name": "Godot_SetSceneProperty",
                                 "arguments": {"path": "Hud/Target", "property": "script",
                                               "value": "just a string"}}})
        check(refused(reply), "a bare string was accepted into a resource property")
        check("res://" in refusal_text(reply),
              "the refusal does not say what was wanted: %r" % refusal_text(reply))

        reply = call({"jsonrpc": "2.0", "id": 223, "method": "tools/call",
                      "params": {"name": "Godot_SetSceneProperty",
                                 "arguments": {"path": "Hud/Target", "property": "script",
                                               "value": "res://nothing-here.gd"}}})
        check(refused(reply), "a res:// path to nothing was accepted")
        print("PASS a resource property takes a res:// path, and refuses anything else")

        # --- a theme override, on the first write ------------------------------
        # A theme override does not exist as a property until something sets it, so it
        # is absent from the property list *and* reads back as a default. Coercing
        # against either wrote an array into a Color, the engine refused it, and the
        # read-back returned the black that was already there - a successful-looking
        # write of entirely the wrong value, and a whole UI of black text on a dark
        # panel. Writing it twice used to be the workaround, so this asserts the first.
        reply = call({"jsonrpc": "2.0", "id": 225, "method": "tools/call",
                      "params": {"name": "Godot_SetSceneProperty",
                                 "arguments": {"path": "Hud/Target",
                                               "property": "theme_override_colors/font_color",
                                               "value": [1.0, 0.8, 0.2, 1.0]}}})
        check(not refused(reply), "setting a theme override failed: %s" % refusal_text(reply))
        colour = reply["result"]["structuredContent"]["value_written"]
        check("0.8" in colour and "Color(" in colour,
              "the first write of a theme override stored %r instead of the colour given"
              % colour)
        print("PASS a theme override takes its colour on the first write, not the second")

        # --- parse errors are visible ------------------------------------------
        reply = call({"jsonrpc": "2.0", "id": 226, "method": "tools/call",
                      "params": {"name": "Godot_CheckScript",
                                 "arguments": {"path": "res://scripts/broken.gd"}}})
        check(not refused(reply), "checking a script failed: %s" % refusal_text(reply))
        checked = reply["result"]["structuredContent"]
        check(checked["valid"] is False, "a script that does not parse was called valid")
        check(checked["errors"] and checked["errors"][0]["line"] == 3,
              "the parse error does not name the line: %r" % checked["errors"])
        check(checked["errors"][0]["message"],
              "the parse error has no message: %r" % checked["errors"][0])

        reply = call({"jsonrpc": "2.0", "id": 227, "method": "tools/call",
                      "params": {"name": "Godot_CheckScript",
                                 "arguments": {"path": "res://scripts/target.gd"}}})
        check(not refused(reply),
              "checking a valid script failed: %s" % refusal_text(reply))
        good = reply["result"]["structuredContent"]
        check(good["valid"] is True and good["errors"] == [],
              "a script that parses was reported broken: %r" % good)
        print("PASS Godot_CheckScript finds the line a script fails to parse on")

        # --- a class_name is usable immediately -------------------------------
        # The editor's filesystem scan is asynchronous, so a script written and then used
        # in the same breath failed with "Identifier not declared" - and writing the same
        # bytes a second time appeared to fix it, which is not a discoverable workaround.
        # No sleep here on purpose: the point is that no wait is needed.
        reply = call({"jsonrpc": "2.0", "id": 240, "method": "tools/call",
                      "params": {"name": "Godot_WriteTextFile",
                                 "arguments": {"path": "res://fresh/registered.gd",
                                               "text": "class_name FreshlyRegistered\nextends Node\n",
                                               "create_directories": True}}})
        check(not refused(reply), "writing into a new folder failed: %s" % refusal_text(reply))
        reply = call({"jsonrpc": "2.0", "id": 241, "method": "tools/call",
                      "params": {"name": "Godot_CheckScript",
                                 "arguments": {"path": "res://fresh/registered.gd"}}})
        registered = reply["result"]["structuredContent"]
        check(registered["class_name"] == "FreshlyRegistered",
              "the class name was not read: %r" % registered)
        check(registered["class_registered"] is True,
              "a class written into a brand-new folder is not registered yet, so anything "
              "referring to it fails with 'Identifier not declared'")

        reply = call({"jsonrpc": "2.0", "id": 242, "method": "tools/call",
                      "params": {"name": "Godot_WriteTextFile",
                                 "arguments": {"path": "res://fresh/user.gd",
                                               "text": "extends Node\n\nfunc _ready() -> void:\n\tprint(FreshlyRegistered)\n",
                                               "create_directories": True}}})
        reply = call({"jsonrpc": "2.0", "id": 243, "method": "tools/call",
                      "params": {"name": "Godot_CheckScript",
                                 "arguments": {"path": "res://fresh/user.gd"}}})
        consumer = reply["result"]["structuredContent"]
        check(consumer["valid"] is True and consumer["errors"] == [],
              "a script referring to a class written moments earlier does not parse: %r"
              % consumer["errors"])
        print("PASS a class_name is usable in the very next call, with no wait")

        # --- creating a scene --------------------------------------------------
        # Godot_ManageNode edits the open scene; it cannot bring one into existence. That
        # left the instructions contradicting themselves: never hand-write a .tscn, and
        # no structured way to make the first one.
        reply = call({"jsonrpc": "2.0", "id": 244, "method": "tools/call",
                      "params": {"name": "Godot_CreateScene",
                                 "arguments": {"path": "res://levels/arena.tscn",
                                               "root_type": "Node2D", "root_name": "Arena"}}})
        check(not refused(reply), "creating a scene failed: %s" % refusal_text(reply))
        made = reply["result"]["structuredContent"]
        check(made["opened"] is True, "the new scene was not opened: %r" % made)

        # It has to be a real scene the structured tools can build on.
        reply = call({"jsonrpc": "2.0", "id": 245, "method": "tools/call",
                      "params": {"name": "Godot_ManageNode",
                                 "arguments": {"action": "create", "parent": ".",
                                               "type": "Label", "name": "Title"}}})
        check(not refused(reply), "building on the new scene failed: %s" % refusal_text(reply))
        call({"jsonrpc": "2.0", "id": 246, "method": "tools/call",
              "params": {"name": "Godot_SaveScene"}})
        with open(os.path.join(project, "levels", "arena.tscn")) as handle:
            written_scene = handle.read()
        check("[gd_scene" in written_scene and 'type="Label"' in written_scene,
              "the saved scene is not a real .tscn: %r" % written_scene[:120])
        check("uid://" in written_scene,
              "the scene has no uid, which a hand-written stub would also lack")

        for identifier, arguments, why in (
                (247, {"path": "res://levels/arena.tscn", "root_type": "Node2D"},
                 "an existing scene was overwritten without being asked"),
                (248, {"path": "res://levels/no.tscn", "root_type": "NotAClass"},
                 "a class that does not exist was accepted"),
                (249, {"path": "res://levels/no.tscn", "root_type": "Resource"},
                 "a non-Node was accepted as a scene root"),
                (250, {"path": "res://levels/no.txt", "root_type": "Node2D"},
                 "a path that is not a .tscn was accepted")):
            reply = call({"jsonrpc": "2.0", "id": identifier, "method": "tools/call",
                          "params": {"name": "Godot_CreateScene", "arguments": arguments}})
            check(refused(reply), why)
        print("PASS Godot_CreateScene makes a scene the structured tools can build on")

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
        if has_display:
            check(main_windows[0]["width"] > 0 and main_windows[0]["height"] > 0,
                  "the main window has no size: %r" % main_windows[0])
        else:
            check(main_windows[0]["width"] == 0 and main_windows[0]["height"] == 0,
                  "the headless main window reported a physical size: %r" % main_windows[0])
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
        #
        # Only where there is a display. Unattended, the tool answers at once instead of
        # opening a dialog nobody could ever see, so there would be no pending question
        # to look for - and its immediate error would arrive as the next reply on the
        # wire and be read as the answer to some other call.
        if has_display:
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

        if has_display:
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
        else:
            print("SKIP listing an open dialog: unattended, no dialog is ever opened")

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

        # --- undoing a task, not twelve checkpoints ----------------------------
        #
        # Checkpoints are per-tool-call, which is the right granularity to take them at
        # and the wrong one to be the only way back: a twelve-call session that went
        # wrong had to be unwound twelve times by hand. Each snapshot carries the goal
        # that was current when it ran, so a task undoes as one thing.
        reply = call({"jsonrpc": "2.0", "id": 830, "method": "tools/call",
                      "params": {"name": "Godot_SetIntent",
                                 "arguments": {"goal": "e2e task undo",
                                               "activity": "writing three files"}}})
        check(not refused(reply), "declaring a goal failed: %s" % refusal_text(reply))

        for index, name in enumerate(("one", "two", "three")):
            reply = call({"jsonrpc": "2.0", "id": 831 + index, "method": "tools/call",
                          "params": {"name": "Godot_WriteTextFile",
                                     "arguments": {"path": "res://task_%s.txt" % name,
                                                   "text": "written by the task"}}})
            check(not refused(reply), "the task's write failed: %s" % refusal_text(reply))

        # A file from a *different* task must survive: grouping by goal is the whole
        # reason this is not "undo everything recent".
        reply = call({"jsonrpc": "2.0", "id": 834, "method": "tools/call",
                      "params": {"name": "Godot_SetIntent",
                                 "arguments": {"goal": "e2e unrelated work",
                                               "activity": "writing one more file"}}})
        check(not refused(reply), "changing goal failed: %s" % refusal_text(reply))
        reply = call({"jsonrpc": "2.0", "id": 835, "method": "tools/call",
                      "params": {"name": "Godot_WriteTextFile",
                                 "arguments": {"path": "res://task_other.txt",
                                               "text": "not part of the task"}}})
        check(not refused(reply), "the unrelated write failed: %s" % refusal_text(reply))

        for name in ("one", "two", "three", "other"):
            check(os.path.exists(os.path.join(project, "task_%s.txt" % name)),
                  "task_%s.txt was never written" % name)

        reply = call({"jsonrpc": "2.0", "id": 836, "method": "tools/call",
                      "params": {"name": "Godot_ListCheckpoints"}})
        listed = reply["result"]["structuredContent"]
        task_names = [t["task"] for t in listed["tasks"]]
        check("e2e task undo" in task_names,
              "the task is not listed among the checkpoints' tasks: %r" % task_names)
        undone_task = [t for t in listed["tasks"] if t["task"] == "e2e task undo"][0]
        check(undone_task["checkpoints"] == 3,
              "the task does not hold three checkpoints: %r" % undone_task)

        # Naming both is refused: they undo different amounts, and the difference is
        # the point.
        reply = call({"jsonrpc": "2.0", "id": 837, "method": "tools/call",
                      "params": {"name": "Godot_RestoreCheckpoint",
                                 "arguments": {"id": listed["checkpoints"][0]["id"],
                                               "task": "e2e task undo"}}})
        check(refused(reply), "naming both an id and a task was accepted")

        reply = call({"jsonrpc": "2.0", "id": 838, "method": "tools/call",
                      "params": {"name": "Godot_RestoreCheckpoint",
                                 "arguments": {"task": "e2e task undo"}}})
        check(not refused(reply), "undoing the task failed: %s" % refusal_text(reply))
        undone = reply["result"]["structuredContent"]
        check(undone["checkpoints_restored"] == 3,
              "the task undo rolled back %r checkpoints, not 3: %r"
              % (undone["checkpoints_restored"], undone))
        check(undone["files_removed"] == 3,
              "the task's three created files were not removed: %r" % undone)

        for name in ("one", "two", "three"):
            check(not os.path.exists(os.path.join(project, "task_%s.txt" % name)),
                  "task_%s.txt survived the task undo" % name)
        check(os.path.exists(os.path.join(project, "task_other.txt")),
              "undoing one task removed another task's file")
        print("PASS a whole task undoes in one call, and leaves other tasks alone")

        # --- what the user is looking at ---------------------------------------
        #
        # The worst friction in the journey: a node is selected in the scene tree and
        # the request still has to describe it in prose, because the editor knew and
        # the agent did not. Carried on Godot_GetEditorStatus rather than a new tool so
        # a caller already asking what the editor is doing gets it without knowing to
        # ask twice.
        reply = call({"jsonrpc": "2.0", "id": 820, "method": "tools/call",
                      "params": {"name": "Godot_GetEditorStatus"}})
        check(not refused(reply), "reading editor status failed: %s" % refusal_text(reply))
        status = reply["result"]["structuredContent"]
        for field in ("main_screen", "selection", "open_script"):
            check(field in status, "editor status has no %r: %r" % (field, sorted(status)))
        check(isinstance(status["selection"], list),
              "selection is not a list: %r" % (status["selection"],))
        check(isinstance(status["main_screen"], str) and status["main_screen"],
              "no main screen was reported: %r" % (status["main_screen"],))

        # Whatever the editor happens to have selected, each entry has to be usable as
        # an argument to the other tools - a path relative to the edited scene root,
        # the same form everything else here takes.
        for entry in status["selection"]:
            for field in ("name", "path", "class"):
                check(field in entry, "a selection entry has no %r: %r" % (field, entry))
            check(not entry["path"].startswith("/root"),
                  "a selection path is absolute rather than scene-relative: %r" % entry)
            # The name has to be findable in the edited scene, or the path is a label
            # rather than an address and the next tool call will miss.
            tree = call({"jsonrpc": "2.0", "id": 821, "method": "tools/call",
                         "params": {"name": "Godot_GetEditedSceneTree"}})
            check(entry["name"] in json.dumps(tree["result"]["structuredContent"]),
                  "a selected node is not in the edited scene tree: %r" % entry)
        print("PASS the editor reports what the user is looking at (%d selected, screen %r)"
              % (len(status["selection"]), status["main_screen"]))

        # --- what the project remembers between sessions -----------------------
        #
        # `.agent/` is memory for the tooling project; the game being edited had none,
        # so every session relearned it. The checks that matter are the ones that keep
        # a memory store from becoming a liability: recall must not return everything,
        # and a name must not be able to become a path.
        reply = call({"jsonrpc": "2.0", "id": 800, "method": "tools/call",
                      "params": {"name": "Godot_RecallProjectMemory", "arguments": {}}})
        check(not refused(reply), "recalling an empty memory errored: %s" % refusal_text(reply))
        empty = reply["result"]["structuredContent"]
        check(empty["count"] == 0, "a fresh project already remembered something: %r" % empty)
        check("nothing has been recorded" in empty["note"].lower(),
              "an empty store does not say so: %r" % empty)

        reply = call({"jsonrpc": "2.0", "id": 801, "method": "tools/call",
                      "params": {"name": "Godot_UpdateProjectMemory",
                                 "arguments": {"name": "Patrol Routes!",
                                               "subject": "How enemies patrol",
                                               "body": "PatrolRoute owns the waypoints. "
                                                       "Enemies never own their own path."}}})
        check(not refused(reply), "remembering failed: %s" % refusal_text(reply))
        remembered = reply["result"]["structuredContent"]
        check(remembered["name"] == "patrol-routes",
              "the name was not normalised: %r" % remembered)
        check(remembered["path"] == "res://.godot_ai/memory/patrol-routes.md",
              "the note landed somewhere unexpected: %r" % remembered)

        # It is a file in the project, which is what makes it reviewable and
        # correctable by a person rather than an opaque store.
        note_file = os.path.join(project, ".godot_ai", "memory", "patrol-routes.md")
        check(os.path.exists(note_file), "the note was not written to the project")
        with open(note_file) as handle:
            note_text = handle.read()
        check(note_text.startswith("---\n"), "the note is not readable markdown: %r" % note_text[:40])
        check("subject: How enemies patrol" in note_text,
              "the note lost its subject: %r" % note_text[:200])

        reply = call({"jsonrpc": "2.0", "id": 802, "method": "tools/call",
                      "params": {"name": "Godot_RecallProjectMemory", "arguments": {}}})
        index = reply["result"]["structuredContent"]
        check(index["count"] == 1, "the note is not in the index: %r" % index)
        entry = index["notes"][0]
        # The index summarises. Returning every body on every recall is how a memory
        # store turns into context poisoning.
        check("body" not in entry, "the index returned a whole body: %r" % entry)
        check(entry["summary"].startswith("PatrolRoute owns"),
              "the index has no useful summary: %r" % entry)

        reply = call({"jsonrpc": "2.0", "id": 803, "method": "tools/call",
                      "params": {"name": "Godot_RecallProjectMemory",
                                 "arguments": {"name": "patrol-routes"}}})
        full = reply["result"]["structuredContent"]["notes"][0]
        check("Enemies never own their own path." in full["body"],
              "reading a note by name did not return it in full: %r" % full)

        # A name is reduced to [a-z0-9-] before it is ever a path, so traversal is
        # unrepresentable rather than merely rejected. This writes to the store, not
        # above it.
        reply = call({"jsonrpc": "2.0", "id": 804, "method": "tools/call",
                      "params": {"name": "Godot_UpdateProjectMemory",
                                 "arguments": {"name": "../../escaped",
                                               "subject": "Confinement",
                                               "body": "this must land inside the store"}}})
        check(not refused(reply), "a slugged name was rejected: %s" % refusal_text(reply))
        check(reply["result"]["structuredContent"]["path"]
              == "res://.godot_ai/memory/escaped.md",
              "a traversing name escaped the store: %r" % reply["result"]["structuredContent"])
        check(not os.path.exists(os.path.join(os.path.dirname(project), "escaped.md")),
              "a note was written outside the project")

        reply = call({"jsonrpc": "2.0", "id": 805, "method": "tools/call",
                      "params": {"name": "Godot_UpdateProjectMemory",
                                 "arguments": {"action": "forget", "name": "escaped"}}})
        check(not refused(reply), "forgetting failed: %s" % refusal_text(reply))
        check(reply["result"]["structuredContent"]["count"] == 1,
              "forgetting removed the wrong number of notes: %r"
              % reply["result"]["structuredContent"])
        reply = call({"jsonrpc": "2.0", "id": 806, "method": "tools/call",
                      "params": {"name": "Godot_UpdateProjectMemory",
                                 "arguments": {"action": "forget", "name": "escaped"}}})
        check(refused(reply), "forgetting a note twice succeeded the second time")
        print("PASS project memory writes readable notes, indexes without dumping, "
              "and cannot be named out of its folder")

        # --- exploring the editor's own class reference ------------------------
        #
        # 800-odd documented classes were sitting in the tree unread while the model
        # worked from a remembered API - on a 4.8-dev fork, and with no way at all to
        # know the project's own classes.
        reply = call({"jsonrpc": "2.0", "id": 810, "method": "tools/call",
                      "params": {"name": "Godot_LookupClass",
                                 "arguments": {"class_name": "CharacterBody2D"}}})
        check(not refused(reply), "looking up a core class failed: %s" % refusal_text(reply))
        looked_up = reply["result"]["structuredContent"]
        check(looked_up["api_type"] == "core", "CharacterBody2D is not core: %r" % looked_up)
        check("PhysicsBody2D" in looked_up["inheritance_chain"],
              "the inheritance chain is wrong: %r" % looked_up["inheritance_chain"])
        check("Node" in looked_up["inheritance_chain"],
              "the chain does not reach Node: %r" % looked_up["inheritance_chain"])
        # A big class must be clipped rather than dumped, and must say that it was.
        check(looked_up["truncated"] is False or "note" in looked_up,
              "a clipped answer did not say so: %r" % looked_up)

        reply = call({"jsonrpc": "2.0", "id": 811, "method": "tools/call",
                      "params": {"name": "Godot_LookupClass",
                                 "arguments": {"class_name": "CharacterBody2D",
                                               "member": "move_and_slide"}}})
        member = reply["result"]["structuredContent"]["members"][0]
        check(member["kind"] == "method", "move_and_slide is not a method: %r" % member)
        check(member["signature"].startswith("move_and_slide()"),
              "the signature does not read like a call: %r" % member)
        check("[" not in member["description"],
              "BBCode markup reached the caller: %r" % member["description"][:120])

        # The project's own class, which is the half no model can have seen.
        reply = call({"jsonrpc": "2.0", "id": 812, "method": "tools/call",
                      "params": {"name": "Godot_LookupClass",
                                 "arguments": {"class_name": "PatrolRoute"}}})
        check(not refused(reply),
              "the project's own class is not in the reference: %s" % refusal_text(reply))
        own = reply["result"]["structuredContent"]
        check(own["api_type"] == "project script",
              "a script class is not marked as one: %r" % own)
        check(own["script_path"] == "res://scripts/patrol_route.gd",
              "the script class does not point at its file: %r" % own)
        kinds = {m["name"]: m["kind"] for m in own["members"]}
        check(kinds.get("dwell_seconds") == "property",
              "an exported variable is missing from the reference: %r" % kinds)
        check(kinds.get("loop_completed") == "signal",
              "a declared signal is missing from the reference: %r" % kinds)
        check(kinds.get("next_waypoint") == "method",
              "a documented method is missing from the reference: %r" % kinds)

        reply = call({"jsonrpc": "2.0", "id": 813, "method": "tools/call",
                      "params": {"name": "Godot_LookupClass",
                                 "arguments": {"search": "PatrolRou"}}})
        found = reply["result"]["structuredContent"]["classes"]
        check(any(c["class_name"] == "PatrolRoute" for c in found),
              "searching did not find the project's class: %r" % found)

        # A near miss is the common case - wrong case, or a name remembered from
        # another engine version - so the refusal has to be more use than "no".
        reply = call({"jsonrpc": "2.0", "id": 814, "method": "tools/call",
                      "params": {"name": "Godot_LookupClass",
                                 "arguments": {"class_name": "characterbody2d"}}})
        check(refused(reply), "a wrongly-cased class name was accepted")
        check("CharacterBody2D" in refusal_text(reply),
              "the refusal does not suggest the right name: %s" % refusal_text(reply))
        print("PASS the class reference answers for core classes, the project's own "
              "scripts, and near misses")

        # --- the stop control, last ------------------------------------------
        # Deliberately the final check that needs a working agent: there is no resume
        # tool, by design, so once this runs nothing else can act. A control the held
        # party could lift would be advisory rather than a control.
        reply = call({"jsonrpc": "2.0", "id": 270, "method": "tools/call",
                      "params": {"name": "Godot_SetAgentControl",
                                 "arguments": {"action": "stop",
                                               "reason": "checking the gate holds"}}})
        check(not refused(reply), "stopping the agent failed: %s" % refusal_text(reply))

        reply = call({"jsonrpc": "2.0", "id": 271, "method": "tools/call",
                      "params": {"name": "Godot_WriteTextFile",
                                 "arguments": {"path": "res://should_not_exist.txt",
                                               "text": "the gate should have refused this"}}})
        check(refused(reply), "a stopped agent was allowed to write a file")
        check("checking the gate holds" in refusal_text(reply),
              "the refusal does not say why it was stopped: %r" % refusal_text(reply))
        check(not os.path.exists(os.path.join(project, "should_not_exist.txt")),
              "the refused write reached the disk anyway")

        # Reads that explain what already happened still work, or the dock loses its own
        # data source at the moment it matters most.
        reply = call({"jsonrpc": "2.0", "id": 272, "method": "tools/call",
                      "params": {"name": "Godot_GetActivity", "arguments": {"limit": 3}}})
        check(not refused(reply), "a stopped agent could not read its own activity")
        control = reply["result"]["structuredContent"]["control"]
        check(control["state"] == "stopped", "the stream does not report the hold: %r" % control)
        check("checking the gate holds" in control["reason"],
              "the stream does not carry the reason: %r" % control)

        # And it cannot let itself go.
        reply = call({"jsonrpc": "2.0", "id": 273, "method": "tools/call",
                      "params": {"name": "Godot_SetAgentControl",
                                 "arguments": {"action": "pause"}}})
        check(refused(reply), "a stopped agent could still change its own control state")
        print("PASS a stopped agent is genuinely stopped, and cannot release itself")

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

    except Failure:
        # The editor's last words, which is the only evidence there is when it died rather
        # than answered. Printed here rather than at the top level so the tail belongs to
        # the editor the failure happened in.
        tail = editor_output.tail()
        if tail:
            print("\n--- the editor's last %d line(s) ---\n%s\n--- end ---"
                  % (len(editor_output.lines), tail), file=sys.stderr)
        raise
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
    if args.headless:
        display = virtual_display.VirtualDisplay("", 0, 0, 0)
    elif sys.platform == "darwin":
        display = NativeMacOSDisplay()
    else:
        display = virtual_display.ensure(width=1280, height=800)
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
