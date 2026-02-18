"""MCP edge-case tests for the Godot MCP server.

Tests cover slow byte-by-byte HTTP delivery, large payloads near the body
size limit, game crash resilience, keep-alive connection reuse,
Content-Length mismatches, concurrent requests, output ring buffer
behaviour, and unknown JSON-RPC methods.
"""

import json
import socket
import threading
import time

import pytest

from mcp_test_client import MCPClient, MCP_HOST, MCP_PORT, MCP_AUTH_TOKEN


# ---------------------------------------------------------------------------
# EC-01: HTTP Split (P0)
# ---------------------------------------------------------------------------

class TestHTTPSplit:
    """EC-01 -- Deliver a valid POST request byte-by-byte with 10ms delays."""

    @pytest.mark.p0
    def test_byte_by_byte_delivery(self, mcp_server_available):
        """EC-01: Sending a valid initialize request one byte at a time with
        10ms delays must still produce an HTTP 200 with a JSON result."""
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-06-18",
                "clientInfo": {"name": "split-test", "version": "1.0.0"},
                "capabilities": {},
            },
        })

        auth_hdr = ""
        if MCP_AUTH_TOKEN:
            auth_hdr = f"Authorization: Bearer {MCP_AUTH_TOKEN}\r\n"
        raw_request = (
            "POST /mcp HTTP/1.1\r\n"
            f"Host: {MCP_HOST}:{MCP_PORT}\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            f"{auth_hdr}"
            "Connection: keep-alive\r\n"
            "\r\n"
            f"{body}"
        ).encode("utf-8")

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        try:
            sock.connect((MCP_HOST, MCP_PORT))

            # Send byte-by-byte with 10ms delay.
            for byte in raw_request:
                sock.sendall(bytes([byte]))
                time.sleep(0.01)

            # Read the full response.
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                data += chunk

            # Parse status line.
            header_end = data.index(b"\r\n\r\n") + 4
            header_block = data[:header_end].decode("utf-8")
            status_line = header_block.split("\r\n")[0]
            status_code = int(status_line.split(" ", 2)[1])

            # Extract Content-Length and read body.
            content_length = 0
            for line in header_block.split("\r\n"):
                if line.lower().startswith("content-length:"):
                    content_length = int(line.split(":", 1)[1].strip())

            remaining = data[header_end:]
            while len(remaining) < content_length:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                remaining += chunk

            resp_body = remaining[:content_length].decode("utf-8")

            assert status_code == 200, f"Expected 200, got {status_code}"
            parsed = json.loads(resp_body)
            assert "result" in parsed, f"Response missing 'result': {parsed}"

            # Clean up: delete the session if one was returned.
            session_id = None
            for line in header_block.split("\r\n"):
                if line.lower().startswith("mcp-session-id:"):
                    session_id = line.split(":", 1)[1].strip()

            if session_id:
                delete_req = (
                    "DELETE /mcp HTTP/1.1\r\n"
                    f"Host: {MCP_HOST}:{MCP_PORT}\r\n"
                    f"Mcp-Session-Id: {session_id}\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                ).encode("utf-8")
                try:
                    sock.sendall(delete_req)
                    sock.recv(4096)
                except Exception:
                    pass
        finally:
            sock.close()


# ---------------------------------------------------------------------------
# EC-02: 4MB Boundary (P1)
# ---------------------------------------------------------------------------

class TestBodySizeLimit:
    """EC-02 -- Verify the server enforces a ~4MB body-size limit."""

    @pytest.mark.p1
    def test_02a_under_4mb_accepted(self, client: MCPClient):
        """EC-02a: A ~3.9MB JSON-RPC ping with padding should succeed."""
        # Build a body of roughly 3.9MB.
        target_size = int(3.9 * 1024 * 1024)
        base = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "ping",
            "params": {"padding": ""},
        }
        base_json = json.dumps(base)
        # Calculate how many padding characters we need.
        padding_needed = target_size - len(base_json)
        if padding_needed > 0:
            base["params"]["padding"] = "x" * padding_needed
        body = json.dumps(base)

        client.reconnect()
        status, _, resp_body = client._send_http("POST", body)
        assert status == 200, f"Expected 200 for ~3.9MB body, got {status}"
        parsed = json.loads(resp_body)
        assert "result" in parsed

    @pytest.mark.p1
    def test_02b_over_4mb_rejected(self, client: MCPClient):
        """EC-02b: A body > 4MB should be rejected with 400 or connection close."""
        target_size = 4 * 1024 * 1024 + 1024  # 4MB + 1KB
        base = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "ping",
            "params": {"padding": ""},
        }
        base_json = json.dumps(base)
        padding_needed = target_size - len(base_json)
        if padding_needed > 0:
            base["params"]["padding"] = "x" * padding_needed
        body = json.dumps(base)

        client.reconnect()
        try:
            status, _, _ = client._send_http("POST", body)
            # Server should reject with 400 (or possibly 413).
            assert status in (400, 413), (
                f"Expected 400 or 413 for >4MB body, got {status}"
            )
        except (ConnectionError, ConnectionResetError, BrokenPipeError, OSError):
            # Connection closed by server is also acceptable.
            pass


# ---------------------------------------------------------------------------
# EC-03: Game Crash During Request (P0)
# ---------------------------------------------------------------------------

class TestGameCrashDuringRequest:
    """EC-03 -- Server must remain responsive when the game becomes unavailable."""

    @pytest.mark.p0
    @pytest.mark.requires_game
    def test_server_survives_game_stop(self, client: MCPClient):
        """EC-03: Start a game, stop it immediately, query the scene tree,
        and verify the server is still responsive afterward."""
        # Step 1: Start the game.
        client.start_game_and_wait()

        # Step 2: Stop the game immediately to simulate unavailability.
        client.stop_game()
        time.sleep(0.5)

        # Step 3: Attempt to get the scene tree (game no longer running).
        client.reconnect()
        resp = client.call_tool("runtime/get_scene_tree")
        # The response may be an error (game not running) -- that is fine.
        assert resp is not None, "No response received from runtime/get_scene_tree"

        # Step 4: Verify the server is still alive.
        client.reconnect()
        ping_resp = client.ping()
        assert "result" in ping_resp, "Server failed to respond to ping after game stop"


# ---------------------------------------------------------------------------
# EC-04: Keep-Alive Connection Reuse (P1)
# ---------------------------------------------------------------------------

class TestKeepAliveConnectionReuse:
    """EC-04 -- Multiple requests over a single TCP connection."""

    @pytest.mark.p1
    def test_single_tcp_connection_reuse(self, mcp_server_available):
        """EC-04: Send initialize, notifications/initialized, and tools/list
        over one TCP connection without reconnecting."""
        c = MCPClient()
        c.connect()
        original_sock = c.sock

        try:
            # Request 1: initialize.
            init_result = c.initialize()
            assert "result" in init_result, "initialize failed"
            assert c.sock is original_sock, "Socket changed after initialize"

            # Request 2: notifications/initialized.
            status = c.send_initialized()
            assert status == 202, f"initialized notification failed: {status}"
            assert c.sock is original_sock, "Socket changed after initialized"

            # Request 3: tools/list.
            tools_resp = c.list_tools()
            tools = tools_resp["result"]["tools"]
            assert isinstance(tools, list) and len(tools) > 0
            assert c.sock is original_sock, "Socket changed after tools/list"
        finally:
            try:
                c.delete_session()
            except Exception:
                pass
            c.disconnect()


# ---------------------------------------------------------------------------
# EC-05: Content-Length Mismatch (P1)
# ---------------------------------------------------------------------------

class TestContentLengthMismatch:
    """EC-05 -- Server must survive a Content-Length vs actual body mismatch."""

    @pytest.mark.p1
    def test_content_length_too_large(self, client: MCPClient):
        """EC-05: Send Content-Length: 1000 but only 50 bytes of body, then
        close the connection. Verify the server still works on a new connection."""
        # Build a raw request with mismatched Content-Length.
        short_body = '{"jsonrpc":"2.0","id":1,"method":"ping"}'
        raw = (
            "POST /mcp HTTP/1.1\r\n"
            f"Host: {MCP_HOST}:{MCP_PORT}\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 1000\r\n"
            f"Mcp-Session-Id: {client.session_id}\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            f"{short_body}"
        ).encode("utf-8")

        # Step 1: Send the malformed request and immediately close.
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5.0)
        try:
            sock.connect((MCP_HOST, MCP_PORT))
            sock.sendall(raw)
            # Give the server a moment to start processing, then close.
            time.sleep(0.5)
        finally:
            sock.close()

        # Step 2: Open a new connection and verify server health.
        time.sleep(0.5)
        client.reconnect()
        ping_resp = client.ping()
        assert "result" in ping_resp, (
            "Server did not respond to ping after Content-Length mismatch"
        )


# ---------------------------------------------------------------------------
# EC-08: Concurrent Requests (P1)
# ---------------------------------------------------------------------------

class TestConcurrentRequests:
    """EC-08 -- Two clients making simultaneous requests."""

    @pytest.mark.p1
    def test_concurrent_project_get_overview(self, mcp_server_available):
        """EC-08: Two separate clients call project/get_overview at the same time.
        Both should receive valid responses."""
        results = [None, None]
        errors = [None, None]

        def worker(index):
            c = MCPClient()
            try:
                c.full_handshake()
                resp = c.call_tool("project/get_overview")
                results[index] = resp
            except Exception as exc:
                errors[index] = exc
            finally:
                try:
                    c.delete_session()
                except Exception:
                    pass
                c.disconnect()

        t0 = threading.Thread(target=worker, args=(0,))
        t1 = threading.Thread(target=worker, args=(1,))

        t0.start()
        t1.start()
        t0.join(timeout=15.0)
        t1.join(timeout=15.0)

        # Both threads must have completed.
        assert not t0.is_alive(), "Thread 0 timed out"
        assert not t1.is_alive(), "Thread 1 timed out"

        # Neither should have raised.
        assert errors[0] is None, f"Client 0 error: {errors[0]}"
        assert errors[1] is None, f"Client 1 error: {errors[1]}"

        # Both responses must be valid.
        for i in range(2):
            assert results[i] is not None, f"Client {i} returned None"
            assert "result" in results[i], (
                f"Client {i} response missing 'result': {results[i]}"
            )


# ---------------------------------------------------------------------------
# EC-09: Output Ring Buffer (P1)
# ---------------------------------------------------------------------------

class TestOutputRingBuffer:
    """EC-09 -- Verify the debug output ring buffer cursor behaviour."""

    @pytest.mark.p1
    @pytest.mark.requires_game
    def test_output_cursor_advances_no_duplicates(self, client: MCPClient):
        """EC-09: Start a game, read output with no cursor, record the cursor,
        read again with that cursor, and confirm no duplicate lines."""
        # Step 1: Start the game (which prints STARTUP_COMPLETE).
        client.start_game_and_wait()

        try:
            # Step 2: First call to runtime/get_output with no cursor.
            client.reconnect()
            resp1 = client.call_tool("runtime/get_output")
            result1 = resp1.get("result", {})
            sc1 = result1.get("structuredContent", {})

            # Extract cursor from the first response.
            cursor1 = sc1.get("cursor", sc1.get("next_cursor"))
            lines1 = sc1.get("lines", sc1.get("output", []))
            if isinstance(lines1, str):
                lines1 = lines1.strip().splitlines()

            assert cursor1 is not None, (
                f"No cursor in first get_output response: {sc1}"
            )

            # Step 3: Second call with the cursor from step 2.
            client.reconnect()
            resp2 = client.call_tool("runtime/get_output", {"cursor": cursor1})
            result2 = resp2.get("result", {})
            sc2 = result2.get("structuredContent", {})

            cursor2 = sc2.get("cursor", sc2.get("next_cursor"))
            lines2 = sc2.get("lines", sc2.get("output", []))
            if isinstance(lines2, str):
                lines2 = lines2.strip().splitlines()

            # Step 4: Cursor should have advanced or stayed equal (no new output).
            assert cursor2 is not None, (
                f"No cursor in second get_output response: {sc2}"
            )
            assert cursor2 >= cursor1, (
                f"Cursor went backwards: {cursor2} < {cursor1}"
            )

            # Step 5: No duplicate lines -- lines2 should not repeat lines1.
            if lines1 and lines2:
                set1 = set(lines1) if isinstance(lines1[0], str) else set()
                set2 = set(lines2) if isinstance(lines2[0], str) else set()
                overlap = set1 & set2
                assert len(overlap) == 0, (
                    f"Duplicate lines between reads: {overlap}"
                )
        finally:
            client.stop_game()


# ---------------------------------------------------------------------------
# EC-10: Unknown JSON-RPC Method (P1)
# ---------------------------------------------------------------------------

class TestUnknownMethod:
    """EC-10 -- Sending an unknown method must return -32601 (Method Not Found)."""

    @pytest.mark.p1
    def test_unknown_method_returns_32601(self, client: MCPClient):
        """EC-10: Send a request with method 'nonexistent/method' and verify
        the error code is -32601."""
        client.reconnect()
        status, _, resp_body = client.send_jsonrpc("nonexistent/method")
        parsed = json.loads(resp_body)

        assert "error" in parsed, f"Expected error response, got: {parsed}"
        assert parsed["error"]["code"] == -32601, (
            f"Expected error code -32601, got {parsed['error']['code']}"
        )
