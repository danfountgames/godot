#!/usr/bin/env python3
"""Tests for the relay's HTTP transport.

Imported by run_tests.py, which owns the runner. These use real HTTP over a real
socket against the real binary, with the fake editor standing in for Godot.

What is worth testing here is not "does JSON go in and come out" - the stdio suite
already covers the protocol. It is the three properties HTTP adds and stdio never
had: an endpoint anyone on the machine can reach (so, authorisation), more than one
client at once (so, session isolation), and a session that outlives a connection (so,
session lifetime).
"""

import http.client
import json
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from fake_editor import FakeEditor, free_port  # noqa: E402
from relay_harness import RelayProcess  # noqa: E402

TOKEN = "test-token-0123456789"

INITIALIZE = {
    "jsonrpc": "2.0", "id": 1, "method": "initialize",
    "params": {"protocolVersion": "2025-06-18", "capabilities": {},
               "clientInfo": {"name": "http-test", "version": "1"}},
}


class HttpRelay:
    """A relay serving HTTP, with the fake editor behind it."""

    def __init__(self, token=TOKEN, extra_args=(), multi_connection=True, **editor_kwargs):
        self.editor = FakeEditor(multi_connection=multi_connection, **editor_kwargs)
        self.port = free_port()
        args = ["--http-port", str(self.port), "--client-name", "http-test"]
        if token is not None:
            args += ["--http-token", token]
        args += list(extra_args)
        self.token = token
        self.relay = RelayProcess(args=args)
        self.relay.write_instance(port=self.editor.port)
        self._wait_until_listening()

    def _wait_until_listening(self, timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.relay.process.poll() is not None:
                raise AssertionError("the relay exited before it started listening")
            probe = socket.socket()
            probe.settimeout(0.5)
            try:
                probe.connect(("127.0.0.1", self.port))
                return
            except OSError:
                time.sleep(0.05)
            finally:
                probe.close()
        raise AssertionError("the relay never started listening on %d" % self.port)

    def request(self, method, body=None, session=None, token="__default__", path="/mcp"):
        """Returns (status, headers, parsed-body-or-raw-text)."""
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        headers = {"Content-Type": "application/json"}
        if token == "__default__":
            token = self.token
        if token is not None:
            headers["Authorization"] = "Bearer " + token
        if session is not None:
            headers["Mcp-Session-Id"] = session
        payload = json.dumps(body) if body is not None else None
        if payload is not None:
            headers["Content-Length"] = str(len(payload))
        connection.request(method, path, body=payload, headers=headers)
        response = connection.getresponse()
        raw = response.read().decode("utf-8")
        result = raw
        if raw:
            try:
                result = json.loads(raw)
            except ValueError:
                pass
        pairs = {key.lower(): value for key, value in response.getheaders()}
        connection.close()
        return response.status, pairs, result

    def open_session(self):
        status, headers, body = self.request("POST", INITIALIZE)
        assert status == 200, "initialize failed: %s %s" % (status, body)
        return headers["mcp-session-id"], body

    def cleanup(self):
        self.relay.cleanup()
        self.editor.close()


def register(test, assert_eq, assert_in):
    """Registers these cases with run_tests.py's runner."""

    @test
    def test_http_requires_a_bearer_token():
        server = HttpRelay()
        try:
            status, headers, body = server.request("POST", INITIALIZE, token=None)
            assert_eq(status, 401, "status without a token")
            assert_in("Bearer", headers.get("www-authenticate", ""), "challenge header")
            # No session may be created by an unauthorised request.
            assert_eq(server.editor.connection_count, 0, "editor connections")
        finally:
            server.cleanup()

    @test
    def test_http_rejects_a_wrong_token():
        server = HttpRelay()
        try:
            status, _, _ = server.request("POST", INITIALIZE, token="not-the-token")
            assert_eq(status, 401, "status with a wrong token")
            # Same length, one byte different: the comparison must not leak where.
            status, _, _ = server.request("POST", INITIALIZE, token=TOKEN[:-1] + "X")
            assert_eq(status, 401, "status with a near-miss token")
        finally:
            server.cleanup()

    @test
    def test_http_initialize_opens_a_session():
        server = HttpRelay()
        try:
            status, headers, body = server.request("POST", INITIALIZE)
            assert_eq(status, 200, "initialize status")
            session = headers.get("mcp-session-id", "")
            assert_eq(len(session), 32, "session id length")
            assert_eq(body["id"], 1, "response id")
            assert_in("result", body, "initialize response")
        finally:
            server.cleanup()

    @test
    def test_http_requires_a_session_for_anything_but_initialize():
        server = HttpRelay()
        try:
            status, _, body = server.request(
                "POST", {"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
            assert_eq(status, 400, "status without a session")
            assert_in("initialize", body["error"]["message"], "error message")
        finally:
            server.cleanup()

    @test
    def test_http_rejects_an_unknown_session():
        server = HttpRelay()
        try:
            status, _, body = server.request(
                "POST", {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                session="0" * 32)
            assert_eq(status, 404, "status with an unknown session")
            assert_in("unknown or expired", body["error"]["message"], "error message")
        finally:
            server.cleanup()

    @test
    def test_http_sessions_are_isolated():
        server = HttpRelay()
        try:
            first, _ = server.open_session()
            second, _ = server.open_session()
            if first == second:
                raise AssertionError("two sessions were given the same id")
            # One editor connection per session is what keeps their replies apart.
            assert_eq(server.editor.connection_count, 2, "editor connections")

            # Each session's traffic reaches the editor tagged with its own id, and
            # each gets its own answer back.
            status, _, body = server.request(
                "POST", {"jsonrpc": "2.0", "id": 11, "method": "tools/list"}, session=first)
            assert_eq(status, 200, "first session status")
            assert_eq(body["id"], 11, "first session response id")

            status, _, body = server.request(
                "POST", {"jsonrpc": "2.0", "id": 22, "method": "tools/list"}, session=second)
            assert_eq(status, 200, "second session status")
            assert_eq(body["id"], 22, "second session response id")

            # Closing one session must not disturb the other.
            status, _, _ = server.request("DELETE", session=first)
            assert_eq(status, 204, "delete status")
            status, _, _ = server.request(
                "POST", {"jsonrpc": "2.0", "id": 33, "method": "tools/list"}, session=first)
            assert_eq(status, 404, "closed session status")
            status, _, body = server.request(
                "POST", {"jsonrpc": "2.0", "id": 44, "method": "tools/list"}, session=second)
            assert_eq(status, 200, "surviving session status")
            assert_eq(body["id"], 44, "surviving session response id")
        finally:
            server.cleanup()

    @test
    def test_http_serves_concurrent_clients():
        server = HttpRelay()
        try:
            sessions = [server.open_session()[0] for _ in range(4)]
            assert_eq(len(set(sessions)), 4, "distinct sessions")
            # Interleaved traffic: every session keeps answering while others are busy.
            for round_number in range(3):
                for index, session in enumerate(sessions):
                    identifier = 100 * (round_number + 1) + index
                    status, _, body = server.request(
                        "POST", {"jsonrpc": "2.0", "id": identifier, "method": "tools/list"},
                        session=session)
                    assert_eq(status, 200, "status for session %d" % index)
                    assert_eq(body["id"], identifier, "response id for session %d" % index)
        finally:
            server.cleanup()

    @test
    def test_http_notification_is_accepted_with_no_body():
        server = HttpRelay()
        try:
            session, _ = server.open_session()
            status, _, body = server.request(
                "POST", {"jsonrpc": "2.0", "method": "notifications/initialized"},
                session=session)
            assert_eq(status, 202, "notification status")
            assert_eq(body, "", "notification body")
        finally:
            server.cleanup()

    @test
    def test_http_rejects_batches_and_junk():
        server = HttpRelay()
        try:
            session, _ = server.open_session()
            status, _, body = server.request("POST", [INITIALIZE], session=session)
            assert_eq(status, 400, "batch status")
            assert_in("batched", body["error"]["message"], "batch error")

            connection = http.client.HTTPConnection("127.0.0.1", server.port, timeout=30)
            connection.request("POST", "/mcp", body="{not json",
                               headers={"Authorization": "Bearer " + TOKEN,
                                        "Content-Type": "application/json"})
            response = connection.getresponse()
            assert_eq(response.status, 400, "malformed JSON status")
            connection.close()
        finally:
            server.cleanup()

    @test
    def test_http_rejects_other_methods_and_paths():
        server = HttpRelay()
        try:
            status, _, body = server.request("GET")
            assert_eq(status, 405, "GET status")
            assert_in("POST", body["error"]["message"], "GET error names the fix")

            status, _, body = server.request("PUT", INITIALIZE)
            assert_eq(status, 405, "PUT status")

            status, _, body = server.request("POST", INITIALIZE, path="/elsewhere")
            assert_eq(status, 404, "unknown path status")
            assert_in("/mcp", body["error"]["message"], "path error names the endpoint")
        finally:
            server.cleanup()

    @test
    def test_http_refuses_to_bind_off_loopback_without_consent():
        editor = FakeEditor()
        relay = RelayProcess(args=["--http-port", str(free_port()),
                                   "--http-host", "0.0.0.0",
                                   "--http-token", TOKEN])
        try:
            relay.process.wait(timeout=10)
            assert_eq(relay.process.returncode, 2, "exit status")
            stderr = relay.process.stderr.read().decode("utf-8")
            assert_in("--http-allow-remote", stderr, "refusal explains the override")
        finally:
            relay.cleanup()
            editor.close()

    @test
    def test_http_generates_a_token_when_none_is_given():
        server = HttpRelay(token=None, extra_args=["--log-level", "info"])
        try:
            # Without the generated token, nothing works...
            status, _, _ = server.request("POST", INITIALIZE, token="guessed")
            assert_eq(status, 401, "status with a guessed token")

            # ...and the token is announced on stderr, never on stdout, because stdout
            # is protocol territory even in this mode.
            deadline = time.time() + 10
            token = None
            while time.time() < deadline and token is None:
                for line in server.relay.drain_stderr().splitlines():
                    if "generated bearer token:" in line:
                        token = line.split("generated bearer token:")[1].strip()
                time.sleep(0.1)
            if token is None:
                raise AssertionError("the generated token was never printed to stderr")
            server.token = token
            status, _, _ = server.request("POST", INITIALIZE)
            assert_eq(status, 200, "status with the generated token")
            assert_eq(server.relay.drain_stdout(), "", "stdout must stay empty")
        finally:
            server.cleanup()
