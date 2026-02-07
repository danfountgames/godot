#!/bin/bash
set -e

# Godot Engine [FI] — Android Export Templates
# Requires ANDROID_SDK_ROOT and ANDROID_NDK_ROOT environment variables

echo "=== arm64 ==="
BUILD_NAME=fi scons platform=android target=template_debug arch=arm64 extra_suffix=fi -j$(nproc)
BUILD_NAME=fi scons platform=android target=template_release arch=arm64 extra_suffix=fi -j$(nproc)

echo "=== x86_64 (for emulator) ==="
BUILD_NAME=fi scons platform=android target=template_debug arch=x86_64 extra_suffix=fi -j$(nproc)
BUILD_NAME=fi scons platform=android target=template_release arch=x86_64 extra_suffix=fi -j$(nproc)

echo ""
echo "Build complete. Run 'cd platform/android/java && ./gradlew generateGodotTemplates' to package."
