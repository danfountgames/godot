#!/bin/bash
# Connects Claude Code to Godot FI editor(s) via the MCP proxy.
# Starts the proxy if not already running, then registers it with Claude Code.
# The proxy auto-discovers all running Godot editors with MCP enabled.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROXY_SCRIPT="$SCRIPT_DIR/modules/mcp_server/godot_mcp_proxy.py"
DISCOVERY_DIR="$HOME/Library/Application Support/Godot/mcp_server"
PROXY_DISCOVERY="$DISCOVERY_DIR/proxy_discovery.json"
INSTANCE_DISCOVERY_DIR="$DISCOVERY_DIR/discovery"
PROXY_PORT=6100
PROXY_HOST="127.0.0.1"

# --- Check for per-instance discovery files (new format) ---
if [ -d "$INSTANCE_DISCOVERY_DIR" ] && [ "$(ls -A "$INSTANCE_DISCOVERY_DIR" 2>/dev/null)" ]; then
    echo "Found Godot editor instance(s):"
    for f in "$INSTANCE_DISCOVERY_DIR"/*.json; do
        port=$(basename "$f" .json)
        project=$(python3 -c "import json; print(json.load(open('$f')).get('project_name', 'Unknown'))" 2>/dev/null)
        pid=$(python3 -c "import json; print(json.load(open('$f')).get('pid', '?'))" 2>/dev/null)
        echo "  - Port $port: $project (PID $pid)"
    done
    echo ""
else
    # Fall back to legacy discovery.json
    LEGACY="$DISCOVERY_DIR/discovery.json"
    if [ -f "$LEGACY" ]; then
        echo "Found Godot editor (legacy discovery)."
    else
        echo "No Godot editor instances found. Start a Godot FI editor with MCP enabled first."
        exit 1
    fi
fi

# --- Start proxy if not already running ---
if [ -f "$PROXY_DISCOVERY" ]; then
    PROXY_PID=$(python3 -c "import json; print(json.load(open('$PROXY_DISCOVERY'))['pid'])" 2>/dev/null)
    if [ -n "$PROXY_PID" ] && kill -0 "$PROXY_PID" 2>/dev/null; then
        echo "Proxy already running (PID $PROXY_PID)."
    else
        echo "Stale proxy discovery file found, removing."
        rm -f "$PROXY_DISCOVERY"
        PROXY_PID=""
    fi
fi

if [ -z "$PROXY_PID" ] || ! kill -0 "$PROXY_PID" 2>/dev/null; then
    # Kill anything squatting on our port
    STALE=$(lsof -ti :"$PROXY_PORT" 2>/dev/null)
    if [ -n "$STALE" ]; then
        echo "Killing stale process on port $PROXY_PORT (PID $STALE)..."
        kill "$STALE" 2>/dev/null
        sleep 1
        kill -9 "$STALE" 2>/dev/null
        sleep 0.5
    fi

    echo "Starting MCP proxy on port $PROXY_PORT..."
    python3 "$PROXY_SCRIPT" --port "$PROXY_PORT" --host "$PROXY_HOST" &
    PROXY_BG_PID=$!

    # Wait for proxy to write its discovery file (up to 5 seconds)
    for i in $(seq 1 50); do
        if [ -f "$PROXY_DISCOVERY" ]; then
            break
        fi
        sleep 0.1
    done

    if [ ! -f "$PROXY_DISCOVERY" ]; then
        echo "Proxy failed to start (no discovery file after 5s)."
        kill "$PROXY_BG_PID" 2>/dev/null
        exit 1
    fi

    PROXY_PID=$(python3 -c "import json; print(json.load(open('$PROXY_DISCOVERY'))['pid'])" 2>/dev/null)
    echo "Proxy started (PID $PROXY_PID)."
fi

# --- Read proxy token and endpoint ---
TOKEN=$(python3 -c "import json; print(json.load(open('$PROXY_DISCOVERY'))['token'])")
ENDPOINT=$(python3 -c "import json; print(json.load(open('$PROXY_DISCOVERY'))['endpoint'])")

# Verify proxy is responding
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" --max-time 2 "$ENDPOINT" 2>/dev/null)
if [ "$HTTP_CODE" = "000" ]; then
    echo "Proxy at $ENDPOINT is not responding."
    exit 1
fi

# --- Register proxy with Claude Code ---
claude mcp remove godot-engine -s user 2>/dev/null
claude mcp add -t http -s user \
    godot-engine "$ENDPOINT" \
    -H "Authorization: Bearer $TOKEN"

echo ""
echo "Proxy registered at $ENDPOINT (PID $PROXY_PID)"
echo "The proxy auto-discovers all running Godot editors."
echo "Use the proxy/list_instances tool to see editors, and proxy/switch_instance to switch."
echo ""
echo "If this is your first time, restart Claude Code to pick up the new MCP server."
