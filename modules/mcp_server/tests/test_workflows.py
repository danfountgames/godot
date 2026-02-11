"""LLM workflow scenario tests for the Godot MCP server.

Simulates multi-step tool call patterns that LLM agents use in practice:
  - Read-validate-write-validate cycles (bug fixing)
  - UI automation via scene tree inspection and control clicking
  - Performance investigation and screenshot capture
  - Session resumption with output cursors

Each test is self-contained and uses try/finally to guarantee cleanup.
"""

import time

import pytest

from conftest import get_structured, get_text, is_error


# ---------------------------------------------------------------------------
# Workflow 1: Fix a GDScript Bug
# ---------------------------------------------------------------------------

BROKEN_GD_ORIGINAL = (
    "extends Node\n"
    "\n"
    "var health: int = 100\n"
    "\n"
    "func broken_syntax( -> void:  # Line 5: syntax error (missing closing paren)\n"
    "\tpass\n"
    "\n"
    "func _ready() -> void:\n"
    "\tpass\n"
    "\n"
    "func bad_type() -> void:\n"
    "\tvar x: int = \"not an int\"  # Line 12: type error\n"
)

FIXED_GD_CONTENT = (
    "extends Node\n"
    "\n"
    "var health: int = 100\n"
    "\n"
    "func fixed_syntax() -> void:\n"
    "\tpass\n"
    "\n"
    "func _ready() -> void:\n"
    "\tpass\n"
    "\n"
    "func good_type() -> void:\n"
    "\tvar x: int = 42\n"
)


@pytest.mark.p1
def test_workflow_fix_gdscript_bug(client):
    """Workflow 1: Read-validate-write-validate cycle for fixing a broken script.

    Simulates an LLM agent that discovers a bug, reads the file, checks for
    errors, writes a fix, validates the fix, runs the project, and confirms
    startup output.
    """
    # Step 1: Get project info -- verify metadata and main_scene.
    resp = client.call_tool("project/get_info")
    assert not is_error(resp), f"project/get_info failed: {resp}"
    sc = get_structured(resp)
    text = get_text(resp)
    # Project info may be in structured content or text; either way it should
    # contain reference to the project and a main scene.
    combined = str(sc) + text
    assert "main_scene" in combined.lower() or "main" in combined.lower(), (
        f"project/get_info should mention main_scene: {combined[:500]}"
    )

    # Step 2: List files -- expect 20+ .gd files, including broken.gd.
    resp = client.call_tool("editor/list_files", {
        "directory": "res://",
        "recursive": True,
        "extension": "gd",
    })
    assert not is_error(resp), f"editor/list_files failed: {resp}"
    sc = get_structured(resp)
    text = get_text(resp)
    combined = str(sc) + text
    # The file list may be in structured content (as a list) or text.
    assert "broken.gd" in combined, (
        f"File listing should contain broken.gd: {combined[:1000]}"
    )

    # Step 3: Read the broken file.
    resp = client.call_tool("editor/read_file", {"path": "res://scripts/broken.gd"})
    assert not is_error(resp), f"editor/read_file failed: {resp}"
    original_content = get_text(resp)
    sc = get_structured(resp)
    if not original_content and sc:
        original_content = sc.get("content", str(sc))
    assert "broken_syntax" in original_content or "bad_type" in original_content, (
        f"broken.gd should contain error markers: {original_content[:500]}"
    )

    # Step 4: Check for errors in the broken file.
    resp = client.call_tool("gdscript/check_errors", {"path": "res://scripts/broken.gd"})
    assert not is_error(resp), f"gdscript/check_errors failed: {resp}"
    sc = get_structured(resp)
    text = get_text(resp)
    combined = str(sc) + text
    # Should report invalid / errors, referencing line 5.
    assert "false" in combined.lower() or "error" in combined.lower(), (
        f"check_errors should report invalid for broken.gd: {combined[:500]}"
    )
    assert "5" in combined, (
        f"check_errors should reference line 5: {combined[:500]}"
    )

    # Steps 5-10 modify the file, so wrap in try/finally to restore it.
    try:
        # Step 5: Write the fixed version.
        resp = client.call_tool("editor/write_file", {
            "path": "res://scripts/broken.gd",
            "content": FIXED_GD_CONTENT,
        })
        assert not is_error(resp), f"editor/write_file (fix) failed: {resp}"

        # Step 6: Check errors again -- should be valid now.
        resp = client.call_tool("gdscript/check_errors", {
            "path": "res://scripts/broken.gd",
        })
        assert not is_error(resp), f"gdscript/check_errors (fixed) failed: {resp}"
        sc = get_structured(resp)
        text = get_text(resp)
        combined = str(sc) + text
        # Should indicate valid / no errors.
        assert "true" in combined.lower() or "valid" in combined.lower(), (
            f"check_errors should report valid after fix: {combined[:500]}"
        )
        # Should NOT contain error references now.
        has_errors = "error" in combined.lower() and "no error" not in combined.lower()
        if has_errors:
            # Allow structured content that has an empty errors list.
            errors_list = sc.get("errors", []) if isinstance(sc, dict) else []
            assert len(errors_list) == 0, (
                f"check_errors should have no errors after fix: {combined[:500]}"
            )

        # Step 7: Run the project.
        resp = client.call_tool("runtime/run_project")
        assert not is_error(resp), f"runtime/run_project failed: {resp}"
        sc = get_structured(resp)
        text = get_text(resp)
        combined = (str(sc) + text).lower()
        assert "launch" in combined or "queued" in combined or "running" in combined, (
            f"runtime/run_project should indicate launching: {combined[:300]}"
        )

        try:
            # Step 8: Poll runtime/get_status until running (up to 10s).
            deadline = time.time() + 10.0
            state = None
            while time.time() < deadline:
                resp = client.call_tool("runtime/get_status")
                sc = get_structured(resp)
                state = sc.get("state", "") if isinstance(sc, dict) else ""
                if state == "running":
                    break
                time.sleep(0.1)
            assert state == "running", (
                f"Game did not reach running state within 10s, last state: {state}"
            )

            # Step 9: Get output -- should contain STARTUP_COMPLETE.
            # The output may take a moment to be captured after the game
            # reaches the running state, so poll with retries.
            combined = ""
            for _attempt in range(10):
                time.sleep(0.5)
                resp = client.call_tool("runtime/get_output")
                assert not is_error(resp), f"runtime/get_output failed: {resp}"
                sc = get_structured(resp)
                text = get_text(resp)
                combined = str(sc) + text
                if "STARTUP_COMPLETE" in combined:
                    break
            assert "STARTUP_COMPLETE" in combined, (
                f"Game output should contain STARTUP_COMPLETE: {combined[:1000]}"
            )

        finally:
            # Step 10: Stop the game.
            try:
                client.call_tool("runtime/stop")
                time.sleep(0.5)
            except Exception:
                pass

    finally:
        # IMPORTANT: Always restore the original broken.gd content.
        client.call_tool("editor/write_file", {
            "path": "res://scripts/broken.gd",
            "content": BROKEN_GD_ORIGINAL,
        })


# ---------------------------------------------------------------------------
# Workflow 2: UI Automation
# ---------------------------------------------------------------------------

@pytest.mark.p1
@pytest.mark.requires_game
def test_workflow_ui_automation(client):
    """Workflow 2: Launch a UI scene, inspect the tree, click a button, verify.

    Simulates an LLM agent that runs a UI test scene, finds interactive
    controls via scene tree search, clicks a button, and verifies the click
    was processed via game output.
    """
    # Step 1: Run the UI test scene.
    resp = client.call_tool("runtime/run_scene", {"scene": "res://scenes/ui_test.tscn"})
    assert not is_error(resp), f"runtime/run_scene failed: {resp}"
    sc = get_structured(resp)
    text = get_text(resp)
    combined = (str(sc) + text).lower()
    assert "launch" in combined or "queued" in combined or "running" in combined, (
        f"runtime/run_scene should indicate launch queued: {combined[:300]}"
    )

    try:
        # Step 2: Poll runtime/get_status until running.
        deadline = time.time() + 10.0
        state = None
        while time.time() < deadline:
            resp = client.call_tool("runtime/get_status")
            sc = get_structured(resp)
            state = sc.get("state", "") if isinstance(sc, dict) else ""
            if state == "running":
                break
            time.sleep(0.1)
        assert state == "running", (
            f"Scene did not reach running state, last state: {state}"
        )

        # Step 3: Get scene tree -- should contain UITest, Panel, StartButton.
        resp = client.call_tool("runtime/get_scene_tree")
        assert not is_error(resp), f"runtime/get_scene_tree failed: {resp}"
        sc = get_structured(resp)
        text = get_text(resp)
        combined = str(sc) + text
        assert "UITest" in combined, (
            f"Scene tree should contain UITest node: {combined[:1000]}"
        )
        assert "Panel" in combined, (
            f"Scene tree should contain Panel node: {combined[:1000]}"
        )
        assert "StartButton" in combined, (
            f"Scene tree should contain StartButton node: {combined[:1000]}"
        )

        # Step 4: Search scene tree for buttons.
        resp = client.call_tool("runtime/search_scene_tree", {
            "name_pattern": "*Button*",
        })
        assert not is_error(resp), f"runtime/search_scene_tree failed: {resp}"
        sc = get_structured(resp)
        text = get_text(resp)
        combined = str(sc) + text
        assert "StartButton" in combined, (
            f"search_scene_tree should find StartButton: {combined[:500]}"
        )

        # Step 5: Click the start button.
        resp = client.call_tool("runtime/input/click_control", {
            "node_path": "/root/UITest/Panel/StartButton",
        })
        assert not is_error(resp), f"runtime/input/click_control failed: {resp}"

        # Step 6: Wait a few frames for the click to propagate.
        resp = client.call_tool("runtime/wait_frames", {"frames": 5})
        assert not is_error(resp), f"runtime/wait_frames failed: {resp}"
        sc = get_structured(resp)
        text = get_text(resp)
        combined = str(sc) + text
        assert "5" in combined or "waited" in combined.lower(), (
            f"wait_frames should confirm waiting 5 frames: {combined[:300]}"
        )

        # Step 7: Get output -- should contain BUTTON_CLICKED.
        resp = client.call_tool("runtime/get_output")
        assert not is_error(resp), f"runtime/get_output failed: {resp}"
        sc = get_structured(resp)
        text = get_text(resp)
        combined = str(sc) + text
        assert "BUTTON_CLICKED" in combined, (
            f"Game output should contain BUTTON_CLICKED: {combined[:1000]}"
        )

    finally:
        # Step 8: Always stop the game.
        try:
            client.call_tool("runtime/stop")
            time.sleep(0.5)
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Workflow 3: Performance Investigation
# ---------------------------------------------------------------------------

@pytest.mark.p1
@pytest.mark.requires_game
def test_workflow_session_investigation(client):
    """Workflow 3: Run the project, get session summary, evaluate, screenshot.

    Simulates an LLM agent investigating runtime state: using the session
    summary for FPS/node counts, evaluating expressions, and capturing a screenshot.
    """
    # Step 1: Run the project.
    resp = client.call_tool("runtime/run_project")
    assert not is_error(resp), f"runtime/run_project failed: {resp}"

    try:
        # Step 2: Poll until running.
        deadline = time.time() + 10.0
        state = None
        while time.time() < deadline:
            resp = client.call_tool("runtime/get_status")
            sc = get_structured(resp)
            state = sc.get("state", "") if isinstance(sc, dict) else ""
            if state == "running":
                break
            time.sleep(0.1)
        assert state == "running", (
            f"Game did not reach running state, last state: {state}"
        )

        # Step 3: Session summary includes performance data.
        resp = client.call_tool("runtime/get_session_summary")
        assert not is_error(resp), f"runtime/get_session_summary failed: {resp}"
        sc = get_structured(resp)
        text = get_text(resp)

        # Performance is embedded in the session summary.
        perf = sc.get("performance", {}) if isinstance(sc, dict) else {}
        fps = perf.get("fps", 0) if isinstance(perf, dict) else 0
        node_count = perf.get("node_count", 0) if isinstance(perf, dict) else 0

        # Fallback: parse from text.
        if not fps and text:
            import re
            fps_match = re.search(r"FPS[:\s]+(\d+)", text)
            if fps_match:
                fps = int(fps_match.group(1))

        assert int(fps) > 0, (
            f"FPS should be > 0, got {fps}. Full response: {sc} / {text[:300]}"
        )

        # Step 4: Evaluate a simple expression.
        resp = client.call_tool("runtime/evaluate", {"expression": "2+2"})
        assert not is_error(resp), f"runtime/evaluate failed: {resp}"
        sc = get_structured(resp)
        text = get_text(resp)
        combined = str(sc) + text
        assert "4" in combined, (
            f"Evaluating 2+2 should produce 4: {combined[:300]}"
        )

        # Step 5: Take a screenshot -- should have image content with PNG data.
        resp = client.call_tool("runtime/get_screenshot")
        assert not is_error(resp), f"runtime/get_screenshot failed: {resp}"
        result = resp.get("result", {})
        content = result.get("content", [])
        # Look for image content in the response.
        has_image = False
        for item in content:
            if item.get("type") == "image":
                has_image = True
                # PNG files start with specific magic bytes; the data field
                # should be base64-encoded PNG.
                data = item.get("data", "")
                assert len(data) > 0, "Screenshot image data should not be empty"
                # Base64 encoded PNG starts with "iVBOR" (for the PNG header).
                assert data.startswith("iVBOR"), (
                    f"Screenshot data should be base64 PNG, "
                    f"starts with: {data[:20]}"
                )
                break
        assert has_image, (
            f"runtime/get_screenshot should return image content: "
            f"{[item.get('type') for item in content]}"
        )

    finally:
        # Step 6: Always stop the game.
        try:
            client.call_tool("runtime/stop")
            time.sleep(0.5)
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Workflow 4: Resume Session with Summary
# ---------------------------------------------------------------------------

@pytest.mark.p1
@pytest.mark.requires_game
def test_workflow_resume_session_with_summary(client):
    """Workflow 4: Get session summary, wait, then fetch only new output.

    Simulates an LLM agent resuming a session: it gets a summary of the
    current state, records the output cursor, waits for more activity, then
    fetches only the output produced after the summary -- verifying no
    duplicate lines appear.
    """
    # Step 1: Run the project.
    resp = client.call_tool("runtime/run_project")
    assert not is_error(resp), f"runtime/run_project failed: {resp}"

    try:
        # Step 2: Poll until running.
        deadline = time.time() + 10.0
        state = None
        while time.time() < deadline:
            resp = client.call_tool("runtime/get_status")
            sc = get_structured(resp)
            state = sc.get("state", "") if isinstance(sc, dict) else ""
            if state == "running":
                break
            time.sleep(0.1)
        assert state == "running", (
            f"Game did not reach running state, last state: {state}"
        )

        # Step 3: Get session summary.
        resp = client.call_tool("runtime/get_session_summary")
        assert not is_error(resp), f"runtime/get_session_summary failed: {resp}"
        sc = get_structured(resp)
        text = get_text(resp)
        combined = str(sc) + text

        # Summary should include status information (running).
        assert "running" in combined.lower(), (
            f"Session summary should show running status: {combined[:500]}"
        )

        # Summary should include scene tree information.
        assert "scene" in combined.lower() or "tree" in combined.lower(), (
            f"Session summary should include scene tree info: {combined[:500]}"
        )

        # Summary should include recent output.
        assert "output" in combined.lower() or "recent" in combined.lower(), (
            f"Session summary should include recent output: {combined[:500]}"
        )

        # Extract the output cursor from the structured content.
        cursor = None
        if isinstance(sc, dict):
            recent_output = sc.get("recent_output", {})
            if isinstance(recent_output, dict):
                cursor = recent_output.get("cursor")
            # Also check top-level cursor.
            if cursor is None:
                cursor = sc.get("cursor")

        # Record lines from the summary for duplicate detection.
        summary_output_text = ""
        if isinstance(sc, dict):
            recent_output = sc.get("recent_output", {})
            if isinstance(recent_output, dict):
                lines = recent_output.get("lines", [])
                if isinstance(lines, list):
                    summary_output_text = "\n".join(str(l) for l in lines)
                elif isinstance(recent_output.get("text"), str):
                    summary_output_text = recent_output["text"]

        # Step 4: Wait about 1 second (60 frames at ~60fps).
        resp = client.call_tool("runtime/wait_frames", {"frames": 60})
        assert not is_error(resp), f"runtime/wait_frames failed: {resp}"

        # Step 5: Get output using the cursor -- should return only new output.
        if cursor is not None:
            resp = client.call_tool("runtime/get_output", {"cursor": cursor})
        else:
            # Fallback: get all output if no cursor was available.
            resp = client.call_tool("runtime/get_output")

        assert not is_error(resp), f"runtime/get_output (with cursor) failed: {resp}"
        sc = get_structured(resp)
        text = get_text(resp)
        new_output = str(sc) + text

        # If we had a cursor, verify no duplicate lines from the summary.
        if cursor is not None and summary_output_text:
            summary_lines = [
                line.strip() for line in summary_output_text.split("\n")
                if line.strip()
            ]
            new_lines = [
                line.strip() for line in new_output.split("\n")
                if line.strip()
            ]
            for summary_line in summary_lines:
                # Skip very short or generic lines that might legitimately
                # appear again (like empty lines or frame markers).
                if len(summary_line) < 5:
                    continue
                duplicate_count = new_lines.count(summary_line)
                assert duplicate_count == 0, (
                    f"Output after cursor should not duplicate summary line: "
                    f"{summary_line!r} (appeared {duplicate_count} time(s))"
                )

    finally:
        # Step 6: Always stop the game.
        try:
            client.call_tool("runtime/stop")
            time.sleep(0.5)
        except Exception:
            pass
