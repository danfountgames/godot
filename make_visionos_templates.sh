#!/bin/bash
set -e

# Godot Engine [FI] — visionOS Export Templates (device + simulator)
# No extra_suffix on templates — the editor expects standard filenames.

# Build device templates
BUILD_NAME=fi scons platform=visionos target=template_debug arch=arm64 vulkan=no -j$(sysctl -n hw.ncpu)
BUILD_NAME=fi scons platform=visionos target=template_release arch=arm64 vulkan=no -j$(sysctl -n hw.ncpu)

# Build simulator templates
BUILD_NAME=fi scons platform=visionos target=template_debug simulator=yes arch=arm64 vulkan=no -j$(sysctl -n hw.ncpu)
BUILD_NAME=fi scons platform=visionos target=template_release simulator=yes arch=arm64 vulkan=no -j$(sysctl -n hw.ncpu)

# Assemble export templates
mkdir -p templates
rm -rf templates/visionos
cp -r misc/dist/apple_embedded_xcode templates/visionos

# Device
cp bin/libgodot.visionos.template_debug.arm64.a \
    templates/visionos/libgodot.visionos.debug.xcframework/xros-arm64/libgodot.a
cp bin/libgodot.visionos.template_release.arm64.a \
    templates/visionos/libgodot.visionos.release.xcframework/xros-arm64/libgodot.a

# Simulator
cp bin/libgodot.visionos.template_debug.arm64.simulator.a \
    templates/visionos/libgodot.visionos.debug.xcframework/xros-arm64-simulator/libgodot.a
cp bin/libgodot.visionos.template_release.arm64.simulator.a \
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
