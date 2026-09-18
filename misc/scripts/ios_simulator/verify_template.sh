#!/usr/bin/env bash
set -euo pipefail

archive="${1:-bin/godot_ios.zip}"
[[ "$(uname -s)" == "Darwin" ]] || { echo "error: verifier requires macOS" >&2; exit 2; }
[[ -f "$archive" ]] || { echo "error: archive not found: $archive" >&2; exit 2; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
ditto -x -k "$archive" "$tmp"

for kind in debug release; do
  xc="$tmp/libgodot.ios.$kind.xcframework"
  dev="$xc/ios-arm64/libgodot.a"
  sim="$xc/ios-arm64_x86_64-simulator/libgodot.a"
  info="$xc/Info.plist"
  [[ -f "$dev" && -f "$sim" && -f "$info" ]] || { echo "missing $kind XCFramework content" >&2; exit 1; }
  dev_archs="$(lipo -archs "$dev")"
  sim_archs="$(lipo -archs "$sim")"
  echo "[$kind] device: $dev_archs"
  echo "[$kind] simulator: $sim_archs"
  [[ " $dev_archs " == *" arm64 "* ]] || { echo "$kind device lacks arm64" >&2; exit 1; }
  [[ " $sim_archs " == *" arm64 "* ]] || { echo "$kind simulator lacks arm64" >&2; exit 1; }
  [[ " $sim_archs " == *" x86_64 "* ]] || { echo "$kind simulator lacks x86_64" >&2; exit 1; }
  plutil -lint "$info" >/dev/null
done

echo "Archive structure/architectures look valid. Runtime Simulator launch is still required."
