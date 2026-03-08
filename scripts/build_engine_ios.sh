#!/usr/bin/env bash
set -euo pipefail

# Build the GodotBeam DevPlayer engine for iOS (editor target with devplayer module)
# This is the target deployment build.

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

OUTPUT_DIR="$ROOT_DIR/build-output/ios"
mkdir -p "$OUTPUT_DIR"

echo "=== Building GodotBeam DevPlayer for iOS ==="
echo "Root: $ROOT_DIR"

# Build iOS arm64 with editor target (TOOLS_ENABLED) and devplayer module
scons \
    platform=ios \
    target=editor \
    arch=arm64 \
    module_devplayer_enabled=yes \
    module_mono_enabled=no \
    dev_build=yes \
    vulkan=no \
    metal=yes \
    -j$(nproc)

# Copy output library
BUILT_LIB=$(ls "$ROOT_DIR/bin/libgodot"*.a 2>/dev/null | head -1)
if [ -n "$BUILT_LIB" ]; then
    cp "$BUILT_LIB" "$OUTPUT_DIR/libgodot_devplayer_ios.a"
    echo "=== Build complete ==="
    echo "Output: $OUTPUT_DIR/libgodot_devplayer_ios.a"
else
    echo "ERROR: No .a library found in bin/"
    exit 1
fi
