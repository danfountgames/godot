"""MCP test client for the Godot MCP server.

A reusable pytest-compatible MCP client that handles session lifecycle,
tool invocation, resource reading, and SSE stream parsing over raw TCP.
"""

import json
import os
import socket
import time
from pathlib import Path
from typing import Any, Optional


MCP_HOST = "127.0.0.1"
MCP_PORT = 6009

# Default discovery file location (Godot writes this on startup).
_DISCOVERY_PATHS = [
    Path.home() / ".local" / "share" / "godot" / "mcp_server" / "discovery.json",
    Path.home() / "Library" / "Application Support" / "Godot" / "mcp_server" / "discovery.json",
    Path.home() / "AppData" / "Roaming" / "Godot" / "mcp_server" / "discovery.json",
]


def load_discovery() -> dict:
    """Load MCP discovery file to get endpoint and auth token."""
    for p in _DISCOVERY_PATHS:
        if p.exists():
            with open(p) as f:
                return json.load(f)
    return {}


# Auto-load token from discovery file on import.
_DISCOVERY = load_discovery()
MCP_AUTH_TOKEN: Optional[str] = _DISCOVERY.get("token") or os.environ.get("MCP_AUTH_TOKEN")


class MCPClient:
    """Low-level MCP client using raw TCP for precise control."""

    def __init__(self, host: str = MCP_HOST, port: int = MCP_PORT,
                 auth_token: Optional[str] = None):
        self.host = host
        self.port = port
        self.auth_token = auth_token or MCP_AUTH_TOKEN
        self.session_id: Optional[str] = None
        self.sock: Optional[socket.socket] = None
        self._next_id = 1

    def connect(self) -> None:
        """Open a TCP connection to the MCP server."""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(30.0)
        self.sock.connect((self.host, self.port))

    def disconnect(self) -> None:
        """Close the TCP connection."""
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None

    def reconnect(self) -> None:
        """Close and reopen the TCP connection (preserves session_id)."""
        self.disconnect()
        self.connect()

    def _next_request_id(self) -> int:
        rid = self._next_id
        self._next_id += 1
        return rid

    # -----------------------------------------------------------------------
    # HTTP transport
    # -----------------------------------------------------------------------

    def _send_http(
        self,
        method: str,
        body: Optional[str] = None,
        headers: Optional[dict] = None,
        content_type: str = "application/json",
        path: str = "/mcp",
    ) -> tuple[int, dict, str]:
        """Send an HTTP request and return (status_code, response_headers, body)."""
        if self.sock is None:
            self.connect()

        hdrs: dict[str, str] = {
            "Host": f"{self.host}:{self.port}",
            "Connection": "keep-alive",
        }
        if self.auth_token:
            hdrs["Authorization"] = f"Bearer {self.auth_token}"
        if body is not None:
            hdrs["Content-Type"] = content_type
            hdrs["Content-Length"] = str(len(body.encode("utf-8")))
        if self.session_id:
            hdrs["Mcp-Session-Id"] = self.session_id
        if headers:
            hdrs.update(headers)

        request_line = f"{method} {path} HTTP/1.1\r\n"
        header_lines = "".join(f"{k}: {v}\r\n" for k, v in hdrs.items())
        raw = request_line + header_lines + "\r\n"
        if body:
            raw += body

        self.sock.sendall(raw.encode("utf-8"))
        return self._read_response()

    def _read_response(self) -> tuple[int, dict, str]:
        """Read an HTTP response from the socket."""
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("Connection closed while reading headers")
            data += chunk

        header_end = data.index(b"\r\n\r\n") + 4
        header_block = data[:header_end].decode("utf-8")
        remaining = data[header_end:]

        lines = header_block.split("\r\n")
        status_line = lines[0]
        status_code = int(status_line.split(" ", 2)[1])

        resp_headers: dict[str, str] = {}
        content_length = 0
        is_chunked = False
        for line in lines[1:]:
            if ":" in line:
                key, value = line.split(":", 1)
                key_lower = key.strip().lower()
                resp_headers[key_lower] = value.strip()
                if key_lower == "content-length":
                    content_length = int(value.strip())
                if key_lower == "transfer-encoding" and "chunked" in value.lower():
                    is_chunked = True

        # Read body based on Content-Length.
        if content_length > 0:
            while len(remaining) < content_length:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                remaining += chunk
            body = remaining[:content_length].decode("utf-8")
        elif is_chunked:
            body = self._read_chunked(remaining)
        else:
            body = remaining.decode("utf-8") if remaining else ""

        return status_code, resp_headers, body

    def _read_chunked(self, initial: bytes) -> str:
        """Read chunked transfer encoding."""
        data = initial
        while True:
            # Find chunk size line.
            while b"\r\n" not in data:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                data += chunk
            line_end = data.index(b"\r\n")
            size_str = data[:line_end].decode("utf-8").strip()
            chunk_size = int(size_str, 16)
            data = data[line_end + 2:]
            if chunk_size == 0:
                break
            while len(data) < chunk_size + 2:
                more = self.sock.recv(4096)
                if not more:
                    break
                data += more
            data = data[chunk_size + 2:]  # Skip chunk data + trailing CRLF.
        return data.decode("utf-8") if data else ""

    # -----------------------------------------------------------------------
    # SSE (Server-Sent Events) support
    # -----------------------------------------------------------------------

    def _send_http_sse(
        self,
        body: str,
        headers: Optional[dict] = None,
    ) -> tuple[int, dict, socket.socket]:
        """Send an HTTP POST expecting an SSE response. Returns the socket for
        streaming reads instead of consuming the full body."""
        if self.sock is None:
            self.connect()

        hdrs: dict[str, str] = {
            "Host": f"{self.host}:{self.port}",
            "Accept": "text/event-stream",
            "Content-Type": "application/json",
            "Content-Length": str(len(body.encode("utf-8"))),
            "Connection": "keep-alive",
        }
        if self.auth_token:
            hdrs["Authorization"] = f"Bearer {self.auth_token}"
        if self.session_id:
            hdrs["Mcp-Session-Id"] = self.session_id
        if headers:
            hdrs.update(headers)

        request_line = "POST /mcp HTTP/1.1\r\n"
        header_lines = "".join(f"{k}: {v}\r\n" for k, v in hdrs.items())
        raw = request_line + header_lines + "\r\n" + body

        self.sock.sendall(raw.encode("utf-8"))

        # Read HTTP response headers only.
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("Connection closed")
            data += chunk

        header_end = data.index(b"\r\n\r\n") + 4
        header_block = data[:header_end].decode("utf-8")

        lines = header_block.split("\r\n")
        status_code = int(lines[0].split(" ", 2)[1])

        resp_headers: dict[str, str] = {}
        for line in lines[1:]:
            if ":" in line:
                key, value = line.split(":", 1)
                resp_headers[key.strip().lower()] = value.strip()

        # Push back any leftover data after headers.
        leftover = data[header_end:]
        if leftover:
            # Store leftover for the SSE reader.
            self._sse_buffer = leftover
        else:
            self._sse_buffer = b""

        return status_code, resp_headers, self.sock

    def read_sse_events(self, timeout: float = 10.0, max_events: int = 100) -> list[dict]:
        """Read SSE events from the socket after _send_http_sse(). Returns
        a list of parsed JSON objects from the SSE data fields."""
        events = []
        buffer = getattr(self, "_sse_buffer", b"")
        self.sock.settimeout(timeout)

        try:
            while len(events) < max_events:
                # Read more data.
                try:
                    chunk = self.sock.recv(4096)
                    if not chunk:
                        break
                    buffer += chunk
                except socket.timeout:
                    break

                # Parse complete SSE frames from buffer.
                while b"\n\n" in buffer:
                    frame_end = buffer.index(b"\n\n") + 2
                    frame = buffer[:frame_end].decode("utf-8")
                    buffer = buffer[frame_end:]

                    # Extract data lines from the frame.
                    data_lines = []
                    for line in frame.split("\n"):
                        if line.startswith("data: "):
                            data_lines.append(line[6:])
                        elif line.startswith("data:"):
                            data_lines.append(line[5:])

                    if data_lines:
                        try:
                            event_data = json.loads("\n".join(data_lines))
                            events.append(event_data)

                            # If this is a result (has "id" and "result"), we're done.
                            if "result" in event_data and "id" in event_data:
                                return events
                        except json.JSONDecodeError:
                            pass  # Skip malformed events.
        except Exception:
            pass
        finally:
            self.sock.settimeout(30.0)
            self._sse_buffer = b""

        return events

    def open_get_sse_stream(self) -> tuple[int, dict]:
        """Open a GET SSE stream for server push notifications."""
        if self.sock is None:
            self.connect()

        hdrs: dict[str, str] = {
            "Host": f"{self.host}:{self.port}",
            "Accept": "text/event-stream",
            "Connection": "keep-alive",
        }
        if self.auth_token:
            hdrs["Authorization"] = f"Bearer {self.auth_token}"
        if self.session_id:
            hdrs["Mcp-Session-Id"] = self.session_id

        request_line = "GET /mcp HTTP/1.1\r\n"
        header_lines = "".join(f"{k}: {v}\r\n" for k, v in hdrs.items())
        raw = request_line + header_lines + "\r\n"

        self.sock.sendall(raw.encode("utf-8"))

        # Read response headers.
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("Connection closed")
            data += chunk

        header_end = data.index(b"\r\n\r\n") + 4
        header_block = data[:header_end].decode("utf-8")

        lines = header_block.split("\r\n")
        status_code = int(lines[0].split(" ", 2)[1])

        resp_headers: dict[str, str] = {}
        for line in lines[1:]:
            if ":" in line:
                key, value = line.split(":", 1)
                resp_headers[key.strip().lower()] = value.strip()

        self._sse_buffer = data[header_end:]
        return status_code, resp_headers

    # -----------------------------------------------------------------------
    # MCP protocol methods
    # -----------------------------------------------------------------------

    def initialize(self) -> dict:
        """Perform the MCP initialize handshake."""
        rid = self._next_request_id()
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": rid,
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-06-18",
                "clientInfo": {"name": "mcp-test-client", "version": "1.0.0"},
                "capabilities": {},
            },
        })
        status, headers, resp_body = self._send_http("POST", body)
        assert status == 200, f"Initialize failed: HTTP {status} {resp_body}"

        self.session_id = headers.get("mcp-session-id")
        assert self.session_id, "No Mcp-Session-Id in response headers"

        result = json.loads(resp_body)
        return result

    def send_initialized(self) -> int:
        """Send notifications/initialized notification."""
        body = json.dumps({
            "jsonrpc": "2.0",
            "method": "notifications/initialized",
        })
        status, _, _ = self._send_http("POST", body)
        return status

    def full_handshake(self) -> dict:
        """Initialize + notifications/initialized in one call."""
        result = self.initialize()
        status = self.send_initialized()
        assert status == 202, f"Initialized notification failed: HTTP {status}"
        return result

    def call_tool(self, name: str, arguments: Optional[dict] = None) -> dict:
        """Call an MCP tool and return the parsed JSON-RPC response."""
        rid = self._next_request_id()
        params: dict[str, Any] = {"name": name}
        if arguments is not None:
            params["arguments"] = arguments
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": rid,
            "method": "tools/call",
            "params": params,
        })
        status, _, resp_body = self._send_http("POST", body)
        assert status == 200, f"Tool call failed: HTTP {status} {resp_body}"
        return json.loads(resp_body)

    def list_tools(self) -> dict:
        """Call tools/list and return the parsed response."""
        rid = self._next_request_id()
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": rid,
            "method": "tools/list",
        })
        status, _, resp_body = self._send_http("POST", body)
        assert status == 200, f"tools/list failed: HTTP {status} {resp_body}"
        return json.loads(resp_body)

    def list_resources(self) -> dict:
        """Call resources/list."""
        rid = self._next_request_id()
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": rid,
            "method": "resources/list",
        })
        status, _, resp_body = self._send_http("POST", body)
        assert status == 200, f"resources/list failed: HTTP {status} {resp_body}"
        return json.loads(resp_body)

    def read_resource(self, uri: str) -> dict:
        """Call resources/read and return the parsed response."""
        rid = self._next_request_id()
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": rid,
            "method": "resources/read",
            "params": {"uri": uri},
        })
        status, _, resp_body = self._send_http("POST", body)
        return json.loads(resp_body)

    def list_resource_templates(self) -> dict:
        """Call resources/templates/list."""
        rid = self._next_request_id()
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": rid,
            "method": "resources/templates/list",
        })
        status, _, resp_body = self._send_http("POST", body)
        assert status == 200, f"resources/templates/list failed: HTTP {status} {resp_body}"
        return json.loads(resp_body)

    def subscribe_resource(self, uri: str) -> int:
        """Send resources/subscribe notification."""
        body = json.dumps({
            "jsonrpc": "2.0",
            "method": "resources/subscribe",
            "params": {"uri": uri},
        })
        status, _, _ = self._send_http("POST", body)
        return status

    def set_log_level(self, level: str) -> dict:
        """Send logging/setLevel."""
        rid = self._next_request_id()
        body = json.dumps({
            "jsonrpc": "2.0",
            "id": rid,
            "method": "logging/setLevel",
            "params": {"level": level},
        })
        status, _, resp_body = self._send_http("POST", body)
        return json.loads(resp_body) if resp_body else {}

    def ping(self) -> dict:
        """Send a JSON-RPC ping."""
        rid = self._next_request_id()
        body = json.dumps({"jsonrpc": "2.0", "id": rid, "method": "ping"})
        status, _, resp_body = self._send_http("POST", body)
        return json.loads(resp_body)

    def send_cancel(self, request_id: Any, reason: str = "cancelled by test") -> int:
        """Send notifications/cancelled."""
        body = json.dumps({
            "jsonrpc": "2.0",
            "method": "notifications/cancelled",
            "params": {"requestId": request_id, "reason": reason},
        })
        status, _, _ = self._send_http("POST", body)
        return status

    def delete_session(self) -> int:
        """Send DELETE to terminate the session."""
        status, _, _ = self._send_http("DELETE")
        return status

    def send_raw(self, raw_bytes: bytes) -> tuple[int, dict, str]:
        """Send raw bytes for edge-case testing."""
        if self.sock is None:
            self.connect()
        self.sock.sendall(raw_bytes)
        return self._read_response()

    def send_jsonrpc(self, method: str, params: Optional[dict] = None,
                     request_id: Optional[Any] = None) -> tuple[int, dict, str]:
        """Send an arbitrary JSON-RPC message. If request_id is None, auto-assigns one."""
        msg: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
        if request_id is not None:
            msg["id"] = request_id
        else:
            msg["id"] = self._next_request_id()
        if params is not None:
            msg["params"] = params
        body = json.dumps(msg)
        return self._send_http("POST", body)

    # -----------------------------------------------------------------------
    # Game lifecycle helpers
    # -----------------------------------------------------------------------

    def wait_for_game_running(self, timeout: float = 10.0, poll_interval: float = 0.1) -> dict:
        """Poll debug/get_status until the game is running or timeout."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            result = self.call_tool("debug/get_status")
            sc = result.get("result", {}).get("structuredContent", {})
            if sc.get("state") == "running":
                return result
            time.sleep(poll_interval)
        raise TimeoutError(f"Game did not reach running state within {timeout}s")

    def start_game_and_wait(self, scene: Optional[str] = None, timeout: float = 15.0) -> dict:
        """Start the game (optionally a specific scene) and wait for it to run.

        If the game is already running, stops it first and waits for it to
        fully stop before relaunching.
        """
        # If a game is already running, stop it first.
        status = self.call_tool("debug/get_status")
        state = status.get("result", {}).get("structuredContent", {}).get("state")
        if state == "running":
            self.call_tool("debug/stop")
            time.sleep(1.0)

        if scene:
            self.call_tool("debug/run_scene", {"scene": scene})
        else:
            self.call_tool("debug/run_project")
        return self.wait_for_game_running(timeout=timeout)

    def stop_game(self) -> dict:
        """Stop the running game."""
        return self.call_tool("debug/stop")
