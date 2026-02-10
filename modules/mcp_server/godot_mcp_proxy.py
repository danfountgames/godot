#!/usr/bin/env python3
"""
Godot MCP Proxy Server

A lightweight proxy that sits between Claude Code and multiple Godot editor
instances. Discovers running editors via per-instance discovery files,
allows switching the active backend via proxy-specific tools.

Usage:
    python3 godot_mcp_proxy.py [--port 6100] [--host 127.0.0.1]

Requires only Python 3 stdlib (no pip).
"""

import argparse
import json
import logging
import os
import platform
import secrets
import signal
import socket
import sys
import threading
import time
import urllib.request
import urllib.error
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

PROXY_DEFAULT_PORT = 6100
PROXY_DEFAULT_HOST = "127.0.0.1"
DISCOVERY_SCAN_INTERVAL = 2.0  # seconds
BACKEND_CONNECT_TIMEOUT = 3.0  # seconds

log = logging.getLogger("godot-mcp-proxy")

# ---------------------------------------------------------------------------
# Discovery
# ---------------------------------------------------------------------------


def get_discovery_dir():
    """Return the path to Godot's per-instance MCP discovery directory."""
    system = platform.system()
    if system == "Darwin":
        base = os.path.expanduser("~/Library/Application Support/Godot")
    elif system == "Windows":
        base = os.path.join(os.environ.get("APPDATA", ""), "Godot")
    else:  # Linux / BSD
        base = os.path.join(
            os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share")),
            "godot",
        )
    return os.path.join(base, "mcp_server", "discovery")


def get_proxy_discovery_path():
    """Return path to the proxy's own discovery file."""
    system = platform.system()
    if system == "Darwin":
        base = os.path.expanduser("~/Library/Application Support/Godot")
    elif system == "Windows":
        base = os.path.join(os.environ.get("APPDATA", ""), "Godot")
    else:
        base = os.path.join(
            os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share")),
            "godot",
        )
    return os.path.join(base, "mcp_server", "proxy_discovery.json")


def is_process_alive(pid):
    """Check whether a process with the given PID is running."""
    try:
        os.kill(pid, 0)
        return True
    except (OSError, ProcessLookupError):
        return False


def is_port_open(host, port, timeout=1.0):
    """Quick TCP connect check to verify a backend is listening."""
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except (OSError, ConnectionRefusedError, TimeoutError):
        return False


class Instance:
    """Represents a discovered Godot editor backend."""

    def __init__(self, port, endpoint, token, pid, godot_version,
                 project_path="", project_name=""):
        self.port = port
        self.endpoint = endpoint
        self.token = token
        self.pid = pid
        self.godot_version = godot_version
        self.project_path = project_path
        self.project_name = project_name
        self.alive = True

    def to_dict(self, is_active=False):
        return {
            "port": self.port,
            "pid": self.pid,
            "project_name": self.project_name,
            "project_path": self.project_path,
            "godot_version": self.godot_version,
            "alive": self.alive,
            "active": is_active,
        }


class DiscoveryScanner:
    """Watches the discovery directory and maintains a live instance list."""

    def __init__(self):
        self._instances = {}  # port -> Instance
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        self._thread = threading.Thread(target=self._scan_loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=5)

    def _scan_loop(self):
        while not self._stop.is_set():
            self._scan_once()
            self._stop.wait(DISCOVERY_SCAN_INTERVAL)

    def _scan_once(self):
        discovery_dir = get_discovery_dir()
        if not os.path.isdir(discovery_dir):
            with self._lock:
                self._instances.clear()
            return

        found_ports = set()
        for fname in os.listdir(discovery_dir):
            if not fname.endswith(".json"):
                continue
            fpath = os.path.join(discovery_dir, fname)
            try:
                with open(fpath, "r") as f:
                    data = json.load(f)
                port = int(fname.replace(".json", ""))
                pid = data.get("pid", 0)

                # Check if process is still alive
                alive = is_process_alive(pid) if pid else False
                if not alive:
                    # Clean up stale file
                    try:
                        os.unlink(fpath)
                        log.info("Cleaned stale discovery file: %s", fname)
                    except OSError:
                        pass
                    continue

                # Verify port is actually open
                endpoint = data.get("endpoint", "")
                host_part = "127.0.0.1"
                alive = is_port_open(host_part, port)

                inst = Instance(
                    port=port,
                    endpoint=endpoint,
                    token=data.get("token", ""),
                    pid=pid,
                    godot_version=data.get("godot_version", ""),
                    project_path=data.get("project_path", ""),
                    project_name=data.get("project_name", ""),
                )
                inst.alive = alive
                found_ports.add(port)

                with self._lock:
                    self._instances[port] = inst

            except (json.JSONDecodeError, ValueError, OSError) as e:
                log.debug("Skipping discovery file %s: %s", fname, e)

        # Remove instances whose files are gone
        with self._lock:
            for port in list(self._instances.keys()):
                if port not in found_ports:
                    del self._instances[port]

    def get_instances(self):
        with self._lock:
            return dict(self._instances)

    def get_instance(self, port):
        with self._lock:
            return self._instances.get(port)


# ---------------------------------------------------------------------------
# Instance Manager
# ---------------------------------------------------------------------------


class InstanceManager:
    """Tracks which backend is active and manages switching."""

    def __init__(self, scanner):
        self.scanner = scanner
        self._active_port = None
        self._lock = threading.Lock()
        self._backend_session_id = None

    @property
    def active_port(self):
        with self._lock:
            return self._active_port

    def get_active_instance(self):
        """Return the active Instance, auto-selecting if needed."""
        with self._lock:
            instances = self.scanner.get_instances()
            alive = {p: i for p, i in instances.items() if i.alive}

            # If current active is still alive, keep it
            if self._active_port and self._active_port in alive:
                return alive[self._active_port]

            # Auto-select if only one alive
            if len(alive) == 1:
                port = next(iter(alive))
                self._active_port = port
                self._backend_session_id = None
                log.info("Auto-selected instance on port %d", port)
                return alive[port]

            # If active died but others exist, clear active
            if self._active_port and self._active_port not in alive:
                log.info("Active instance on port %d is gone", self._active_port)
                self._active_port = None
                self._backend_session_id = None

            return None

    def switch_to(self, port):
        """Switch active backend to the given port. Returns (success, message)."""
        inst = self.scanner.get_instance(port)
        if not inst:
            return False, f"No instance found on port {port}"
        if not inst.alive:
            return False, f"Instance on port {port} is not responding"

        with self._lock:
            old_port = self._active_port
            self._active_port = port
            self._backend_session_id = None

        log.info("Switched active backend: port %s -> %d", old_port, port)
        return True, f"Switched to instance on port {port} ({inst.project_name})"

    def get_backend_session_id(self):
        with self._lock:
            return self._backend_session_id

    def set_backend_session_id(self, sid):
        with self._lock:
            self._backend_session_id = sid


# ---------------------------------------------------------------------------
# SSE notification support
# ---------------------------------------------------------------------------


class SSEManager:
    """Manages SSE notification streams for downstream clients."""

    def __init__(self):
        self._streams = []  # list of (wfile, lock) pairs
        self._lock = threading.Lock()

    def add_stream(self, wfile, stream_lock):
        with self._lock:
            self._streams.append((wfile, stream_lock))

    def remove_stream(self, wfile):
        with self._lock:
            self._streams = [(w, l) for w, l in self._streams if w is not wfile]

    def send_event(self, data):
        """Send an SSE event to all connected streams."""
        if isinstance(data, dict):
            payload = json.dumps(data, separators=(",", ":"))
        else:
            payload = data
        message = f"data: {payload}\n\n"
        encoded = message.encode("utf-8")

        with self._lock:
            dead = []
            for wfile, stream_lock in self._streams:
                try:
                    with stream_lock:
                        wfile.write(encoded)
                        wfile.flush()
                except Exception:
                    dead.append(wfile)
            self._streams = [(w, l) for w, l in self._streams if w not in dead]

    def send_tools_changed(self):
        """Send a tools/list_changed notification to all downstream clients."""
        self.send_event({
            "jsonrpc": "2.0",
            "method": "notifications/tools/list_changed",
        })


# ---------------------------------------------------------------------------
# Backend communication
# ---------------------------------------------------------------------------


def forward_to_backend(instance, payload, session_id=None):
    """Send a JSON-RPC request to a Godot backend and return the response."""
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {instance.token}",
        "Accept": "application/json",
    }
    if session_id:
        headers["Mcp-Session-Id"] = session_id

    data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    req = urllib.request.Request(
        instance.endpoint,
        data=data,
        headers=headers,
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=BACKEND_CONNECT_TIMEOUT) as resp:
            resp_headers = dict(resp.headers)
            body = resp.read().decode("utf-8")
            return resp.status, resp_headers, body
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8") if e.fp else ""
        return e.code, dict(e.headers), body
    except Exception as e:
        return None, {}, str(e)


def initialize_backend_session(instance):
    """Run the initialize + notifications/initialized handshake with a backend."""
    payload = {
        "jsonrpc": "2.0",
        "id": "proxy-init",
        "method": "initialize",
        "params": {
            "protocolVersion": "2025-06-18",
            "clientInfo": {"name": "godot-mcp-proxy", "version": "1.0.0"},
            "capabilities": {},
        },
    }
    status, headers, body = forward_to_backend(instance, payload)
    if status != 200:
        log.error("Backend initialize failed: status=%s body=%s", status, body)
        return None

    session_id = headers.get("Mcp-Session-Id") or headers.get("mcp-session-id")
    if not session_id:
        log.error("Backend did not return Mcp-Session-Id")
        return None

    # Send notifications/initialized
    notif = {
        "jsonrpc": "2.0",
        "method": "notifications/initialized",
    }
    forward_to_backend(instance, notif, session_id=session_id)
    return session_id


def ensure_backend_session(manager):
    """Ensure we have a valid session with the active backend."""
    inst = manager.get_active_instance()
    if not inst:
        return None, None

    sid = manager.get_backend_session_id()
    if sid:
        return inst, sid

    # Need to initialize
    sid = initialize_backend_session(inst)
    if sid:
        manager.set_backend_session_id(sid)
    return inst, sid


# ---------------------------------------------------------------------------
# Proxy-specific tool definitions
# ---------------------------------------------------------------------------

PROXY_TOOLS = [
    {
        "name": "proxy/list_instances",
        "description": (
            "List all discovered Godot editor instances. Shows port, PID, "
            "project name, alive status, and which instance is currently active."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {},
            "required": [],
        },
        "annotations": {
            "readOnlyHint": True,
            "destructiveHint": False,
            "idempotentHint": True,
            "openWorldHint": False,
        },
    },
    {
        "name": "proxy/switch_instance",
        "description": (
            "Switch the active Godot editor backend to a different instance by port number. "
            "Use proxy/list_instances first to see available instances."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "port": {
                    "type": "integer",
                    "description": "The port number of the Godot editor instance to switch to.",
                },
            },
            "required": ["port"],
        },
        "annotations": {
            "readOnlyHint": False,
            "destructiveHint": False,
            "idempotentHint": True,
            "openWorldHint": False,
        },
    },
]


def handle_proxy_tool(tool_name, arguments, manager, sse_manager):
    """Handle a proxy-specific tool call. Returns a JSON-RPC result dict."""
    if tool_name == "proxy/list_instances":
        instances = manager.scanner.get_instances()
        active_port = manager.active_port
        instance_list = [
            inst.to_dict(is_active=(port == active_port))
            for port, inst in sorted(instances.items())
        ]
        text = json.dumps(instance_list, indent=2)
        return {
            "content": [{"type": "text", "text": text}],
        }

    elif tool_name == "proxy/switch_instance":
        port = arguments.get("port")
        if not port:
            return {
                "content": [{"type": "text", "text": "Missing required argument: port"}],
                "isError": True,
            }

        success, message = manager.switch_to(int(port))
        if success:
            # Notify downstream that tools may have changed
            sse_manager.send_tools_changed()

        return {
            "content": [{"type": "text", "text": message}],
            "isError": not success,
        }

    return {
        "content": [{"type": "text", "text": f"Unknown proxy tool: {tool_name}"}],
        "isError": True,
    }


# ---------------------------------------------------------------------------
# HTTP Handler
# ---------------------------------------------------------------------------


class ProxyHandler(BaseHTTPRequestHandler):
    """HTTP handler implementing MCP Streamable HTTP transport as a proxy."""

    # Shared state set by the server
    proxy_token = None
    instance_manager = None
    sse_manager = None
    proxy_session_id = None

    def log_message(self, fmt, *args):
        log.debug(fmt, *args)

    # -- Auth & validation -------------------------------------------------

    def check_auth(self):
        auth = self.headers.get("Authorization", "")
        expected = f"Bearer {self.proxy_token}"
        if not secrets.compare_digest(auth, expected):
            self.send_error_response(401, "Unauthorized")
            return False
        return True

    def send_error_response(self, code, message):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        body = json.dumps({"error": message}).encode("utf-8")
        self.wfile.write(body)

    def send_jsonrpc_error(self, req_id, code, message):
        resp = {
            "jsonrpc": "2.0",
            "id": req_id,
            "error": {"code": code, "message": message},
        }
        body = json.dumps(resp, separators=(",", ":")).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Mcp-Session-Id", self.proxy_session_id)
        self.end_headers()
        self.wfile.write(body)

    def send_jsonrpc_result(self, req_id, result):
        resp = {
            "jsonrpc": "2.0",
            "id": req_id,
            "result": result,
        }
        body = json.dumps(resp, separators=(",", ":")).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Mcp-Session-Id", self.proxy_session_id)
        self.end_headers()
        self.wfile.write(body)

    # -- Method handlers ---------------------------------------------------

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS")
        self.send_header("Access-Control-Allow-Headers",
                         "Content-Type, Authorization, Mcp-Session-Id, Accept")
        self.end_headers()

    def do_POST(self):
        if not self.check_auth():
            return

        content_length = int(self.headers.get("Content-Length", 0))
        if content_length == 0:
            self.send_error_response(400, "Empty body")
            return

        body = self.rfile.read(content_length)
        try:
            payload = json.loads(body)
        except json.JSONDecodeError:
            self.send_error_response(400, "Invalid JSON")
            return

        method = payload.get("method", "")
        req_id = payload.get("id")
        params = payload.get("params", {})

        # --- Handle initialize locally ---
        if method == "initialize":
            result = {
                "protocolVersion": "2025-06-18",
                "serverInfo": {
                    "name": "godot-mcp-proxy",
                    "title": "Godot MCP Proxy",
                    "version": "1.0.0",
                    "description": (
                        "Proxy for multiple Godot editor instances. "
                        "Use proxy/list_instances and proxy/switch_instance to manage backends."
                    ),
                },
                "capabilities": {
                    "tools": {"listChanged": True},
                },
            }
            self.send_jsonrpc_result(req_id, result)
            return

        # --- Handle notifications/initialized locally ---
        if method == "notifications/initialized":
            self.send_response(202)
            self.send_header("Mcp-Session-Id", self.proxy_session_id)
            self.end_headers()
            return

        # --- Handle tools/list: forward + inject proxy tools ---
        if method == "tools/list":
            inst, sid = ensure_backend_session(self.instance_manager)
            if not inst:
                # No backend available — return only proxy tools
                self.send_jsonrpc_result(req_id, {"tools": list(PROXY_TOOLS)})
                return

            status, headers, resp_body = forward_to_backend(inst, payload, session_id=sid)
            if status == 200:
                try:
                    resp = json.loads(resp_body)
                    tools = resp.get("result", {}).get("tools", [])
                    tools.extend(PROXY_TOOLS)
                    resp["result"]["tools"] = tools
                    resp_body = json.dumps(resp, separators=(",", ":"))
                except (json.JSONDecodeError, KeyError):
                    pass

            self._relay_response(status, headers, resp_body)
            return

        # --- Handle tools/call: dispatch proxy tools or forward ---
        if method == "tools/call":
            tool_name = params.get("name", "")
            if tool_name.startswith("proxy/"):
                arguments = params.get("arguments", {})
                result = handle_proxy_tool(
                    tool_name, arguments,
                    self.instance_manager, self.sse_manager,
                )
                self.send_jsonrpc_result(req_id, result)
                return

            # Forward to backend
            inst, sid = ensure_backend_session(self.instance_manager)
            if not inst:
                self.send_jsonrpc_error(
                    req_id, -32002,
                    "No active Godot editor. Use proxy/list_instances to check available instances.",
                )
                return

            # Check if client wants SSE streaming
            accept = self.headers.get("Accept", "")
            if "text/event-stream" in accept:
                self._forward_sse_post(inst, sid, payload)
            else:
                status, headers, resp_body = forward_to_backend(inst, payload, session_id=sid)
                self._relay_response(status, headers, resp_body)
            return

        # --- Handle notifications (ping, cancelled, etc.) ---
        if "id" not in payload:
            # It's a notification — try to forward
            inst, sid = ensure_backend_session(self.instance_manager)
            if inst and sid:
                forward_to_backend(inst, payload, session_id=sid)
            self.send_response(202)
            self.send_header("Mcp-Session-Id", self.proxy_session_id)
            self.end_headers()
            return

        # --- Default: forward everything else ---
        inst, sid = ensure_backend_session(self.instance_manager)
        if not inst:
            self.send_jsonrpc_error(
                req_id, -32002,
                "No active Godot editor. Use proxy/list_instances to check available instances.",
            )
            return

        status, headers, resp_body = forward_to_backend(inst, payload, session_id=sid)
        self._relay_response(status, headers, resp_body)

    def do_GET(self):
        if not self.check_auth():
            return

        # SSE notification stream
        accept = self.headers.get("Accept", "")
        if "text/event-stream" not in accept:
            self.send_error_response(400, "GET requires Accept: text/event-stream")
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("Mcp-Session-Id", self.proxy_session_id)
        self.end_headers()

        stream_lock = threading.Lock()
        self.sse_manager.add_stream(self.wfile, stream_lock)

        try:
            # Keep alive until client disconnects
            while True:
                time.sleep(30)
                try:
                    with stream_lock:
                        self.wfile.write(b": keepalive\n\n")
                        self.wfile.flush()
                except Exception:
                    break
        finally:
            self.sse_manager.remove_stream(self.wfile)

    def do_DELETE(self):
        if not self.check_auth():
            return
        self.send_response(204)
        self.send_header("Mcp-Session-Id", self.proxy_session_id)
        self.end_headers()

    # -- Helpers -----------------------------------------------------------

    def _relay_response(self, status, headers, body):
        """Relay a backend response to the downstream client."""
        if status is None:
            self.send_jsonrpc_error(None, -32603, f"Backend error: {body}")
            return

        self.send_response(status)
        self.send_header("Content-Type", headers.get("Content-Type", "application/json"))
        self.send_header("Mcp-Session-Id", self.proxy_session_id)
        self.end_headers()
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.wfile.write(body)

    def _forward_sse_post(self, instance, session_id, payload):
        """Forward a POST request expecting an SSE response, streaming it through."""
        headers_dict = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {instance.token}",
            "Accept": "text/event-stream",
        }
        if session_id:
            headers_dict["Mcp-Session-Id"] = session_id

        data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        req = urllib.request.Request(
            instance.endpoint,
            data=data,
            headers=headers_dict,
            method="POST",
        )

        try:
            resp = urllib.request.urlopen(req, timeout=120)
            content_type = resp.headers.get("Content-Type", "")

            if "text/event-stream" in content_type:
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "keep-alive")
                self.send_header("Mcp-Session-Id", self.proxy_session_id)
                self.end_headers()

                # Stream SSE data through
                while True:
                    line = resp.readline()
                    if not line:
                        break
                    self.wfile.write(line)
                    self.wfile.flush()
            else:
                # Non-SSE response, relay normally
                body = resp.read().decode("utf-8")
                self._relay_response(resp.status, dict(resp.headers), body)

        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8") if e.fp else ""
            self._relay_response(e.code, dict(e.headers), body)
        except Exception as e:
            self.send_jsonrpc_error(payload.get("id"), -32603, f"Backend error: {e}")


# ---------------------------------------------------------------------------
# Threaded HTTP Server
# ---------------------------------------------------------------------------


class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def write_proxy_discovery(host, port, token):
    """Write the proxy's own discovery file."""
    path = get_proxy_discovery_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)

    data = {
        "endpoint": f"http://{host}:{port}/mcp",
        "token": token,
        "pid": os.getpid(),
        "type": "proxy",
    }

    with open(path, "w") as f:
        json.dump(data, f, indent="\t")

    # Restrict permissions on Unix
    if platform.system() != "Windows":
        os.chmod(path, 0o600)

    log.info("Proxy discovery written: %s", path)


def delete_proxy_discovery():
    path = get_proxy_discovery_path()
    if os.path.exists(path):
        try:
            os.unlink(path)
        except OSError:
            pass


def main():
    parser = argparse.ArgumentParser(description="Godot MCP Proxy Server")
    parser.add_argument("--port", type=int, default=PROXY_DEFAULT_PORT,
                        help=f"Port to listen on (default: {PROXY_DEFAULT_PORT})")
    parser.add_argument("--host", default=PROXY_DEFAULT_HOST,
                        help=f"Host to bind to (default: {PROXY_DEFAULT_HOST})")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Enable verbose logging")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="[%(name)s] %(levelname)s: %(message)s",
    )

    # Generate proxy auth token
    token = secrets.token_hex(16)
    session_id = secrets.token_hex(32)

    # Start discovery scanner
    scanner = DiscoveryScanner()
    scanner.start()

    # Create instance manager and SSE manager
    manager = InstanceManager(scanner)
    sse_manager = SSEManager()

    # Configure handler class
    ProxyHandler.proxy_token = token
    ProxyHandler.instance_manager = manager
    ProxyHandler.sse_manager = sse_manager
    ProxyHandler.proxy_session_id = session_id

    # Start HTTP server
    server = ThreadedHTTPServer((args.host, args.port), ProxyHandler)

    write_proxy_discovery(args.host, args.port, token)

    def shutdown_handler(signum, frame):
        log.info("Shutting down...")
        scanner.stop()
        delete_proxy_discovery()
        server.shutdown()
        sys.exit(0)

    signal.signal(signal.SIGINT, shutdown_handler)
    signal.signal(signal.SIGTERM, shutdown_handler)

    log.info("Godot MCP Proxy started on %s:%d", args.host, args.port)
    log.info("Discovery dir: %s", get_discovery_dir())

    # Do an initial scan
    scanner._scan_once()
    instances = scanner.get_instances()
    if instances:
        log.info("Found %d Godot instance(s): %s",
                 len(instances),
                 ", ".join(f"port {p}" for p in sorted(instances.keys())))
    else:
        log.info("No Godot instances found yet (will scan every %.0fs)", DISCOVERY_SCAN_INTERVAL)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        scanner.stop()
        delete_proxy_discovery()
        server.server_close()


if __name__ == "__main__":
    main()
