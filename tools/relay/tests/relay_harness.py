"""Process harness for driving the real godot-ai-relay binary from tests."""

import json
import os
import shutil
import subprocess
import tempfile
import time

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
RELAY_BINARY = os.path.join(REPO_ROOT, "bin", "godot-ai-relay")


class RelayProcess:
    """Runs the relay with a private state directory and line-oriented stdio."""

    def __init__(self, args=None, home=None, env=None, owns_home=None):
        if not os.path.exists(RELAY_BINARY):
            raise RuntimeError(
                "relay binary not found at %s; run tools/relay/build.sh" % RELAY_BINARY
            )
        self.home = home or tempfile.mkdtemp(prefix="godot-ai-relay-test-")
        # A caller that had to create the directory itself (to seed it before the
        # relay starts) can still hand over cleanup.
        self._owns_home = (home is None) if owns_home is None else owns_home
        os.makedirs(os.path.join(self.home, "instances"), exist_ok=True)

        process_env = dict(os.environ)
        process_env["GODOT_AI_HOME"] = self.home
        if env:
            process_env.update(env)

        self.process = subprocess.Popen(
            [RELAY_BINARY] + list(args or []),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=process_env,
        )

    # -- instance registry -------------------------------------------------

    def write_instance(self, port, pid=4242, project_path="/tmp/project",
                       project_name="Test", started_at=1000.0,
                       protocol_version="1", editor_version="4.3.dev"):
        descriptor = {
            "pid": pid,
            "port": port,
            "project_path": project_path,
            "project_name": project_name,
            "editor_version": editor_version,
            "protocol_version": protocol_version,
            "started_at": started_at,
        }
        path = os.path.join(self.home, "instances", "%d.json" % pid)
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(descriptor, handle)
        return path

    # -- stdio -------------------------------------------------------------

    def send_raw(self, text):
        self.process.stdin.write(text.encode("utf-8"))
        self.process.stdin.flush()

    def send_line(self, text):
        self.send_raw(text + "\n")

    def send_message(self, message):
        self.send_line(json.dumps(message))

    def read_message(self, timeout=5.0):
        line = self.read_line(timeout=timeout)
        if line is None:
            return None
        return json.loads(line)

    def read_line(self, timeout=5.0):
        deadline = time.time() + timeout
        os.set_blocking(self.process.stdout.fileno(), False)
        buffer = b""
        while time.time() < deadline:
            chunk = self.process.stdout.readline()
            if chunk:
                buffer += chunk
                if buffer.endswith(b"\n"):
                    return buffer.decode("utf-8").rstrip("\n")
                continue
            if self.process.poll() is not None and not chunk:
                if buffer:
                    return buffer.decode("utf-8").rstrip("\n")
                return None
            time.sleep(0.01)
        return None

    def drain_stderr(self):
        """Everything written to stderr so far, without blocking.

        The HTTP mode announces a generated token there, and a test that blocked on a
        pipe the relay is not finished with would hang instead of reading it.
        """
        return self._drain(self.process.stderr)

    def drain_stdout(self):
        """Everything written to stdout so far, without blocking."""
        return self._drain(self.process.stdout)

    @staticmethod
    def _drain(stream):
        if stream is None or stream.closed:
            return ""
        os.set_blocking(stream.fileno(), False)
        chunks = []
        while True:
            try:
                chunk = stream.read()
            except (BlockingIOError, ValueError):
                break
            if not chunk:
                break
            chunks.append(chunk)
        return b"".join(chunks).decode("utf-8", "replace")

    def close_stdin(self):
        if self.process.stdin and not self.process.stdin.closed:
            self.process.stdin.close()

    def wait(self, timeout=5.0):
        return self.process.wait(timeout=timeout)

    def finish(self, timeout=5.0):
        """Closes stdin, waits for exit, returns (exit_code, stdout, stderr)."""
        self.close_stdin()
        try:
            self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=timeout)
            raise
        stdout = self.process.stdout.read().decode("utf-8")
        stderr = self.process.stderr.read().decode("utf-8")
        return self.process.returncode, stdout, stderr

    def cleanup(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            try:
                if stream and not stream.closed:
                    stream.close()
            except OSError:
                pass
        if self._owns_home:
            shutil.rmtree(self.home, ignore_errors=True)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.cleanup()


def run_relay_one_shot(args, home, timeout=30.0):
    """Runs the relay in --call mode with a private state directory."""
    process_env = dict(os.environ)
    process_env["GODOT_AI_HOME"] = home
    return subprocess.run(
        [RELAY_BINARY] + list(args),
        input=b"",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        env=process_env,
    )


def run_relay(args, timeout=10.0, env=None):
    """Runs the relay to completion with empty stdin. Returns CompletedProcess."""
    process_env = dict(os.environ)
    if env:
        process_env.update(env)
    return subprocess.run(
        [RELAY_BINARY] + list(args),
        input=b"",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        env=process_env,
    )
