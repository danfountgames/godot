#!/usr/bin/env bash
set -euo pipefail

# Run the GodotBeam DevPlayer with a test project
# Usage: ./scripts/run_devplayer_test.sh [test_project_name]
# Example: ./scripts/run_devplayer_test.sh minimal_2d

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$ROOT_DIR/bin/godot.linuxbsd.editor.x86_64"
TEST_PROJECT="${1:-minimal_2d}"
PROJECT_PATH="$ROOT_DIR/test_projects/$TEST_PROJECT"

if [ ! -f "$BINARY" ]; then
    echo "ERROR: Binary not found at $BINARY"
    echo "Run scripts/build_engine_linux.sh first."
    exit 1
fi

if [ ! -d "$PROJECT_PATH" ]; then
    echo "ERROR: Test project not found at $PROJECT_PATH"
    echo "Available test projects:"
    ls "$ROOT_DIR/test_projects/"
    exit 1
fi

echo "=== Running DevPlayer with test project: $TEST_PROJECT ==="
echo "Binary: $BINARY"
echo "Project: $PROJECT_PATH"

"$BINARY" --devplayer --path "$PROJECT_PATH" "$@"
