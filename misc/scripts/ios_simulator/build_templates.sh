#!/usr/bin/env bash
set -euo pipefail

# Build device + universal Simulator iOS templates, then assemble bin/godot_ios.zip.
# Run on macOS/Xcode from the repository root.

SCONS="${SCONS:-scons}"
JOBS="${JOBS:-}"
EXTRA_SCONS_ARGS="${EXTRA_SCONS_ARGS:-}"

[[ "$(uname -s)" == "Darwin" ]] || { echo "error: requires macOS/Xcode" >&2; exit 2; }
xcrun --sdk iphoneos --show-sdk-path >/dev/null
xcrun --sdk iphonesimulator --show-sdk-path >/dev/null

common=(platform=ios)
if [[ -n "$JOBS" ]]; then common+=("-j$JOBS"); fi
# shellcheck disable=SC2206
extra=($EXTRA_SCONS_ARGS)

run() { printf "\n==>"; printf " %q" "$@"; printf "\n"; "$@"; }

# Device renderer configuration is intentionally left unchanged.
run "$SCONS" "${common[@]}" target=template_debug arch=arm64 "${extra[@]}"
run "$SCONS" "${common[@]}" target=template_release arch=arm64 "${extra[@]}"

# detect.py forces Compatibility/OpenGL ES for Simulator builds.
run "$SCONS" "${common[@]}" target=template_debug arch=arm64 simulator=yes "${extra[@]}"
run "$SCONS" "${common[@]}" target=template_release arch=arm64 simulator=yes "${extra[@]}"
run "$SCONS" "${common[@]}" target=template_debug arch=x86_64 simulator=yes "${extra[@]}"
run "$SCONS" "${common[@]}" target=template_release arch=x86_64 simulator=yes generate_bundle=yes "${extra[@]}"

archive="bin/godot_ios.zip"
[[ -f "$archive" ]] || { echo "error: expected $archive" >&2; exit 1; }
echo "Built $archive"
bash misc/scripts/ios_simulator/verify_template.sh "$archive"
