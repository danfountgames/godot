#!/bin/bash
set -e

# Godot Engine [FI] — visionOS Export Templates (device only)
# No extra_suffix on templates — the editor expects standard filenames.

# Load PCK encryption key (godot.gdkey, gitignored)
GDKEY_FILE="$(dirname "$0")/godot.gdkey"
if [ -f "$GDKEY_FILE" ]; then
    export SCRIPT_AES256_ENCRYPTION_KEY=$(cat "$GDKEY_FILE" | tr -d '[:space:]')
    echo "*** Encryption key loaded from godot.gdkey"
else
    echo "WARNING: godot.gdkey not found — building WITHOUT encryption support"
fi

BUILD_NAME=fi scons platform=visionos target=template_debug arch=arm64 -j$(sysctl -n hw.ncpu)
BUILD_NAME=fi scons platform=visionos target=template_release arch=arm64 -j$(sysctl -n hw.ncpu)

# Assemble export templates
mkdir -p templates
rm -rf templates/visionos
cp -r misc/dist/apple_embedded_xcode templates/visionos

cp bin/libgodot.visionos.template_debug.arm64.a \
    templates/visionos/libgodot.visionos.debug.xcframework/xros-arm64/libgodot.a
cp bin/libgodot.visionos.template_release.arm64.a \
    templates/visionos/libgodot.visionos.release.xcframework/xros-arm64/libgodot.a

# MoltenVK for Vulkan support
cp -R thirdparty/moltenvk/MoltenVK.xcframework templates/visionos/

# Remove simulator slices and iOS frameworks
rm -rf templates/visionos/libgodot.visionos.debug.xcframework/xros-arm64-simulator
rm -rf templates/visionos/libgodot.visionos.release.xcframework/xros-arm64-simulator
rm -rf templates/visionos/libgodot.ios.debug.xcframework
rm -rf templates/visionos/libgodot.ios.release.xcframework

# Package
rm -f templates/visionos.zip
cd templates/visionos
zip -0 -r ../visionos.zip *
cd ../../
rm -rf templates/visionos

echo ""
echo "Build complete: templates/visionos.zip"
