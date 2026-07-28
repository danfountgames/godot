#!/usr/bin/env sh
# Builds the standalone godot-ai-relay binary.
#
# The relay has no engine dependency on purpose: it must build in seconds so the
# protocol/transport layer can be iterated without a full editor build.
#
# Usage: tools/relay/build.sh [--debug] [--output <path>]

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)

CXX=${CXX:-c++}
output="$repo_root/bin/godot-ai-relay"
flags="-O2"
extra_libs=""

while [ $# -gt 0 ]; do
	case "$1" in
	--debug)
		flags="-O0 -g -fsanitize=address,undefined"
		shift
		;;
	--output)
		output="$2"
		shift 2
		;;
	--windows)
		# Cross-compile check. The Windows backend cannot be run here, but it can be
		# kept compiling, which is what stops it rotting between releases.
		CXX="x86_64-w64-mingw32-g++"
		output="$repo_root/bin/godot-ai-relay.exe"
		extra_libs="-lws2_32 -static"
		shift
		;;
	*)
		echo "build.sh: unknown option: $1" >&2
		exit 2
		;;
	esac
done

mkdir -p "$(dirname "$output")"

# shellcheck disable=SC2086
"$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror $flags \
	-o "$output" \
	"$script_dir/src/main.cpp" \
	"$script_dir/src/relay.cpp" \
	"$script_dir/src/json.cpp" \
	"$script_dir/src/platform_posix.cpp" \
	"$script_dir/src/platform_windows.cpp" \
	$extra_libs

echo "built $output"
