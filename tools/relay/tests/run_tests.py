#!/usr/bin/env python3
"""Integration tests for godot-ai-relay.

These drive the real relay binary over real pipes and a real loopback socket.
Run with:  python3 tools/relay/tests/run_tests.py [-k <name-substring>]
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from fake_editor import FakeEditor, free_port  # noqa: E402
from relay_harness import RELAY_BINARY, RelayProcess, run_relay  # noqa: E402

TESTS = []


def test(func):
    TESTS.append(func)
    return func


def wait_for(predicate, timeout=5.0, message="condition"):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("timed out waiting for %s" % message)


def assert_eq(actual, expected, what="value"):
    if actual != expected:
        raise AssertionError("%s: expected %r, got %r" % (what, expected, actual))


def assert_in(needle, haystack, what="text"):
    if needle not in haystack:
        raise AssertionError("%s: expected %r to contain %r" % (what, haystack, needle))


def connected_relay(editor, extra_args=(), **kwargs):
    """Starts a relay pointed at a fake editor via the instance registry."""
    relay = RelayProcess(args=list(extra_args), **kwargs)
    relay.write_instance(port=editor.port)
    return relay


# --------------------------------------------------------------- CLI surface ---


@test
def test_version_and_help_keep_stdout_clean():
    """R5/R7: informational output must never pollute the protocol stream."""
    for args in (["--version"], ["--help"]):
        result = run_relay(args)
        assert_eq(result.returncode, 0, "exit code for %s" % args)
        assert_eq(result.stdout, b"", "stdout for %s" % args)
        if not result.stderr.strip():
            raise AssertionError("expected stderr output for %s" % args)
    assert_in("godot-ai-relay", run_relay(["--version"]).stderr.decode(), "version banner")
    assert_in("--approval-mode", run_relay(["--help"]).stderr.decode(), "usage text")


@test
def test_unknown_option_is_a_usage_error():
    """R5: bad invocation fails fast with exit code 2 and a stderr message."""
    result = run_relay(["--not-a-flag"])
    assert_eq(result.returncode, 2, "exit code")
    assert_eq(result.stdout, b"", "stdout")
    assert_in("unknown option", result.stderr.decode(), "stderr")


@test
def test_invalid_option_values_are_rejected():
    """R5: each validated option reports its own value back to the user."""
    for args, needle in (
        (["--editor-socket", "not-a-port"], "invalid --editor-socket"),
        (["--editor-socket", "99999"], "invalid --editor-socket"),
        (["--instance", "abc"], "invalid --instance"),
        (["--log-level", "verbose"], "invalid --log-level"),
        (["--approval-mode", "maybe"], "invalid --approval-mode"),
        (["--handshake-timeout", "0"], "invalid --handshake-timeout"),
        (["--project"], "--project requires a value"),
    ):
        result = run_relay(args)
        assert_eq(result.returncode, 2, "exit code for %s" % args)
        assert_in(needle, result.stderr.decode(), "stderr for %s" % args)


# ------------------------------------------------------------------ handshake ---


@test
def test_handshake_carries_client_identity_and_mode():
    """R3/F6: the editor learns who is connecting and under which policy."""
    editor = FakeEditor()
    try:
        with connected_relay(
            editor,
            extra_args=["--read-only", "--approval-mode", "allow", "--client-name", "test-client"],
        ):
            wait_for(lambda: editor.handshake_params is not None, message="handshake")
            params = editor.handshake_params
            assert_eq(params["read_only"], True, "read_only")
            assert_eq(params["approval_mode"], "allow", "approval_mode")
            assert_eq(params["client_name"], "test-client", "client_name")
            assert_eq(params["bridge_versions"], ["1"], "bridge_versions")
            if not isinstance(params.get("pid"), (int, float)):
                raise AssertionError("handshake must carry the relay pid")
    finally:
        editor.close()


@test
def test_handshake_response_is_not_forwarded_to_the_client():
    """R7: bridge-level traffic must not leak into the MCP stream."""
    editor = FakeEditor()
    try:
        with connected_relay(editor) as relay:
            wait_for(lambda: editor.handshake_params is not None, message="handshake")
            relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})
            message = relay.read_message()
            assert_eq(message["id"], 1, "first client-visible response id")
            _, stdout, _ = relay.finish()
            assert_eq(stdout, "", "no trailing frames")
    finally:
        editor.close()


@test
def test_bridge_version_mismatch_is_reported_actionably():
    """R4: a mismatched editor produces a distinct, explanatory error."""
    editor = FakeEditor(bridge_version="99")
    try:
        with connected_relay(editor) as relay:
            relay.send_message({"jsonrpc": "2.0", "id": 7, "method": "tools/list"})
            message = relay.read_message()
            assert_eq(message["id"], 7, "id")
            assert_eq(message["error"]["code"], -32003, "error code")
            assert_in("bridge protocol mismatch", message["error"]["message"], "message")
            assert_in("update the relay and the editor", message["error"]["message"], "remedy")
    finally:
        editor.close()


@test
def test_rejected_handshake_is_reported_and_not_retried():
    """F6: an editor that denies the client says so on every request."""
    editor = FakeEditor(reject_reason="client not approved by the user")
    try:
        with connected_relay(editor) as relay:
            for request_id in (1, 2):
                relay.send_message({"jsonrpc": "2.0", "id": request_id, "method": "tools/list"})
                message = relay.read_message()
                assert_eq(message["id"], request_id, "id")
                assert_eq(message["error"]["code"], -32004, "error code")
                assert_in("not approved", message["error"]["message"], "reason")
            # Only one handshake attempt: a denial is not retried in a tight loop.
            assert_eq(len(editor.requests_for("godot/hello")), 1, "handshake attempts")
    finally:
        editor.close()


@test
def test_malformed_handshake_response_is_retryable():
    """R4: a garbled handshake is transient, unlike a denial."""
    editor = FakeEditor(raw_handshake_response="this is not json")
    try:
        with connected_relay(editor) as relay:
            relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})
            message = relay.read_message()
            assert_eq(message["error"]["code"], -32001, "error code")
            relay.send_message({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
            relay.read_message()
            wait_for(
                lambda: len(editor.requests_for("godot/hello")) >= 2,
                message="handshake retry",
            )
    finally:
        editor.close()


@test
def test_hung_editor_times_out_instead_of_stalling_the_relay():
    """R4/R6: an editor that accepts but never answers must not pin the relay."""
    editor = FakeEditor(answer_handshake=False)
    try:
        with connected_relay(editor, extra_args=["--handshake-timeout", "300"]) as relay:
            relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "ping"})
            message = relay.read_message(timeout=5)
            if message is None:
                raise AssertionError("relay never answered a request to a hung editor")
            assert_eq(message["error"]["code"], -32001, "error code")
            assert_in("did not answer the bridge handshake", message["error"]["message"], "message")
            exit_code, _, _ = relay.finish(timeout=5)
            assert_eq(exit_code, 0, "exit code")
    finally:
        editor.close()


# ------------------------------------------------------------------- forwarding ---


@test
def test_request_and_response_round_trip():
    """R1/R2: a normal request reaches the editor and its reply reaches the client."""
    editor = FakeEditor()
    try:
        with connected_relay(editor) as relay:
            relay.send_message({
                "jsonrpc": "2.0",
                "id": 1,
                "method": "tools/call",
                "params": {"name": "Godot_ListScenes", "arguments": {}},
            })
            message = relay.read_message()
            assert_eq(message["result"], {"echo": "tools/call"}, "result")
            forwarded = [m for m in editor.received if m.get("method") == "tools/call"]
            assert_eq(len(forwarded), 1, "forwarded requests")
            assert_eq(forwarded[0]["params"]["name"], "Godot_ListScenes", "params survived")
    finally:
        editor.close()


@test
def test_multiple_messages_in_one_write():
    """R2: several frames arriving in a single read are all handled."""
    editor = FakeEditor()
    try:
        with connected_relay(editor) as relay:
            payload = "".join(
                json.dumps({"jsonrpc": "2.0", "id": i, "method": "ping"}) + "\n"
                for i in range(1, 4)
            )
            relay.send_raw(payload)
            ids = sorted(relay.read_message()["id"] for _ in range(3))
            assert_eq(ids, [1, 2, 3], "response ids")
    finally:
        editor.close()


@test
def test_partial_frame_is_buffered_until_complete():
    """R2: a frame split across writes is not truncated or misparsed."""
    editor = FakeEditor()
    try:
        with connected_relay(editor) as relay:
            text = json.dumps({"jsonrpc": "2.0", "id": 9, "method": "tools/list"})
            relay.send_raw(text[:12])
            time.sleep(0.15)
            relay.send_raw(text[12:])
            time.sleep(0.15)
            relay.send_raw("\n")
            message = relay.read_message()
            assert_eq(message["id"], 9, "id")
    finally:
        editor.close()


@test
def test_crlf_line_endings_are_accepted():
    """R2/R8: Windows clients terminate frames with CRLF."""
    editor = FakeEditor()
    try:
        with connected_relay(editor) as relay:
            relay.send_raw(json.dumps({"jsonrpc": "2.0", "id": 5, "method": "ping"}) + "\r\n")
            assert_eq(relay.read_message()["id"], 5, "id")
    finally:
        editor.close()


@test
def test_notifications_are_forwarded_without_a_response():
    """P5/R2: id-less messages pass through and produce no reply."""
    editor = FakeEditor()
    try:
        with connected_relay(editor) as relay:
            relay.send_message({"jsonrpc": "2.0", "method": "notifications/initialized"})
            wait_for(
                lambda: editor.requests_for("notifications/initialized"),
                message="notification forwarded",
            )
            relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "ping"})
            assert_eq(relay.read_message()["id"], 1, "only the request is answered")
    finally:
        editor.close()


@test
def test_editor_initiated_notifications_reach_the_client():
    """P5: tools/list_changed originates in the editor and must flow outward."""
    editor = FakeEditor()
    try:
        with connected_relay(editor) as relay:
            wait_for(lambda: editor.handshake_params is not None, message="handshake")
            editor.send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
            message = relay.read_message()
            assert_eq(message["method"], "notifications/tools/list_changed", "method")
    finally:
        editor.close()


@test
def test_unicode_payloads_survive_the_round_trip():
    """R2: non-ASCII content must not be corrupted or re-escaped incorrectly."""
    editor = FakeEditor()
    text = "scène — ノード éè"
    editor.set_responder(
        lambda message: {"jsonrpc": "2.0", "id": message["id"], "result": {"text": text}}
    )
    try:
        with connected_relay(editor) as relay:
            relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "echo", "params": {"text": text}})
            message = relay.read_message()
            assert_eq(message["result"]["text"], text, "round-tripped text")
            assert_eq(editor.received[-1]["params"]["text"], text, "forwarded text")
    finally:
        editor.close()


# ------------------------------------------------------------ malformed input ---


@test
def test_malformed_client_frame_yields_parse_error_and_is_not_forwarded():
    """P4/R2: bad client input is answered locally, never relayed."""
    editor = FakeEditor()
    try:
        with connected_relay(editor) as relay:
            wait_for(lambda: editor.handshake_params is not None, message="handshake")
            relay.send_line("{not json at all")
            message = relay.read_message()
            assert_eq(message["id"], None, "id must be null")
            assert_eq(message["error"]["code"], -32700, "parse error code")
            relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "ping"})
            assert_eq(relay.read_message()["id"], 1, "relay still healthy")
            methods = [m.get("method") for m in editor.received]
            if any(m is None for m in methods):
                raise AssertionError("malformed frame was forwarded to the editor")
    finally:
        editor.close()


@test
def test_non_object_client_frame_is_an_invalid_request():
    """P4: a bare JSON array/scalar is not a JSON-RPC message."""
    editor = FakeEditor()
    try:
        with connected_relay(editor) as relay:
            relay.send_line("[1, 2, 3]")
            message = relay.read_message()
            assert_eq(message["error"]["code"], -32600, "invalid request code")
    finally:
        editor.close()


@test
def test_blank_lines_are_ignored():
    """R2: keep-alive newlines must not produce parse errors."""
    editor = FakeEditor()
    try:
        with connected_relay(editor) as relay:
            relay.send_raw("\n   \n\t\n")
            relay.send_message({"jsonrpc": "2.0", "id": 3, "method": "ping"})
            assert_eq(relay.read_message()["id"], 3, "id")
    finally:
        editor.close()


@test
def test_malformed_editor_frame_is_dropped_not_forwarded():
    """R7: stdout purity holds even when the editor misbehaves."""
    editor = FakeEditor()
    try:
        with connected_relay(editor) as relay:
            wait_for(lambda: editor.handshake_params is not None, message="handshake")
            editor.send_raw("garbage not json")
            editor.send_raw("[1,2,3]")
            editor.send({"jsonrpc": "2.0", "id": 1, "result": {"ok": True}})
            message = relay.read_message()
            assert_eq(message["result"], {"ok": True}, "only the valid frame arrives")
    finally:
        editor.close()


# ------------------------------------------------------- editor availability ---


@test
def test_editor_unavailable_produces_an_actionable_error():
    """R4: with no editor running, requests fail cleanly instead of hanging."""
    with RelayProcess() as relay:
        relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})
        message = relay.read_message()
        assert_eq(message["id"], 1, "id")
        assert_eq(message["error"]["code"], -32001, "error code")
        assert_in("no running Godot editor", message["error"]["message"], "message")
        exit_code, _, stderr = relay.finish()
        assert_eq(exit_code, 0, "exit code")
        assert_in("editor", stderr, "stderr diagnostic")


@test
def test_notification_is_dropped_when_the_editor_is_unavailable():
    """R4/R7: an unanswerable notification must not fabricate a response."""
    with RelayProcess() as relay:
        relay.send_message({"jsonrpc": "2.0", "method": "notifications/initialized"})
        time.sleep(0.3)
        exit_code, stdout, _ = relay.finish()
        assert_eq(exit_code, 0, "exit code")
        assert_eq(stdout, "", "stdout must stay empty")


@test
def test_stale_instance_descriptor_is_pruned():
    """R4: a descriptor for a dead editor is removed, not retried forever."""
    port = free_port()
    home = tempfile.mkdtemp(prefix="godot-ai-relay-stale-")
    os.makedirs(os.path.join(home, "instances"), exist_ok=True)
    try:
        relay = RelayProcess(home=home)
        descriptor = relay.write_instance(port=port)
        try:
            relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})
            message = relay.read_message()
            assert_eq(message["error"]["code"], -32001, "error code")
            wait_for(lambda: not os.path.exists(descriptor), message="descriptor pruned")
            _, _, stderr = relay.finish()
            assert_in("could not connect", stderr, "stderr explains the refused connection")
            assert_in("pruning stale instance descriptor", stderr, "stderr records the prune")
        finally:
            relay.cleanup()
    finally:
        shutil.rmtree(home, ignore_errors=True)


@test
def test_explicit_editor_socket_bypasses_discovery():
    """R3: --editor-socket connects without consulting the registry."""
    editor = FakeEditor()
    try:
        with RelayProcess(args=["--editor-socket", str(editor.port)]) as relay:
            relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "ping"})
            assert_eq(relay.read_message()["id"], 1, "id")
        with RelayProcess(args=["--editor-socket", "127.0.0.1:%d" % editor.port]):
            wait_for(lambda: len(editor.requests_for("godot/hello")) >= 2, message="host:port form")
    finally:
        editor.close()


@test
def test_ambiguous_instances_require_an_explicit_selector():
    """R3: two editors running is a user decision, not a coin flip."""
    first = FakeEditor(project_path="/tmp/alpha")
    second = FakeEditor(project_path="/tmp/beta")
    try:
        with RelayProcess() as relay:
            relay.write_instance(port=first.port, pid=101, project_path="/tmp/alpha", started_at=1.0)
            relay.write_instance(port=second.port, pid=102, project_path="/tmp/beta", started_at=2.0)
            relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "ping"})
            message = relay.read_message()
            assert_eq(message["error"]["code"], -32001, "error code")
            assert_in("--project or --instance", message["error"]["message"], "message")
    finally:
        first.close()
        second.close()


@test
def test_instance_selection_by_project_and_pid():
    """R3: --project and --instance each pick the intended editor."""
    first = FakeEditor(project_path="/tmp/alpha")
    second = FakeEditor(project_path="/tmp/beta")
    try:
        for args, expected in (
            (["--project", "/tmp/beta"], second),
            (["--instance", "101"], first),
        ):
            with RelayProcess(args=args) as relay:
                relay.write_instance(port=first.port, pid=101, project_path="/tmp/alpha", started_at=1.0)
                relay.write_instance(port=second.port, pid=102, project_path="/tmp/beta", started_at=2.0)
                relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "ping"})
                assert_eq(relay.read_message()["id"], 1, "id for %s" % args)
                wait_for(
                    lambda e=expected: len(e.requests_for("godot/hello")) >= 1,
                    message="handshake with the selected editor",
                )
    finally:
        first.close()
        second.close()


@test
def test_unknown_project_selector_reports_the_project():
    """R3: a wrong --project value explains what was not found."""
    editor = FakeEditor(project_path="/tmp/alpha")
    try:
        with RelayProcess(args=["--project", "/tmp/nothing-here"]) as relay:
            relay.write_instance(port=editor.port, pid=101, project_path="/tmp/alpha")
            relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "ping"})
            message = relay.read_message()
            assert_in("/tmp/nothing-here", message["error"]["message"], "message")
    finally:
        editor.close()


@test
def test_corrupt_instance_descriptor_is_skipped():
    """R4: an unparsable descriptor must not crash discovery."""
    editor = FakeEditor()
    try:
        with RelayProcess() as relay:
            with open(os.path.join(relay.home, "instances", "bad.json"), "w") as handle:
                handle.write("{ this is not json")
            relay.write_instance(port=editor.port)
            relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "ping"})
            assert_eq(relay.read_message()["id"], 1, "id")
    finally:
        editor.close()


@test
def test_editor_disconnect_fails_inflight_requests():
    """R4: losing the editor mid-request reports an error rather than hanging."""
    editor = FakeEditor()
    editor.set_responder(lambda message: None)  # Never answers.
    try:
        with connected_relay(editor) as relay:
            relay.send_message({"jsonrpc": "2.0", "id": 42, "method": "tools/call"})
            wait_for(lambda: editor.requests_for("tools/call"), message="request forwarded")
            editor.close_connection()
            message = relay.read_message()
            assert_eq(message["id"], 42, "id")
            assert_eq(message["error"]["code"], -32002, "error code")
            assert_in("closed the connection", message["error"]["message"], "message")
    finally:
        editor.close()


@test
def test_relay_reconnects_after_the_editor_returns():
    """R4: a dropped connection is re-established without restarting the relay."""
    editor = FakeEditor()
    try:
        with connected_relay(editor) as relay:
            relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "ping"})
            assert_eq(relay.read_message()["id"], 1, "first response")
            editor.close_connection()
            time.sleep(0.3)
            relay.send_message({"jsonrpc": "2.0", "id": 2, "method": "ping"})
            message = relay.read_message()
            if message is None:
                raise AssertionError("relay hung after the editor dropped the connection")
            assert_eq(message["id"], 2, "reconnected response")
            if "error" in message:
                raise AssertionError("expected a successful reconnect, got %r" % message)
            assert_eq(editor.connection_count, 2, "editor accepted a second connection")
    finally:
        editor.close()


# ---------------------------------------------------------------- lifecycle ---


@test
def test_stdin_eof_shuts_down_cleanly():
    """R6: closing stdin terminates the relay and releases the editor socket."""
    editor = FakeEditor()
    try:
        relay = connected_relay(editor)
        try:
            wait_for(lambda: editor.connected.is_set(), message="editor connection")
            exit_code, _, _ = relay.finish()
            assert_eq(exit_code, 0, "exit code")
            wait_for(lambda: editor.disconnected.is_set(), message="socket released")
        finally:
            relay.cleanup()
    finally:
        editor.close()


@test
def test_sigterm_shuts_down_cleanly():
    """R6: a terminating signal is handled instead of killing mid-write."""
    editor = FakeEditor()
    try:
        relay = connected_relay(editor)
        try:
            wait_for(lambda: editor.connected.is_set(), message="editor connection")
            relay.process.terminate()
            assert_eq(relay.wait(timeout=5), 0, "exit code")
            wait_for(lambda: editor.disconnected.is_set(), message="socket released")
        finally:
            relay.cleanup()
    finally:
        editor.close()


@test
def test_no_child_processes_are_spawned():
    """R6: the relay is a pipe, not a process launcher."""
    editor = FakeEditor()
    try:
        with connected_relay(editor) as relay:
            wait_for(lambda: editor.connected.is_set(), message="editor connection")
            children = subprocess.run(
                ["ps", "-o", "pid=", "--ppid", str(relay.process.pid)],
                stdout=subprocess.PIPE,
            ).stdout.decode().strip()
            assert_eq(children, "", "child processes")
    finally:
        editor.close()


@test
def test_debug_logging_stays_on_stderr():
    """R7: even the noisiest log level cannot contaminate stdout."""
    editor = FakeEditor()
    try:
        with connected_relay(editor, extra_args=["--log-level", "debug"]) as relay:
            relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "ping"})
            assert_eq(relay.read_message()["id"], 1, "id")
            exit_code, stdout, stderr = relay.finish()
            assert_eq(exit_code, 0, "exit code")
            assert_eq(stdout, "", "no extra stdout")
            assert_in("-> editor", stderr, "debug log present on stderr")
    finally:
        editor.close()


@test
def test_every_stdout_line_is_valid_json():
    """R7: the contract is one JSON object per line, always."""
    editor = FakeEditor()
    try:
        with connected_relay(editor) as relay:
            relay.send_line("{bad")
            relay.send_message({"jsonrpc": "2.0", "id": 1, "method": "ping"})
            for _ in range(2):
                line = relay.read_line()
                if line is None:
                    raise AssertionError("expected two stdout lines")
                parsed = json.loads(line)
                if not isinstance(parsed, dict):
                    raise AssertionError("stdout line is not a JSON object: %r" % line)
                assert_eq(parsed["jsonrpc"], "2.0", "jsonrpc member")
    finally:
        editor.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-k", dest="filter", default=None, help="only run matching tests")
    parser.add_argument("-v", dest="verbose", action="store_true")
    args = parser.parse_args()

    if not os.path.exists(RELAY_BINARY):
        print("relay binary missing: run tools/relay/build.sh first", file=sys.stderr)
        return 2

    selected = [t for t in TESTS if not args.filter or args.filter in t.__name__]
    failures = []
    for func in selected:
        name = func.__name__
        try:
            func()
            print("PASS %s" % name)
        except Exception as error:  # noqa: BLE001 - test runner boundary
            failures.append((name, error, traceback.format_exc()))
            print("FAIL %s: %s" % (name, error))
            if args.verbose:
                print(failures[-1][2])

    print("\n%d passed, %d failed, %d total" % (
        len(selected) - len(failures), len(failures), len(selected)))
    if failures:
        print("\nFailures:")
        for name, error, _ in failures:
            print("- %s: %s" % (name, error))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
