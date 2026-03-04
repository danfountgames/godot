#!/bin/bash
set -e

# Godot Engine [FI] — Export Templates "Raincaster" (2D only, stripped-down, LTO)
# Release only. No 3D, no physics, no deprecated APIs, no unused modules.
# Targets: iOS (device), macOS, Web, Windows, Linux.

# Load PCK encryption key (godot.gdkey, gitignored)
GDKEY_FILE="$(dirname "$0")/godot.gdkey"
if [ -f "$GDKEY_FILE" ]; then
    export SCRIPT_AES256_ENCRYPTION_KEY=$(cat "$GDKEY_FILE" | tr -d '[:space:]')
    echo "*** Encryption key loaded from godot.gdkey"
else
    echo "WARNING: godot.gdkey not found — building WITHOUT encryption support"
fi

RC_OPTS="extra_suffix=raincaster lto=thin deprecated=no \
    disable_3d=yes \
    disable_physics_2d=yes \
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
BUILD_NAME=fi scons platform=ios target=template_release arch=arm64 $RC_OPTS -j$(sysctl -n hw.ncpu)

echo "=== macOS (arm64) ==="
BUILD_NAME=fi scons platform=macos target=template_release arch=arm64 vulkan_sdk_path=thirdparty/moltenvk/MoltenVK.xcframework $RC_OPTS -j$(sysctl -n hw.ncpu)

echo "=== Web (wasm32) ==="
BUILD_NAME=fi scons platform=web target=template_release arch=wasm32 $RC_OPTS -j$(sysctl -n hw.ncpu)
BUILD_NAME=fi scons platform=web target=template_release arch=wasm32 threads=no $RC_OPTS -j$(sysctl -n hw.ncpu)

echo "=== Windows (x86_64, cross-compile) ==="
#BUILD_NAME=fi scons platform=windows target=template_release arch=x86_64 $RC_OPTS -j$(sysctl -n hw.ncpu)

echo "=== Linux (x86_64, cross-compile) ==="
#BUILD_NAME=fi scons platform=linuxbsd target=template_release arch=x86_64 $RC_OPTS -j$(sysctl -n hw.ncpu)

# --- iOS xcframework ---
mkdir -p templates
rm -rf templates/ios_raincaster
cp -r misc/dist/apple_embedded_xcode templates/ios_raincaster

cp bin/libgodot.ios.template_release.arm64.raincaster.a \
    templates/ios_raincaster/libgodot.ios.release.xcframework/ios-arm64/libgodot.a

# MoltenVK for Vulkan support
cp -R thirdparty/moltenvk/MoltenVK.xcframework templates/ios_raincaster/

rm -rf templates/ios_raincaster/libgodot.ios.debug.xcframework
rm -rf templates/ios_raincaster/libgodot.ios.release.xcframework/ios-arm64_x86_64-simulator
#rm -rf templates/ios_raincaster/libgodot.visionos.debug.xcframework
#rm -rf templates/ios_raincaster/libgodot.visionos.release.xcframework

rm -f templates/ios_raincaster.zip
cd templates/ios_raincaster
zip -0 -r ../ios_raincaster.zip *
cd ../../
rm -rf templates/ios_raincaster

# --- macOS .app bundle ---
rm -rf bin/macos_template.app
cp -r misc/dist/macos_template.app bin/macos_template.app
mkdir -p bin/macos_template.app/Contents/MacOS
cp bin/godot.macos.template_release.arm64.raincaster \
    bin/macos_template.app/Contents/MacOS/godot_macos_release.arm64
# Use release binary for debug slot too (release-only build)
cp bin/godot.macos.template_release.arm64.raincaster \
    bin/macos_template.app/Contents/MacOS/godot_macos_debug.arm64

rm -f templates/macos_raincaster.zip
cd bin
zip -r ../templates/macos_raincaster.zip macos_template.app
cd ..
rm -rf bin/macos_template.app

# --- Web templates (already zipped by SCons) ---
cp bin/godot.web.template_release.wasm32.raincaster.zip templates/web_raincaster.zip
cp bin/godot.web.template_release.wasm32.nothreads.raincaster.zip templates/web_nothreads_raincaster.zip

echo ""
echo "Build complete:"
echo "  iOS:     templates/ios_raincaster.zip"
echo "  macOS:   templates/macos_raincaster.zip"
echo "  Web:     templates/web_raincaster.zip"
echo "  Web:     templates/web_nothreads_raincaster.zip"
#echo "  Windows: bin/godot.windows.template_release.x86_64.raincaster.exe"
#echo "  Linux:   bin/godot.linuxbsd.template_release.x86_64.raincaster"
