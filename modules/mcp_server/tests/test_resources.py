"""MCP resource tests."""

import json

import pytest

from conftest import get_structured, get_text, is_error


# ---------------------------------------------------------------------------
# R-01: resources/list  (P1)
# ---------------------------------------------------------------------------

@pytest.mark.p1
def test_r01_resources_list_with_and_without_game(client):
    """resources/list reflects game-dependent resources only when game runs."""

    # -- Game NOT running --
    resp = client.list_resources()
    uris = [r["uri"] for r in resp["result"]["resources"]]

    # Always-available resources must be present.
    assert "godot://project/info" in uris
    assert "godot://project/file-tree" in uris
    assert "godot://project/input-map" in uris
    assert "godot://project/settings" in uris
    assert "godot://game/status" in uris

    # Game-only resources must be absent.
    assert "godot://game/scene-tree" not in uris
    assert "godot://game/output" not in uris
    assert "godot://game/errors" not in uris

    # -- Start game --
    client.start_game_and_wait()

    try:
        resp = client.list_resources()
        uris = [r["uri"] for r in resp["result"]["resources"]]

        # Game-only resources must now be present.
        assert "godot://game/scene-tree" in uris
        assert "godot://game/output" in uris
        assert "godot://game/errors" in uris
    finally:
        client.stop_game()


# ---------------------------------------------------------------------------
# R-02: resources/read  godot://project/info  (P1)
# ---------------------------------------------------------------------------

@pytest.mark.p1
def test_r02_read_project_info(client):
    """Reading project/info returns JSON with the project name."""

    resp = client.read_resource("godot://project/info")
    contents = resp["result"]["contents"]
    assert len(contents) > 0

    entry = contents[0]
    assert entry["mimeType"] == "application/json"

    data = json.loads(entry["text"])
    assert data["project_name"] == "MCP Test Project"


# ---------------------------------------------------------------------------
# R-03: resources/read  godot://project/file-tree  (P1)
# ---------------------------------------------------------------------------

@pytest.mark.p1
def test_r03_read_project_file_tree(client):
    """Reading project/file-tree returns a non-empty tree with expected dirs."""

    resp = client.read_resource("godot://project/file-tree")
    contents = resp["result"]["contents"]
    data = json.loads(contents[0]["text"])

    assert data["total_files"] > 0
    # directories may be a flat list of strings or a list of dicts with 'path'.
    dirs = data["directories"]
    if dirs and isinstance(dirs[0], dict):
        dir_paths = [d.get("path", "") for d in dirs]
    else:
        dir_paths = dirs
    assert "res://scripts" in dir_paths, (
        f"Expected 'res://scripts' in directories, got: {dirs}"
    )


# ---------------------------------------------------------------------------
# R-04: resources/read  godot://file/{path}  (P1)
# ---------------------------------------------------------------------------

@pytest.mark.p1
def test_r04_read_file_template(client):
    """Reading a project file via the file template returns its content."""

    resp = client.read_resource("godot://file/scripts/valid.gd")
    # Resource responses use "contents" (not "content" like tool responses),
    # so extract text directly from the contents array.
    contents = resp.get("result", {}).get("contents", [])
    text = ""
    for entry in contents:
        text += entry.get("text", "")
    # If resource URI format differs, try alternate forms.
    if not text:
        resp = client.read_resource("godot://file/res://scripts/valid.gd")
        contents = resp.get("result", {}).get("contents", [])
        for entry in contents:
            text += entry.get("text", "")
    assert "extends Node" in text, (
        f"Expected 'extends Node' in file content, got: {text[:500]}"
    )


# ---------------------------------------------------------------------------
# R-05: resources/read  godot://file/ with path traversal  (P1)
# ---------------------------------------------------------------------------

@pytest.mark.p1
def test_r05_path_traversal_rejected(client):
    """Path traversal attempts must be rejected with an error."""

    resp = client.read_resource("godot://file/../../../etc/passwd")
    assert "error" in resp


# ---------------------------------------------------------------------------
# R-06: resources/read  nonexistent URI  (P1)
# ---------------------------------------------------------------------------

@pytest.mark.p1
def test_r06_read_nonexistent_uri(client):
    """Reading a URI that does not exist must return an error."""

    resp = client.read_resource("godot://nonexistent/thing")
    assert is_error(resp)


# ---------------------------------------------------------------------------
# R-07: resources/templates/list  (P1)
# ---------------------------------------------------------------------------

@pytest.mark.p1
def test_r07_resource_templates_list(client):
    """resources/templates/list includes file and node-properties templates."""

    resp = client.list_resource_templates()
    templates = resp["result"]["resourceTemplates"]
    uri_templates = [t["uriTemplate"] for t in templates]

    assert "godot://file/{path}" in uri_templates
    assert any("node" in u and "properties" in u for u in uri_templates)


# ---------------------------------------------------------------------------
# R-08: resources/subscribe  (P1)
# ---------------------------------------------------------------------------

@pytest.mark.p1
def test_r08_subscribe_accepts_valid_uri(client):
    """resources/subscribe for a known URI returns 202 (accepted)."""
    status = client.subscribe_resource("godot://project/info")
    assert status == 202, f"Expected 202 for subscribe, got {status}"


@pytest.mark.p1
def test_r09_subscribe_unknown_uri_accepted(client):
    """resources/subscribe for an unknown URI still returns 202.

    The MCP spec says subscribe is a notification (no id), so the server
    should accept it silently even for unknown URIs.
    """
    status = client.subscribe_resource("godot://nonexistent/resource")
    assert status == 202, f"Expected 202 for subscribe to unknown URI, got {status}"


# ---------------------------------------------------------------------------
# R-10: resources/read game resources  (P1)
# ---------------------------------------------------------------------------

@pytest.mark.p1
def test_r10_read_game_status(client):
    """Reading godot://game/status returns state information."""
    resp = client.read_resource("godot://game/status")
    contents = resp["result"]["contents"]
    assert len(contents) > 0
    data = json.loads(contents[0]["text"])
    assert "state" in data


@pytest.mark.p1
def test_r11_read_project_settings(client):
    """Reading godot://project/settings returns project configuration."""
    resp = client.read_resource("godot://project/settings")
    contents = resp["result"]["contents"]
    assert len(contents) > 0
    text = contents[0]["text"]
    # Settings may be JSON or plain text; either way it should be non-empty
    # and contain the project name.
    assert len(text) > 0
    assert "MCP Test Project" in text or "mcp_test" in text.lower() or "project" in text.lower(), (
        f"Expected project name in settings, got: {text[:500]}"
    )
