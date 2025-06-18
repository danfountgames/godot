#!/bin/bash
set -e  # Exit immediately if a command exits with a non-zero status

scons platform=linux arch=x86_64 dev_build=true tools_enabled=true target=editor
if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

chmod +x bin/godot.linuxbsd.editor.dev.x86_64