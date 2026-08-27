#!/usr/bin/env python3
"""Call one Godot AI tool on an already-running editor.

This is intentionally tiny: it is useful while manually verifying editor tools and
keeps JSON framing details out of shell scripts. The tool result is written as JSON
to stdout; relay diagnostics remain on stderr.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from relay_harness import RelayProcess  # noqa: E402


def exchange(relay, message, timeout=30):
    relay.send_message(message)
    reply = relay.read_message(timeout=timeout)
    if reply is None:
        raise RuntimeError("no reply from the running editor")
    return reply


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("tool", nargs="?", default="")
    parser.add_argument("arguments", nargs="?", default="{}")
    parser.add_argument("--batch", default="", help="JSON array of {tool, arguments} calls")
    parser.add_argument("--home", default=os.environ.get("GODOT_AI_HOME", ""))
    parser.add_argument("--project", default="")
    parser.add_argument("--editor-socket", default="")
    parser.add_argument("--timeout", type=float, default=30.0)
    options = parser.parse_args()
    if not options.tool and not options.batch:
        parser.error("provide a tool or --batch")
    arguments = json.loads(options.arguments)
    if options.batch:
        with open(options.batch, encoding="utf-8") as handle:
            calls = json.load(handle)
    else:
        calls = [{"tool": options.tool, "arguments": arguments}]

    relay_args = ["--client-name", "manual-verification", "--approval-mode", "allow"]
    if options.project:
        relay_args.extend(["--project", options.project])
    if options.editor_socket:
        relay_args.extend(["--editor-socket", options.editor_socket])
    relay = RelayProcess(args=relay_args, home=options.home or None)
    try:
        initialized = exchange(
            relay,
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {},
                    "clientInfo": {"name": "manual-verification", "version": "1"},
                },
            },
            options.timeout,
        )
        if "error" in initialized:
            print(json.dumps(initialized, indent=2))
            return 2
        relay.send_message({"jsonrpc": "2.0", "method": "notifications/initialized"})
        replies = []
        failed = False
        for index, call in enumerate(calls, 2):
            reply = exchange(
                relay,
                {
                    "jsonrpc": "2.0",
                    "id": index,
                    "method": "tools/call",
                    "params": {
                        "name": call["tool"],
                        "arguments": call.get("arguments", {}),
                    },
                },
                options.timeout,
            )
            replies.append(reply)
            failed = failed or "error" in reply or reply.get("result", {}).get("isError") is True
        print(json.dumps(replies[0] if len(replies) == 1 else replies, indent=2))
        return 1 if failed else 0
    finally:
        relay.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
