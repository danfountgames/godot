#!/bin/bash
# Claude Code SessionStart hook: inject game architecture docs into context.
# Stdout from this script is added as context that Claude sees at session start.
#
# Install: copy to your game project's .claude/hooks/ directory and add to
# .claude/settings.json (see settings.json.example in this directory).

ARCH_FILE="docs/architecture.md"

if [ -f "$ARCH_FILE" ]; then
    echo "=== GAME ARCHITECTURE ==="
    cat "$ARCH_FILE"
    echo "=== END ARCHITECTURE ==="
else
    echo "No game architecture docs found at res://docs/architecture.md"
    echo "Use project/init_docs to create them when the game has enough structure."
fi
