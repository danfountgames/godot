#!/bin/bash
set -e

# Godot Engine [FI] — macOS Editor (arm64)
BUILD_NAME=fi scons platform=macos arch=arm64 dev_build=true extra_suffix=fi -j$(nproc)

# Bundle as "Godot FI.app" so it sits alongside stock Godot.app
rm -rf "./Godot FI.app"
cp -r misc/dist/macos_tools.app "./Godot FI.app"
mkdir -p "Godot FI.app/Contents/MacOS"
cp bin/godot.macos.editor.dev.fi.arm64 "Godot FI.app/Contents/MacOS/Godot"
chmod +x "Godot FI.app/Contents/MacOS/Godot"
codesign --force --timestamp --options=runtime \
    --entitlements misc/dist/macos/editor_debug.entitlements -s - "Godot FI.app"

echo ""
echo "Build complete: Godot FI.app"
