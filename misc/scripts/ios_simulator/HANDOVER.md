# iOS Simulator template — macOS agent handover

## Objective

Make one custom Godot iOS export template usable from the same exported Xcode project for both physical iOS devices and Xcode iOS Simulator destinations.

The Simulator is for **layout, input, safe-area/window geometry, lifecycle, keyboard and native iOS integration testing**. Rendering fidelity is not a goal. Missing/degraded shaders, refraction, HDR, particles or other effects are acceptable. Physical-device rendering must remain unchanged.

This branch is `ios-simulator-template`, created directly from `main`.

## Implemented on this branch

### 1. Simulator builds always contain Compatibility rendering

`platform/ios/detect.py` already marks simulator builds with `IOS_SIMULATOR`, switches to the iPhoneSimulator SDK, and disables Metal/Vulkan. This branch additionally sets `env["opengl3"] = True` whenever `simulator=yes`.

This makes a plain Simulator build self-contained; callers do not have to remember `opengl3=yes`.

### 2. Simulator startup forces Compatibility

`main/main.cpp` now has an `#ifdef IOS_SIMULATOR` override after project renderer settings have been loaded and before rendering-driver/DisplayServer creation:

    rendering_method = "gl_compatibility";
    rendering_driver = "opengl3";

This intentionally overrides project and command-line renderer choices in the Simulator binary. It must happen before rendering initialization. Do not move it into GDScript/game code.

The block is compile-time Simulator-only. Physical iOS templates must retain the project's normal Mobile/Metal behaviour.

### 3. Template build script

Run from the repository root on a Mac with full Xcode:

    bash misc/scripts/ios_simulator/build_templates.sh

If this checkout uses the bundled SCons entry point:

    SCONS=./scons.py bash misc/scripts/ios_simulator/build_templates.sh

Optional:

    JOBS=12 EXTRA_SCONS_ARGS='production=yes' bash misc/scripts/ios_simulator/build_templates.sh

The script builds debug/release for:
- device arm64
- Simulator arm64
- Simulator x86_64

The final build uses Godot's existing `generate_bundle=yes` machinery. Expected output: `bin/godot_ios.zip`.

Both Simulator architectures are built because the existing XCFramework structure is named `ios-arm64_x86_64-simulator` and its metadata describes both. If current Xcode can no longer build x86_64 Simulator, do not silently put arm64-only bytes under metadata claiming both architectures. Change the XCFramework metadata/directory deliberately and update the verifier.

### 4. Archive verifier

    bash misc/scripts/ios_simulator/verify_template.sh bin/godot_ios.zip

It checks both debug/release XCFrameworks, device arm64, Simulator arm64+x86_64, and plist syntax. Passing this is only a packaging check, not runtime proof.

## Why this uses existing packaging

`platform/ios/platform_ios_builders.py` already calls `generate_bundle_apple_embedded()` with:
- device directory: `ios-arm64`
- Simulator directory: `ios-arm64_x86_64-simulator`

`platform_methods.py` already finds normal and `.simulator` libraries, lipos architecture outputs, and copies them into both debug and release XCFrameworks. The correct approach is therefore to feed that existing machinery the missing Simulator builds rather than invent another template format.

## First Mac session

Start clean and record the environment:

    git checkout ios-simulator-template
    git status
    xcode-select -p
    xcodebuild -version
    xcrun --sdk iphoneos --show-sdk-path
    xcrun --sdk iphonesimulator --show-sdk-path

Then run the build script. Fix compilation/linking issues rather than deleting the Simulator slice.

After a successful archive, make a minimal test project whose normal mobile renderer is Mobile/Metal. Export with this custom template, open the generated Xcode project, select an Apple-silicon iPhone Simulator and run.

## Required runtime acceptance

Simulator:
- Xcode selects the Simulator XCFramework slice without platform mismatch errors.
- App installs and reaches the first Godot scene.
- Console shows the Compatibility override when another renderer was requested.
- A basic Control hierarchy and text are visible.
- Touch/button input coordinates are correct.
- Viewport dimensions match the simulated device.
- Rotation/size changes reach Godot where supported.
- Safe-area values can be queried and drive layout.
- Text input can show/dismiss the software keyboard.
- Background/foreground lifecycle survives.
- Native sheets/callbacks required by the target app work when Apple supports them in Simulator.
- Test both debug and release.

Then, from the **same exported Xcode project**, select a physical iPhone:
- device slice links;
- app launches;
- no Simulator renderer override appears;
- configured Mobile/Metal rendering remains in use;
- test debug and release.

Do not declare this branch complete until both platform families have been run.

## Visual-degradation policy

Acceptable in Simulator:
- flat/placeholder materials;
- unsupported custom shaders;
- no refraction;
- no compute effects;
- no HDR;
- different AA/lighting/colour;
- disabled particles or cosmetic effects.

Not acceptable:
- changed Control geometry caused by the fallback;
- wrong viewport/safe-area coordinate conversion;
- touch coordinates not matching controls;
- startup failure because unsupported rendering resources initialize eagerly;
- lost lifecycle/orientation/window events;
- any Simulator workaround changing physical-device behaviour.

If an app shader/resource crashes Compatibility during load, use a Simulator-specific harmless fallback at project level while preserving dimensions/anchors. Do not re-engineer high-fidelity rendering merely for Simulator.

## Native extensions/plugins

Every required native dependency also needs a Simulator build. arm64-device and arm64-simulator are different Apple platforms despite sharing the CPU architecture name.

For each static library, GDExtension, Swift/Objective-C++ component or iOS plugin:
1. determine whether it is required for boot;
2. build against iPhoneSimulator with the correct target triple;
3. package device + Simulator variants as an XCFramework where appropriate;
4. inspect with `lipo -info`, `xcrun vtool` and Xcode link logs;
5. if an external SDK cannot run in Simulator, use a Simulator stub only when layout/integration testing remains meaningful.

A stub must report unsupported behaviour; it must not fake successful native operations.

## OS-integration testing rule

Keep synthetic and real tests separate.

A synthetic safe-area/keyboard/orientation fixture proves that game layout reacts to supplied values. It does **not** prove UIKit/Swift/Objective-C events reach Godot.

For safe areas, keyboard, orientation, lifecycle, native sheets, URL callbacks and similar features, perform an actual Simulator test and record it separately. Do not hard-code device names, notch dimensions or future form-factor geometry in this engine branch.

## Failure triage

1. **Link failure:** check iphoneos vs iphonesimulator for every library, not only libgodot.
2. **dyld failure:** inspect embedded frameworks and their platform slices.
3. **Exit before DisplayServer:** verify GLES3 was compiled and the `IOS_SIMULATOR` startup block executes.
4. **OpenGL/DisplayServer creation failure:** capture full console, Xcode version, SDK and Simulator runtime. Confirm Apple's current runtime still supports the APIs Godot's iOS GLES path needs.
5. **Scene starts then resource/shader fails:** treat as app compatibility and degrade the effect.
6. **Layout wrong:** inspect viewport, content scale, safe area and coordinate conversion. Do not add arbitrary device padding.
7. **Native API fails:** first establish whether Apple implements that API in Simulator.

Record exact commands, Xcode/SDK/runtime versions, errors and fixes in this document or a dated adjacent note.

## Important non-goals

- Do not implement a CPU software renderer.
- Do not downgrade physical iOS to Compatibility.
- Do not mutate project renderer settings on disk.
- Do not require developers to switch renderer settings before/after Simulator runs.
- Do not fake OS-integration success with injected values.
- Do not put Glass-specific layout policy into Godot.

"Software rendering" in the original request means "a low-fidelity rendering path sufficient to see/test the app." The current implementation uses Godot Compatibility/OpenGL ES for that purpose.

## After first successful launch

Only after the minimal route works:
- add a macOS CI/build-machine job for all six template builds;
- add a tiny smoke project configured for Mobile that asserts the Simulator reports `gl_compatibility`;
- add an engine test for the forced renderer if practical;
- document the custom-template release process so every release includes both device and Simulator slices;
- consider arm64-only Simulator packaging if Intel Simulator support is no longer needed, but update XCFramework metadata correctly.

## Current status

Source changes and build/verification automation are committed on this branch. They have **not been compiled or runtime-tested on macOS/Xcode from this session**. The next agent's first responsibility is therefore build + runtime validation, not adding more features.
