scons platform=macos arch=arm64 dev_build=true metal=true
rm -rf ./Godot.app
cp -r misc/dist/macos_tools.app ./Godot.app
mkdir -p Godot.app/Contents/MacOS
cp bin/godot.macos.editor.dev.arm64 Godot.app/Contents/MacOS/Godot
chmod +x Godot.app/Contents/MacOS/Godot
codesign --force --timestamp --options=runtime --entitlements misc/dist/macos/editor_debug.entitlements -s - Godot.app


