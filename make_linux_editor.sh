#!/bin/bash
set -e

# Godot Engine [FI] — Linux Editor (x86_64)
BUILD_NAME=fi scons platform=linuxbsd target=editor arch=x86_64 dev_build=true extra_suffix=fi -j$(nproc)

# Symlink with FI-branded name
ln -sf godot.linuxbsd.editor.dev.x86_64.fi bin/godot-fi

echo ""
echo "Build complete: bin/godot.linuxbsd.editor.dev.x86_64.fi"
echo "  (symlinked as bin/godot-fi)"
