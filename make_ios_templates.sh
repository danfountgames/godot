#!/bin/bash
set -e

# Build device templates
BUILD_NAME=fi scons platform=ios target=template_debug arch=arm64 extra_suffix=fi -j$(nproc)
BUILD_NAME=fi scons platform=ios target=template_release arch=arm64 extra_suffix=fi -j$(nproc)

# Build simulator templates
BUILD_NAME=fi scons platform=ios target=template_debug ios_simulator=yes arch=arm64 extra_suffix=fi -j$(nproc)
BUILD_NAME=fi scons platform=ios target=template_release ios_simulator=yes arch=arm64 extra_suffix=fi -j$(nproc)

# Assemble export templates
mkdir -p templates
rm -rf templates/ios
cp -r misc/dist/ios_xcode templates/ios

cp bin/libgodot.ios.template_debug.fi.arm64.a \
    templates/ios/libgodot.ios.debug.xcframework/ios-arm64/libgodot.a
cp bin/libgodot.ios.template_release.fi.arm64.a \
    templates/ios/libgodot.ios.release.xcframework/ios-arm64/libgodot.a
cp bin/libgodot.ios.template_debug.fi.arm64.simulator.a \
    templates/ios/libgodot.ios.debug.xcframework/ios-arm64_x86_64-simulator/libgodot.a
cp bin/libgodot.ios.template_release.fi.arm64.simulator.a \
    templates/ios/libgodot.ios.release.xcframework/ios-arm64_x86_64-simulator/libgodot.a

# Remove x86_64 arch references (arm64-only simulator)
sed -i '' '/<string>x86_64<\/string>/d' templates/ios/libgodot.ios.debug.xcframework/Info.plist
sed -i '' '/<string>x86_64<\/string>/d' templates/ios/libgodot.ios.release.xcframework/Info.plist

# Package
rm -f templates/ios.zip
cd templates/ios
zip -0 -r ../ios.zip *
cd ../../
rm -rf templates/ios

echo ""
echo "Build complete: templates/ios.zip"
