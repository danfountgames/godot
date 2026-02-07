#!/bin/bash
set -e

# Godot Engine [FI] — visionOS Export Templates (device + simulator)

# Build device templates
BUILD_NAME=fi scons platform=visionos target=template_debug arch=arm64 extra_suffix=fi -j$(nproc)
BUILD_NAME=fi scons platform=visionos target=template_release arch=arm64 extra_suffix=fi -j$(nproc)

# Build simulator templates
BUILD_NAME=fi scons platform=visionos target=template_debug simulator=yes arch=arm64 extra_suffix=fi -j$(nproc)
BUILD_NAME=fi scons platform=visionos target=template_release simulator=yes arch=arm64 extra_suffix=fi -j$(nproc)

# Assemble export templates
mkdir -p templates
rm -rf templates/visionos
cp -r misc/dist/apple_embedded_xcode templates/visionos

# Device
cp bin/libgodot.visionos.template_debug.fi.arm64.a \
    templates/visionos/libgodot.visionos.debug.xcframework/xros-arm64/libgodot.a
cp bin/libgodot.visionos.template_release.fi.arm64.a \
    templates/visionos/libgodot.visionos.release.xcframework/xros-arm64/libgodot.a

# Simulator
cp bin/libgodot.visionos.template_debug.fi.arm64.simulator.a \
    templates/visionos/libgodot.visionos.debug.xcframework/xros-arm64-simulator/libgodot.a
cp bin/libgodot.visionos.template_release.fi.arm64.simulator.a \
    templates/visionos/libgodot.visionos.release.xcframework/xros-arm64-simulator/libgodot.a

# Remove iOS frameworks (visionOS-only zip)
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
