#!/usr/bin/env python3
"""The editor serving MCP itself, with no relay process anywhere (DEC-0014).

Starts a headless editor, reads its instance descriptor for the HTTP port and token,
and drives the Streamable HTTP endpoint the way the terminal's agent does: initialize,
session header, tool calls, and the refusals - wrong token, unknown session, read-only.

Everything here goes through urllib. If a relay binary exists on this machine, nothing
in this test touches it; that is the point of the test.
"""
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.request
import urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import run_editor_e2e as e2e  # noqa: E402

FAILURES = []


def check(condition, message):
    if condition:
        print("PASS %s" % message)
    else:
        print("FAIL %s" % message)
        FAILURES.append(message)


def request(url, token, body=None, method=None, session=None, extra_headers=None):
    """One HTTP exchange; returns (status, headers, parsed-or-raw body)."""
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method or ("POST" if data else "GET"))
    if token is not None:
        req.add_header("Authorization", "Bearer %s" % token)
    if session:
        req.add_header("Mcp-Session-Id", session)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    for name, value in (extra_headers or {}).items():
        req.add_header(name, value)
    try:
        with urllib.request.urlopen(req, timeout=30) as reply:
            raw = reply.read().decode()
            parsed = json.loads(raw) if raw else None
            return reply.status, dict(reply.headers), parsed
    except urllib.error.HTTPError as refusal:
        return refusal.code, dict(refusal.headers), None


def main():
    editor_binary = e2e.default_editor()
    if not os.path.exists(editor_binary):
        print("SKIP: no editor at %s" % editor_binary)
        return 0

    workspace = tempfile.mkdtemp(prefix="godot-ai-http-e2e-")
    project = os.path.join(workspace, "project")
    home = os.path.join(workspace, "home")
    os.makedirs(home)
    e2e.build_project(project)

    environment = dict(os.environ)
    environment["GODOT_AI_HOME"] = home
    environment["GODOT_AI_AUTO_APPROVE"] = "1"

    editor = subprocess.Popen(
        [editor_binary, "--path", project, "--editor", "--headless"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env=environment, cwd=workspace,
    )
    try:
        descriptor = e2e.wait_for_instance(os.path.join(home, "instances"), editor)
        check("http_port" in descriptor and descriptor["http_port"] > 0,
              "the descriptor advertises an HTTP port (%r)" % descriptor.get("http_port"))
        check(len(descriptor.get("http_token", "")) == 32,
              "the descriptor carries a 32-hex token")
        descriptor_file = [os.path.join(home, "instances", entry)
                          for entry in os.listdir(os.path.join(home, "instances"))][0]
        mode = os.stat(descriptor_file).st_mode & 0o777
        check(mode == 0o600, "the descriptor holding the token is 0600 (got %o)" % mode)

        url = "http://127.0.0.1:%d/mcp" % descriptor["http_port"]
        token = descriptor["http_token"]

        status, _, _ = request(url, "not-the-token",
                               {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
        check(status == 401, "a wrong token is refused with 401 (%r)" % status)
        status, _, _ = request(url, None,
                               {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
        check(status == 401, "a missing token is refused with 401 (%r)" % status)

        initialize = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                      "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                                 "clientInfo": {"name": "http-e2e", "version": "1"}}}
        status, headers, reply = request(url, token, initialize,
                                         extra_headers={"X-Godot-AI-Client-Name": "http-e2e"})
        check(status == 200 and reply and "result" in reply, "initialize answered (%r)" % status)
        session = headers.get("Mcp-Session-Id", "")
        check(len(session) == 32, "initialize opened a session (%r)" % session)

        status, _, _ = request(url, token, {"jsonrpc": "2.0",
                                            "method": "notifications/initialized"},
                               session=session)
        check(status == 202, "a notification is accepted with 202 (%r)" % status)

        status, _, reply = request(url, token,
                                   {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                                   session=session)
        tools = [t["name"] for t in reply["result"]["tools"]] if reply and "result" in reply else []
        check(status == 200 and len(tools) > 50,
              "tools/list returned the registry over HTTP (%d tools)" % len(tools))

        status, _, reply = request(url, token,
                                   {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                                    "params": {"name": "Godot_GetEditorStatus", "arguments": {}}},
                                   session=session)
        content = reply.get("result", {}).get("structuredContent", {}) if reply else {}
        check(status == 200 and content.get("display_server") == "headless",
              "a tool call answered with real editor state (%r)" % content.get("display_server"))

        # A session opened without the id header cannot be resumed by guessing one.
        status, _, _ = request(url, token, {"jsonrpc": "2.0", "id": 4, "method": "tools/list"},
                               session="0" * 32)
        check(status == 404, "an unknown session id is 404, not a fresh session (%r)" % status)

        # Read-only, declared at session creation, enforced by the same permission layer.
        status, headers, reply = request(url, token, initialize,
                                         extra_headers={"X-Godot-AI-Read-Only": "1"})
        ro_session = headers.get("Mcp-Session-Id", "")
        status, _, reply = request(url, token,
                                   {"jsonrpc": "2.0", "id": 5, "method": "tools/call",
                                    "params": {"name": "Godot_WriteTextFile",
                                               "arguments": {"path": "res://x.txt",
                                                             "content": "no"}}},
                                   session=ro_session)
        # The permission layer refuses at the protocol level, before the tool runs, so
        # the shape is a JSON-RPC error naming the capability - not a tool result.
        error = (reply or {}).get("error", {})
        check(status == 200 and "read-only" in error.get("message", "") and
              error.get("data", {}).get("capability") == "edit_files",
              "a read-only session's write is refused for being read-only (%r)"
              % error.get("message", ""))
        check(not os.path.exists(os.path.join(project, "x.txt")),
              "and the file was really not written")

        status, _, _ = request(url, token, method="DELETE", session=session)
        check(status == 200, "DELETE ends the session (%r)" % status)
        status, _, _ = request(url, token, {"jsonrpc": "2.0", "id": 6, "method": "tools/list"},
                               session=session)
        check(status == 404, "the ended session is gone (%r)" % status)
    finally:
        editor.terminate()
        try:
            editor.wait(timeout=20)
        except subprocess.TimeoutExpired:
            editor.kill()

    print()
    if FAILURES:
        print("editor HTTP e2e: %d checks failed" % len(FAILURES))
        return 1
    print("editor HTTP e2e: all checks passed, no relay involved")
    return 0


if __name__ == "__main__":
    sys.exit(main())
