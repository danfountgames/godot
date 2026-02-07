#!/bin/bash
set -e

# Godot Engine [FI] — Web Export Templates
# Requires Emscripten SDK: source /path/to/emsdk/emsdk_env.sh
BUILD_NAME=fi scons platform=web target=template_debug extra_suffix=fi -j$(nproc)
BUILD_NAME=fi scons platform=web target=template_release extra_suffix=fi -j$(nproc)

echo ""
echo "Build complete:"
echo "  bin/godot.web.template_debug.fi.wasm32.zip"
echo "  bin/godot.web.template_release.fi.wasm32.zip"
