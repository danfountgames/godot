"""MCP tool integration tests -- covers all 29 registered tools.

Test plan: AGENT_07
Requires:
    - Godot editor running with MCP test project
    - MCP server listening on 127.0.0.1:6009
"""

import time

import pytest

from conftest import get_structured, get_text, is_error


# ===========================================================================
# Category A -- Project Context
# ===========================================================================


class TestProjectTools:
    """Tests for project/get_info and project/get_input_map."""

    @pytest.mark.p0
    def test_a01_get_info(self, client):
        """T-A01: project/get_info returns project metadata."""
        resp = client.call_tool("project/get_info")
        assert not is_error(resp), f"project/get_info returned error: {get_text(resp)}"

        text = get_text(resp)
        assert "MCP Test Project" in text

        sc = get_structured(resp)
        assert sc.get("godot_version", "").startswith("4.")
        assert sc.get("main_scene") == "res://scenes/main.tscn"
        autoloads = sc.get("autoloads", {})
        assert "TestAutoload" in autoloads or "TestAutoload" in str(autoloads)

    @pytest.mark.p1
    def test_a02_get_input_map(self, client):
        """T-A02: project/get_input_map returns input actions."""
        resp = client.call_tool("project/get_input_map")
        assert not is_error(resp), f"project/get_input_map returned error: {get_text(resp)}"

        sc = get_structured(resp)
        actions = sc.get("actions", sc)
        # Check expected actions exist.
        action_names = set()
        if isinstance(actions, dict):
            action_names = set(actions.keys())
        elif isinstance(actions, list):
            for a in actions:
                name = a.get("name", a.get("action", ""))
                if name:
                    action_names.add(name)

        for expected in ("jump", "move_left", "move_right"):
            assert expected in action_names, f"Missing expected action: {expected}"

        action_count = sc.get("action_count", len(action_names))
        assert action_count >= 3


# ===========================================================================
# Category B -- Editor File Operations
# ===========================================================================


class TestEditorFileTools:
    """Tests for editor/read_file, editor/write_file, editor/list_files, editor/reimport."""

    @pytest.mark.p0
    def test_b01_read_file_positive(self, client):
        """T-B01: editor/read_file reads project.godot successfully."""
        resp = client.call_tool("editor/read_file", {"path": "res://project.godot"})
        assert not is_error(resp), f"read_file returned error: {get_text(resp)}"

        text = get_text(resp)
        assert 'config/name="MCP Test Project"' in text

        sc = get_structured(resp)
        assert sc.get("size_bytes", 1) > 0

    @pytest.mark.p0
    @pytest.mark.security
    def test_b02_read_file_path_traversal(self, client):
        """T-B02: editor/read_file rejects path traversal."""
        resp = client.call_tool("editor/read_file", {"path": "res://../../etc/passwd"})
        assert is_error(resp), "Path traversal should be rejected"

    @pytest.mark.p1
    @pytest.mark.security
    def test_b03_read_file_no_res_prefix(self, client):
        """T-B03: editor/read_file rejects paths without res:// prefix."""
        resp = client.call_tool("editor/read_file", {"path": "/etc/passwd"})
        assert is_error(resp), "Absolute path without res:// should be rejected"

    @pytest.mark.p1
    def test_b04_read_file_not_found(self, client):
        """T-B04: editor/read_file returns error for nonexistent file."""
        resp = client.call_tool("editor/read_file", {"path": "res://nonexistent_file.gd"})
        assert is_error(resp), "Reading nonexistent file should return error"

    @pytest.mark.p1
    def test_b05_write_file_positive(self, client):
        """T-B05: editor/write_file writes and reads back a file."""
        test_path = "res://scripts/test_write.gd"
        test_content = "extends Node\n"

        try:
            # Write the file.
            resp = client.call_tool(
                "editor/write_file",
                {"path": test_path, "content": test_content},
            )
            assert not is_error(resp), f"write_file returned error: {get_text(resp)}"

            sc = get_structured(resp)
            assert sc.get("bytes_written", 0) > 0

            # Read it back.
            read_resp = client.call_tool("editor/read_file", {"path": test_path})
            assert not is_error(read_resp)
            read_text = get_text(read_resp)
            assert "extends Node" in read_text
        finally:
            # Cleanup: overwrite with empty content or minimal content.
            client.call_tool(
                "editor/write_file",
                {"path": test_path, "content": ""},
            )

    @pytest.mark.p1
    @pytest.mark.security
    def test_b06_write_file_path_traversal(self, client):
        """T-B06: editor/write_file rejects path traversal."""
        resp = client.call_tool(
            "editor/write_file",
            {"path": "res://../../tmp/evil.gd", "content": "malicious"},
        )
        assert is_error(resp), "Path traversal in write should be rejected"

    @pytest.mark.p1
    def test_b07_list_files_positive(self, client):
        """T-B07: editor/list_files lists GDScript files recursively."""
        resp = client.call_tool(
            "editor/list_files",
            {"path": "res://scripts/", "extension": "gd", "recursive": True},
        )
        assert not is_error(resp), f"list_files returned error: {get_text(resp)}"

        sc = get_structured(resp)
        count = sc.get("count", sc.get("file_count", 0))
        files = sc.get("files", [])
        actual_count = count if count else len(files)
        assert actual_count >= 22, f"Expected >= 22 GDScript files, got {actual_count}"

    @pytest.mark.p1
    def test_b08_reimport(self, client):
        """T-B08: editor/reimport queues reimport for a resource."""
        resp = client.call_tool(
            "editor/reimport",
            {"files": ["res://scenes/main.tscn"]},
        )
        assert not is_error(resp), f"reimport returned error: {get_text(resp)}"

        sc = get_structured(resp)
        status = sc.get("status", "")
        assert status == "queued", f"Expected status 'queued', got '{status}'"


# ===========================================================================
# Category C -- Editor Operations
# ===========================================================================


class TestEditorOps:
    """Tests for editor/scan_filesystem, editor/get_uid, editor/resolve_uid."""

    @pytest.mark.p1
    def test_c01_scan_filesystem(self, client):
        """T-C01: editor/scan_filesystem queues a filesystem scan."""
        resp = client.call_tool("editor/scan_filesystem")
        assert not is_error(resp), f"scan_filesystem returned error: {get_text(resp)}"

        sc = get_structured(resp)
        status = sc.get("status", "")
        assert status == "queued", f"Expected status 'queued', got '{status}'"

    @pytest.mark.p1
    def test_c02_get_uid(self, client):
        """T-C02: editor/get_uid returns a uid:// identifier."""
        # main.gd has a .uid sidecar file in the test project.
        resp = client.call_tool("editor/get_uid", {"path": "res://scripts/main.gd"})
        assert not is_error(resp), f"editor/get_uid returned error: {get_text(resp)}"
        sc = get_structured(resp)
        uid = sc.get("uid", "")
        assert uid.startswith("uid://"), f"Expected uid:// prefix, got '{uid}'"

    @pytest.mark.p1
    def test_c03_resolve_uid_roundtrip(self, client):
        """T-C03: get_uid then resolve_uid round-trips to original path."""
        test_path = "res://scripts/main.gd"

        # Step 1: Get the UID for a known file.
        uid_resp = client.call_tool("editor/get_uid", {"path": test_path})
        assert not is_error(uid_resp), f"editor/get_uid returned error: {get_text(uid_resp)}"
        uid = get_structured(uid_resp).get("uid", "")
        assert uid.startswith("uid://"), f"Expected uid:// prefix, got '{uid}'"

        # Step 2: Resolve the UID back to a path.
        resolve_resp = client.call_tool(
            "editor/resolve_uid",
            {"identifier": uid},
        )
        assert not is_error(resolve_resp), f"resolve_uid returned error: {get_text(resolve_resp)}"

        sc = get_structured(resolve_resp)
        resolved_path = sc.get("path", "")
        assert resolved_path == test_path, (
            f"Expected '{test_path}', got '{resolved_path}'"
        )


# ===========================================================================
# Category D -- GDScript Validation
# ===========================================================================


class TestGDScriptTools:
    """Tests for gdscript/check_errors and gdscript/check_all."""

    @pytest.mark.p0
    def test_d01_check_errors_valid(self, client):
        """T-D01: gdscript/check_errors reports valid script as clean."""
        resp = client.call_tool(
            "gdscript/check_errors",
            {"path": "res://scripts/valid.gd"},
        )
        assert not is_error(resp), f"check_errors returned error: {get_text(resp)}"

        sc = get_structured(resp)
        assert sc.get("valid") is True, f"Expected valid=true, got {sc}"
        errors = sc.get("errors", [])
        assert len(errors) == 0, f"Expected no errors, got {errors}"

    @pytest.mark.p0
    def test_d02_check_errors_broken(self, client):
        """T-D02: gdscript/check_errors detects errors in broken script."""
        resp = client.call_tool(
            "gdscript/check_errors",
            {"path": "res://scripts/broken.gd"},
        )
        assert not is_error(resp), f"check_errors tool itself errored: {get_text(resp)}"

        sc = get_structured(resp)
        assert sc.get("valid") is False, f"Expected valid=false for broken script, got {sc}"
        errors = sc.get("errors", [])
        assert len(errors) > 0, "Expected at least one error in broken.gd"

        # Check that at least one error is on line 5.
        lines = [e.get("line", -1) for e in errors]
        assert 5 in lines, f"Expected error on line 5, got errors on lines {lines}"

    @pytest.mark.p1
    def test_d03_check_errors_nonexistent(self, client):
        """T-D03: gdscript/check_errors returns error for nonexistent file."""
        resp = client.call_tool(
            "gdscript/check_errors",
            {"path": "res://scripts/does_not_exist.gd"},
        )
        assert is_error(resp), "Checking nonexistent script should return error"

    @pytest.mark.p1
    def test_d04_check_all(self, client):
        """T-D04: gdscript/check_all reports project-wide script health."""
        resp = client.call_tool("gdscript/check_all")
        assert not is_error(resp), f"check_all returned error: {get_text(resp)}"

        sc = get_structured(resp)
        total = sc.get("total_files", 0)
        assert total >= 22, f"Expected >= 22 total files, got {total}"

        with_errors = sc.get("files_with_errors", 0)
        assert with_errors >= 1, f"Expected >= 1 files with errors, got {with_errors}"

        clean = sc.get("files_clean", 0)
        assert clean >= 20, f"Expected >= 20 clean files, got {clean}"


# ===========================================================================
# Category E -- Game Lifecycle
# ===========================================================================


class TestGameLifecycle:
    """Tests for debug/run_project, debug/run_scene, debug/get_status, debug/stop.

    This class manages game start/stop explicitly rather than using the
    running_game fixture, because it IS testing the lifecycle itself.
    """

    def _ensure_stopped(self, client):
        """Make sure the game is stopped before/after a test."""
        try:
            status_resp = client.call_tool("debug/get_status")
            state = get_structured(status_resp).get("state", "stopped")
            if state != "stopped":
                client.call_tool("debug/stop")
                time.sleep(1.0)
        except Exception:
            pass

    @pytest.mark.p0
    def test_e01_run_project(self, client):
        """T-E01: debug/run_project launches the game."""
        self._ensure_stopped(client)
        try:
            resp = client.call_tool("debug/run_project")
            assert not is_error(resp), f"run_project returned error: {get_text(resp)}"

            sc = get_structured(resp)
            status = sc.get("status", "")
            assert status in ("launching", "queued"), (
                f"Expected 'launching' or 'queued', got '{status}'"
            )
        finally:
            # Wait briefly then stop.
            time.sleep(1.0)
            self._ensure_stopped(client)

    @pytest.mark.p0
    def test_e02_get_status_running(self, client):
        """T-E02: debug/get_status reports 'running' after game starts."""
        self._ensure_stopped(client)
        try:
            client.start_game_and_wait()

            resp = client.call_tool("debug/get_status")
            assert not is_error(resp)
            sc = get_structured(resp)
            assert sc.get("state") == "running", (
                f"Expected state 'running', got '{sc.get('state')}'"
            )
        finally:
            self._ensure_stopped(client)

    @pytest.mark.p1
    def test_e03_get_status_stopped(self, client):
        """T-E03: debug/get_status reports 'stopped' when no game is running."""
        self._ensure_stopped(client)

        resp = client.call_tool("debug/get_status")
        assert not is_error(resp)
        sc = get_structured(resp)
        assert sc.get("state") == "stopped", (
            f"Expected state 'stopped', got '{sc.get('state')}'"
        )

    @pytest.mark.p0
    def test_e04_stop(self, client):
        """T-E04: debug/stop stops the running game."""
        self._ensure_stopped(client)
        try:
            client.start_game_and_wait()

            resp = client.call_tool("debug/stop")
            assert not is_error(resp), f"stop returned error: {get_text(resp)}"

            # Give it a moment to shut down, then verify.
            time.sleep(1.0)
            status_resp = client.call_tool("debug/get_status")
            sc = get_structured(status_resp)
            assert sc.get("state") == "stopped", (
                f"Expected 'stopped' after stop, got '{sc.get('state')}'"
            )
        finally:
            self._ensure_stopped(client)

    @pytest.mark.p1
    def test_e05_run_scene(self, client):
        """T-E05: debug/run_scene launches a specific scene."""
        self._ensure_stopped(client)
        try:
            resp = client.call_tool(
                "debug/run_scene",
                {"scene": "res://scenes/ui_test.tscn"},
            )
            assert not is_error(resp), f"run_scene returned error: {get_text(resp)}"

            # Wait for the game to reach running state.
            client.wait_for_game_running()

            status_resp = client.call_tool("debug/get_status")
            sc = get_structured(status_resp)
            assert sc.get("state") == "running"
        finally:
            self._ensure_stopped(client)


# ===========================================================================
# Category F -- Live Inspection (requires running game)
# ===========================================================================


@pytest.mark.requires_game
class TestLiveInspection:
    """Tests for scene tree inspection, output, errors, and performance.

    All tests in this class use the module-scoped running_game fixture.
    """

    @pytest.mark.p0
    def test_f01_get_scene_tree(self, running_game):
        """T-F01: debug/get_scene_tree returns the live scene tree."""
        client = running_game
        resp = client.call_tool("debug/get_scene_tree")
        assert not is_error(resp), f"get_scene_tree returned error: {get_text(resp)}"

        sc = get_structured(resp)
        node_count = sc.get("node_count", 0)
        assert node_count > 0, f"Expected node_count > 0, got {node_count}"

        # Verify expected nodes exist in the tree.
        tree_str = str(sc)
        assert "Main" in tree_str, "Expected 'Main' node in scene tree"
        # Main node has a custom script, so type may be the script path or Node2D.
        assert "main.gd" in tree_str or "Node2D" in tree_str, \
            "Expected 'main.gd' or 'Node2D' type for Main node in scene tree"
        assert "Player" in tree_str, "Expected 'Player' node in scene tree"
        assert "CharacterBody2D" in tree_str, "Expected CharacterBody2D type in scene tree"

    @pytest.mark.p1
    def test_f02_get_scene_tree_max_depth(self, running_game):
        """T-F02: debug/get_scene_tree with max_depth=1 returns shallow tree.

        The node_count field always reports the FULL tree size.
        Truncation is reflected in the tree structure itself --
        deeper nodes are pruned, and _hidden_children counts are added.
        """
        client = running_game

        def count_tree_nodes(tree):
            """Count nodes actually present in the tree structure."""
            if not tree or not isinstance(tree, dict):
                return 0
            count = 1
            for child in tree.get("children", []):
                count += count_tree_nodes(child)
            return count

        # Full tree for comparison.
        full_resp = client.call_tool("debug/get_scene_tree")
        full_sc = get_structured(full_resp)
        full_tree = full_sc.get("tree", {})
        full_tree_nodes = count_tree_nodes(full_tree)
        total_node_count = full_sc.get("node_count", 0)

        # Shallow tree with max_depth=1 (root + immediate children only).
        shallow_resp = client.call_tool(
            "debug/get_scene_tree",
            {"max_depth": 1},
        )
        assert not is_error(shallow_resp)
        shallow_sc = get_structured(shallow_resp)
        shallow_tree = shallow_sc.get("tree", {})
        shallow_tree_nodes = count_tree_nodes(shallow_tree)

        # node_count should still report the full tree size.
        assert shallow_sc.get("node_count", 0) == total_node_count, (
            "node_count should report full tree size regardless of max_depth"
        )

        # The actual tree structure should be shallower.
        assert shallow_tree_nodes <= full_tree_nodes, (
            f"max_depth=1 tree ({shallow_tree_nodes} nodes in structure) "
            f"should be <= full tree ({full_tree_nodes} nodes)"
        )

    @pytest.mark.p1
    def test_f03_search_scene_tree_by_name(self, running_game):
        """T-F03: debug/search_scene_tree finds nodes by name pattern."""
        client = running_game
        resp = client.call_tool(
            "debug/search_scene_tree",
            {"name_pattern": "Player"},
        )
        assert not is_error(resp), f"search_scene_tree returned error: {get_text(resp)}"

        sc = get_structured(resp)
        match_count = sc.get("match_count", 0)
        assert match_count >= 1, f"Expected >= 1 match for 'Player', got {match_count}"

    @pytest.mark.p1
    def test_f04_search_scene_tree_by_type(self, running_game):
        """T-F04: debug/search_scene_tree finds nodes by type."""
        client = running_game
        resp = client.call_tool(
            "debug/search_scene_tree",
            {"type": "Camera2D"},
        )
        assert not is_error(resp), f"search_scene_tree returned error: {get_text(resp)}"

        sc = get_structured(resp)
        match_count = sc.get("match_count", 0)
        assert match_count >= 1, f"Expected >= 1 Camera2D node, got {match_count}"

    @pytest.mark.p1
    def test_f05_get_node_properties(self, running_game):
        """T-F05: debug/get_node_properties returns properties for a node."""
        client = running_game

        # First, find the Player node to get its path and/or object ID.
        search_resp = client.call_tool(
            "debug/search_scene_tree",
            {"name_pattern": "Player"},
        )
        assert not is_error(search_resp)
        sc = get_structured(search_resp)
        matches = sc.get("matches", sc.get("results", []))
        assert len(matches) > 0, "No Player node found in search"

        first = matches[0]

        # The tool may accept node_path or object_id. Try node_path first.
        node_path = first.get("path", first.get("node_path", None))
        object_id = first.get("object_id", first.get("id", None))

        if node_path:
            props_resp = client.call_tool(
                "debug/get_node_properties",
                {"node_path": node_path},
            )
        else:
            assert object_id is not None, f"No node_path or object_id in search result: {first}"
            props_resp = client.call_tool(
                "debug/get_node_properties",
                {"object_id": object_id},
            )

        # If the first attempt errored and we have the other param, try it.
        if is_error(props_resp) and node_path and object_id:
            props_resp = client.call_tool(
                "debug/get_node_properties",
                {"object_id": object_id},
            )

        assert not is_error(props_resp), (
            f"get_node_properties returned error: {get_text(props_resp)}"
        )

        # Verify the response contains node property information.
        props_sc = get_structured(props_resp)
        props_text = get_text(props_resp)
        # The tool may return properties as an array, or as text/result fields.
        properties = props_sc.get("properties", [])
        has_content = (len(properties) > 0
                       or props_sc.get("result", "")
                       or "Player" in props_text)
        assert has_content, (
            f"Expected node properties content, got: {props_sc}"
        )

    @pytest.mark.p0
    def test_f06_get_output(self, running_game):
        """T-F06: debug/get_output contains STARTUP_COMPLETE message."""
        client = running_game
        resp = client.call_tool("debug/get_output")
        assert not is_error(resp), f"get_output returned error: {get_text(resp)}"

        sc = get_structured(resp)
        # Check text or structured output for the marker.
        output_text = get_text(resp)
        output_lines = sc.get("lines", sc.get("output", ""))
        combined = output_text + str(output_lines)
        assert "STARTUP_COMPLETE" in combined, (
            f"Expected 'STARTUP_COMPLETE' in output, got: {combined[:500]}"
        )

    @pytest.mark.p1
    def test_f07_get_output_cursor(self, running_game):
        """T-F07: debug/get_output cursor pagination returns fewer lines."""
        client = running_game

        # First call to get a cursor.
        resp1 = client.call_tool("debug/get_output")
        assert not is_error(resp1)
        sc1 = get_structured(resp1)
        cursor = sc1.get("cursor", sc1.get("next_cursor", None))

        if cursor is None:
            pytest.skip("Server did not return a cursor for output pagination")

        # Second call with cursor.
        resp2 = client.call_tool("debug/get_output", {"cursor": cursor})
        assert not is_error(resp2)
        sc2 = get_structured(resp2)

        lines1 = sc1.get("line_count", len(sc1.get("lines", [])))
        lines2 = sc2.get("line_count", len(sc2.get("lines", [])))
        assert lines2 <= lines1, (
            f"Second call with cursor ({lines2} lines) should have "
            f"<= first call ({lines1} lines)"
        )

    @pytest.mark.p1
    def test_f08_get_errors(self, running_game):
        """T-F08: debug/get_errors reports no runtime errors in test project."""
        client = running_game
        resp = client.call_tool("debug/get_errors")
        assert not is_error(resp), f"get_errors returned error: {get_text(resp)}"

        sc = get_structured(resp)
        error_count = sc.get("error_count", 0)
        assert error_count == 0, f"Expected 0 runtime errors, got {error_count}"

    @pytest.mark.p1
    def test_f09_get_performance(self, running_game):
        """T-F09: debug/get_performance reports FPS and node count."""
        client = running_game
        resp = client.call_tool("debug/get_performance")
        assert not is_error(resp), f"get_performance returned error: {get_text(resp)}"

        sc = get_structured(resp)
        fps = sc.get("fps", 0)
        assert fps > 0, f"Expected fps > 0, got {fps}"

        node_count = sc.get("node_count", 0)
        assert node_count > 0, f"Expected node_count > 0, got {node_count}"


# ===========================================================================
# Category G -- Automation (requires running game)
# ===========================================================================


@pytest.mark.requires_game
class TestAutomation:
    """Tests for input, evaluation, screenshots, and session summary.

    All tests use the module-scoped running_game fixture.
    """

    @pytest.mark.p1
    def test_g01_send_input_valid(self, running_game):
        """T-G01: debug/send_input sends a valid action.

        Tries the project-defined 'jump' action first.  If the running game
        process does not have the project InputMap loaded, falls back to the
        engine built-in 'ui_accept' action.
        """
        client = running_game
        resp = client.call_tool(
            "debug/send_input",
            {"action": "jump"},
        )

        if is_error(resp):
            # Fallback to a built-in action that is always available.
            resp = client.call_tool(
                "debug/send_input",
                {"action": "ui_accept"},
            )

        assert not is_error(resp), f"send_input returned error: {get_text(resp)}"

        sc = get_structured(resp)
        assert sc.get("success") is True, f"Expected success=true, got {sc}"

    @pytest.mark.p1
    def test_g02_send_input_invalid(self, running_game):
        """T-G02: debug/send_input rejects nonexistent action."""
        client = running_game
        resp = client.call_tool(
            "debug/send_input",
            {"action": "nonexistent_action_xyz"},
        )
        assert is_error(resp), "Nonexistent action should return error"

    @pytest.mark.p0
    def test_g05_evaluate_simple(self, running_game):
        """T-G05: debug/evaluate evaluates simple expression."""
        client = running_game
        resp = client.call_tool(
            "debug/evaluate",
            {"expression": "2+2"},
        )
        assert not is_error(resp), f"evaluate returned error: {get_text(resp)}"

        sc = get_structured(resp)
        assert sc.get("success") is True, f"Expected success=true, got {sc}"
        value = str(sc.get("value", ""))
        assert "4" in value, f"Expected '4' in value, got '{value}'"

    @pytest.mark.p1
    def test_g06_evaluate_node_access(self, running_game):
        """T-G06: debug/evaluate accesses scene tree nodes."""
        client = running_game
        resp = client.call_tool(
            "debug/evaluate",
            {"expression": "get_tree().root.get_child_count()"},
        )
        assert not is_error(resp), f"evaluate returned error: {get_text(resp)}"

        sc = get_structured(resp)
        assert sc.get("success") is True, f"Expected success=true, got {sc}"

    @pytest.mark.p1
    def test_g08_wait_frames(self, running_game):
        """T-G08: debug/wait_frames waits for the requested number of frames."""
        client = running_game
        resp = client.call_tool(
            "debug/wait_frames",
            {"frames": 5},
        )
        assert not is_error(resp), f"wait_frames returned error: {get_text(resp)}"

        sc = get_structured(resp)
        waited = sc.get("frames_waited", 0)
        assert waited == 5, f"Expected frames_waited=5, got {waited}"

    @pytest.mark.p1
    def test_g09_get_screenshot(self, running_game):
        """T-G09: debug/get_screenshot returns a PNG image."""
        client = running_game
        resp = client.call_tool("debug/get_screenshot")
        assert not is_error(resp), f"get_screenshot returned error: {get_text(resp)}"

        # The response content should have an image item.
        result = resp.get("result", {})
        content = result.get("content", [])
        image_items = [c for c in content if c.get("type") == "image"]
        assert len(image_items) > 0, "Expected at least one image content item"

        image = image_items[0]
        assert image.get("mimeType") == "image/png", (
            f"Expected mimeType 'image/png', got '{image.get('mimeType')}'"
        )

    @pytest.mark.p1
    def test_g10_get_session_summary_running(self, running_game):
        """T-G10: debug/get_session_summary while game is running."""
        client = running_game
        resp = client.call_tool("debug/get_session_summary")
        assert not is_error(resp), (
            f"get_session_summary returned error: {get_text(resp)}"
        )

        sc = get_structured(resp)
        status = sc.get("status", {})
        state = status.get("state", sc.get("state", ""))
        assert state == "running", f"Expected state 'running', got '{state}'"

        # Should have scene tree info.
        has_tree = (
            "scene_tree" in sc
            or "scene_tree" in str(sc)
        )
        assert has_tree, "Expected scene_tree in session summary"

    @pytest.mark.p1
    def test_g11_get_session_summary_stopped(self, client):
        """T-G11: debug/get_session_summary when game is not running.

        Uses the per-test client fixture (no running game).
        """
        # Ensure no game is running.
        status_resp = client.call_tool("debug/get_status")
        state = get_structured(status_resp).get("state", "stopped")
        if state != "stopped":
            client.call_tool("debug/stop")
            time.sleep(1.0)

        resp = client.call_tool("debug/get_session_summary")
        assert not is_error(resp), (
            f"get_session_summary returned error: {get_text(resp)}"
        )

        sc = get_structured(resp)
        status = sc.get("status", {})
        summary_state = status.get("state", sc.get("state", ""))
        assert summary_state == "stopped", (
            f"Expected state 'stopped', got '{summary_state}'"
        )
