#!/bin/bash
set -e

# Godot Engine [FI] — Windows Editor (cross-compile from Linux/macOS)
# Requires mingw-w64: apt install mingw-w64 (Linux) or brew install mingw-w64 (macOS)
BUILD_NAME=fi scons platform=windows target=editor arch=x86_64 dev_build=true extra_suffix=fi -j$(nproc)

echo ""
echo "Build complete: bin/godot.windows.editor.dev.fi.x86_64.exe"
