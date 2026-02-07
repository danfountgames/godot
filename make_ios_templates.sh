#!/bin/bash
set -e

# Godot Engine [FI] — iOS Export Templates (device + simulator)

# Build device templates
BUILD_NAME=fi scons platform=ios target=template_debug arch=arm64 extra_suffix=fi -j$(nproc)
BUILD_NAME=fi scons platform=ios target=template_release arch=arm64 extra_suffix=fi -j$(nproc)

# Build simulator templates
BUILD_NAME=fi scons platform=ios target=template_debug ios_simulator=yes arch=arm64 extra_suffix=fi -j$(nproc)
BUILD_NAME=fi scons platform=ios target=template_release ios_simulator=yes arch=arm64 extra_suffix=fi -j$(nproc)

# Assemble export templates
mkdir -p templates
rm -rf templates/ios
cp -r misc/dist/apple_embedded_xcode templates/ios

# Device
cp bin/libgodot.ios.template_debug.fi.arm64.a \
    templates/ios/libgodot.ios.debug.xcframework/ios-arm64/libgodot.a
cp bin/libgodot.ios.template_release.fi.arm64.a \
    templates/ios/libgodot.ios.release.xcframework/ios-arm64/libgodot.a

# Simulator
cp bin/libgodot.ios.template_debug.fi.arm64.simulator.a \
    templates/ios/libgodot.ios.debug.xcframework/ios-arm64_x86_64-simulator/libgodot.a
cp bin/libgodot.ios.template_release.fi.arm64.simulator.a \
    templates/ios/libgodot.ios.release.xcframework/ios-arm64_x86_64-simulator/libgodot.a

# Remove x86_64 arch references (arm64-only simulator)
sed -i '' '/<string>x86_64<\/string>/d' templates/ios/libgodot.ios.debug.xcframework/Info.plist
sed -i '' '/<string>x86_64<\/string>/d' templates/ios/libgodot.ios.release.xcframework/Info.plist

# Remove visionOS frameworks (iOS-only zip)
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
