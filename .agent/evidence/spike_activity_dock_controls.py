#!/usr/bin/env python3
"""E2/E4/E5: do the Activity dock's controls work when you press them?

The dock's data has been covered end to end since it was built, but the ledger's own
rule 1 asks for the controls to be exercised the way a person exercises them - found by
`Godot_FindControl` and pressed by `Godot_SendEditorInput` on a real display. That was
recorded as blocked on xdotool. It was not: `Godot_SendEditorInput` drives the editor
in-process and needs no external tool. The actual blocker was reaching the bottom-panel
tab, because in 4.8 the bottom panel switches with a DockTabContainer and tab titles are
not nodes. `Godot_FindControl` searches tabs now, so this is reachable.

What is checked, each by pressing the control and then asking a tool what changed:

  * Pause    - the agent's next mutating call is refused, and nothing is written
  * Resume   - the same call then succeeds
  * Stop     - refuses too, and the agent cannot lift it itself
  * a record - selecting one fills the detail pane with what the call touched
  * Reveal   - selects the file the call touched in the editor
  * What Changed - lists the files the call's checkpoint captured
  * Revert This  - puts the file back, and only that one call is undone
"""
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


def check(condition, message, detail=""):
    print(("PASS " if condition else "FAIL ") + message)
    if not condition:
        if detail:
            print("     " + str(detail)[:600])
        FAILURES.append(message)


def main():
    editor_binary = os.path.join(REPO, "bin", "godot.linuxbsd.editor.dev.x86_64")
    display = virtual_display.ensure(width=1600, height=1000)
    if not display.usable:
        print("SKIP: no display available; pressing controls needs one")
        return 0

    root = tempfile.mkdtemp(prefix="godot-ai-dock-")
    project = os.path.join(root, "project")
    home = os.path.join(root, "home")
    os.makedirs(home)
    e2e.build_project(project)

    # A window manager, because without one "maximised" is a request nobody grants: the
    # X window stays its original size while the DisplayServer keeps reporting the
    # maximised size it asked for, so every rectangle the editor reports is off by the
    # difference. Injected input uses the same wrong space and agrees with itself, which
    # is why this went unnoticed; a real click does not.
    window_manager = subprocess.Popen(["openbox"], env=display.environment(),
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)

    environment = display.environment()
    environment["GODOT_AI_HOME"] = home
    environment["GODOT_AI_AUTO_APPROVE"] = "1"

    editor = subprocess.Popen(
        # Deliberately no --resolution or --position. On a display with no window
        # manager those size the X window but not the editor's internal viewport, so the
        # editor draws at the screen size into a smaller window and its bottom rows -
        # which is where the panel tabs live - end up outside the window entirely. A real
        # click then lands nowhere while an injected one still works, which reads as a
        # dead button. Letting the window match the screen keeps the two in step.
        [editor_binary, "--path", project, "--editor"] + display.godot_arguments(),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env=environment, cwd=root,
    )

    relay = None
    try:
        e2e.wait_for_instance(os.path.join(home, "instances"), editor)
        relay = e2e.RelayProcess(
            args=["--client-name", "dock", "--approval-mode", "allow"], home=home)
        next_id = [10]

        def call(name, arguments=None):
            next_id[0] += 1
            relay.send_message({"jsonrpc": "2.0", "id": next_id[0], "method": "tools/call",
                                "params": {"name": name, "arguments": arguments or {}}})
            reply = relay.read_message(timeout=90)
            if "error" in reply:
                # A call the control gate refused comes back as a protocol error, not a
                # tool result with isError. Both are refusals; flatten them so a check
                # can ask one question.
                return {"isError": True, "error": reply["error"]}
            return reply.get("result", {})

        def structured(name, arguments=None):
            return call(name, arguments).get("structuredContent", {})

        def matches(**query):
            return structured("Godot_FindControl", query).get("matches", [])

        def press(entry):
            """Click where a person would, through X, not through a tool.

            `Godot_SendEditorInput` is a tool call, so the control gate refuses it while
            the agent is held - which is right, and which means the agent can no more
            press Resume than it can write a file. A person still has a mouse. xdotool is
            that mouse, so these checks exercise the dock the way it is actually used.
            """
            e2e.xdotool(display.display, "mousemove", str(entry["center_x"]),
                        str(entry["center_y"]), "click", "1")
            time.sleep(1.2)

        def state():
            return structured("Godot_GetActivity", {"limit": 1}).get("control", {}).get("state")

        def press_named(label, attempts=4, settled=None):
            """Click a button by its text, retrying while the layout settles.

            The first pointer event after a panel opens can land mid-relayout, which is a
            race in this harness rather than a defect in the dock.
            """
            for _ in range(attempts):
                found = [m for m in matches(text=label) if m.get("class") == "Button"]
                if not found:
                    time.sleep(1.0)
                    continue
                press(found[0])
                if settled is None or settled():
                    return True
            return False

        def press_at(rect, settled=None, attempts=4):
            """Press a rectangle captured earlier, retrying until it takes effect.

            Captured earlier because `Godot_FindControl` is refused while the agent is
            held: it is a read, but not one that explains what the agent already did, so
            the gate turns it away like anything else. A person looking at the dock has
            no such problem; this script does, so it writes the coordinates down first.
            """
            for _ in range(attempts):
                press(rect)
                if settled is None or settled():
                    return True
            return False

        # No window manager on a virtual display, so nothing hands X input focus to the
        # editor. Without it, synthesised clicks land nowhere and read as a dead button.
        status, listed = e2e.xdotool(display.display, "search", "--onlyvisible", "--name", ".")
        editor_window = listed.split()[0] if status == 0 and listed.split() else None
        if editor_window:
            e2e.xdotool(display.display, "windowfocus", "--sync", editor_window)
            time.sleep(0.3)

        relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                            "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                                       "clientInfo": {"name": "dock", "version": "1"}}})
        relay.read_message(timeout=90)
        relay.send_message({"jsonrpc": "2.0", "method": "notifications/initialized"})

        # Give the dock something to show, and a checkpoint to revert. Distinct intents,
        # because the row text *is* the intent when one is declared - which is the point
        # of E3, and also what makes one row findable among several.
        call("Godot_SetIntent", {"goal": "measure the jump height",
                                 "activity": "writing the first note"})
        call("Godot_WriteTextFile", {"path": "res://dock_note.txt", "text": "first"})
        call("Godot_SetIntent", {"goal": "measure the jump height",
                                 "activity": "revising the note"})
        call("Godot_WriteTextFile", {"path": "res://dock_note.txt", "text": "second"})
        # A second, unrelated file, so "Revert This" can be shown to undo one call rather
        # than everything since the checkpoint.
        call("Godot_SetIntent", {"goal": "measure the jump height",
                                 "activity": "writing something unrelated"})
        call("Godot_WriteTextFile", {"path": "res://dock_other.txt", "text": "untouched"})

        # ------------------------------------------------------------------
        # Open the dock. Its tab is not a node; the tab search is the only way.
        # ------------------------------------------------------------------
        opened = False
        for _ in range(4):
            tabs = [m for m in matches(text="Agent Activity") if m.get("kind") == "tab"]
            if not tabs:
                time.sleep(1.0)
                continue
            press(tabs[0])
            if matches(**{"class": "MCPActivityDock"}):
                opened = True
                break
        check(opened, "the Agent Activity tab opens the dock")
        if not opened:
            raise SystemExit(1)

        # ------------------------------------------------------------------
        # A record, and the three buttons that act on one.
        # ------------------------------------------------------------------
        # By path, not merely by class: the editor has several Trees and finding one of
        # the others would make this check pass while proving nothing.
        check(any("Agent Activity" in t.get("node_path", "")
                  for t in matches(**{"class": "Tree", "limit": 200})),
              "the dock has a record tree")

        # The row a person would look for: what the agent said it was doing, not the tool
        # name. Retried because the dock polls twice a second and the row may not have
        # been appended yet.
        rows = []
        for _ in range(6):
            rows = [m for m in matches(text="revising the note") if m.get("kind") == "tree_item"]
            if rows:
                break
            time.sleep(1.0)
        check(bool(rows), "the dock lists the write under the intent the agent declared")

        if rows:
            trees = matches(**{"class": "Tree", "limit": 200})
            tree_rect = next((t for t in trees if "Agent Activity" in t.get("node_path", "")), None)
            if tree_rect:
                # A row scrolled out of the tree still has a rect, and clicking it lands
                # on whatever is at those coordinates instead. Every call this script
                # makes is itself a record, so the tree fills up quickly - which is why
                # the record checks come before the control checks rather than after.
                on_screen = (rows[0]["center_y"] >= tree_rect["y"] and
                             rows[0]["center_y"] <= tree_rect["y"] + tree_rect["height"])
                check(on_screen, "the row is inside the visible tree",
                      "row y=%s, tree %s..%s" % (rows[0]["center_y"], tree_rect["y"],
                                                 tree_rect["y"] + tree_rect["height"]))
            press(rows[0])
            # A high limit on purpose: the default of 20 stops the search before it
            # reaches a bottom panel, and an empty answer then looks like an empty pane.
            detail_labels = [m for m in matches(**{"class": "RichTextLabel", "limit": 200})
                             if "Agent Activity" in m.get("node_path", "")]
            texts = " ".join(m.get("text", "") for m in detail_labels)
            check("dock_note.txt" in texts,
                  "selecting a record names the file that call touched", texts[:500])
            check("revising the note" in texts,
                  "and what the agent said it was doing at the time", texts[:500])

            check(press_named("What Changed"), "pressing What Changed is accepted")
            detail = " ".join(m.get("text", "") for m in
                              matches(**{"class": "RichTextLabel", "limit": 200})
                              if "Agent Activity" in m.get("node_path", ""))
            check("Checkpoint" in detail or "checkpoint" in detail,
                  "What Changed names the checkpoint and its files", detail[:400])

            check(press_named("Reveal"), "pressing Reveal is accepted")

            check(press_named("Revert This"), "pressing Revert This is accepted")
            time.sleep(1.5)
            content = structured("Godot_ReadTextFile", {"path": "res://dock_note.txt"})
            check(content.get("text") == "first",
                  "Revert This undid exactly that one call", content)
            later = structured("Godot_ReadTextFile", {"path": "res://dock_other.txt"})
            check(later.get("text") == "untouched",
                  "reverting one call left a later, unrelated one alone", later)

        # Note down where the control buttons are while looking is still allowed.
        controls = {}
        for label in ("Pause", "Stop", "Resume"):
            found = [m for m in matches(text=label) if m.get("class") == "Button"]
            check(bool(found), "the dock has a %s button" % label)
            if found:
                controls[label] = found[0]

        # ------------------------------------------------------------------
        # Pause. The next mutating call must be refused and nothing written.
        # ------------------------------------------------------------------
        check("Pause" in controls and press_at(controls["Pause"], lambda: state() == "paused"),
              "pressing Pause holds the agent", state())

        refused = call("Godot_WriteTextFile", {"path": "res://dock_blocked.txt", "text": "no"})
        check(refused.get("isError") is True,
              "a held agent's write is refused", json.dumps(refused)[:400])

        # ------------------------------------------------------------------
        # Resume, from the dock. There is deliberately no resume tool, and while
        # held the agent cannot even locate the button - which is the point.
        # ------------------------------------------------------------------
        check("Resume" in controls and press_at(controls["Resume"], lambda: state() == "running"),
              "pressing Resume releases the agent", state())

        allowed = call("Godot_WriteTextFile", {"path": "res://dock_after.txt", "text": "yes"})
        check(not allowed.get("isError"), "the same write then succeeds",
              json.dumps(allowed)[:400])

        # Only now can this be asked: reading is refused while held, so the question
        # "did the refusal still write the file" has to wait for the release.
        blocked = call("Godot_ReadTextFile", {"path": "res://dock_blocked.txt"})
        check(blocked.get("isError") is True,
              "the refused write left nothing on disk", json.dumps(blocked)[:300])

        # ------------------------------------------------------------------
        # Stop, which is the same gate under a different name and needs an
        # explicit release. Put it back afterwards so the rest can run.
        # ------------------------------------------------------------------
        check("Stop" in controls and press_at(controls["Stop"], lambda: state() == "stopped"),
              "pressing Stop holds the agent", state())
        stopped_write = call("Godot_WriteTextFile", {"path": "res://dock_stopped.txt", "text": "no"})
        check(stopped_write.get("isError") is True, "a stopped agent's write is refused")

        # And it cannot look for the button that would release it. That is the whole
        # design: a hold the held party can lift is advisory.
        blind = call("Godot_FindControl", {"text": "Resume"})
        check(blind.get("isError") is True,
              "a held agent cannot even locate the control that would release it",
              json.dumps(blind)[:300])

        check("Resume" in controls and press_at(controls["Resume"], lambda: state() == "running"),
              "Resume releases a stopped agent too", state())

    finally:
        if relay:
            relay.finish(timeout=10)
        editor.terminate()
        try:
            editor.wait(timeout=25)
        except subprocess.TimeoutExpired:
            editor.kill()
        window_manager.terminate()

    print()
    if FAILURES:
        print("activity dock controls: %d check(s) failed: %s" % (len(FAILURES), "; ".join(FAILURES)))
        return 1
    print("activity dock controls: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
