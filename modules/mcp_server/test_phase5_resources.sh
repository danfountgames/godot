#!/bin/bash
# Phase 5 Integration Test: MCP Resources
# Tests resources/list, resources/read, resources/templates/list,
# resources/subscribe, resources/unsubscribe, and capability declaration.
#
# Prerequisites: Godot editor built with MCP server module.
# Usage: ./test_phase5_resources.sh [path-to-godot-binary] [path-to-project]

set -euo pipefail

GODOT_BIN="${1:-/home/dan/Code/GodotPatch/godot-4.6-fork/bin/godot.linuxbsd.editor.x86_64}"
PROJECT_DIR="${2:-/home/dan/Code/GodotPatch/lens-effects-addon}"
MCP_PORT=6009
MCP_URL="http://127.0.0.1:${MCP_PORT}/mcp"
SESSION_ID=""
GODOT_PID=""

PASS=0
FAIL=0
TOTAL=0

cleanup() {
    if [ -n "$GODOT_PID" ] && kill -0 "$GODOT_PID" 2>/dev/null; then
        kill "$GODOT_PID" 2>/dev/null || true
        wait "$GODOT_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Start Godot in the background.
echo "Starting Godot editor with project: $PROJECT_DIR"
"$GODOT_BIN" --editor --headless --path "$PROJECT_DIR" &>/dev/null &
GODOT_PID=$!

# Wait for MCP server to be ready.
echo "Waiting for MCP server on port $MCP_PORT..."
for i in $(seq 1 30); do
    if curl -s -o /dev/null -w '' "http://127.0.0.1:${MCP_PORT}/mcp" 2>/dev/null; then
        echo "MCP server is ready."
        break
    fi
    if ! kill -0 "$GODOT_PID" 2>/dev/null; then
        echo "FATAL: Godot process exited early."
        exit 1
    fi
    sleep 1
done

# Helper: send a JSON-RPC POST request and print the response body.
rpc() {
    local method="$1"
    local params="${2:-\{\}}"
    local id="${3:-1}"
    curl -s -X POST "$MCP_URL" \
        -H "Content-Type: application/json" \
        -H "Host: 127.0.0.1:${MCP_PORT}" \
        -H "Mcp-Session-Id: ${SESSION_ID}" \
        -d "{\"jsonrpc\":\"2.0\",\"id\":${id},\"method\":\"${method}\",\"params\":${params}}" 2>/dev/null
}

# Test helper.
assert_contains() {
    local test_name="$1"
    local haystack="$2"
    local needle="$3"
    TOTAL=$((TOTAL + 1))
    if echo "$haystack" | grep -qF "$needle"; then
        echo "  PASS: $test_name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $test_name (expected to contain: '$needle')"
        echo "        Got: $(echo "$haystack" | head -c 500)"
        FAIL=$((FAIL + 1))
    fi
}

assert_not_contains() {
    local test_name="$1"
    local haystack="$2"
    local needle="$3"
    TOTAL=$((TOTAL + 1))
    if echo "$haystack" | grep -qF "$needle"; then
        echo "  FAIL: $test_name (should NOT contain: '$needle')"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS: $test_name"
        PASS=$((PASS + 1))
    fi
}

echo ""
echo "============================================"
echo "Phase 5: MCP Resources Integration Tests"
echo "============================================"

# ── Step 1: Initialize session ──
echo ""
echo "--- Initialize Session ---"
INIT_RAW=$(curl -s -i -X POST "$MCP_URL" \
    -H "Content-Type: application/json" \
    -H "Host: 127.0.0.1:${MCP_PORT}" \
    -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}' 2>/dev/null)

SESSION_ID=$(echo "$INIT_RAW" | grep -i "Mcp-Session-Id:" | tr -d '\r' | sed 's/.*: *//')
INIT_BODY=$(echo "$INIT_RAW" | sed -n '/^{/,$ p' | head -1)

if [ -z "$SESSION_ID" ]; then
    echo "FATAL: Could not extract session ID from initialize response."
    echo "Response: $INIT_RAW"
    exit 1
fi
echo "Session ID: ${SESSION_ID:0:16}..."

# Test 1: Resources capability in initialize response.
assert_contains "T1: resources capability in initialize" "$INIT_BODY" '"resources"'
assert_contains "T1b: subscribe in resources cap" "$INIT_BODY" '"subscribe":true'
assert_contains "T1c: listChanged in resources cap" "$INIT_BODY" '"listChanged":true'

# Send notifications/initialized.
NOTIFY_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$MCP_URL" \
    -H "Content-Type: application/json" \
    -H "Host: 127.0.0.1:${MCP_PORT}" \
    -H "Mcp-Session-Id: ${SESSION_ID}" \
    -d '{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}' 2>/dev/null)
echo "notifications/initialized -> HTTP $NOTIFY_CODE"

TOTAL=$((TOTAL + 1))
if [ "$NOTIFY_CODE" = "202" ]; then
    echo "  PASS: T1d: notifications/initialized accepted"
    PASS=$((PASS + 1))
else
    echo "  FAIL: T1d: notifications/initialized (expected 202, got $NOTIFY_CODE)"
    FAIL=$((FAIL + 1))
fi

sleep 0.5

# ── Step 2: resources/list (no game running) ──
echo ""
echo "--- resources/list (no game) ---"
LIST_RESP=$(rpc "resources/list" '{}')

assert_contains "T2: resources/list has resources array" "$LIST_RESP" '"resources"'
assert_contains "T2b: project/info listed" "$LIST_RESP" 'godot://project/info'
assert_contains "T2c: project/settings listed" "$LIST_RESP" 'godot://project/settings'
assert_contains "T2d: project/file-tree listed" "$LIST_RESP" 'godot://project/file-tree'
assert_contains "T2e: project/input-map listed" "$LIST_RESP" 'godot://project/input-map'
assert_contains "T2f: game/status listed (always)" "$LIST_RESP" 'godot://game/status'
assert_not_contains "T2g: game/scene-tree NOT listed" "$LIST_RESP" 'godot://game/scene-tree'
assert_not_contains "T2h: game/output NOT listed" "$LIST_RESP" 'godot://game/output'
assert_not_contains "T2i: game/errors NOT listed" "$LIST_RESP" 'godot://game/errors'
assert_not_contains "T2j: game/performance NOT listed" "$LIST_RESP" 'godot://game/performance'

# ── Step 3: resources/read - project/info ──
echo ""
echo "--- resources/read godot://project/info ---"
READ_INFO=$(rpc "resources/read" '{"uri":"godot://project/info"}')

assert_contains "T3: read project/info has contents" "$READ_INFO" '"contents"'
assert_contains "T3b: has project_name" "$READ_INFO" 'project_name'
assert_contains "T3c: has godot_version" "$READ_INFO" 'godot_version'
assert_contains "T3d: has main_scene" "$READ_INFO" 'main_scene'
assert_contains "T3e: has renderer" "$READ_INFO" 'renderer'

# ── Step 4: resources/read - project/settings ──
echo ""
echo "--- resources/read godot://project/settings ---"
READ_SETTINGS=$(rpc "resources/read" '{"uri":"godot://project/settings"}')

assert_contains "T4: read project/settings has contents" "$READ_SETTINGS" '"contents"'
assert_contains "T4b: has config section" "$READ_SETTINGS" 'config/name'

# ── Step 5: resources/read - project/file-tree ──
echo ""
echo "--- resources/read godot://project/file-tree ---"
READ_TREE=$(rpc "resources/read" '{"uri":"godot://project/file-tree"}')

assert_contains "T5: read project/file-tree has contents" "$READ_TREE" '"contents"'
assert_contains "T5b: has directories" "$READ_TREE" 'directories'
assert_contains "T5c: has total_files" "$READ_TREE" 'total_files'

# ── Step 6: resources/read - project/input-map ──
echo ""
echo "--- resources/read godot://project/input-map ---"
READ_INPUT=$(rpc "resources/read" '{"uri":"godot://project/input-map"}')

assert_contains "T6: read project/input-map has contents" "$READ_INPUT" '"contents"'
assert_contains "T6b: has actions" "$READ_INPUT" 'actions'

# ── Step 7: resources/read - game/status (no game) ──
echo ""
echo "--- resources/read godot://game/status (no game) ---"
READ_STATUS=$(rpc "resources/read" '{"uri":"godot://game/status"}')

assert_contains "T7: read game/status has contents" "$READ_STATUS" '"contents"'
assert_contains "T7b: shows stopped" "$READ_STATUS" 'stopped'

# ── Step 8: resources/read - game/scene-tree (no game, should error) ──
echo ""
echo "--- resources/read godot://game/scene-tree (no game, expect error) ---"
READ_SCENE=$(rpc "resources/read" '{"uri":"godot://game/scene-tree"}')

assert_contains "T8: scene-tree gated (error)" "$READ_SCENE" 'requires a running game'

# ── Step 9: resources/read - unknown URI ──
echo ""
echo "--- resources/read godot://unknown/thing (expect error) ---"
READ_UNKNOWN=$(rpc "resources/read" '{"uri":"godot://unknown/thing"}')

assert_contains "T9: unknown URI returns error" "$READ_UNKNOWN" 'Resource not found'

# ── Step 10: resources/read - missing uri param ──
echo ""
echo "--- resources/read (no uri param) ---"
READ_NOARG=$(rpc "resources/read" '{}')

assert_contains "T10: missing uri error" "$READ_NOARG" 'Missing required parameter'

# ── Step 11: resources/templates/list ──
echo ""
echo "--- resources/templates/list ---"
TMPL_RESP=$(rpc "resources/templates/list" '{}')

assert_contains "T11: templates list has resourceTemplates" "$TMPL_RESP" '"resourceTemplates"'
assert_contains "T11b: file template" "$TMPL_RESP" 'godot://file/{path}'
assert_contains "T11c: node properties template" "$TMPL_RESP" 'godot://game/node/{node_id}/properties'

# ── Step 12: File template - read project.godot ──
echo ""
echo "--- resources/read godot://file/project.godot ---"
READ_FILE=$(rpc "resources/read" '{"uri":"godot://file/project.godot"}')

assert_contains "T12: read file template has contents" "$READ_FILE" '"contents"'
assert_contains "T12b: has project file content" "$READ_FILE" 'config/name'

# ── Step 13: File template - path traversal attack ──
echo ""
echo "--- resources/read godot://file/../../etc/passwd (attack) ---"
READ_ATTACK=$(rpc "resources/read" '{"uri":"godot://file/../../etc/passwd"}')

assert_contains "T13: path traversal rejected" "$READ_ATTACK" 'path traversal'

# ── Step 14: File template - nonexistent file ──
echo ""
echo "--- resources/read godot://file/nonexistent.gd ---"
READ_NOFILE=$(rpc "resources/read" '{"uri":"godot://file/nonexistent.gd"}')

assert_contains "T14: nonexistent file error" "$READ_NOFILE" 'File not found'

# ── Step 15: File template - absolute path attack ──
echo ""
echo "--- resources/read godot://file//etc/passwd (absolute path) ---"
READ_ABS=$(rpc "resources/read" '{"uri":"godot://file//etc/passwd"}')

assert_contains "T15: absolute path rejected" "$READ_ABS" 'must be a relative project path'

# ── Step 16: Node properties template (no game) ──
echo ""
echo "--- resources/read godot://game/node/12345/properties (no game) ---"
READ_NODE=$(rpc "resources/read" '{"uri":"godot://game/node/12345/properties"}')

assert_contains "T16: node properties without game" "$READ_NODE" 'not running'

# ── Step 17: Node properties template - invalid node_id ──
echo ""
echo "--- resources/read godot://game/node/abc/properties (invalid id) ---"
READ_BADID=$(rpc "resources/read" '{"uri":"godot://game/node/abc/properties"}')

assert_contains "T17: invalid node_id" "$READ_BADID" 'must be a numeric object ID'

# ── Step 18: resources/subscribe (stub) ──
echo ""
echo "--- resources/subscribe ---"
SUB_RESP=$(rpc "resources/subscribe" '{"uri":"godot://game/status"}')

assert_contains "T18: subscribe returns result" "$SUB_RESP" '"result"'

# ── Step 19: resources/unsubscribe (stub) ──
echo ""
echo "--- resources/unsubscribe ---"
UNSUB_RESP=$(rpc "resources/unsubscribe" '{"uri":"godot://game/status"}')

assert_contains "T19: unsubscribe returns result" "$UNSUB_RESP" '"result"'

# ── Step 20: Subscribe missing uri ──
echo ""
echo "--- resources/subscribe (no uri) ---"
SUB_NOARG=$(rpc "resources/subscribe" '{}')

assert_contains "T20: subscribe missing uri error" "$SUB_NOARG" 'Missing required parameter'

# ── Summary ──
echo ""
echo "============================================"
echo "Results: $PASS / $TOTAL PASS, $FAIL FAIL"
echo "============================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
