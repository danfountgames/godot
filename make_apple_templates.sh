#!/bin/bash
set -e

# Build all Apple templates (iOS device + simulator)
BUILD_NAME=fi scons platform=ios target=template_debug arch=arm64 extra_suffix=fi -j$(nproc)
BUILD_NAME=fi scons platform=ios target=template_release arch=arm64 extra_suffix=fi -j$(nproc)
BUILD_NAME=fi scons platform=ios target=template_debug ios_simulator=yes arch=arm64 extra_suffix=fi -j$(nproc)
BUILD_NAME=fi scons platform=ios target=template_release ios_simulator=yes arch=arm64 extra_suffix=fi -j$(nproc)

# Copy into ios_xcode structure
cp -r misc/dist/ios_xcode .

cp bin/libgodot.ios.template_debug.fi.arm64.a \
    ios_xcode/libgodot.ios.debug.xcframework/ios-arm64/libgodot.a
cp bin/libgodot.ios.template_release.fi.arm64.a \
    ios_xcode/libgodot.ios.release.xcframework/ios-arm64/libgodot.a
cp bin/libgodot.ios.template_debug.fi.arm64.simulator.a \
    ios_xcode/libgodot.ios.debug.xcframework/ios-arm64_x86_64-simulator/libgodot.a
cp bin/libgodot.ios.template_release.fi.arm64.simulator.a \
    ios_xcode/libgodot.ios.release.xcframework/ios-arm64_x86_64-simulator/libgodot.a

echo ""
echo "Build complete: ios_xcode/"
