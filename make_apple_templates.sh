#!/bin/bash
set -e

# Godot Engine [FI] — All Apple Templates (iOS + visionOS, device + simulator)

echo "=== iOS Device ==="
BUILD_NAME=fi scons platform=ios target=template_debug arch=arm64 extra_suffix=fi -j$(nproc)
BUILD_NAME=fi scons platform=ios target=template_release arch=arm64 extra_suffix=fi -j$(nproc)

echo "=== iOS Simulator ==="
BUILD_NAME=fi scons platform=ios target=template_debug ios_simulator=yes arch=arm64 extra_suffix=fi -j$(nproc)
BUILD_NAME=fi scons platform=ios target=template_release ios_simulator=yes arch=arm64 extra_suffix=fi -j$(nproc)

echo "=== visionOS Device ==="
BUILD_NAME=fi scons platform=visionos target=template_debug arch=arm64 extra_suffix=fi -j$(nproc)
BUILD_NAME=fi scons platform=visionos target=template_release arch=arm64 extra_suffix=fi -j$(nproc)

echo "=== visionOS Simulator ==="
BUILD_NAME=fi scons platform=visionos target=template_debug simulator=yes arch=arm64 extra_suffix=fi -j$(nproc)
BUILD_NAME=fi scons platform=visionos target=template_release simulator=yes arch=arm64 extra_suffix=fi -j$(nproc)

# Assemble combined apple_embedded_xcode structure
rm -rf apple_embedded_xcode
cp -r misc/dist/apple_embedded_xcode .

# iOS
cp bin/libgodot.ios.template_debug.fi.arm64.a \
    apple_embedded_xcode/libgodot.ios.debug.xcframework/ios-arm64/libgodot.a
cp bin/libgodot.ios.template_release.fi.arm64.a \
    apple_embedded_xcode/libgodot.ios.release.xcframework/ios-arm64/libgodot.a
cp bin/libgodot.ios.template_debug.fi.arm64.simulator.a \
    apple_embedded_xcode/libgodot.ios.debug.xcframework/ios-arm64_x86_64-simulator/libgodot.a
cp bin/libgodot.ios.template_release.fi.arm64.simulator.a \
    apple_embedded_xcode/libgodot.ios.release.xcframework/ios-arm64_x86_64-simulator/libgodot.a

# visionOS
cp bin/libgodot.visionos.template_debug.fi.arm64.a \
    apple_embedded_xcode/libgodot.visionos.debug.xcframework/xros-arm64/libgodot.a
cp bin/libgodot.visionos.template_release.fi.arm64.a \
    apple_embedded_xcode/libgodot.visionos.release.xcframework/xros-arm64/libgodot.a
cp bin/libgodot.visionos.template_debug.fi.arm64.simulator.a \
    apple_embedded_xcode/libgodot.visionos.debug.xcframework/xros-arm64-simulator/libgodot.a
cp bin/libgodot.visionos.template_release.fi.arm64.simulator.a \
    apple_embedded_xcode/libgodot.visionos.release.xcframework/xros-arm64-simulator/libgodot.a

echo ""
echo "Build complete: apple_embedded_xcode/"
echo "  iOS:      libgodot.ios.{debug,release}.xcframework"
echo "  visionOS: libgodot.visionos.{debug,release}.xcframework"
