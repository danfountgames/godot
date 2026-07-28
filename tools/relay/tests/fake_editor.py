"""A scriptable stand-in for the editor-side MCP service.

The relay's real integration boundary is "NDJSON over a loopback socket", so the
relay tests drive the real relay binary against this fake endpoint rather than
against a mocked internal function. The editor module has its own tests.
"""

import json
import socket
import threading


def free_port():
    """Returns a port with nothing listening on it (for stale-instance tests)."""
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


class FakeEditor:
    """Accepts relay connections and answers bridge/protocol messages.

    Like the real editor service this accepts repeatedly, so reconnect paths are
    exercised against a listener that is actually still listening.
    """

    def __init__(
        self,
        bridge_version="1",
        reject_reason=None,
        editor_version="4.3.dev.godot-ai",
        project_path="/tmp/project",
        answer_handshake=True,
        raw_handshake_response=None,
    ):
        self.bridge_version = bridge_version
        self.reject_reason = reject_reason
        self.editor_version = editor_version
        self.project_path = project_path
        self.answer_handshake = answer_handshake
        self.raw_handshake_response = raw_handshake_response

        self.received = []           # Every decoded message from the relay.
        self.handshake_params = None
        self.connection_count = 0
        self.connected = threading.Event()
        self.disconnected = threading.Event()
        self._lock = threading.Lock()
        self._conn = None
        self._responder = None       # Optional callable(message) -> reply dict/None.

        self._server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server.bind(("127.0.0.1", 0))
        self._server.listen(1)
        self.port = self._server.getsockname()[1]

        self._stop = False
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def set_responder(self, responder):
        self._responder = responder

    def _serve(self):
        while not self._stop:
            try:
                conn, _ = self._server.accept()
            except OSError:
                return
            if self._stop:
                conn.close()
                return
            with self._lock:
                self._conn = conn
            self.connected.set()
            self.connection_count += 1
            buffer = b""
            try:
                while not self._stop:
                    chunk = conn.recv(8192)
                    if not chunk:
                        break
                    buffer += chunk
                    while b"\n" in buffer:
                        line, buffer = buffer.split(b"\n", 1)
                        if line.strip():
                            self._handle(line.decode("utf-8"))
            except (OSError, RuntimeError):
                pass
            finally:
                with self._lock:
                    if self._conn is conn:
                        self._conn = None
                try:
                    conn.close()
                except OSError:
                    pass
                self.disconnected.set()

    def _handle(self, line):
        message = json.loads(line)
        self.received.append(message)
        if message.get("method") == "godot/hello":
            self.handshake_params = message.get("params")
            if not self.answer_handshake:
                return
            if self.raw_handshake_response is not None:
                self.send_raw(self.raw_handshake_response)
                return
            if self.reject_reason:
                self.send({
                    "jsonrpc": "2.0",
                    "id": message["id"],
                    "error": {"code": -32000, "message": self.reject_reason},
                })
                return
            self.send({
                "jsonrpc": "2.0",
                "id": message["id"],
                "result": {
                    "bridge_version": self.bridge_version,
                    "editor_version": self.editor_version,
                    "project_path": self.project_path,
                },
            })
            return

        if self._responder is not None:
            reply = self._responder(message)
            if reply is not None:
                self.send(reply)
            return

        if "id" in message and message["id"] is not None:
            self.send({
                "jsonrpc": "2.0",
                "id": message["id"],
                "result": {"echo": message.get("method")},
            })

    def send(self, message):
        self.send_raw(json.dumps(message))

    def send_raw(self, text):
        with self._lock:
            if self._conn is None:
                return
            self._conn.sendall(text.encode("utf-8") + b"\n")

    def requests_for(self, method):
        return [m for m in self.received if m.get("method") == method]

    def close_connection(self):
        with self._lock:
            if self._conn is not None:
                try:
                    # shutdown() first: close() alone does not send FIN while another
                    # thread is blocked in recv() on the same fd, so the peer would not
                    # observe the disconnect until it next wrote.
                    self._conn.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                self._conn.close()
                self._conn = None

    def close(self):
        self._stop = True
        self.close_connection()
        try:
            # shutdown() before close() reliably wakes a thread blocked in accept().
            self._server.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self._server.close()
        except OSError:
            pass
        self._thread.join(timeout=2)
