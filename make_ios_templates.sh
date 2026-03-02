#!/bin/bash
set -e

# Godot Engine [FI] — iOS Export Templates (device only)
# No extra_suffix on templates — the editor expects standard filenames.

# Load PCK encryption key (godot.gdkey, gitignored)
GDKEY_FILE="$(dirname "$0")/godot.gdkey"
if [ -f "$GDKEY_FILE" ]; then
    export SCRIPT_AES256_ENCRYPTION_KEY=$(cat "$GDKEY_FILE" | tr -d '[:space:]')
    echo "*** Encryption key loaded from godot.gdkey"
else
    echo "WARNING: godot.gdkey not found — building WITHOUT encryption support"
fi

BUILD_NAME=fi scons platform=ios target=template_debug arch=arm64 -j$(sysctl -n hw.ncpu)
BUILD_NAME=fi scons platform=ios target=template_release arch=arm64 -j$(sysctl -n hw.ncpu)

# Assemble export templates
mkdir -p templates
rm -rf templates/ios
cp -r misc/dist/apple_embedded_xcode templates/ios

cp bin/libgodot.ios.template_debug.arm64.a \
    templates/ios/libgodot.ios.debug.xcframework/ios-arm64/libgodot.a
cp bin/libgodot.ios.template_release.arm64.a \
    templates/ios/libgodot.ios.release.xcframework/ios-arm64/libgodot.a

# MoltenVK for Vulkan support
cp -R thirdparty/moltenvk/MoltenVK.xcframework templates/ios/

# Remove simulator slices and visionOS frameworks
rm -rf templates/ios/libgodot.ios.debug.xcframework/ios-arm64_x86_64-simulator
rm -rf templates/ios/libgodot.ios.release.xcframework/ios-arm64_x86_64-simulator
rm -rf templates/ios/libgodot.visionos.debug.xcframework
rm -rf templates/ios/libgodot.visionos.release.xcframework

# Package
rm -f templates/ios.zip
cd templates/ios
zip -0 -r ../ios.zip *
cd ../../
rm -rf templates/ios

echo ""
echo "Build complete: templates/ios.zip"
