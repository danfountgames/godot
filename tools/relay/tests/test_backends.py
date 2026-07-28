#!/usr/bin/env python3
"""Tests for the packaged agent backends.

Imported by run_tests.py. These run the real binary against real files.

The three things worth pinning down are the ones a user would only discover the hard
way: that a secret never reaches the file, that installing does not destroy the rest
of a client's configuration, and that a stale entry is reported rather than left to
fail mysteriously against a bridge it cannot speak.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from relay_harness import RELAY_BINARY  # noqa: E402


def run_relay(args):
    """Runs the relay to completion and returns (status, stdout, stderr)."""
    result = subprocess.run([RELAY_BINARY] + list(args), stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=60)
    return result.returncode, result.stdout.decode("utf-8"), result.stderr.decode("utf-8")


def register(test, assert_eq, assert_in):
    """Registers these cases with run_tests.py's runner."""

    @test
    def test_backends_can_be_listed():
        status, out, _ = run_relay(["--list-backends"])
        assert_eq(status, 0, "exit status")
        assert_in("stdio", out, "listing")
        assert_in("http", out, "listing")
        assert_in("never written to the file", out, "listing explains token handling")

    @test
    def test_installing_a_backend_writes_a_usable_entry():
        directory = tempfile.mkdtemp(prefix="godot-ai-backend-")
        try:
            # A path whose directory does not exist yet: a client that has never been
            # configured has no configuration directory either.
            path = os.path.join(directory, "nested", "config.json")
            status, out, err = run_relay(["--install-backend", "stdio",
                                          "--backend-config", path,
                                          "--project", "/tmp/demo-project",
                                          "--read-only",
                                          "--client-name", "demo"])
            assert_eq(status, 0, "exit status: %s" % err)
            assert_in("added", out, "summary")

            with open(path) as handle:
                document = json.load(handle)
            entry = document["mcpServers"]["godot-ai"]
            assert_eq(entry["type"], "stdio", "transport")
            assert_eq(entry["command"], os.path.realpath(RELAY_BINARY), "command path")
            assert_in("--project", entry["args"], "args carry the project")
            assert_in("/tmp/demo-project", entry["args"], "args carry the project path")
            assert_in("--read-only", entry["args"], "args carry the policy")
            assert_eq(entry["x-godot-ai"]["backend"], "stdio", "pinned backend")
            # Version pinning: what wrote it, and against which bridge.
            assert_in("relay_version", entry["x-godot-ai"], "pinned relay version")
            assert_in("bridge_version", entry["x-godot-ai"], "pinned bridge version")
        finally:
            shutil.rmtree(directory, ignore_errors=True)

    @test
    def test_installing_preserves_other_servers_and_replaces_our_own():
        directory = tempfile.mkdtemp(prefix="godot-ai-backend-")
        try:
            path = os.path.join(directory, "config.json")
            with open(path, "w") as handle:
                json.dump({"mcpServers": {"someone-else": {"command": "/bin/true"}},
                           "unrelatedSetting": 42}, handle)

            status, out, err = run_relay(["--install-backend", "stdio", "--backend-config", path])
            assert_eq(status, 0, "first install: %s" % err)
            assert_in("added", out, "first install summary")

            # Installing twice must update in place, not duplicate or multiply.
            status, out, err = run_relay(["--install-backend", "stdio", "--backend-config", path])
            assert_eq(status, 0, "second install: %s" % err)
            assert_in("updated", out, "second install summary")

            with open(path) as handle:
                document = json.load(handle)
            assert_eq(document["unrelatedSetting"], 42, "unrelated settings survive")
            assert_eq(document["mcpServers"]["someone-else"]["command"], "/bin/true",
                      "other servers survive")
            assert_eq(len(document["mcpServers"]), 2, "server count")
        finally:
            shutil.rmtree(directory, ignore_errors=True)

    @test
    def test_the_http_entry_names_a_variable_and_never_a_token():
        directory = tempfile.mkdtemp(prefix="godot-ai-backend-")
        try:
            path = os.path.join(directory, "config.json")
            status, _, err = run_relay(["--install-backend", "http", "--backend-config", path,
                                        "--http-port", "7345"])
            assert_eq(status, 0, "exit status: %s" % err)
            with open(path) as handle:
                raw = handle.read()
            entry = json.loads(raw)["mcpServers"]["godot-ai"]
            assert_eq(entry["type"], "http", "transport")
            assert_in("7345", entry["url"], "url carries the port")
            assert_eq(entry["headers"]["Authorization"], "Bearer ${GODOT_AI_HTTP_TOKEN}",
                      "authorization references the variable, not the value")
        finally:
            shutil.rmtree(directory, ignore_errors=True)

    @test
    def test_installing_refuses_to_write_a_token_into_the_file():
        directory = tempfile.mkdtemp(prefix="godot-ai-backend-")
        try:
            path = os.path.join(directory, "config.json")
            status, _, err = run_relay(["--install-backend", "http", "--backend-config", path,
                                        "--http-token", "s3cret-token-value"])
            assert_eq(status, 2, "exit status")
            assert_in("refusing", err, "refusal")
            assert_in("GODOT_AI_HTTP_TOKEN", err, "refusal names the alternative")
            # And nothing was written: a refusal that half-wrote the file would be worse
            # than no refusal at all.
            assert_eq(os.path.exists(path), False, "no file was created")
        finally:
            shutil.rmtree(directory, ignore_errors=True)

    @test
    def test_checking_reports_a_stale_entry():
        directory = tempfile.mkdtemp(prefix="godot-ai-backend-")
        try:
            path = os.path.join(directory, "config.json")
            run_relay(["--install-backend", "stdio", "--backend-config", path])

            status, out, _ = run_relay(["--check-backends", "--backend-config", path])
            assert_eq(status, 0, "status for a current entry")
            assert_in("current", out, "summary for a current entry")

            # An entry written by an older relay against an older bridge.
            with open(path) as handle:
                document = json.load(handle)
            document["mcpServers"]["godot-ai"]["x-godot-ai"]["bridge_version"] = "0"
            with open(path, "w") as handle:
                json.dump(document, handle)

            status, out, _ = run_relay(["--check-backends", "--backend-config", path])
            assert_eq(status, 1, "status for a stale entry")
            assert_in("--install-backend", out, "summary names the fix")

            # An entry somebody wrote by hand has no metadata to compare.
            document["mcpServers"]["godot-ai"].pop("x-godot-ai")
            with open(path, "w") as handle:
                json.dump(document, handle)
            status, out, _ = run_relay(["--check-backends", "--backend-config", path])
            assert_eq(status, 1, "status for a hand-written entry")
            assert_in("not written by this tool", out, "summary for a hand-written entry")
        finally:
            shutil.rmtree(directory, ignore_errors=True)

    @test
    def test_checking_a_missing_or_unconfigured_file_is_not_an_error():
        directory = tempfile.mkdtemp(prefix="godot-ai-backend-")
        try:
            path = os.path.join(directory, "config.json")
            status, out, _ = run_relay(["--check-backends", "--backend-config", path])
            assert_eq(status, 1, "status with no file")
            assert_in("no configuration", out, "summary with no file")

            with open(path, "w") as handle:
                json.dump({"mcpServers": {}}, handle)
            status, out, _ = run_relay(["--check-backends", "--backend-config", path])
            assert_eq(status, 1, "status with no entry")
            assert_in("run --install-backend", out, "summary names the fix")
        finally:
            shutil.rmtree(directory, ignore_errors=True)

    @test
    def test_backend_commands_refuse_bad_input():
        status, _, err = run_relay(["--install-backend", "no-such-backend",
                                    "--backend-config", "/tmp/whatever.json"])
        assert_eq(status, 2, "unknown backend status")
        assert_in("unknown backend", err, "unknown backend message")

        status, _, err = run_relay(["--install-backend", "stdio"])
        assert_eq(status, 2, "missing config path status")
        assert_in("--backend-config", err, "missing config path message")

        directory = tempfile.mkdtemp(prefix="godot-ai-backend-")
        try:
            path = os.path.join(directory, "config.json")
            with open(path, "w") as handle:
                handle.write("[not an object]")
            status, _, err = run_relay(["--install-backend", "stdio", "--backend-config", path])
            assert_eq(status, 2, "status for a non-object configuration")
            assert_in("not a JSON object", err, "message for a non-object configuration")
        finally:
            shutil.rmtree(directory, ignore_errors=True)
