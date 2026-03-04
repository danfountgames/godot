#!/bin/bash
set -e

# Godot Engine [FI] — Export Templates "Glass" (stripped-down, LTO)
# Release only, device only. 3D enabled, but no physics, no deprecated APIs, no unused modules.
# Targets: iOS (device), macOS.

# Load PCK encryption key (godot.gdkey, gitignored)
GDKEY_FILE="$(dirname "$0")/godot.gdkey"
if [ -f "$GDKEY_FILE" ]; then
    export SCRIPT_AES256_ENCRYPTION_KEY=$(cat "$GDKEY_FILE" | tr -d '[:space:]')
    echo "*** Encryption key loaded from godot.gdkey"
else
    echo "WARNING: godot.gdkey not found — building WITHOUT encryption support"
fi

GLASS_OPTS="extra_suffix=glass \
    disable_physics_2d=yes \
    disable_physics_3d=yes \
    module_multiplayer_enabled=no \
    module_theora_enabled=no \
    module_vhacd_enabled=no \
    module_csg_enabled=no \
    module_gridmap_enabled=no \
    module_openxr_enabled=no \
    module_mobile_vr_enabled=no \
    module_webrtc_enabled=no \
    module_mono_enabled=no"

echo "=== iOS (arm64) ==="
BUILD_NAME=fi scons platform=ios target=template_release arch=arm64 $GLASS_OPTS -j$(sysctl -n hw.ncpu)

echo "=== macOS (arm64) ==="
BUILD_NAME=fi scons platform=macos target=template_release arch=arm64 vulkan_sdk_path=thirdparty/moltenvk/MoltenVK.xcframework $GLASS_OPTS -j$(sysctl -n hw.ncpu)

# --- iOS xcframework ---
mkdir -p templates
rm -rf templates/ios_glass
cp -r misc/dist/apple_embedded_xcode templates/ios_glass

cp bin/libgodot.ios.template_release.arm64.glass.a \
    templates/ios_glass/libgodot.ios.release.xcframework/ios-arm64/libgodot.a

# MoltenVK for Vulkan support
cp -R thirdparty/moltenvk/MoltenVK.xcframework templates/ios_glass/

# Remove simulator, debug, and visionOS frameworks
rm -rf templates/ios_glass/libgodot.ios.debug.xcframework
rm -rf templates/ios_glass/libgodot.ios.release.xcframework/ios-arm64_x86_64-simulator
rm -rf templates/ios_glass/libgodot.visionos.debug.xcframework
rm -rf templates/ios_glass/libgodot.visionos.release.xcframework

# Package
rm -f templates/ios_glass.zip
cd templates/ios_glass
zip -0 -r ../ios_glass.zip *
cd ../../
rm -rf templates/ios_glass

# --- macOS .app bundle ---
rm -rf bin/macos_template.app
cp -r misc/dist/macos_template.app bin/macos_template.app
mkdir -p bin/macos_template.app/Contents/MacOS
cp bin/godot.macos.template_release.arm64.glass \
    bin/macos_template.app/Contents/MacOS/godot_macos_release.arm64
# Use release binary for debug slot too (release-only build)
cp bin/godot.macos.template_release.arm64.glass \
    bin/macos_template.app/Contents/MacOS/godot_macos_debug.arm64

rm -f templates/macos_glass.zip
cd bin
zip -r ../templates/macos_glass.zip macos_template.app
cd ..
rm -rf bin/macos_template.app

echo ""
echo "Build complete:"
echo "  iOS:     templates/ios_glass.zip"
echo "  macOS:   templates/macos_glass.zip"
