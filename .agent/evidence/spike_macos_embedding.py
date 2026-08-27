#!/usr/bin/env python3
"""W7: does macOS embed more than one game at a time?

macOS is the one platform where multi-embedding was never measured. X11 and Windows
embed by reparenting a native window, which needs nothing from the game. macOS cannot
reparent another process's NSWindow, so it shares a CALayer context instead, and that
needs a handshake *back* from the game: the game sends `game_view:set_context_id` over
the debugger channel and only then can the editor attach the layer.

`.agent/MACOS_EMBEDDING_SPIKE.md` predicts, from reading the source on Linux, that N
games' context ids all land on one embedder and the last one wins. This runs it.

Deliberately not ported from the X11 spike: `xwininfo` window enumeration. On macOS an
embedded game has no on-screen window of its own - its layer is composited into the
editor - so counting windows would answer a question this platform does not ask. What
is looked at instead is what the editor itself reports about its embedders, plus a
screenshot, because "three processes are alive" and "three games are visible" are
different claims and on macOS the second is the one in doubt.
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

import run_editor_e2e as e2e  # noqa: E402

FAILURES = []
NOTES = []


def check(condition, message):
    if condition:
        print("PASS %s" % message)
    else:
        print("FAIL %s" % message)
        FAILURES.append(message)


def note(message):
    print("  .. %s" % message)
    NOTES.append(message)


def main():
    if sys.platform != "darwin":
        print("SKIP: this spike is about the macOS CALayer embedding path")
        return 0

    editor_binary = e2e.default_editor()
    if not os.path.exists(editor_binary):
        print("SKIP: no editor at %s" % editor_binary)
        return 0

    workspace_dir = tempfile.mkdtemp(prefix="godot-ai-w7-")
    project = os.path.join(workspace_dir, "project")
    home = os.path.join(workspace_dir, "home")
    os.makedirs(home)
    e2e.build_project(project)

    environment = dict(os.environ)
    environment["GODOT_AI_HOME"] = home
    environment["GODOT_AI_AUTO_APPROVE"] = "1"

    # The editor's own output is the evidence here, not a side effect: the embedding
    # refusal is printed by the engine and never reaches the protocol.
    editor_log_path = os.path.join(HERE, "spike_macos_embedding_editor.log")
    editor_log = open(editor_log_path, "wb")
    editor = subprocess.Popen(
        [editor_binary, "--path", project, "--editor",
         "--resolution", "1560x960", "--position", "0,0"],
        stdout=editor_log, stderr=subprocess.STDOUT,
        env=environment, cwd=workspace_dir,
    )

    try:
        descriptor = e2e.wait_for_instance(os.path.join(home, "instances"), editor)
        relay = e2e.RelayProcess(
            args=["--client-name", "w7", "--approval-mode", "allow"], home=home)

        def call(message):
            relay.send_message(message)
            return relay.read_message(timeout=90)

        call({"jsonrpc": "2.0", "id": 1, "method": "initialize",
              "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                         "clientInfo": {"name": "w7", "version": "1"}}})
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

        # --- Spike A, the part that matters most: does ONE still embed? ------------
        # This is the regression risk from W1/W2, which extracted the embed command
        # line and the callback list on Linux and were never run here.
        first, error = tool(20, "Godot_LaunchInstance",
                            {"label": "Solo", "role": "candidate",
                             "task": "does one embed at all",
                             "retention": "interactive"})
        check(error is None and first is not None,
              "a single instance launched on macOS (%s)" % error)
        if first:
            note("solo instance %s pid %s lifecycle %s rect %s"
                 % (first.get("instance_id"), first.get("pid"),
                    first.get("lifecycle"), first.get("embed_rect")))
            check(first.get("debugger_connected") is True,
                  "the solo instance's debugger session connected, which is what carries "
                  "the macOS context-id handshake (%r)" % first.get("debugger_connected"))
        time.sleep(6)

        # --- Spike B: and does a second? ------------------------------------------
        ids = [first["instance_id"]] if first else []
        for index, label in enumerate(["Second", "Third"]):
            payload, error = tool(22 + index, "Godot_LaunchInstance",
                                  {"label": label, "role": "candidate",
                                   "task": "does more than one embed",
                                   "retention": "interactive"})
            if error:
                check(False, "launching %s: %s" % (label, error))
                continue
            ids.append(payload["instance_id"])
            note("%s -> %s pid %s rect %s"
                 % (label, payload["instance_id"], payload.get("pid"),
                    payload.get("embed_rect")))
            time.sleep(6)

        check(len(ids) == 3, "three instances launched (%d)" % len(ids))

        listed, error = tool(30, "Godot_ListInstances", {"live_only": True})
        if listed:
            check(listed["live_count"] == 3,
                  "three instances are live (%r)" % listed["live_count"])

        # macOS pgrep has no -c: it is BSD pgrep, not procps. Count the lines.
        listing = subprocess.run(["pgrep", "-f", "godot.macos.editor.dev"],
                                 capture_output=True, text=True).stdout.split()
        note("godot processes alive: %d" % len(listing))
        check(len(listing) >= 4,
              "the editor and three game processes are all running (%d)" % len(listing))

        # Where the editor put the tiles. This is the editor's *intent*; on macOS it is
        # not proof that anything drew, because the layer attach is what draws.
        found = call({"jsonrpc": "2.0", "id": 29, "method": "tools/call",
                      "params": {"name": "Godot_FindControl",
                                 "arguments": {"class": "MCPWorkspaceTile", "limit": 10}}})
        tiles = found.get("result", {}).get("structuredContent", {}).get("matches", [])
        for entry in tiles:
            note("tile at x=%s y=%s %sx%s" % (entry["x"], entry["y"],
                                              entry["width"], entry["height"]))
        check(len(tiles) == 3, "the workspace laid out three tiles (%d)" % len(tiles))

        embedders = call({"jsonrpc": "2.0", "id": 28, "method": "tools/call",
                          "params": {"name": "Godot_FindControl",
                                     "arguments": {"class": "EmbeddedProcessMacOS",
                                                   "limit": 10}}})
        embed_rects = embedders.get("result", {}).get("structuredContent", {}).get("matches", [])
        note("EmbeddedProcessMacOS controls found: %d" % len(embed_rects))
        for entry in embed_rects:
            note("embedder at x=%s y=%s %sx%s" % (entry["x"], entry["y"],
                                                  entry["width"], entry["height"]))
        # The claim this spike exists to settle. macOS embeds by sharing a CALayer
        # context, which only `EmbeddedProcessMacOS` knows how to receive; the generic
        # `EmbeddedProcess` embeds by reparenting a native window, which macOS cannot do
        # and whose `DisplayServer::embed_process()` it therefore never implements.
        # A tile holding the generic embedder is a tile that will stay blank.
        check(len(embed_rects) == len(tiles),
              "each tile holds a macOS embedder: %d embedders for %d tiles"
              % (len(embed_rects), len(tiles)))

        # The evidence that settles it. Each tile is captured on its own, so a blank tile
        # beside a drawing one is visible rather than inferred.
        for index, entry in enumerate(tiles):
            payload, error = tool(90 + index, "Godot_CaptureEditorControl",
                                  {"node_path": entry["node_path"],
                                   "path": "res://tile_%d.png" % index,
                                   "highlight": False})
            if error:
                note("capturing tile %d refused: %s" % (index, error))
            else:
                note("tile %d captured to %s (%sx%s)"
                     % (index, payload.get("path"), payload.get("width"),
                        payload.get("height")))

        reply = call({"jsonrpc": "2.0", "id": 99, "method": "tools/call",
                      "params": {"name": "Godot_CaptureEditorWindow", "arguments": {}}})
        for item in reply.get("result", {}).get("content", []):
            if item.get("type") == "image":
                path = os.path.join(HERE, "spike_macos_embedding.png")
                with open(path, "wb") as handle:
                    handle.write(base64.b64decode(item["data"]))
                print("  screenshot: %s" % path)

        for index, entry in enumerate(tiles):
            source = os.path.join(project, "tile_%d.png" % index)
            if os.path.exists(source):
                target = os.path.join(HERE, "spike_macos_tile_%d.png" % index)
                with open(source, "rb") as src, open(target, "wb") as dst:
                    dst.write(src.read())
                print("  tile %d: %s" % (index, target))

        # Independent control, the other half of W4/W6. If the router works here it
        # works regardless of what the layer did.
        if len(ids) >= 3:
            paused, error = tool(40, "Godot_ControlInstance",
                                 {"instance_id": ids[0], "action": "pause"})
            check(error is None and paused and paused.get("applied") is True,
                  "pausing one instance reached exactly that one (%s / %r)"
                  % (error, paused.get("note") if paused else None))
            after, _ = tool(41, "Godot_ListInstances", {"live_only": True})
            if after:
                check(after["live_count"] == 3,
                      "pausing one stopped none of them (%r)" % after["live_count"])

        all_stopped, error = tool(60, "Godot_StopAllInstances", {})
        check(error is None and all_stopped and all_stopped["live_count"] == 0,
              "stopping all agent instances left none running (%s)" % error)
    finally:
        editor.terminate()
        try:
            editor.wait(timeout=20)
        except subprocess.TimeoutExpired:
            editor.kill()
        subprocess.run(["pkill", "-f", "godot.macos.editor.dev.*--path"],
                       capture_output=True)

    embed_complaints = []
    if os.path.exists(editor_log_path):
        with open(editor_log_path, "r", errors="replace") as handle:
            for line in handle:
                if "embed" in line.lower() or "context" in line.lower():
                    embed_complaints.append(line.rstrip())
    if embed_complaints:
        print()
        print("what the editor said about embedding:")
        for line in embed_complaints[:20]:
            print("  %s" % line)
    print("  editor log: %s" % editor_log_path)
    refused = [line for line in embed_complaints
               if "not supported by this display server" in line]
    check(not refused,
          "the display server refused to embed, %d time(s), so no tile can draw a game"
          % len(refused))
    if not refused:
        print("PASS the display server never refused an embed")

    print()
    if FAILURES:
        print("macOS embedding spike: %d checks failed" % len(FAILURES))
        return 1
    print("macOS embedding spike: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
