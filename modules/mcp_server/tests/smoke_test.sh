#!/usr/bin/env bash
# File: tests/smoke_test.sh
# Quick manual verification of the MCP server using curl.
# Assumes a running Godot editor with MCP enabled on port 6009.
set -euo pipefail

BASE="http://127.0.0.1:6009/mcp"
CT="Content-Type: application/json"
PASS=0
FAIL=0

green() { printf '\033[32m%s\033[0m\n' "$1"; }
red()   { printf '\033[31m%s\033[0m\n' "$1"; }

check() {
    local name="$1" expected="$2" actual="$3"
    if [[ "$actual" == *"$expected"* ]]; then
        green "  PASS: $name"
        ((PASS++))
    else
        red "  FAIL: $name (expected '$expected', got '$actual')"
        ((FAIL++))
    fi
}

check_status() {
    local name="$1" expected="$2" actual="$3"
    if [[ "$actual" == "$expected" ]]; then
        green "  PASS: $name (HTTP $actual)"
        ((PASS++))
    else
        red "  FAIL: $name (expected HTTP $expected, got HTTP $actual)"
        ((FAIL++))
    fi
}

echo "============================================"
echo "  MCP Server Smoke Tests"
echo "============================================"
echo ""

# ---- TC-01: Initialize ----
echo "=== TC-01: Initialize ==="
INIT_HEADERS=$(mktemp)
INIT_BODY=$(curl -s -D "$INIT_HEADERS" -X POST "$BASE" \
  -H "$CT" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"smoke","version":"0.1"},"capabilities":{}}}')

INIT_STATUS=$(head -1 "$INIT_HEADERS" | awk '{print $2}')
check_status "Initialize HTTP status" "200" "$INIT_STATUS"

SID=$(grep -i 'mcp-session-id' "$INIT_HEADERS" | head -1 | awk '{print $2}' | tr -d '\r\n')
if [[ -n "$SID" ]]; then
    green "  PASS: Session ID present ($SID)"
    ((PASS++))
else
    red "  FAIL: No Mcp-Session-Id in response"
    ((FAIL++))
fi

check "protocolVersion in body" '"protocolVersion"' "$INIT_BODY"
check "serverInfo in body" '"serverInfo"' "$INIT_BODY"
rm -f "$INIT_HEADERS"

echo ""

# ---- TC-02: Send initialized notification ----
echo "=== TC-02: Initialized Notification ==="
NOTIF_STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE" \
  -H "$CT" \
  -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}')
check_status "Initialized notification" "202" "$NOTIF_STATUS"

echo ""

# ---- TC-03: Ping ----
echo "=== TC-03: Ping ==="
PING_BODY=$(curl -s -X POST "$BASE" \
  -H "$CT" \
  -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":2,"method":"ping"}')
check "Ping has result" '"result"' "$PING_BODY"
check "Ping has id 2" '"id":2' "$PING_BODY"

echo ""

# ---- TC-04: tools/list ----
echo "=== TC-04: tools/list ==="
TOOLS_BODY=$(curl -s -X POST "$BASE" \
  -H "$CT" \
  -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/list"}')
check "tools/list has tools array" '"tools"' "$TOOLS_BODY"
check "Has project/get_info" 'project/get_info' "$TOOLS_BODY"
check "Has gdscript/check_errors" 'gdscript/check_errors' "$TOOLS_BODY"
check "Has debug/run_project" 'debug/run_project' "$TOOLS_BODY"

echo ""

# ---- TC-05: project/get_info ----
echo "=== TC-05: project/get_info ==="
INFO_BODY=$(curl -s -X POST "$BASE" \
  -H "$CT" \
  -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"project/get_info","arguments":{}}}')
check "Has content" '"content"' "$INFO_BODY"
check "Has godot_version" 'godot_version' "$INFO_BODY"

echo ""

# ---- TC-06: resources/list ----
echo "=== TC-06: resources/list ==="
RES_BODY=$(curl -s -X POST "$BASE" \
  -H "$CT" \
  -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":5,"method":"resources/list"}')
check "Has resources array" '"resources"' "$RES_BODY"
check "Has project/info" 'project/info' "$RES_BODY"

echo ""

# ---- TC-07: Session ID enforcement ----
echo "=== TC-07: Session ID Enforcement ==="
NO_SID_STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE" \
  -H "$CT" \
  -d '{"jsonrpc":"2.0","id":6,"method":"tools/list"}')
check_status "No session ID -> 400" "400" "$NO_SID_STATUS"

BAD_SID_STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE" \
  -H "$CT" \
  -H "Mcp-Session-Id: 0000000000000000000000000000000000000000000000000000000000000000" \
  -d '{"jsonrpc":"2.0","id":7,"method":"tools/list"}')
check_status "Bad session ID -> 404" "404" "$BAD_SID_STATUS"

echo ""

# ---- TC-08: Delete session ----
echo "=== TC-08: Delete Session ==="
DEL_STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X DELETE "$BASE" \
  -H "Mcp-Session-Id: $SID")
check_status "DELETE session" "200" "$DEL_STATUS"

echo ""

# ---- TC-09: Verify deleted session is rejected ----
echo "=== TC-09: Post-Delete Rejection ==="
DEAD_STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE" \
  -H "$CT" \
  -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":8,"method":"ping"}')
check_status "Deleted session rejected" "404" "$DEAD_STATUS"

echo ""

# ---- TC-10: CORS - evil origin rejected ----
echo "=== TC-10: CORS (Evil Origin) ==="
EVIL_STATUS=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE" \
  -H "$CT" \
  -H "Origin: http://evil.com" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"evil","version":"0.1"},"capabilities":{}}}')
check_status "Evil origin rejected" "403" "$EVIL_STATUS"

echo ""

# ---- TC-11: Host validation ----
echo "=== TC-11: Host Validation ==="
HOST_EVIL_STATUS=$(curl -s -o /dev/null -w "%{http_code}" \
  --resolve "evil.com:6009:127.0.0.1" \
  -X POST "http://evil.com:6009/mcp" \
  -H "$CT" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"test","version":"0.1"},"capabilities":{}}}')
check_status "Evil host rejected" "403" "$HOST_EVIL_STATUS"

echo ""

# ---- Summary ----
echo "============================================"
echo "  Results: $PASS passed, $FAIL failed"
echo "============================================"

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
