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
		#
		# Prefer the posix-threads variant. The relay's Windows backend reads stdin on a
		# std::thread, and libstdc++ only provides std::thread, std::mutex and
		# std::condition_variable for the win32 thread model from GCC 13 onwards. Older
		# toolchains - Ubuntu 22.04's, which is what CI runs - default to win32 and fail
		# to find std::mutex, while a newer local toolchain compiles the same source
		# happily. Naming the variant removes the difference.
		if command -v x86_64-w64-mingw32-g++-posix >/dev/null 2>&1; then
			CXX="x86_64-w64-mingw32-g++-posix"
		else
			CXX="x86_64-w64-mingw32-g++"
		fi
		output="$repo_root/bin/godot-ai-relay.exe"
		extra_libs="-lws2_32 -lbcrypt -static"
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
	"$script_dir/src/http_server.cpp" \
	"$script_dir/src/backends.cpp" \
	"$script_dir/src/json.cpp" \
	"$script_dir/src/platform_posix.cpp" \
	"$script_dir/src/platform_windows.cpp" \
	$extra_libs

echo "built $output"
