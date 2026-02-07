#!/bin/bash
set -e

# Godot Engine [FI] — Linux Editor (x86_64)
BUILD_NAME=fi scons platform=linuxbsd target=editor arch=x86_64 dev_build=true extra_suffix=fi -j$(nproc)

# Rename binary to include FI branding
cp bin/godot.linuxbsd.editor.dev.fi.x86_64 bin/godot-fi.linuxbsd.editor.dev.x86_64
chmod +x bin/godot-fi.linuxbsd.editor.dev.x86_64

echo ""
echo "Build complete: bin/godot-fi.linuxbsd.editor.dev.x86_64"
echo "  (also available as bin/godot.linuxbsd.editor.dev.fi.x86_64)"
