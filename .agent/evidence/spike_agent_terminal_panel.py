#!/usr/bin/env python3
"""Does the Agent Terminal panel actually run a process and draw its output?

The unit tests cover the emulator, the key mapping and the launch plan. None of them
prove the thing a user meets: a bottom panel in the editor that starts a program under a
pseudo-terminal and shows what it prints. This drives the real panel, in a real editor,
on a virtual display, through the editor's own MCP tools.

`claude` is not installed in this container, so the panel is pointed at `/bin/sh`
instead. That exercises every layer that matters here - the pty, the VT emulator, the
widget's drawing and the panel's lifecycle - and leaves only the choice of binary
untested, which is a string in a text field.

Evidence collected:
  * whether Godot_FindControl can see the panel's controls at all
  * a screenshot after the shell has printed something, so a human can read it
  * the editor's own output, checked for the crashes this port exists to avoid
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

    workspace = tempfile.mkdtemp(prefix="godot-ai-terminal-")
    project = os.path.join(workspace, "project")
    home = os.path.join(workspace, "home")
    os.makedirs(home)
    e2e.build_project(project)

    environment = display.environment()
    environment["GODOT_AI_HOME"] = home
    environment["GODOT_AI_AUTO_APPROVE"] = "1"

    editor = subprocess.Popen(
        [editor_binary, "--path", project, "--editor",
         "--resolution", "1560x860", "--position", "0,0"] + display.godot_arguments(),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env=environment, cwd=workspace,
    )

    relay = None
    failures = []
    next_id = [10]

    def check(label, condition, detail=""):
        print(("PASS " if condition else "FAIL ") + label)
        if not condition:
            if detail:
                print("     " + detail)
            failures.append(label)

    try:
        descriptor = e2e.wait_for_instance(os.path.join(home, "instances"), editor)
        print("editor up on port %d" % descriptor["port"])
        relay = e2e.RelayProcess(
            args=["--client-name", "terminal-spike", "--approval-mode", "allow"], home=home)

        def call(name, arguments=None):
            next_id[0] += 1
            relay.send_message({"jsonrpc": "2.0", "id": next_id[0], "method": "tools/call",
                                "params": {"name": name, "arguments": arguments or {}}})
            reply = relay.read_message(timeout=60)
            return reply.get("result", {})

        def matches(**query):
            result = call("Godot_FindControl", query)
            return result.get("structuredContent", {}).get("matches", [])

        relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                            "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                                       "clientInfo": {"name": "spike", "version": "1"}}})
        relay.read_message(timeout=60)
        relay.send_message({"jsonrpc": "2.0", "method": "notifications/initialized"})

        # ------------------------------------------------------------------
        # 1. The panel exists. It is not on screen yet, so look for it anyway.
        # ------------------------------------------------------------------
        panels = matches(**{"class": "MCPAgentTerminalPanel", "visible_only": False})
        check("the Agent Terminal panel is in the editor's control tree", len(panels) >= 1,
              json.dumps(panels)[:400])

        widgets = matches(**{"class": "MCPTerminalWidget", "visible_only": False})
        check("the terminal widget was built inside it", len(widgets) >= 1,
              json.dumps(widgets)[:400])

        # ------------------------------------------------------------------
        # 2. Open the bottom panel so the widget is visible and sized.
        # ------------------------------------------------------------------
        buttons = matches(text="Agent Terminal")
        check("the bottom panel has a button to open it", len(buttons) >= 1,
              json.dumps(buttons)[:400])
        # Clicking a tab is a real pointer event into a real editor, and the first one
        # after start-up can land while the layout is still settling. Try a few times
        # rather than reporting a lifecycle bug that is really a race in the harness.
        visible_widgets = []
        for attempt in range(4):
            if not buttons:
                break
            call("Godot_SendEditorInput", {"action": "click",
                                           "x": buttons[0]["center_x"], "y": buttons[0]["center_y"]})
            time.sleep(1.5)
            visible_widgets = matches(**{"class": "MCPTerminalWidget"})
            if visible_widgets:
                break
            hidden = matches(**{"class": "MCPTerminalWidget", "visible_only": False})
            print("     attempt %d: not visible yet (%d in tree)" % (attempt + 1, len(hidden)))
            buttons = matches(text="Agent Terminal") or buttons

        check("the terminal widget is on screen once the panel is open", len(visible_widgets) >= 1,
              json.dumps(visible_widgets)[:400])

        # ------------------------------------------------------------------
        # 3. Point it at a shell and start it. `claude` is not installed here.
        # ------------------------------------------------------------------
        fields = matches(**{"class": "LineEdit", "text": "claude"})
        check("the command field defaults to claude", len(fields) >= 1, json.dumps(fields)[:400])

        if fields:
            call("Godot_SendEditorInput", {"action": "click",
                                           "x": fields[0]["center_x"], "y": fields[0]["center_y"]})
            time.sleep(0.3)
            for _ in range(len("claude")):
                call("Godot_SendEditorInput", {"action": "key_tap", "key": "Backspace"})
            call("Godot_SendEditorInput", {"action": "type", "text": "sh"})
            time.sleep(0.5)
            retyped = matches(**{"class": "LineEdit", "text": "sh"})
            check("the command field took the new command", len(retyped) >= 1,
                  json.dumps(matches(**{"class": "LineEdit"}))[:400])

        starts = [b for b in matches(text="Start") if b.get("class") == "Button"]
        check("there is a Start button", len(starts) >= 1, json.dumps(matches(text="Start"))[:400])

        # ------------------------------------------------------------------
        # 4. The status line is the panel's own account of what happened. It says
        #    something specific either way, so a failure to start is legible rather
        #    than a panel that just sits there.
        # ------------------------------------------------------------------
        status_texts = []
        for attempt in range(4):
            if not starts:
                break
            print("     start click: %s" % json.dumps(call("Godot_SendEditorInput", {
                "action": "click", "x": starts[0]["center_x"], "y": starts[0]["center_y"]}
            ).get("structuredContent", {})))
            time.sleep(2.5)
            status_texts = [m.get("text", "") for m in matches(**{"class": "Label"})]
            if any(t != "Not started." and ("Running" in t or "Could not" in t or "not listening" in t)
                   for t in status_texts):
                break
            print("     attempt %d: status still %s" % (attempt + 1, json.dumps(status_texts)[:200]))
            starts = [b for b in matches(text="Start") if b.get("class") == "Button"] or starts

        started = any("Running" in t for t in status_texts)
        check("the panel reports a running agent", started,
              "status labels: " + json.dumps(status_texts)[:400])

        # Stop should be live now, Start disabled.
        stops = [b for b in matches(text="Stop") if b.get("class") == "Button"]
        check("there is a Stop button", len(stops) >= 1)

        # ------------------------------------------------------------------
        # 5. Type into the terminal and see the shell answer.
        # ------------------------------------------------------------------
        widgets = matches(**{"class": "MCPTerminalWidget"})
        if widgets:
            call("Godot_SendEditorInput", {"action": "click",
                                           "x": widgets[0]["x"] + 40, "y": widgets[0]["y"] + 20})
            time.sleep(0.3)
            call("Godot_SendEditorInput", {"action": "type", "text": "echo TERMINALSPIKEOK"})
            call("Godot_SendEditorInput", {"action": "key_tap", "key": "Enter"})
            time.sleep(2.5)

        shot = call("Godot_CaptureEditorWindow", {})
        wrote = None
        for item in shot.get("content", []):
            if item.get("type") == "image":
                wrote = os.path.join(OUT, "spike_agent_terminal_panel.png")
                with open(wrote, "wb") as handle:
                    handle.write(base64.b64decode(item["data"]))
        check("a screenshot of the editor was captured", wrote is not None)
        print("screenshot: %s" % wrote)

        # ------------------------------------------------------------------
        # 6. Stop it, and make sure the panel says so rather than pretending.
        # ------------------------------------------------------------------
        after = []
        for attempt in range(4):
            stops = [b for b in matches(text="Stop") if b.get("class") == "Button"]
            if not stops:
                break
            call("Godot_SendEditorInput", {"action": "click",
                                           "x": stops[0]["center_x"], "y": stops[0]["center_y"]})
            time.sleep(2)
            after = [m.get("text", "") for m in matches(**{"class": "Label"})]
            if any("Stopped" in t or "exited" in t for t in after):
                break
        check("the panel reports the agent stopped",
              any("Stopped" in t or "exited" in t for t in after),
              "status labels: " + json.dumps(after)[:400])

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
        crashed = [line for line in text.splitlines()
                   if any(word in line for word in
                          ("SIGSEGV", "SIGABRT", "Segmentation", "handle_crash", "Dumping the backtrace"))]
        if crashed:
            print()
            print("EDITOR CRASHED:")
            for line in crashed[:20]:
                print("  " + line)
            failures.append("the editor did not crash")
        noisy = [line for line in text.splitlines()
                 if ("MCPTerminal" in line or "MCPAgentTerminal" in line or "mcp_pty" in line)]
        for line in noisy[:20]:
            print("EDITOR: %s" % line)

    print()
    if failures:
        print("agent terminal spike: %d check(s) failed: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("agent terminal spike: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
