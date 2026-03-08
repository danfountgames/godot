#!/usr/bin/env bash
set -euo pipefail

# SyncServer end-to-end functional test
# Starts the engine with SyncServer, connects via Python WebSocket client,
# exercises the full protocol (hello, manifest, write_small_file, reload_hint,
# sync_complete), and verifies responses and side effects.

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$ROOT_DIR/bin/godot.linuxbsd.editor.x86_64"
PASS_COUNT=0
FAIL_COUNT=0

pass() {
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "  PASS: $1"
}

fail() {
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "  FAIL: $1"
}

check() {
    if echo "$1" | grep "$2" > /dev/null 2>&1; then
        pass "$3"
    else
        fail "$3"
        echo "    Expected to find: $2"
    fi
}

check_not() {
    if echo "$1" | grep "$2" > /dev/null 2>&1; then
        fail "$3"
        echo "    Should NOT contain: $2"
    else
        pass "$3"
    fi
}

if [ ! -f "$BINARY" ]; then
    echo "ERROR: Binary not found at $BINARY"
    exit 1
fi

# Check for Python websockets library.
if ! python3 -c "import websockets" 2>/dev/null; then
    echo "ERROR: Python 'websockets' library not found. Install with: pip3 install websockets"
    exit 1
fi

echo "=== SyncServer End-to-End Test ==="
echo ""

# ========================
# Setup: Create a temporary project that starts SyncServer
# ========================
TMPDIR=$(mktemp -d)
PROJECT_DIR="$TMPDIR/sync_test_project"
mkdir -p "$PROJECT_DIR"

cleanup() {
    echo ""
    echo "Cleaning up $TMPDIR ..."
    # Kill any remaining engine process.
    if [ -n "${ENGINE_PID:-}" ] && kill -0 "$ENGINE_PID" 2>/dev/null; then
        kill "$ENGINE_PID" 2>/dev/null || true
        wait "$ENGINE_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# Use port 16850 to avoid conflict with any running instance.
TEST_PORT=16850

cat > "$PROJECT_DIR/project.godot" << 'PROJEOF'
config_version=5

[application]

config/name="Sync Test"
run/main_scene="res://main.tscn"
config/features=PackedStringArray("4.4")
PROJEOF

cat > "$PROJECT_DIR/main.gd" << GDEOF
extends Node2D

var sync_server = null

func _ready():
	print("Sync test project loaded")
	sync_server = Engine.get_singleton("SyncServer")
	if sync_server == null:
		print("SYNC_ERROR: SyncServer singleton not found")
		return

	var err = sync_server.start_server($TEST_PORT)
	if err != 0:
		print("SYNC_ERROR: Failed to start server, error: " + str(err))
		return

	print("SYNC_READY: Server started on port " + str(sync_server.get_port()))
	print("SYNC_RUNNING: " + str(sync_server.is_running()))

func _process(_delta):
	if sync_server and sync_server.is_running():
		sync_server.poll()
GDEOF

cat > "$PROJECT_DIR/main.tscn" << 'TSCNEOF'
[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://main.gd" id="1"]

[node name="Main" type="Node2D"]
script = ExtResource("1")
TSCNEOF

echo "Test project created at: $PROJECT_DIR"
echo "SyncServer will listen on port $TEST_PORT"
echo ""

# ========================
# Test 1: Start engine with SyncServer
# ========================
echo "--- Test 1: Start SyncServer ---"
ENGINE_LOG="$TMPDIR/engine.log"

# Start engine in background.
"$BINARY" --devplayer --headless --devplayer-mount "$PROJECT_DIR" --quit-after 15000 > "$ENGINE_LOG" 2>&1 &
ENGINE_PID=$!

# Wait for the server to be ready.
echo "  Waiting for SyncServer to start (PID: $ENGINE_PID)..."
MAX_WAIT=10
for i in $(seq 1 $MAX_WAIT); do
    if grep -q "SYNC_READY" "$ENGINE_LOG" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$ENGINE_PID" 2>/dev/null; then
        echo "  Engine exited prematurely"
        break
    fi
    sleep 1
done

ENGINE_OUT=$(cat "$ENGINE_LOG")
check "$ENGINE_OUT" "SYNC_READY" "SyncServer started successfully"
check "$ENGINE_OUT" "SYNC_RUNNING: true" "SyncServer reports running"
echo ""

# ========================
# Test 2: WebSocket protocol exercise
# ========================
echo "--- Test 2: WebSocket protocol test ---"

# Write the Python test client.
PYTHON_RESULT="$TMPDIR/python_result.log"

python3 << PYEOF > "$PYTHON_RESULT" 2>&1 || true
import asyncio
import json
import base64
import websockets

async def test_sync_protocol():
    uri = "ws://127.0.0.1:$TEST_PORT"
    results = []

    try:
        async with websockets.connect(uri, open_timeout=5) as ws:
            results.append("CONNECTED: yes")

            # Test 1: hello
            await ws.send(json.dumps({"type": "hello"}))
            resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
            if resp.get("type") == "hello_ack" and resp.get("engine") == "GodotBeam":
                results.append("HELLO_ACK: ok")
                results.append("HELLO_ENGINE: " + resp.get("engine", ""))
                results.append("HELLO_MOUNTED: " + str(resp.get("project_mounted", "")))
            else:
                results.append("HELLO_ACK: fail - " + json.dumps(resp))

            # Test 2: manifest
            manifest = {
                "type": "manifest",
                "files": {
                    "test_file.txt": "abc123hash",
                    "scenes/main.tscn": "def456hash"
                }
            }
            await ws.send(json.dumps(manifest))
            resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
            if resp.get("type") == "manifest_ack" and resp.get("status") == "ok":
                needs = resp.get("needs_update", [])
                results.append("MANIFEST_ACK: ok")
                results.append("MANIFEST_NEEDS: " + str(len(needs)))
            else:
                results.append("MANIFEST_ACK: fail - " + json.dumps(resp))

            # Test 3: write_small_file
            test_content = b"Hello from SyncServer test!"
            b64_content = base64.b64encode(test_content).decode("ascii")
            write_msg = {
                "type": "write_small_file",
                "path": "sync_test_output.txt",
                "content": b64_content
            }
            await ws.send(json.dumps(write_msg))
            resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
            if resp.get("type") == "write_ack" and resp.get("status") == "ok":
                results.append("WRITE_ACK: ok")
                results.append("WRITE_BYTES: " + str(resp.get("bytes", 0)))
            else:
                results.append("WRITE_ACK: fail - " + json.dumps(resp))

            # Test 4: reload_hint (tier 1 = content)
            reload_msg = {
                "type": "reload_hint",
                "tier": 1,
                "changed_paths": ["textures/icon.png"]
            }
            await ws.send(json.dumps(reload_msg))
            resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
            if resp.get("type") == "reload_ack" and resp.get("status") == "ok":
                results.append("RELOAD_ACK: ok")
                results.append("RELOAD_TIER: " + str(resp.get("tier", -1)))
            else:
                results.append("RELOAD_ACK: fail - " + json.dumps(resp))

            # Test 5: sync_complete
            sync_msg = {
                "type": "sync_complete",
                "files_synced": 3
            }
            await ws.send(json.dumps(sync_msg))
            resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
            if resp.get("type") == "sync_complete_ack" and resp.get("status") == "ok":
                results.append("SYNC_COMPLETE_ACK: ok")
            else:
                results.append("SYNC_COMPLETE_ACK: fail - " + json.dumps(resp))

            # Test 6: path traversal rejection
            bad_msg = {
                "type": "write_small_file",
                "path": "../../../etc/passwd",
                "content": base64.b64encode(b"pwned").decode("ascii")
            }
            await ws.send(json.dumps(bad_msg))
            # Server should NOT send write_ack for traversal attempt.
            # Wait briefly for any response (there shouldn't be one for security rejections).
            try:
                resp = json.loads(await asyncio.wait_for(ws.recv(), timeout=2))
                # If we got a response, check it's not a success.
                if resp.get("status") == "ok":
                    results.append("PATH_TRAVERSAL: fail - accepted malicious path")
                else:
                    results.append("PATH_TRAVERSAL: ok - rejected")
            except asyncio.TimeoutError:
                results.append("PATH_TRAVERSAL: ok - no response (silently rejected)")

            results.append("PROTOCOL_COMPLETE: yes")

    except ConnectionRefusedError:
        results.append("CONNECTED: no - connection refused")
    except Exception as e:
        results.append("ERROR: " + str(e))

    for r in results:
        print(r)

asyncio.run(test_sync_protocol())
PYEOF

PY_OUT=$(cat "$PYTHON_RESULT")
echo "  Python client results:"
echo "$PY_OUT" | sed 's/^/    /'
echo ""

check "$PY_OUT" "CONNECTED: yes" "WebSocket connected to SyncServer"
check "$PY_OUT" "HELLO_ACK: ok" "hello_ack response received"
check "$PY_OUT" "HELLO_ENGINE: GodotBeam" "Engine identified as GodotBeam"
check "$PY_OUT" "HELLO_MOUNTED: True" "Project reported as mounted"
check "$PY_OUT" "MANIFEST_ACK: ok" "manifest_ack response received"
check "$PY_OUT" "MANIFEST_NEEDS: 2" "Manifest reports 2 files need update"
check "$PY_OUT" "WRITE_ACK: ok" "write_ack response received"
check "$PY_OUT" "WRITE_BYTES: 27" "Correct byte count written (27 bytes)"
check "$PY_OUT" "RELOAD_ACK: ok" "reload_ack response received"
check "$PY_OUT" "RELOAD_TIER: 1" "Correct reload tier echoed back"
check "$PY_OUT" "SYNC_COMPLETE_ACK: ok" "sync_complete_ack response received"
check "$PY_OUT" "PATH_TRAVERSAL: ok" "Path traversal attack rejected"
check "$PY_OUT" "PROTOCOL_COMPLETE: yes" "Full protocol exercised successfully"
echo ""

# ========================
# Test 3: Verify file was actually written to disk
# ========================
echo "--- Test 3: Verify file written to disk ---"
WRITTEN_FILE="$PROJECT_DIR/sync_test_output.txt"
if [ -f "$WRITTEN_FILE" ]; then
    WRITTEN_CONTENT=$(cat "$WRITTEN_FILE")
    if [ "$WRITTEN_CONTENT" = "Hello from SyncServer test!" ]; then
        pass "File content matches expected value"
    else
        fail "File content mismatch"
        echo "    Expected: Hello from SyncServer test!"
        echo "    Got: $WRITTEN_CONTENT"
    fi
else
    fail "Written file not found at $WRITTEN_FILE"
fi

# Verify traversal file was NOT created.
if [ ! -f "$TMPDIR/../../../etc/passwd_test" ]; then
    pass "Path traversal file was not created"
else
    fail "Path traversal file was created!"
fi
echo ""

# ========================
# Test 4: Check engine-side logs
# ========================
echo "--- Test 4: Engine-side protocol logs ---"
# Wait for engine to finish processing.
sleep 1
ENGINE_OUT=$(cat "$ENGINE_LOG")
check "$ENGINE_OUT" "Received 'hello'" "Engine logged hello message"
check "$ENGINE_OUT" "Manifest processed" "Engine logged manifest processing"
check "$ENGINE_OUT" "File written: sync_test_output.txt" "Engine logged file write"
check "$ENGINE_OUT" "Reload hint received: tier 1" "Engine logged reload hint"
check "$ENGINE_OUT" "Sync complete from peer" "Engine logged sync complete"
check "$ENGINE_OUT" "Path traversal detected" "Engine logged path traversal rejection"
echo ""

# Kill the engine if still running.
if kill -0 "$ENGINE_PID" 2>/dev/null; then
    kill "$ENGINE_PID" 2>/dev/null || true
    wait "$ENGINE_PID" 2>/dev/null || true
fi

# ========================
# Summary
# ========================
echo "==========================================="
echo "  RESULTS: $PASS_COUNT passed, $FAIL_COUNT failed out of $((PASS_COUNT + FAIL_COUNT)) checks"
echo "==========================================="

if [ $FAIL_COUNT -gt 0 ]; then
    exit 1
fi
