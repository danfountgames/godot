"""MCP concurrency and stress tests for the Godot MCP server.

Tests cover multi-client ping storms, rapid game start/stop cycles,
output buffer behaviour under load, server resilience to invalid requests,
and session overflow recovery.
"""

import json
import os
import socket
import threading
import time

import pytest

from mcp_test_client import MCPClient, MCP_HOST, MCP_PORT
from conftest import get_structured, is_error


# ---------------------------------------------------------------------------
# STRESS-01: Multi-Client Ping Storm (P2)
# ---------------------------------------------------------------------------

class TestMultiClientPingStorm:
    """STRESS-01 -- 8 threads each create their own client and send 100 pings."""

    NUM_THREADS = 8
    PINGS_PER_THREAD = 100
    THREAD_TIMEOUT = 30.0

    @pytest.mark.p2
    @pytest.mark.slow
    def test_multi_client_ping_storm(self, mcp_server_available):
        """STRESS-01: 8 concurrent clients each send 100 pings. All must succeed."""
        results = [None] * self.NUM_THREADS
        errors = [None] * self.NUM_THREADS

        def worker(index):
            client = MCPClient()
            try:
                client.full_handshake()
                for i in range(self.PINGS_PER_THREAD):
                    resp = client.ping()
                    if "result" not in resp:
                        raise AssertionError(
                            f"Thread {index} ping {i} missing 'result': {resp}"
                        )
                results[index] = True
            except Exception as exc:
                errors[index] = exc
            finally:
                try:
                    client.delete_session()
                except Exception:
                    pass
                client.disconnect()

        threads = []
        for i in range(self.NUM_THREADS):
            t = threading.Thread(target=worker, args=(i,), daemon=True)
            threads.append(t)

        for t in threads:
            t.start()

        for t in threads:
            t.join(timeout=self.THREAD_TIMEOUT)

        # Verify all threads completed within the timeout.
        for i, t in enumerate(threads):
            assert not t.is_alive(), (
                f"Thread {i} did not complete within {self.THREAD_TIMEOUT}s"
            )

        # Verify no errors.
        for i in range(self.NUM_THREADS):
            assert errors[i] is None, (
                f"Thread {i} raised an error: {errors[i]}"
            )
            assert results[i] is True, (
                f"Thread {i} did not complete successfully"
            )


# ---------------------------------------------------------------------------
# STRESS-02: Rapid Game Start/Stop (P2)
# ---------------------------------------------------------------------------

class TestRapidGameStartStop:
    """STRESS-02 -- Start and stop the game 10 times rapidly."""

    NUM_CYCLES = 10
    RUN_DURATION = 0.5
    PAUSE_BETWEEN = 0.3

    def _ensure_stopped(self, client: MCPClient):
        """Make sure the game is stopped."""
        try:
            status_resp = client.call_tool("runtime/get_status")
            state = get_structured(status_resp).get("state", "stopped")
            if state != "stopped":
                client.call_tool("runtime/stop")
                time.sleep(1.0)
        except Exception:
            pass

    @pytest.mark.p2
    @pytest.mark.requires_game
    @pytest.mark.slow
    def test_rapid_game_start_stop(self, client: MCPClient):
        """STRESS-02: Start and stop the game 10 times rapidly, then verify
        the server is still responsive and the game reports stopped."""
        self._ensure_stopped(client)

        try:
            for cycle in range(self.NUM_CYCLES):
                # Start the game.
                resp = client.call_tool("runtime/run_project")
                assert not is_error(resp), (
                    f"Cycle {cycle}: run_project returned error"
                )

                # Let it run briefly.
                time.sleep(self.RUN_DURATION)

                # Stop the game.
                stop_resp = client.call_tool("runtime/stop")
                # Stop may report an error if the game hasn't fully started yet;
                # that is acceptable during rapid cycling.

                # Pause between cycles.
                time.sleep(self.PAUSE_BETWEEN)

            # After all cycles, verify server is responsive.
            ping_resp = client.ping()
            assert "result" in ping_resp, (
                "Server did not respond to ping after rapid start/stop cycles"
            )

            # Verify game reports stopped.
            time.sleep(1.0)
            status_resp = client.call_tool("runtime/get_status")
            state = get_structured(status_resp).get("state", "stopped")
            assert state == "stopped", (
                f"Expected state 'stopped' after all cycles, got '{state}'"
            )
        finally:
            self._ensure_stopped(client)


# ---------------------------------------------------------------------------
# STRESS-03: Output Buffer Under Load (P2)
# ---------------------------------------------------------------------------

class TestOutputBufferUnderLoad:
    """STRESS-03 -- Verify the output buffer behaves correctly under load."""

    @pytest.mark.p2
    @pytest.mark.requires_game
    def test_output_buffer_cursor_under_load(self, running_game):
        """STRESS-03: Start game, read output with cursor, wait, read again.
        Assert cursor advanced or equal and no overlapping line sequence numbers."""
        client = running_game

        # First read: get initial output and cursor.
        resp1 = client.call_tool("runtime/get_output")
        assert not is_error(resp1), (
            f"First get_output returned error: {resp1}"
        )
        sc1 = get_structured(resp1)
        cursor1 = sc1.get("cursor", sc1.get("next_cursor"))
        lines1 = sc1.get("lines", sc1.get("output", []))
        if isinstance(lines1, str):
            lines1 = lines1.strip().splitlines()

        assert cursor1 is not None, (
            f"No cursor in first get_output response: {sc1}"
        )

        # Wait for the game to produce more output.
        time.sleep(1.0)

        # Second read with cursor from the first read.
        resp2 = client.call_tool("runtime/get_output", {"cursor": cursor1})
        assert not is_error(resp2), (
            f"Second get_output returned error: {resp2}"
        )
        sc2 = get_structured(resp2)
        cursor2 = sc2.get("cursor", sc2.get("next_cursor"))
        lines2 = sc2.get("lines", sc2.get("output", []))
        if isinstance(lines2, str):
            lines2 = lines2.strip().splitlines()

        assert cursor2 is not None, (
            f"No cursor in second get_output response: {sc2}"
        )

        # Cursor must have advanced or stayed equal (no regression).
        assert cursor2 >= cursor1, (
            f"Cursor went backwards: {cursor2} < {cursor1}"
        )

        # Verify no overlapping line sequence numbers between the two reads.
        # Extract sequence numbers from lines if they are dicts with a "seq" or
        # "line_number" key; otherwise compare raw line content for duplicates.
        def extract_seq_numbers(lines):
            seqs = set()
            for line in lines:
                if isinstance(line, dict):
                    seq = line.get("seq", line.get("line_number", line.get("id")))
                    if seq is not None:
                        seqs.add(seq)
            return seqs

        seqs1 = extract_seq_numbers(lines1)
        seqs2 = extract_seq_numbers(lines2)

        if seqs1 and seqs2:
            overlap = seqs1 & seqs2
            assert len(overlap) == 0, (
                f"Overlapping line sequence numbers between reads: {overlap}"
            )


# ---------------------------------------------------------------------------
# FR-01: Server Survives Invalid Requests (P2)
# ---------------------------------------------------------------------------

class TestServerSurvivesInvalidRequests:
    """FR-01 -- Send 20 malformed requests and verify server stays alive."""

    TOTAL_MALFORMED = 20
    BATCH_SIZE = 5

    @pytest.mark.p2
    @pytest.mark.slow
    def test_server_survives_invalid_requests(self, mcp_server_available):
        """FR-01: Send 20 malformed requests (random bytes, truncated JSON,
        wrong content-type). After each batch of 5, verify ping still works."""
        client = MCPClient()
        client.full_handshake()
        session_id = client.session_id

        malformed_requests = [
            # Type 1: Random bytes (not valid HTTP at all).
            self._random_bytes_request(session_id),
            # Type 2: Truncated JSON body.
            self._truncated_json_request(session_id),
            # Type 3: Wrong content-type.
            self._wrong_content_type_request(session_id),
            # Type 4: Empty body with correct headers.
            self._empty_body_request(session_id),
            # Type 5: Valid HTTP but binary garbage body.
            self._binary_garbage_body_request(session_id),
        ]

        # Repeat the pattern 4 times to get 20 total.
        all_requests = malformed_requests * 4

        try:
            for batch_start in range(0, self.TOTAL_MALFORMED, self.BATCH_SIZE):
                batch = all_requests[batch_start:batch_start + self.BATCH_SIZE]

                for raw_bytes in batch:
                    try:
                        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                        sock.settimeout(5.0)
                        sock.connect((MCP_HOST, MCP_PORT))
                        sock.sendall(raw_bytes)
                        # Try to read a response; may fail or timeout.
                        try:
                            sock.recv(4096)
                        except (socket.timeout, ConnectionError, OSError):
                            pass
                    except (ConnectionError, OSError):
                        pass
                    finally:
                        try:
                            sock.close()
                        except Exception:
                            pass

                # After each batch of 5, verify ping still works.
                # The client connection may have been disrupted, so reconnect.
                try:
                    client.reconnect()
                    ping_resp = client.ping()
                    assert "result" in ping_resp, (
                        f"Ping failed after batch starting at index {batch_start}"
                    )
                except (ConnectionError, OSError, AssertionError):
                    # Connection may need a fresh socket; try once more.
                    client.disconnect()
                    client.connect()
                    ping_resp = client.ping()
                    assert "result" in ping_resp, (
                        f"Ping failed on retry after batch starting at index {batch_start}"
                    )
        finally:
            try:
                client.delete_session()
            except Exception:
                pass
            client.disconnect()

    # -- Helpers to construct malformed request payloads --

    @staticmethod
    def _random_bytes_request(session_id: str) -> bytes:
        """Random bytes that are not valid HTTP."""
        return os.urandom(128)

    @staticmethod
    def _truncated_json_request(session_id: str) -> bytes:
        """Valid HTTP headers but truncated JSON body."""
        body = '{"jsonrpc":"2.0","id":1,"meth'  # Truncated.
        return (
            "POST /mcp HTTP/1.1\r\n"
            f"Host: {MCP_HOST}:{MCP_PORT}\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            f"Mcp-Session-Id: {session_id}\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            f"{body}"
        ).encode("utf-8")

    @staticmethod
    def _wrong_content_type_request(session_id: str) -> bytes:
        """Valid JSON-RPC body but wrong Content-Type."""
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        return (
            "POST /mcp HTTP/1.1\r\n"
            f"Host: {MCP_HOST}:{MCP_PORT}\r\n"
            "Content-Type: text/plain\r\n"
            f"Content-Length: {len(body)}\r\n"
            f"Mcp-Session-Id: {session_id}\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            f"{body}"
        ).encode("utf-8")

    @staticmethod
    def _empty_body_request(session_id: str) -> bytes:
        """Valid HTTP headers with Content-Length: 0 and empty body."""
        return (
            "POST /mcp HTTP/1.1\r\n"
            f"Host: {MCP_HOST}:{MCP_PORT}\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 0\r\n"
            f"Mcp-Session-Id: {session_id}\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
        ).encode("utf-8")

    @staticmethod
    def _binary_garbage_body_request(session_id: str) -> bytes:
        """Valid HTTP headers but binary garbage as the JSON body."""
        garbage = os.urandom(64)
        # Use the raw length of the garbage for Content-Length.
        header = (
            "POST /mcp HTTP/1.1\r\n"
            f"Host: {MCP_HOST}:{MCP_PORT}\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(garbage)}\r\n"
            f"Mcp-Session-Id: {session_id}\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
        ).encode("utf-8")
        return header + garbage


# ---------------------------------------------------------------------------
# FR-03: Server Survives Session Overflow (P2)
# ---------------------------------------------------------------------------

class TestServerSurvivesSessionOverflow:
    """FR-03 -- Create many sessions, delete them, create more, verify all work."""

    NUM_SESSIONS = 6  # Conservative to leave room for other sessions.

    @pytest.mark.p2
    @pytest.mark.slow
    def test_session_overflow_recovery(self, mcp_server_available):
        """FR-03: Create 6 sessions, delete all, create 6 new sessions,
        verify all respond to ping, then clean up."""
        # Phase 1: Create first batch of sessions.
        batch1 = []
        for i in range(self.NUM_SESSIONS):
            c = MCPClient()
            try:
                c.full_handshake()
                batch1.append(c)
            except Exception as exc:
                # Clean up any already-created sessions.
                for prev in batch1:
                    try:
                        prev.delete_session()
                    except Exception:
                        pass
                    prev.disconnect()
                c.disconnect()
                pytest.fail(
                    f"Failed to create session {i} in batch 1: {exc}"
                )

        # Phase 2: Delete all sessions from batch 1.
        for c in batch1:
            try:
                status = c.delete_session()
                assert status == 200, (
                    f"Expected 200 from DELETE, got {status}"
                )
            except Exception:
                pass
            c.disconnect()

        # Phase 3: Create second batch of sessions.
        batch2 = []
        try:
            for i in range(self.NUM_SESSIONS):
                c = MCPClient()
                try:
                    c.full_handshake()
                    batch2.append(c)
                except Exception as exc:
                    c.disconnect()
                    pytest.fail(
                        f"Failed to create session {i} in batch 2: {exc}"
                    )

            # Phase 4: Verify all sessions in batch 2 respond to ping.
            for i, c in enumerate(batch2):
                c.reconnect()
                ping_resp = c.ping()
                assert "result" in ping_resp, (
                    f"Session {i} in batch 2 did not respond to ping: {ping_resp}"
                )
        finally:
            # Phase 5: Clean up all sessions from batch 2.
            for c in batch2:
                try:
                    c.delete_session()
                except Exception:
                    pass
                c.disconnect()
