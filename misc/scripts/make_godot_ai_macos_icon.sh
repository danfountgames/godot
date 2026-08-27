#!/usr/bin/env bash

##############################################################################
# This file is part of Godot Engine: https://godotengine.org                 #
# Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).     #
# Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                      #
#                                                                            #
# Permission is hereby granted, free of charge, to any person obtaining a    #
# copy of this software and associated documentation files (the "Software"), #
# to deal in the Software without restriction, including without limitation #
# the rights to use, copy, modify, merge, publish, distribute, sublicense,   #
# and/or sell copies of the Software, and to permit persons to whom the       #
# Software is furnished to do so, subject to the following conditions:       #
#                                                                            #
# The above copyright notice and this permission notice shall be included in #
# all copies or substantial portions of the Software.                        #
#                                                                            #
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR #
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   #
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE #
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER     #
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    #
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        #
# DEALINGS IN THE SOFTWARE.                                                  #
##############################################################################

set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
repository_root="$(cd "$script_dir/../.." && pwd)"
# The AI-skewed mark, not the stock Godot icns: the brand is the skew, and the badge
# on top keeps GodotAI tellable from stock Godot at Dock sizes where the circuitry
# vanishes. NSImage loads SVG natively, so the badge compositor needs no change.
source_icon="$repository_root/misc/logo/icon.svg"
output_icon="$repository_root/misc/dist/macos_tools.app/Contents/Resources/GodotAI.icns"
icon_work_dir="$(mktemp -d "${TMPDIR:-/tmp}/godotai-icon.XXXXXX")"

cleanup() {
	find "$icon_work_dir" -depth -delete
}
trap cleanup EXIT

source_png="$icon_work_dir/GodotAI.png"
iconset_dir="$icon_work_dir/GodotAI.iconset"
mkdir "$iconset_dir"

swift "$script_dir/make_godot_ai_macos_icon.swift" "$source_icon" "$source_png"

for icon_size in 16 32 128 256 512; do
	sips -z "$icon_size" "$icon_size" "$source_png" --out "$iconset_dir/icon_${icon_size}x${icon_size}.png" >/dev/null
	double_size=$((icon_size * 2))
	sips -z "$double_size" "$double_size" "$source_png" --out "$iconset_dir/icon_${icon_size}x${icon_size}@2x.png" >/dev/null
done

iconutil -c icns "$iconset_dir" -o "$output_icon"
