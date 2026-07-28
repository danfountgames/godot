#!/usr/bin/env sh
# Assembles a distributable godot-ai bundle.
#
# The relay is the only piece a user installs separately: the editor half ships
# inside the Godot binary. This produces a directory that can be zipped and handed
# out, containing the relay for each platform that was built, the example skills, and
# the licence notices Godot's MIT terms require when redistributing.
#
# Usage: tools/relay/package.sh [--output <dir>] [--with-windows]

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)

output="$repo_root/bin/godot-ai-package"
with_windows=0

while [ $# -gt 0 ]; do
	case "$1" in
	--output)
		output="$2"
		shift 2
		;;
	--with-windows)
		with_windows=1
		shift
		;;
	*)
		echo "package.sh: unknown option: $1" >&2
		exit 2
		;;
	esac
done

rm -rf "$output"
mkdir -p "$output/bin" "$output/skills"

"$script_dir/build.sh" --output "$output/bin/godot-ai-relay"
if [ "$with_windows" -eq 1 ]; then
	"$script_dir/build.sh" --windows --output "$output/bin/godot-ai-relay.exe"
fi

cp -R "$repo_root/misc/godot_ai/skills/." "$output/skills/"
cp "$repo_root/modules/godot_ai/README.md" "$output/README.md"

# Godot is MIT: redistributing a derived binary requires carrying the notice.
cp "$repo_root/LICENSE.txt" "$output/LICENSE.txt"
cp "$repo_root/COPYRIGHT.txt" "$output/COPYRIGHT.txt"

cat > "$output/INSTALL.md" <<'INSTALL'
# Installing godot-ai

1. Put `bin/godot-ai-relay` (or `godot-ai-relay.exe`) anywhere on your system. It
   needs no installation and writes nothing outside `$GODOT_AI_HOME`
   (default `~/.godot-ai`).
2. Point your MCP client at it:

   ```json
   {
     "mcpServers": {
       "godot": { "command": "/path/to/godot-ai-relay", "args": ["--mcp"] }
     }
   }
   ```

3. Open your project in a Godot build that includes the AI module. The Output panel
   logs the port it is listening on.
4. The first connection from an unknown client is refused by design. Approve it from
   *Project > Tools > Godot AI: Clients and Skills*, then reconnect.

To use the bundled skills, copy a folder from `skills/` into your project's
`ai_skills/` directory and allow it in the same dialog.

See README.md for the tool catalogue, the permission model, and troubleshooting.
INSTALL

echo "packaged into $output"
