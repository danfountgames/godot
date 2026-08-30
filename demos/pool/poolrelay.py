#!/usr/bin/env python3
"""Call POOL's editor through the relay, without fighting JSON quoting.

Everything the game was built from went through this: the point of the exercise is to
build a game the way the product intends, so nothing here touches the project's files
directly.

Read the three comments in `call` before writing your own version of this. Each of them
is a mistake that turned a working tool into a broken-looking one, and none failed
loudly.
"""
import json
import os
import subprocess
import sys

PROJECT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(PROJECT))
RELAY = os.environ.get("GODOT_AI_RELAY", os.path.join(REPO, "bin/godot-ai-relay"))
HOME = os.environ.get("GODOT_AI_HOME", "")


def call(tool, arguments=None, quiet=False):
    env = dict(os.environ)
    if HOME:
        env["GODOT_AI_HOME"] = HOME
    env["GODOT_AI_APPROVE_CLIENTS"] = "1"
    result = subprocess.run(
        [RELAY, "--call", tool, "--arguments", json.dumps(arguments or {}),
         "--project", PROJECT],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=180)
    try:
        payload = json.loads(result.stdout.decode() or "{}")
    except json.JSONDecodeError:
        print("!! %s: unparseable reply: %r" % (tool, result.stdout[:400].decode()))
        print("   stderr: %s" % result.stderr.decode()[:400])
        raise SystemExit(2)
    # Two different shapes of refusal, and both have to be read for their own text.
    #
    # A protocol-level rejection is a bare JSON-RPC error object at the top level, not
    # nested under "error": {"code": -32602, "message": "..."}.
    #
    # A tool-level refusal is a *successful* MCP result carrying isError, with the
    # reason in content[].text and nothing in "message" at all. Checking the exit status
    # first and reaching for payload["message"] reported every one of those as an empty
    # refusal - which reads exactly like a tool that will not say why, and sent me
    # looking for a defect in three tools that were answering perfectly well.
    if payload.get("isError"):
        text = " ".join(c.get("text", "") for c in payload.get("content", []))
        if not quiet:
            print("!! %s -> %s" % (tool, text[:400]))
        return {"_error": text}
    if result.returncode != 0 or ("code" in payload and "message" in payload):
        message = (payload.get("message") or payload.get("error")
                   or result.stderr.decode()[:200] or "exit %d, no message" % result.returncode)
        if not quiet:
            print("!! %s -> %s" % (tool, message))
        return {"_error": message}
    return payload.get("structuredContent", payload)


def batch(calls, quiet=False):
    """One connection for the whole list. A per-property --call costs a process launch
    and a handshake, so a six-property 'snapshot' of a running game is spread over
    several seconds - which is not a snapshot of anything."""
    env = dict(os.environ)
    if HOME:
        env["GODOT_AI_HOME"] = HOME
    env["GODOT_AI_APPROVE_CLIENTS"] = "1"
    payload = json.dumps([{"name": n, "arguments": a} for n, a in calls])
    result = subprocess.run(
        [RELAY, "--batch", "--continue-on-error", "--project", PROJECT],
        input=payload.encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env=env, timeout=180)
    try:
        out = json.loads(result.stdout.decode() or "[]")
    except json.JSONDecodeError:
        print("!! batch: unparseable reply: %r" % result.stdout[:400].decode())
        print("   stderr: %s" % result.stderr.decode()[:400])
        raise SystemExit(2)
    return out


def write(res_path, text):
    out = call("Godot_WriteTextFile", {"path": res_path, "text": text})
    if "_error" not in out:
        print("wrote %s (%s bytes)" % (res_path, out.get("bytes_written")))
    return out


def ok(result, label):
    if "_error" in result:
        print("FAILED: %s" % label)
        raise SystemExit(1)
    return result


if __name__ == "__main__":
    print(json.dumps(call(sys.argv[1], json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}),
                     indent=1)[:3000])
