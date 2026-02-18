"""Pytest configuration, markers, and shared fixtures for MCP integration tests.

Prerequisites:
    1. Godot editor built with MCP enabled.
    2. Editor launched with the test project:
       ./bin/godot.linuxbsd.editor.x86_64 --editor --path modules/mcp_server/tests/mcp_test_project/
    3. MCP server listening on 127.0.0.1:6009.
"""

import time

import pytest

from mcp_test_client import MCPClient


# ---------------------------------------------------------------------------
# Pytest markers
# ---------------------------------------------------------------------------

def pytest_configure(config):
    config.addinivalue_line("markers", "p0: must pass before release")
    config.addinivalue_line("markers", "p1: must pass for production")
    config.addinivalue_line("markers", "p2: quality/robustness")
    config.addinivalue_line("markers", "requires_game: needs running game instance")
    config.addinivalue_line("markers", "security: security-focused test")
    config.addinivalue_line("markers", "sse: requires SSE support")
    config.addinivalue_line("markers", "slow: test takes >5 seconds")


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session")
def mcp_server_available():
    """Check that the MCP server is reachable before running any tests."""
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2.0)
    try:
        s.connect(("127.0.0.1", 6009))
        s.close()
    except (ConnectionRefusedError, OSError):
        pytest.skip("MCP server not available on 127.0.0.1:6009")


@pytest.fixture
def client(mcp_server_available):
    """A fresh MCP client with completed handshake, torn down after each test."""
    c = MCPClient()
    c.full_handshake()
    yield c
    try:
        c.delete_session()
    except Exception:
        pass
    c.disconnect()


@pytest.fixture
def raw_client(mcp_server_available):
    """A raw MCP client with no handshake -- for protocol-level testing."""
    c = MCPClient()
    c.connect()
    yield c
    c.disconnect()


@pytest.fixture(scope="module")
def module_client(mcp_server_available):
    """A shared MCP client that lives for an entire test module."""
    c = MCPClient()
    c.full_handshake()
    yield c
    try:
        c.delete_session()
    except Exception:
        pass
    c.disconnect()


@pytest.fixture(scope="module")
def running_game(module_client):
    """Ensure the main project is running for the test module.
    Starts the game if needed and stops it after the module."""
    # Check current state first.
    result = module_client.call_tool("runtime/get_status")
    state = result.get("result", {}).get("structuredContent", {}).get("state")
    if state != "running":
        module_client.call_tool("runtime/run_project")
        for _ in range(100):  # Poll for up to 10 seconds.
            result = module_client.call_tool("runtime/get_status")
            state = result.get("result", {}).get("structuredContent", {}).get("state")
            if state == "running":
                break
            time.sleep(0.1)
        else:
            pytest.fail("Game did not reach running state within 10 seconds")
    yield module_client
    try:
        module_client.call_tool("runtime/stop")
        time.sleep(0.5)
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def get_tool_result(response: dict) -> dict:
    """Extract the tool result from a JSON-RPC response."""
    return response.get("result", {})


def get_structured(response: dict) -> dict:
    """Extract structuredContent from a tool response."""
    return response.get("result", {}).get("structuredContent", {})


def get_text(response: dict) -> str:
    """Extract the first text content item from a tool response."""
    content = response.get("result", {}).get("content", [])
    for item in content:
        if item.get("type") == "text":
            return item.get("text", "")
    return ""


def is_error(response: dict) -> bool:
    """Check if a tool response indicates an error.

    Detects both MCP-level errors (result.isError) and JSON-RPC level errors
    (top-level 'error' key in the response).
    """
    # JSON-RPC error: response has "error" key instead of / in addition to "result".
    if "error" in response:
        return True
    return response.get("result", {}).get("isError", False)
