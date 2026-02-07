#!/bin/bash
set -e

# Godot Engine [FI] — Web Export Templates
# No extra_suffix on templates — the editor expects standard filenames.
# Requires Emscripten SDK: source /path/to/emsdk/emsdk_env.sh
BUILD_NAME=fi scons platform=web target=template_debug -j$(nproc)
BUILD_NAME=fi scons platform=web target=template_release -j$(nproc)

echo ""
echo "Build complete:"
echo "  bin/godot.web.template_debug.wasm32.zip"
echo "  bin/godot.web.template_release.wasm32.zip"
