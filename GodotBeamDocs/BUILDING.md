# Building GodotBeam DevPlayer

## Prerequisites

- **Python 3.6+** -- required by the SCons build system
- **SCons 4.0+** -- Godot's build system (`pip install scons`)
- **C++17 compiler** -- GCC 9+, Clang 10+, or Apple Clang (Xcode 13+)
- **Platform-specific dependencies** -- see the [Godot compiling docs](https://docs.godotengine.org/en/stable/contributing/development/compiling/) for your platform's prerequisites (e.g., `libx11-dev`, `libasound2-dev`, etc. on Linux)

For iOS builds additionally:
- **macOS host** with Xcode 13+ installed
- **iOS SDK** (provided by Xcode)
- **MoltenVK** or Metal support (Metal is enabled by default)

## Module Structure

The DevPlayer module lives at `modules/devplayer/` and follows Godot's standard module conventions:

```
modules/devplayer/
  config.py          -- Module configuration (can_build, doc classes)
  SCsub              -- Build script (compiles all *.cpp in the directory)
  register_types.cpp -- Singleton registration and initialization
  *.cpp / *.h        -- 10 subsystem implementations
```

The `config.py` file defines:
- `can_build()` returns `True` for all platforms
- `get_doc_classes()` returns all 10 class names plus `DevPlayerShell`
- `get_doc_path()` returns `"doc_classes"`

The `SCsub` file is minimal -- it clones the module environment and compiles all `.cpp` files:

```python
env_devplayer = env_modules.Clone()
env_devplayer.add_source_files(env.modules_sources, "*.cpp")
```

## Building for Linux

```bash
scons platform=linuxbsd target=editor module_devplayer_enabled=yes -j$(nproc)
```

This produces a binary at:
```
bin/godot.linuxbsd.editor.x86_64
```

Key flags:
- `platform=linuxbsd` -- Linux build
- `target=editor` -- includes editor subsystems (required for import pipeline, EditorFileSystem, etc.)
- `module_devplayer_enabled=yes` -- enables the DevPlayer module
- `-j$(nproc)` -- parallel compilation using all CPU cores

## Building for iOS

```bash
scons platform=ios target=editor tools=yes module_devplayer_enabled=yes arch=arm64
```

Key flags:
- `platform=ios` -- iOS build targeting physical devices
- `target=editor` -- overrides the default `template_debug` target for iOS
- `tools=yes` -- **critical**: enables `TOOLS_ENABLED` preprocessor define
- `module_devplayer_enabled=yes` -- enables the DevPlayer module
- `arch=arm64` -- ARM64 architecture (required for physical iOS devices)

### Why `tools=yes` is Required for iOS

DevPlayer depends on several editor-only APIs at runtime:

- `EditorFileSystem::scan()` -- used by `ImportSessionManager` to trigger asset re-import when a project is mounted
- `ProjectSettings::load_custom()` -- used to load the mounted project's `project.godot` at runtime
- Various resource import pipelines that are gated behind `#ifdef TOOLS_ENABLED`

Without `tools=yes`, these APIs are compiled out and the module will not function correctly.

Standard Godot iOS builds use `target=template_debug` or `target=template_release`, which do not include editor APIs. The GodotBeam fork explicitly supports `target=editor` on iOS by noting in `platform/ios/detect.py` that the `supported` list only blocks "library" builds, not editor builds.

### iOS Auto-Injection

When building with `MODULE_DEVPLAYER_ENABLED`, the iOS entry point (`platform/ios/main_ios.mm`) automatically injects the `--devplayer` flag into the argument list. This means iOS builds always boot directly into the DevPlayer shell without requiring manual argument configuration.

### iOS Simulator Builds

For simulator builds (testing on Mac without a physical device):

```bash
scons platform=ios target=editor tools=yes module_devplayer_enabled=yes arch=x86_64 simulator=yes
```

Note: Metal is automatically disabled for simulator builds. Vulkan is also unsupported on the iOS simulator.

## Disabling the Module

To build standard Godot without the DevPlayer module:

```bash
scons platform=linuxbsd target=editor module_devplayer_enabled=no -j$(nproc)
```

Or simply omit the flag -- modules default to disabled unless explicitly enabled:

```bash
scons platform=linuxbsd target=editor -j$(nproc)
```

## Verifying the Build

### Quick smoke test (headless, auto-quit)

```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer --headless --quit-after 2000
```

Expected output:
```
[DevPlayer] Module initialized. All singletons registered.
DevPlayer: Shell UI created and added to scene tree.
[DevPlayer] Module shutting down...
[DevPlayer] Module shut down.
```

This confirms:
1. The module initialized and all 10 singletons were created
2. The DevPlayerShell UI was built and added to the scene tree
3. Clean shutdown occurred after the 2-second timeout

### Mount a test project

```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer --headless \
    --devplayer-mount test_projects/minimal_2d \
    --quit-after 5000
```

Expected output includes:
```
[LaunchController] === LAUNCHING PROJECT ===
[ProjectSettingsLayerManager] Project settings loaded successfully.
[ProjectDomainManager] Project mounted: ...
[LaunchController] Scene instantiated and added to SceneTree: res://main.tscn
[LaunchController] === PROJECT LAUNCHED in ...s ===
Minimal 2D project loaded
```

### Run the full automated test suite

```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer-test --headless
```

This runs 6 named mount/unmount cycles plus 50 stress cycles. See `TESTING.md` for details.

## Build Variants

| Variant | Command | Use Case |
|---------|---------|----------|
| Linux Debug | `scons platform=linuxbsd target=editor module_devplayer_enabled=yes dev_build=yes -j$(nproc)` | Development with debug symbols and assertions |
| Linux Release | `scons platform=linuxbsd target=editor module_devplayer_enabled=yes -j$(nproc)` | Standard development build |
| iOS Device | `scons platform=ios target=editor tools=yes module_devplayer_enabled=yes arch=arm64` | Physical iPhone/iPad |
| iOS Simulator | `scons platform=ios target=editor tools=yes module_devplayer_enabled=yes arch=x86_64 simulator=yes` | Xcode simulator testing |
