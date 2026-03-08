#!/usr/bin/env bash
set -euo pipefail

# Build the GodotBeam DevPlayer engine for Linux (editor target with devplayer module)
# This is the primary development build for testing on desktop before iOS deployment.

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

echo "=== Building GodotBeam DevPlayer for Linux ==="
echo "Root: $ROOT_DIR"

# Build with editor target + devplayer module enabled
scons \
    platform=linuxbsd \
    target=editor \
    module_devplayer_enabled=yes \
    dev_build=yes \
    -j$(nproc)

echo "=== Build complete ==="
echo "Binary: $ROOT_DIR/bin/godot.linuxbsd.editor.dev.x86_64"
