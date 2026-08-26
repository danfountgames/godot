#!/usr/bin/env python3
"""W8: does anything draw over the GodotAI workspace's tiles?

W8 was filed from a screenshot, and a screenshot cannot tell an embedded game apart
from a floating one. This measures instead. It reads the embedder controls from the
editor - where the editor *intends* each game to be - and the real window geometry from
X, and compares them. Only the second says where a native window actually is, and the
whole of W8 is the gap between the two.

Two arrangements are checked, because they fail differently:

  * the agent's own games in the workspace tiles
  * the user's own game, embedded in the ordinary Game workspace, while the GodotAI
    workspace is the selected one. The Game workspace's embedded window has to stop
    drawing when its screen is hidden, or it covers everything the agent is doing.

A game the user started *without* embedding is a top-level window and will float over
the editor on any desktop; that is the window manager's business, not this fork's, and
is reported rather than failed.
"""
import base64
import json
import os
import re
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

# `xwininfo -root -tree` prints the window id, then the size with its parent-relative
# position, then the absolute position, so all three come from different tokens on one
# line.
TREE_LINE = re.compile(r"^\s*(0x[0-9a-fA-F]+|\d+)\s.*?\s(\d+)x(\d+)\+-?\d+\+-?\d+\s+\+(-?\d+)\+(-?\d+)\s*$")


def check(condition, message):
    print(("PASS " if condition else "FAIL ") + message)
    if not condition:
        FAILURES.append(message)


def mapped_windows(display, max_width=1400):
    """Every *viewable* window on the display, as (x, y, width, height).

    The map state has to be asked for per window. `xwininfo -root -tree` walks the whole
    tree and lists unmapped windows exactly like mapped ones, so reading only the tree
    reports a hidden game as still on screen - which is how W8 first came to be filed
    against the wrong thing.
    """
    environment = display.environment()
    listing = subprocess.run(["xwininfo", "-root", "-tree", "-int"],
                             capture_output=True, text=True, env=environment).stdout
    windows = []
    for line in listing.splitlines():
        found = TREE_LINE.match(line)
        if not found:
            continue
        identifier, width, height, x, y = found.groups()
        width, height, x, y = int(width), int(height), int(x), int(y)
        if width < 80 or height < 80 or width > max_width:
            continue
        detail = subprocess.run(["xwininfo", "-id", identifier, "-stats"],
                                capture_output=True, text=True, env=environment).stdout
        if "IsViewable" not in detail:
            continue
        windows.append((x, y, width, height))
    return windows


def inside(window, rect, slack=3):
    wx, wy, ww, wh = window
    return (wx >= rect["x"] - slack and wy >= rect["y"] - slack and
            wx + ww <= rect["x"] + rect["width"] + slack and
            wy + wh <= rect["y"] + rect["height"] + slack)


def run_once(embed_on_play):
    editor_binary = os.path.join(REPO, "bin", "godot.linuxbsd.editor.dev.x86_64")
    display = virtual_display.ensure(width=1600, height=1000)
    if not display.usable:
        print("SKIP: no display available; embedding needs one")
        return 0

    root = tempfile.mkdtemp(prefix="godot-ai-w8-")
    project = os.path.join(root, "project")
    home = os.path.join(root, "home")
    os.makedirs(home)
    e2e.build_project(project)

    # The user's own run, embedded or not. Both matter and they fail differently, so
    # this script runs the whole thing twice rather than assuming one stands for the
    # other.
    print("=== the user's run with embed_on_play=%s ===" % ("true" if embed_on_play else "false"))
    meta_dir = os.path.join(project, ".godot", "editor")
    os.makedirs(meta_dir, exist_ok=True)
    with open(os.path.join(meta_dir, "project_metadata.cfg"), "w") as handle:
        handle.write("[game_view]\n\nembed_on_play=%s\n" % ("true" if embed_on_play else "false"))

    environment = display.environment()
    environment["GODOT_AI_HOME"] = home
    environment["GODOT_AI_AUTO_APPROVE"] = "1"

    editor = subprocess.Popen(
        [editor_binary, "--path", project, "--editor",
         "--resolution", "1560x960", "--position", "0,0"] + display.godot_arguments(),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env=environment, cwd=root,
    )

    relay = None
    try:
        e2e.wait_for_instance(os.path.join(home, "instances"), editor)
        relay = e2e.RelayProcess(
            args=["--client-name", "w8", "--approval-mode", "allow"], home=home)
        next_id = [10]

        def call(name, arguments=None):
            next_id[0] += 1
            relay.send_message({"jsonrpc": "2.0", "id": next_id[0], "method": "tools/call",
                                "params": {"name": name, "arguments": arguments or {}}})
            return relay.read_message(timeout=90).get("result", {})

        def structured(name, arguments=None):
            return call(name, arguments).get("structuredContent", {})

        relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                            "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                                       "clientInfo": {"name": "w8", "version": "1"}}})
        relay.read_message(timeout=90)
        relay.send_message({"jsonrpc": "2.0", "method": "notifications/initialized"})

        # ------------------------------------------------------------------
        # The user presses play first, in the ordinary Game workspace.
        # ------------------------------------------------------------------
        call("Godot_PlayMainScene", {})
        time.sleep(8)
        before = mapped_windows(display)
        print("  after the user's run: %s" % (before,))
        check(len(before) >= 1, "the user's own game is on screen (%d windows)" % len(before))

        # ------------------------------------------------------------------
        # Now the agent launches two of its own, which switches the main screen
        # to the GodotAI workspace.
        # ------------------------------------------------------------------
        for label in ("Variant A", "Variant B"):
            structured("Godot_LaunchInstance", {"label": label, "role": "candidate"})
        time.sleep(8)

        embedders = structured("Godot_FindControl", {"class": "EmbeddedProcess", "limit": 10})
        rects = embedders.get("matches", [])
        for rect in rects:
            print("  embedder at x=%s y=%s %sx%s visible=%s"
                  % (rect["x"], rect["y"], rect["width"], rect["height"], rect.get("visible")))

        windows = mapped_windows(display)
        print("  mapped game windows: %s" % (windows,))

        visible_rects = [r for r in rects if r.get("visible")]
        check(len(visible_rects) == 2,
              "exactly the two workspace embedders are visible (%d)" % len(visible_rects))

        stray = [w for w in windows if not any(inside(w, r) for r in visible_rects)]
        if embed_on_play:
            # The Game workspace is hidden, so its embedded game must have stopped being
            # drawn. This is the claim W8 was really about.
            check(not stray,
                  "the hidden Game workspace's embedded game is not drawn (stray %s)" % (stray,))
        else:
            # A game started without embedding is a top-level window. The editor cannot
            # stack it behind itself - that is the window manager's job, and this display
            # has no window manager - so it is expected here, not a defect.
            check(bool(stray),
                  "a game started without embedding does float over the tiles, as expected")

        # Either way the workspace has to say which of the two it is in, rather than
        # letting a floating window look like broken embedding.
        labels = [m.get("text", "") for m in
                  structured("Godot_FindControl", {"class": "Label", "limit": 40}).get("matches", [])]
        hinted = any("not embedded" in text for text in labels)
        check(hinted != embed_on_play,
              "the workspace %s that the user's run is unembedded (%s)"
              % ("says" if not embed_on_play else "does not claim",
                 [t for t in labels if "run" in t or "running" in t]))

        shot = call("Godot_CaptureEditorWindow", {})
        for item in shot.get("content", []):
            if item.get("type") == "image":
                path = os.path.join(HERE, "spike_workspace_overdraw_%s.png"
                                    % ("embedded" if embed_on_play else "floating"))
                with open(path, "wb") as handle:
                    handle.write(base64.b64decode(item["data"]))
                print("  screenshot: %s" % path)

        call("Godot_StopAllInstances", {})
        call("Godot_StopPlaying", {})

    finally:
        if relay:
            relay.finish(timeout=10)
        editor.terminate()
        try:
            out, _ = editor.communicate(timeout=25)
        except subprocess.TimeoutExpired:
            editor.kill()
            out, _ = editor.communicate()
        text = out.decode("utf-8", "replace") if out else ""
        probes = [line for line in text.splitlines() if "W8PROBE" in line]
        for line in probes[-40:]:
            print("EDITOR: %s" % line)
        subprocess.run(["pkill", "-f", "godot.linuxbsd.editor.dev.*--path"],
                       capture_output=True)

    return 0


def main():
    for embed_on_play in (True, False):
        run_once(embed_on_play)
        print()

    if FAILURES:
        print("workspace overdraw: %d check(s) failed: %s" % (len(FAILURES), "; ".join(FAILURES)))
        return 1
    print("workspace overdraw: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
