"""MCP protocol conformance tests for the Godot MCP server.

Tests cover handshake, session lifecycle, session ID enforcement,
content-type validation, CORS, host validation, path routing,
malformed JSON handling, ping, and notification responses.
"""

import json
import re

import pytest

from mcp_test_client import MCPClient
from conftest import get_tool_result, get_structured, get_text, is_error


# ---------------------------------------------------------------------------
# TC-01: Initialize Handshake
# ---------------------------------------------------------------------------

class TestInitializeHandshake:
    """TC-01 -- Verify the MCP initialize handshake response."""

    @pytest.mark.p0
    def test_initialize_handshake(self, raw_client: MCPClient):
        """TC-01: POST initialize returns correct protocol version,
        capabilities, server info, and a valid session ID."""
        result = raw_client.initialize()

        # Session ID: 64-char lowercase hex.
        assert raw_client.session_id is not None
        assert re.fullmatch(r"[0-9a-f]{64}", raw_client.session_id), (
            f"Session ID is not 64-char lowercase hex: {raw_client.session_id}"
        )

        # Protocol version.
        assert result["result"]["protocolVersion"] == "2025-06-18"

        # Capabilities.
        caps = result["result"]["capabilities"]
        assert caps["tools"]["listChanged"] is True
        assert "logging" in caps

        # Server info.
        info = result["result"]["serverInfo"]
        assert info["name"] == "godot-mcp"
        # Godot uses "4.6.stable" format, not strict semver "4.6.0".
        assert re.fullmatch(r"\d+\.\d+\.\w+", info["version"]), (
            f"Version does not match expected format: {info['version']}"
        )

        # Instructions.
        instructions = result["result"].get("instructions")
        assert isinstance(instructions, str) and len(instructions) > 0


# ---------------------------------------------------------------------------
# TC-02: Full Session Lifecycle
# ---------------------------------------------------------------------------

class TestSessionLifecycle:
    """TC-02 -- Exercise the full session lifecycle: init -> work -> delete."""

    @pytest.mark.p0
    def test_full_session_lifecycle(self, raw_client: MCPClient):
        """TC-02: Initialize, notify, list tools, call a tool, delete, then
        confirm the deleted session is rejected."""
        # Step 1 -- initialize.
        init_result = raw_client.initialize()
        assert "result" in init_result

        # Step 2 -- notifications/initialized -> 202.
        status = raw_client.send_initialized()
        assert status == 202

        # Step 3 -- tools/list -> non-empty tool list.
        tools_resp = raw_client.list_tools()
        tools = tools_resp["result"]["tools"]
        assert isinstance(tools, list) and len(tools) > 0

        # Step 4 -- tools/call project/get_overview -> has text content.
        call_resp = raw_client.call_tool("project/get_overview")
        text = get_text(call_resp)
        assert len(text) > 0 or get_structured(call_resp)

        # Step 5 -- DELETE session -> 200 or 204.
        del_status = raw_client.delete_session()
        assert del_status in (200, 204)

        # Step 6 -- ping with deleted session -> 404.
        raw_client.reconnect()
        status, _, body = raw_client.send_jsonrpc("ping")
        assert status == 404


# ---------------------------------------------------------------------------
# TC-03: Session ID Enforcement
# ---------------------------------------------------------------------------

class TestSessionIdEnforcement:
    """TC-03 -- Verify that the server enforces session ID requirements."""

    @pytest.mark.p0
    def test_03a_tools_list_without_session_id(self, raw_client: MCPClient):
        """TC-03a: tools/list without Mcp-Session-Id header -> 400 or 401.
        (401 if auth token is required and checked before session ID.)"""
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/list",
        })
        # Build a raw HTTP request with auth but no Mcp-Session-Id header.
        auth_hdr = ""
        if raw_client.auth_token:
            auth_hdr = f"Authorization: Bearer {raw_client.auth_token}\r\n"
        raw = (
            "POST /mcp HTTP/1.1\r\n"
            f"Host: {raw_client.host}:{raw_client.port}\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            f"{auth_hdr}"
            "Connection: keep-alive\r\n"
            "\r\n"
            f"{body}"
        )
        status, _, _ = raw_client.send_raw(raw.encode("utf-8"))
        assert status == 400

    @pytest.mark.p0
    def test_03b_tools_list_with_invalid_session_id(self, raw_client: MCPClient):
        """TC-03b: tools/list with a fabricated Mcp-Session-Id -> 404."""
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/list",
        })
        auth_hdr = ""
        if raw_client.auth_token:
            auth_hdr = f"Authorization: Bearer {raw_client.auth_token}\r\n"
        raw = (
            "POST /mcp HTTP/1.1\r\n"
            f"Host: {raw_client.host}:{raw_client.port}\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            f"{auth_hdr}"
            "Mcp-Session-Id: " + "a" * 64 + "\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            f"{body}"
        )
        status, _, _ = raw_client.send_raw(raw.encode("utf-8"))
        assert status == 404

    @pytest.mark.p0
    def test_03c_initialize_without_session_id(self, raw_client: MCPClient):
        """TC-03c: initialize without Mcp-Session-Id -> 200 (not required).
        Auth token IS still required."""
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-06-18",
                "clientInfo": {"name": "test", "version": "1.0.0"},
                "capabilities": {},
            },
        })
        auth_hdr = ""
        if raw_client.auth_token:
            auth_hdr = f"Authorization: Bearer {raw_client.auth_token}\r\n"
        raw = (
            "POST /mcp HTTP/1.1\r\n"
            f"Host: {raw_client.host}:{raw_client.port}\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            f"{auth_hdr}"
            "Connection: keep-alive\r\n"
            "\r\n"
            f"{body}"
        )
        status, headers, resp_body = raw_client.send_raw(raw.encode("utf-8"))
        assert status == 200
        # Capture session ID for cleanup.
        sid = headers.get("mcp-session-id")
        if sid:
            raw_client.session_id = sid

    @pytest.mark.p0
    def test_03d_delete_with_wrong_session_id(self, raw_client: MCPClient):
        """TC-03d: DELETE with an incorrect session ID -> 404."""
        # First, establish a real session so we have a valid connection.
        raw_client.initialize()
        real_sid = raw_client.session_id

        # Now attempt DELETE with a different (wrong) session ID.
        raw_client.reconnect()
        auth_hdr = ""
        if raw_client.auth_token:
            auth_hdr = f"Authorization: Bearer {raw_client.auth_token}\r\n"
        raw = (
            "DELETE /mcp HTTP/1.1\r\n"
            f"Host: {raw_client.host}:{raw_client.port}\r\n"
            f"{auth_hdr}"
            "Mcp-Session-Id: " + "b" * 64 + "\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
        )
        status, _, _ = raw_client.send_raw(raw.encode("utf-8"))
        assert status == 404

        # Cleanup: restore real session ID so fixture teardown can disconnect.
        raw_client.session_id = real_sid

    @pytest.mark.p0
    def test_03e_tools_list_before_initialized(self, raw_client: MCPClient):
        """TC-03e: tools/list after initialize but before notifications/initialized -> 400."""
        raw_client.initialize()
        raw_client.reconnect()
        status, _, body = raw_client.send_jsonrpc("tools/list")
        assert status == 400

    @pytest.mark.p0
    def test_03f_ping_before_initialized(self, raw_client: MCPClient):
        """TC-03f: ping after initialize but before notifications/initialized -> 200."""
        raw_client.initialize()
        raw_client.reconnect()
        status, _, body = raw_client.send_jsonrpc("ping")
        assert status == 200


# ---------------------------------------------------------------------------
# TC-04: Content-Type Validation
# ---------------------------------------------------------------------------

class TestContentTypeValidation:
    """TC-04 -- Verify the server rejects invalid Content-Type headers."""

    @pytest.mark.p0
    def test_04a_text_plain_rejected(self, client: MCPClient):
        """TC-04a: Content-Type text/plain -> 400."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        client.reconnect()
        status, _, _ = client._send_http(
            "POST", body, content_type="text/plain",
        )
        assert status == 400

    @pytest.mark.p0
    def test_04b_omitted_content_type_rejected(self, client: MCPClient):
        """TC-04b: Omitted Content-Type header -> 400."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        auth_hdr = ""
        if client.auth_token:
            auth_hdr = f"Authorization: Bearer {client.auth_token}\r\n"
        raw = (
            "POST /mcp HTTP/1.1\r\n"
            f"Host: {client.host}:{client.port}\r\n"
            f"{auth_hdr}"
            f"Mcp-Session-Id: {client.session_id}\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            f"{body}"
        )
        client.reconnect()
        status, _, _ = client.send_raw(raw.encode("utf-8"))
        assert status == 400

    @pytest.mark.p0
    def test_04c_json_charset_utf8_accepted(self, client: MCPClient):
        """TC-04c: Content-Type application/json; charset=utf-8 -> 200."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        client.reconnect()
        status, _, _ = client._send_http(
            "POST", body, content_type="application/json; charset=utf-8",
        )
        assert status == 200

    @pytest.mark.p0
    def test_04d_application_json_accepted(self, client: MCPClient):
        """TC-04d: Content-Type application/json -> 200."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        client.reconnect()
        status, _, _ = client._send_http(
            "POST", body, content_type="application/json",
        )
        assert status == 200


# ---------------------------------------------------------------------------
# TC-05: CORS
# ---------------------------------------------------------------------------

class TestCORS:
    """TC-05 -- Verify CORS headers and origin validation."""

    @pytest.mark.p1
    def test_05a_options_preflight(self, client: MCPClient):
        """TC-05a: OPTIONS preflight -> 204 with CORS headers."""
        client.reconnect()
        status, headers, _ = client._send_http(
            "OPTIONS", headers={"Origin": "http://localhost:3000"},
        )
        assert status == 204
        assert "access-control-allow-methods" in headers
        assert "access-control-allow-headers" in headers

    @pytest.mark.p1
    def test_05b_evil_origin_rejected(self, client: MCPClient):
        """TC-05b: Origin http://evil.com -> 403."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        client.reconnect()
        status, _, _ = client._send_http(
            "POST", body, headers={"Origin": "http://evil.com"},
        )
        assert status == 403

    @pytest.mark.p2
    def test_05c_vscode_origin_accepted(self, client: MCPClient):
        """TC-05c: Origin vscode-file://vscode-app -> 200 (if supported) or 403.

        The server may only allow localhost/127.0.0.1 origins and reject
        non-local schemes like vscode-file://.  Both 200 and 403 are acceptable.
        """
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        origin = "vscode-file://vscode-app"
        client.reconnect()
        status, headers, _ = client._send_http(
            "POST", body, headers={"Origin": origin},
        )
        assert status in (200, 403), f"Expected 200 or 403, got {status}"
        if status == 200:
            assert headers.get("access-control-allow-origin") == origin

    @pytest.mark.p1
    def test_05d_localhost_origin_accepted(self, client: MCPClient):
        """TC-05d: Origin http://localhost:3000 -> 200, origin echoed."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        origin = "http://localhost:3000"
        client.reconnect()
        status, headers, _ = client._send_http(
            "POST", body, headers={"Origin": origin},
        )
        assert status == 200
        assert headers.get("access-control-allow-origin") == origin

    @pytest.mark.p1
    def test_05e_loopback_origin_accepted(self, client: MCPClient):
        """TC-05e: Origin http://127.0.0.1:6009 -> 200, origin echoed."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        origin = "http://127.0.0.1:6009"
        client.reconnect()
        status, headers, _ = client._send_http(
            "POST", body, headers={"Origin": origin},
        )
        assert status == 200
        assert headers.get("access-control-allow-origin") == origin

    @pytest.mark.p1
    def test_05f_no_origin_no_acao(self, client: MCPClient):
        """TC-05f: No Origin header -> 200, no Access-Control-Allow-Origin."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        client.reconnect()
        status, headers, _ = client._send_http("POST", body)
        assert status == 200
        assert "access-control-allow-origin" not in headers

    @pytest.mark.p1
    def test_05g_acao_never_wildcard(self, client: MCPClient):
        """TC-05g: Access-Control-Allow-Origin never equals '*'."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        for origin in [
            "http://localhost:3000",
            "http://127.0.0.1:6009",
        ]:
            client.reconnect()
            status, headers, _ = client._send_http(
                "POST", body, headers={"Origin": origin},
            )
            acao = headers.get("access-control-allow-origin", "")
            assert acao != "*", f"ACAO is wildcard for origin {origin}"

    @pytest.mark.p1
    def test_05h_vary_origin_present(self, client: MCPClient):
        """TC-05h: Vary header includes Origin."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        client.reconnect()
        status, headers, _ = client._send_http(
            "POST", body, headers={"Origin": "http://localhost:3000"},
        )
        vary = headers.get("vary", "")
        assert "origin" in vary.lower(), f"Vary header missing Origin: {vary}"

    @pytest.mark.p1
    def test_05i_expose_headers_includes_session_id(self, client: MCPClient):
        """TC-05i: Access-Control-Expose-Headers includes Mcp-Session-Id."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        client.reconnect()
        status, headers, _ = client._send_http(
            "POST", body, headers={"Origin": "http://localhost:3000"},
        )
        expose = headers.get("access-control-expose-headers", "").lower()
        assert "mcp-session-id" in expose, (
            f"Expose-Headers missing Mcp-Session-Id: {expose}"
        )


# ---------------------------------------------------------------------------
# TC-06: Host Validation
# ---------------------------------------------------------------------------

class TestHostValidation:
    """TC-06 -- Verify the server validates the Host header."""

    @pytest.mark.p1
    def test_06a_host_127_0_0_1(self, client: MCPClient):
        """TC-06a: Host: 127.0.0.1:6009 -> 200."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        client.reconnect()
        status, _, _ = client._send_http(
            "POST", body, headers={"Host": "127.0.0.1:6009"},
        )
        assert status == 200

    @pytest.mark.p1
    def test_06b_host_localhost(self, client: MCPClient):
        """TC-06b: Host: localhost:6009 -> 200."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        client.reconnect()
        status, _, _ = client._send_http(
            "POST", body, headers={"Host": "localhost:6009"},
        )
        assert status == 200

    @pytest.mark.p1
    def test_06c_host_evil_rejected(self, client: MCPClient):
        """TC-06c: Host: evil.com:6009 -> 403."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        client.reconnect()
        status, _, _ = client._send_http(
            "POST", body, headers={"Host": "evil.com:6009"},
        )
        assert status == 403

    @pytest.mark.p2
    def test_06d_host_ipv6_loopback(self, client: MCPClient):
        """TC-06d: Host: [::1]:6009 -> 200 or 403.

        The server may only recognise IPv4 loopback (127.0.0.1) and localhost.
        Both 200 and 403 are acceptable.
        """
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        client.reconnect()
        status, _, _ = client._send_http(
            "POST", body, headers={"Host": "[::1]:6009"},
        )
        assert status in (200, 403), f"Expected 200 or 403, got {status}"

    @pytest.mark.p1
    def test_06e_no_host_header(self, client: MCPClient):
        """TC-06e: No Host header -> 200 or 400.

        HTTP/1.1 technically requires a Host header, so 400 (Bad Request) is
        also a valid response when the header is omitted.
        """
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        auth_hdr = ""
        if client.auth_token:
            auth_hdr = f"Authorization: Bearer {client.auth_token}\r\n"
        # Build a raw request without Host header.
        raw = (
            "POST /mcp HTTP/1.1\r\n"
            "Content-Type: application/json\r\n"
            f"{auth_hdr}"
            f"Content-Length: {len(body)}\r\n"
            f"Mcp-Session-Id: {client.session_id}\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            f"{body}"
        )
        client.reconnect()
        status, _, _ = client.send_raw(raw.encode("utf-8"))
        assert status in (200, 400), f"Expected 200 or 400, got {status}"


# ---------------------------------------------------------------------------
# TC-07: Path Validation
# ---------------------------------------------------------------------------

class TestPathValidation:
    """TC-07 -- Verify the server only responds on /mcp."""

    @pytest.mark.p0
    def test_07a_correct_path(self, client: MCPClient):
        """TC-07a: POST /mcp -> 200."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        client.reconnect()
        status, _, _ = client._send_http("POST", body, path="/mcp")
        assert status == 200

    @pytest.mark.p0
    def test_07b_root_path_rejected(self, client: MCPClient):
        """TC-07b: POST / -> 404."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        client.reconnect()
        status, _, _ = client._send_http("POST", body, path="/")
        assert status == 404

    @pytest.mark.p0
    def test_07c_uppercase_mcp_rejected(self, client: MCPClient):
        """TC-07c: POST /MCP -> 404."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        client.reconnect()
        status, _, _ = client._send_http("POST", body, path="/MCP")
        assert status == 404

    @pytest.mark.p0
    def test_07d_extra_path_rejected(self, client: MCPClient):
        """TC-07d: POST /mcp/extra -> 404."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        client.reconnect()
        status, _, _ = client._send_http("POST", body, path="/mcp/extra")
        assert status == 404

    @pytest.mark.p0
    def test_07e_double_slash_rejected(self, client: MCPClient):
        """TC-07e: POST //mcp -> 404."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        client.reconnect()
        status, _, _ = client._send_http("POST", body, path="//mcp")
        assert status == 404


# ---------------------------------------------------------------------------
# TC-08: Malformed JSON
# ---------------------------------------------------------------------------

class TestMalformedJSON:
    """TC-08 -- Verify the server handles malformed JSON gracefully."""

    @pytest.mark.p0
    def test_08a_invalid_json(self, client: MCPClient):
        """TC-08a: {invalid -> 400 with JSON parse error (-32700)."""
        client.reconnect()
        status, _, resp_body = client._send_http("POST", "{invalid")
        assert status == 400
        if resp_body:
            parsed = json.loads(resp_body)
            assert parsed.get("error", {}).get("code") == -32700

    @pytest.mark.p0
    def test_08b_empty_body(self, client: MCPClient):
        """TC-08b: Empty body -> 400."""
        client.reconnect()
        status, _, _ = client._send_http("POST", "")
        assert status == 400

    @pytest.mark.p0
    def test_08c_null_bytes_in_body(self, client: MCPClient):
        """TC-08c: Null bytes in body -> 400, no crash."""
        payload = '{"jsonrpc":"2.0",\x00"id":1,"method":"ping"}'
        client.reconnect()
        status, _, _ = client._send_http("POST", payload)
        assert status == 400

        # Verify the server is still alive by sending a valid ping.
        client.reconnect()
        status2, _, body2 = client._send_http(
            "POST",
            json.dumps({"jsonrpc": "2.0", "id": 99, "method": "ping"}),
        )
        assert status2 == 200

    @pytest.mark.p0
    def test_08d_missing_method_field(self, client: MCPClient):
        """TC-08d: Valid JSON but missing 'method' field -> error response.
        Server may return HTTP 400 or HTTP 200 with a JSON-RPC error body."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1})
        client.reconnect()
        status, _, resp_body = client._send_http("POST", body)
        if status == 400:
            # Server rejected at HTTP level.
            if resp_body:
                parsed = json.loads(resp_body)
                assert parsed.get("error", {}).get("code") == -32600
        else:
            # Server returned 200 with a JSON-RPC error in body.
            assert status == 200
            parsed = json.loads(resp_body)
            assert "error" in parsed, f"Expected error in response: {parsed}"
            assert parsed["error"]["code"] in (-32600, -32601, -32602)


# ---------------------------------------------------------------------------
# TC-09: Ping
# ---------------------------------------------------------------------------

class TestPing:
    """TC-09 -- Verify the ping method."""

    @pytest.mark.p0
    def test_ping_with_string_id(self, client: MCPClient):
        """TC-09: Ping with string ID 'p1' -> result:{}, id:'p1'."""
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": "p1",
            "method": "ping",
        })
        client.reconnect()
        status, _, resp_body = client._send_http("POST", body)
        assert status == 200
        parsed = json.loads(resp_body)
        assert parsed["id"] == "p1"
        assert parsed["result"] == {}


# ---------------------------------------------------------------------------
# TC-10: Notification Response
# ---------------------------------------------------------------------------

class TestNotificationResponse:
    """TC-10 -- Verify notifications get 202 with empty body."""

    @pytest.mark.p0
    def test_initialized_notification_response(self, raw_client: MCPClient):
        """TC-10: notifications/initialized -> 202, empty or zero-length body."""
        raw_client.initialize()
        raw_client.reconnect()
        body = json.dumps({
            "jsonrpc": "2.0",
            "method": "notifications/initialized",
        })
        status, headers, resp_body = raw_client._send_http("POST", body)
        assert status == 202
        content_length = int(headers.get("content-length", len(resp_body)))
        assert content_length == 0 or resp_body.strip() == ""
