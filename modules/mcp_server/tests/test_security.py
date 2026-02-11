"""Security tests for the Godot MCP server.

Covers path traversal, CORS, DNS rebinding, session prediction, and
expression sandboxing.  Every test is marked @pytest.mark.security and
@pytest.mark.p0.
"""

import json
import time

import pytest

from conftest import get_structured, get_text, is_error
from mcp_test_client import MCPClient


# ---------------------------------------------------------------------------
# SEC-01: Path Traversal Variants
# ---------------------------------------------------------------------------

# Paths the server already rejects (validated by validate_path).
TRAVERSAL_PATHS_BASIC = [
    pytest.param("res://../../etc/passwd", id="basic-traversal"),
    pytest.param("res://scripts/../../../etc/passwd", id="multi-level-traversal"),
    pytest.param("res://scripts/../../.ssh/id_rsa", id="ssh-key-theft"),
    pytest.param("user://../../.ssh/id_rsa", id="user-traversal"),
    pytest.param("/etc/passwd", id="absolute-path-no-scheme"),
    pytest.param("file:///etc/passwd", id="file-scheme"),
    pytest.param("res://scripts/....//....//etc/passwd", id="double-dot-variant"),
]

# Exotic traversal variants that require additional server hardening.
TRAVERSAL_PATHS_EXOTIC = [
    pytest.param("res://scripts/\x00evil.gd", id="null-byte-injection"),
    pytest.param("res://.godot/imported/texture.png", id="internal-directory"),
    pytest.param("res://scripts/%2e%2e/%2e%2e/etc/passwd", id="url-encoded-traversal"),
]


@pytest.mark.security
@pytest.mark.p0
class TestPathTraversal:
    """SEC-01: Basic path traversal variants must be rejected."""

    @pytest.mark.parametrize("path", TRAVERSAL_PATHS_BASIC)
    def test_resource_read_rejects_traversal(self, client, path):
        """godot://file/ resource reads must reject traversal paths.

        Paths that begin with res:// are converted to godot://file/ URIs by
        stripping the res:// prefix.  Other schemes are passed through as-is
        to verify the server rejects them.
        """
        if path.startswith("res://"):
            relative = path[len("res://"):]
            uri = f"godot://file/{relative}"
        else:
            # For absolute paths and file:// URIs, pass directly.
            uri = f"godot://file/{path}"

        response = client.read_resource(uri)
        # A traversal resource read should produce a JSON-RPC error or an
        # MCP error (isError flag on the result).
        has_rpc_error = "error" in response
        has_mcp_error = response.get("result", {}).get("isError", False)
        assert has_rpc_error or has_mcp_error, (
            f"godot://file/ read should reject URI {uri!r} but did not "
            f"return an error: {response}"
        )


@pytest.mark.security
@pytest.mark.p2
class TestPathTraversalExotic:
    """SEC-01b: Exotic path traversal variants (null bytes, URL encoding,
    internal directories) require additional server-side hardening."""

    @pytest.mark.parametrize("path", TRAVERSAL_PATHS_EXOTIC)
    def test_resource_read_rejects_traversal(self, client, path):
        """godot://file/ resource reads should reject exotic traversal paths."""
        if path.startswith("res://"):
            relative = path[len("res://"):]
            uri = f"godot://file/{relative}"
        else:
            uri = f"godot://file/{path}"

        response = client.read_resource(uri)
        has_rpc_error = "error" in response
        has_mcp_error = response.get("result", {}).get("isError", False)
        assert has_rpc_error or has_mcp_error, (
            f"godot://file/ read should reject URI {uri!r} but did not "
            f"return an error: {response}"
        )


# ---------------------------------------------------------------------------
# SEC-01c: Auth Token Rejection
# ---------------------------------------------------------------------------

@pytest.mark.security
@pytest.mark.p0
class TestAuthTokenRejection:
    """SEC-01c: Requests with wrong or missing auth tokens must be rejected."""

    def test_missing_auth_token_rejected(self, mcp_server_available):
        """Request with no Authorization header -> 401."""
        c = MCPClient(auth_token="")
        c.connect()
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-06-18",
                "clientInfo": {"name": "no-auth-test", "version": "1.0.0"},
                "capabilities": {},
            },
        })
        # Send raw HTTP without auth header.
        raw = (
            "POST /mcp HTTP/1.1\r\n"
            f"Host: 127.0.0.1:6009\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            f"{body}"
        )
        status, _, _ = c.send_raw(raw.encode("utf-8"))
        c.disconnect()
        assert status == 401, f"Expected 401 for missing auth, got {status}"

    def test_wrong_auth_token_rejected(self, mcp_server_available):
        """Request with an incorrect bearer token -> 401."""
        c = MCPClient(auth_token="wrong-token-1234567890abcdef")
        c.connect()
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-06-18",
                "clientInfo": {"name": "bad-auth-test", "version": "1.0.0"},
                "capabilities": {},
            },
        })
        status, _, _ = c._send_http("POST", body)
        c.disconnect()
        assert status == 401, f"Expected 401 for wrong auth token, got {status}"

    def test_malformed_auth_header_rejected(self, mcp_server_available):
        """Request with malformed Authorization header (no 'Bearer ' prefix) -> 401."""
        c = MCPClient()
        c.connect()
        real_token = c.auth_token or "dummy"
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-06-18",
                "clientInfo": {"name": "bad-auth-test", "version": "1.0.0"},
                "capabilities": {},
            },
        })
        raw = (
            "POST /mcp HTTP/1.1\r\n"
            f"Host: 127.0.0.1:6009\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            f"Authorization: Basic {real_token}\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            f"{body}"
        )
        status, _, _ = c.send_raw(raw.encode("utf-8"))
        c.disconnect()
        assert status == 401, f"Expected 401 for 'Basic' auth scheme, got {status}"


# ---------------------------------------------------------------------------
# SEC-02: CORS Wildcard Absence
# ---------------------------------------------------------------------------

@pytest.mark.security
@pytest.mark.p0
class TestCORSWildcardAbsence:
    """SEC-02: No response must contain Access-Control-Allow-Origin: *."""

    def _collect_headers(self, client):
        """Issue a variety of requests and collect all response header dicts."""
        collected = []

        # ping
        _, hdrs, _ = client._send_http(
            "POST",
            json.dumps({"jsonrpc": "2.0", "id": 9001, "method": "ping"}),
        )
        collected.append(hdrs)

        # tools/list
        _, hdrs, _ = client._send_http(
            "POST",
            json.dumps({"jsonrpc": "2.0", "id": 9002, "method": "tools/list"}),
        )
        collected.append(hdrs)

        # tools/call (a harmless call)
        _, hdrs, _ = client._send_http(
            "POST",
            json.dumps({
                "jsonrpc": "2.0",
                "id": 9003,
                "method": "tools/call",
                "params": {"name": "editor/get_open_files"},
            }),
        )
        collected.append(hdrs)

        # resources/list
        _, hdrs, _ = client._send_http(
            "POST",
            json.dumps({
                "jsonrpc": "2.0", "id": 9004, "method": "resources/list",
            }),
        )
        collected.append(hdrs)

        # Request with an explicit Origin header.
        _, hdrs, _ = client._send_http(
            "POST",
            json.dumps({"jsonrpc": "2.0", "id": 9005, "method": "ping"}),
            headers={"Origin": "http://localhost:3000"},
        )
        collected.append(hdrs)

        return collected

    def test_no_cors_wildcard(self, client):
        """No response should carry Access-Control-Allow-Origin: *."""
        all_headers = self._collect_headers(client)
        for idx, hdrs in enumerate(all_headers):
            acao = hdrs.get("access-control-allow-origin", "")
            assert acao != "*", (
                f"Response #{idx} has Access-Control-Allow-Origin: * "
                f"-- wildcard CORS is forbidden.  Full headers: {hdrs}"
            )

    def test_vary_origin_present(self, client):
        """Responses to requests with an Origin header must include Vary: Origin."""
        _, hdrs, _ = client._send_http(
            "POST",
            json.dumps({"jsonrpc": "2.0", "id": 9010, "method": "ping"}),
            headers={"Origin": "http://localhost:3000"},
        )
        # If the server sets any ACAO header, Vary must include Origin.
        acao = hdrs.get("access-control-allow-origin", "")
        if acao:
            vary = hdrs.get("vary", "")
            assert "origin" in vary.lower(), (
                f"Response has ACAO={acao!r} but Vary header ({vary!r}) "
                f"does not include 'Origin'.  Full headers: {hdrs}"
            )


# ---------------------------------------------------------------------------
# SEC-03: DNS Rebinding Protection
# ---------------------------------------------------------------------------

@pytest.mark.security
@pytest.mark.p0
class TestDNSRebindingProtection:
    """SEC-03: Requests with a non-local Host header must be rejected."""

    def test_evil_host_rejected(self, raw_client):
        """A request with Host: evil.com:6009 must return 403 Forbidden."""
        # Build a raw HTTP request with the malicious Host header.
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-06-18",
                "clientInfo": {"name": "evil-client", "version": "1.0.0"},
                "capabilities": {},
            },
        })
        status, hdrs, resp_body = raw_client._send_http(
            "POST",
            body,
            headers={"Host": "evil.com:6009"},
        )
        assert status == 403, (
            f"Expected 403 Forbidden for Host: evil.com:6009 but got "
            f"HTTP {status}.  Body: {resp_body}"
        )


# ---------------------------------------------------------------------------
# SEC-04: Session Prediction
# ---------------------------------------------------------------------------

@pytest.mark.security
@pytest.mark.p0
class TestSessionPrediction:
    """SEC-04: Session IDs must be cryptographically random and unpredictable."""

    NUM_SESSIONS = 10

    def test_session_id_properties(self, mcp_server_available):
        """Create multiple sessions sequentially and verify randomness."""
        session_ids: list[str] = []

        for _ in range(self.NUM_SESSIONS):
            c = MCPClient()
            try:
                c.full_handshake()
                assert c.session_id is not None, "Session ID must not be None"
                session_ids.append(c.session_id)
            finally:
                try:
                    c.delete_session()
                except Exception:
                    pass
                c.disconnect()
            time.sleep(0.1)  # Small delay to avoid connection limits.

        # --- Verify format: 64-character lowercase hex ---
        for sid in session_ids:
            assert len(sid) == 64, (
                f"Session ID should be 64 characters, got {len(sid)}: {sid!r}"
            )
            assert sid == sid.lower(), (
                f"Session ID should be lowercase hex, got: {sid!r}"
            )
            # Ensure it is valid hexadecimal.
            try:
                int(sid, 16)
            except ValueError:
                pytest.fail(f"Session ID is not valid hex: {sid!r}")

        # --- Verify uniqueness ---
        assert len(set(session_ids)) == len(session_ids), (
            f"All {self.NUM_SESSIONS} session IDs must be unique.  "
            f"Got {len(set(session_ids))} unique out of {len(session_ids)}."
        )

        # --- Verify no common prefix longer than 4 characters ---
        def common_prefix_len(a: str, b: str) -> int:
            length = 0
            for ca, cb in zip(a, b):
                if ca == cb:
                    length += 1
                else:
                    break
            return length

        max_prefix = 0
        for i in range(len(session_ids)):
            for j in range(i + 1, len(session_ids)):
                plen = common_prefix_len(session_ids[i], session_ids[j])
                if plen > max_prefix:
                    max_prefix = plen

        assert max_prefix <= 4, (
            f"Session IDs share a common prefix of {max_prefix} characters "
            f"(max allowed: 4).  This suggests weak randomness."
        )


# ---------------------------------------------------------------------------
# SEC-05: Expression Sandbox
# ---------------------------------------------------------------------------

@pytest.mark.security
@pytest.mark.p0
@pytest.mark.requires_game
class TestExpressionSandbox:
    """SEC-05: Dangerous runtime expressions must be blocked or error out."""

    DANGEROUS_EXPRESSIONS = [
        pytest.param('OS.execute("whoami", [])', id="os-execute"),
        pytest.param('OS.shell_open("http://evil.com")', id="shell-open"),
        pytest.param(
            'FileAccess.open("/etc/passwd", FileAccess.READ)',
            id="file-access-open",
        ),
        pytest.param('DirAccess.open("/")', id="dir-access-open"),
    ]

    @pytest.fixture(autouse=True)
    def _game_lifecycle(self, client):
        """Start the game before each test and stop it after."""
        client.start_game_and_wait()
        yield
        try:
            client.stop_game()
            time.sleep(0.5)
        except Exception:
            pass

    @pytest.mark.parametrize("expression", DANGEROUS_EXPRESSIONS)
    def test_dangerous_expression_rejected(self, client, expression):
        """runtime/evaluate must reject or error on dangerous expressions."""
        response = client.call_tool(
            "runtime/evaluate", {"expression": expression}
        )
        # The server may report the error as an MCP isError flag, as a
        # JSON-RPC error, or return a result whose text makes it clear the
        # operation failed (e.g. null, empty, or explicit error message).
        mcp_error = is_error(response)
        rpc_error = "error" in response

        # Some expressions might technically "succeed" at the protocol level
        # but return an error value from the engine (e.g. null / -1 / ERR_*).
        # Accept that as blocked too.
        result_text = get_text(response).lower()
        structured = get_structured(response)
        result_val = structured.get("value", "") if isinstance(structured, dict) else ""
        result_val_lower = str(result_val).lower() if result_val else ""

        engine_error = any(
            indicator in (result_text + " " + result_val_lower)
            for indicator in [
                "null", "err_", "error", "denied", "forbidden",
                "blocked", "not allowed", "sandboxed", "-1",
            ]
        )

        assert mcp_error or rpc_error or engine_error, (
            f"Expression {expression!r} should be rejected by the sandbox "
            f"but returned a seemingly successful result: {response}"
        )
