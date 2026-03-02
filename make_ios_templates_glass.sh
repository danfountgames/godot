#!/bin/bash
set -e

# Godot Engine [FI] — iOS Export Templates "Glass" (stripped-down, LTO)
# Release only, device only. 3D enabled, but no physics, no deprecated APIs, no unused modules.

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

BUILD_NAME=fi scons platform=ios target=template_release arch=arm64 $GLASS_OPTS -j$(sysctl -n hw.ncpu)

# Assemble export templates
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

echo ""
echo "Build complete: templates/ios_glass.zip"
