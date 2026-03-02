#!/bin/bash
set -e

# Godot Engine [FI] — Web Export Templates
# No extra_suffix on templates — the editor expects standard filenames.
# Requires Emscripten SDK: source /path/to/emsdk/emsdk_env.sh

# Load PCK encryption key (godot.gdkey, gitignored)
GDKEY_FILE="$(dirname "$0")/godot.gdkey"
if [ -f "$GDKEY_FILE" ]; then
    export SCRIPT_AES256_ENCRYPTION_KEY=$(cat "$GDKEY_FILE" | tr -d '[:space:]')
    echo "*** Encryption key loaded from godot.gdkey"
else
    echo "WARNING: godot.gdkey not found — building WITHOUT encryption support"
fi

BUILD_NAME=fi scons platform=web target=template_debug -j$(nproc)
BUILD_NAME=fi scons platform=web target=template_release -j$(nproc)

echo ""
echo "Build complete:"
echo "  bin/godot.web.template_debug.wasm32.zip"
echo "  bin/godot.web.template_release.wasm32.zip"
