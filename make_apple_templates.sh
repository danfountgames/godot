#!/bin/bash
set -e

# Godot Engine [FI] — All Apple Templates (iOS + visionOS, device only)
# No extra_suffix on templates — the editor expects standard filenames.

# Load PCK encryption key (godot.gdkey, gitignored)
GDKEY_FILE="$(dirname "$0")/godot.gdkey"
if [ -f "$GDKEY_FILE" ]; then
    export SCRIPT_AES256_ENCRYPTION_KEY=$(cat "$GDKEY_FILE" | tr -d '[:space:]')
    echo "*** Encryption key loaded from godot.gdkey"
else
    echo "WARNING: godot.gdkey not found — building WITHOUT encryption support"
fi

echo "=== iOS Device ==="
BUILD_NAME=fi scons platform=ios target=template_debug arch=arm64 -j$(sysctl -n hw.ncpu)
BUILD_NAME=fi scons platform=ios target=template_release arch=arm64 -j$(sysctl -n hw.ncpu)

echo "=== visionOS Device ==="
BUILD_NAME=fi scons platform=visionos target=template_debug arch=arm64 -j$(sysctl -n hw.ncpu)
BUILD_NAME=fi scons platform=visionos target=template_release arch=arm64 -j$(sysctl -n hw.ncpu)

# Assemble combined apple_embedded_xcode structure
rm -rf apple_embedded_xcode
cp -r misc/dist/apple_embedded_xcode .

# iOS
cp bin/libgodot.ios.template_debug.arm64.a \
    apple_embedded_xcode/libgodot.ios.debug.xcframework/ios-arm64/libgodot.a
cp bin/libgodot.ios.template_release.arm64.a \
    apple_embedded_xcode/libgodot.ios.release.xcframework/ios-arm64/libgodot.a

# visionOS
cp bin/libgodot.visionos.template_debug.arm64.a \
    apple_embedded_xcode/libgodot.visionos.debug.xcframework/xros-arm64/libgodot.a
cp bin/libgodot.visionos.template_release.arm64.a \
    apple_embedded_xcode/libgodot.visionos.release.xcframework/xros-arm64/libgodot.a

# MoltenVK for Vulkan support
cp -R thirdparty/moltenvk/MoltenVK.xcframework apple_embedded_xcode/

# Remove simulator slices
rm -rf apple_embedded_xcode/libgodot.ios.debug.xcframework/ios-arm64_x86_64-simulator
rm -rf apple_embedded_xcode/libgodot.ios.release.xcframework/ios-arm64_x86_64-simulator
rm -rf apple_embedded_xcode/libgodot.visionos.debug.xcframework/xros-arm64-simulator
rm -rf apple_embedded_xcode/libgodot.visionos.release.xcframework/xros-arm64-simulator

echo ""
echo "Build complete: apple_embedded_xcode/"
echo "  iOS:      libgodot.ios.{debug,release}.xcframework"
echo "  visionOS: libgodot.visionos.{debug,release}.xcframework"
