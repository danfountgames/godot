#!/bin/bash
set -e

# Godot Engine [FI] — Android Export Templates
# No extra_suffix on templates — the editor expects standard filenames.
# Requires ANDROID_SDK_ROOT and ANDROID_NDK_ROOT environment variables

echo "=== arm64 ==="
BUILD_NAME=fi scons platform=android target=template_debug arch=arm64 -j$(nproc)
BUILD_NAME=fi scons platform=android target=template_release arch=arm64 -j$(nproc)

echo "=== x86_64 (for emulator) ==="
BUILD_NAME=fi scons platform=android target=template_debug arch=x86_64 -j$(nproc)
BUILD_NAME=fi scons platform=android target=template_release arch=x86_64 -j$(nproc)

echo ""
echo "Build complete. Run 'cd platform/android/java && ./gradlew generateGodotTemplates' to package."
