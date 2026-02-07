#!/bin/bash
set -e

BUILD_NAME=fi scons platform=linuxbsd target=editor arch=x86_64 dev_build=true extra_suffix=fi -j$(nproc)
chmod +x bin/godot.linuxbsd.editor.dev.fi.x86_64

echo ""
echo "Build complete: bin/godot.linuxbsd.editor.dev.fi.x86_64"
