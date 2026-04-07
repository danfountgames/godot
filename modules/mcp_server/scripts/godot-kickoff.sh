#!/usr/bin/env bash
# ============================================================================
#  godot-kickoff.sh — Bootstrap a Godot 4 game project for AI-assisted dev
# ============================================================================
#
#  Run this INSIDE your project folder (or an empty folder).
#  It writes all scaffolding files into the current directory.
#
#  Two modes:
#    NEW PROJECT (no project.godot):
#      Creates everything from scratch — project.godot, .gitignore, docs,
#      Claude config, hooks, templates, and KickoffPlanningPrompt.md.
#
#    EXISTING PROJECT (project.godot found):
#      Adds the AI-assisted dev layer — docs structure, Claude config, hooks,
#      templates, CLAUDE.md — without touching existing project files.
#      Generates an UpgradePlanningPrompt.md that guides an agent through
#      documenting the existing architecture.
#
#  Creates:
#    - Architecture docs structure (docs/)
#    - Claude Code configuration (.claude/)
#    - Hooks for doc-awareness (load-architecture, post-tool nudges, stop gate)
#    - Templates for system specs and feature plans
#    - CLAUDE.md with permanent project conventions
#    - KickoffPlanningPrompt.md or UpgradePlanningPrompt.md
#    - (new projects only) Minimal project.godot and .gitignore
#    - Git commit with all scaffolding
#
#  Usage:
#    # New project:
#    mkdir my_game && cd my_game
#    bash godot-kickoff.sh "My Game" "A roguelike deckbuilder"
#
#    # Existing project:
#    cd my_existing_game
#    bash godot-kickoff.sh
#
# ============================================================================
set -euo pipefail

PROJECT_DIR="$(pwd)"
DESCRIPTION="${2:-}"

# ── Detect existing project ───────────────────────────────────────────────
EXISTING_PROJECT=false
if [ -f "$PROJECT_DIR/project.godot" ]; then
    EXISTING_PROJECT=true
    # Read project name from project.godot
    DETECTED_NAME=$(grep -oP 'config/name="\K[^"]+' "$PROJECT_DIR/project.godot" 2>/dev/null || true)
    PROJECT_NAME="${1:-${DETECTED_NAME:-$(basename "$PROJECT_DIR")}}"
    echo "Existing Godot project detected: $PROJECT_NAME"
    echo "Adding AI-assisted development scaffolding..."
else
    PROJECT_NAME="${1:-$(basename "$PROJECT_DIR")}"
    echo "Bootstrapping new Godot 4 project: $PROJECT_NAME"
fi
echo "Location: $PROJECT_DIR"
echo ""

# ── Directory skeleton ────────────────────────────────────────────────────
mkdir -p "$PROJECT_DIR"/{docs/{systems,plans/{active,completed},templates},.claude/hooks}

# ── .gdignore — prevent Godot from importing docs ────────────────────────
touch "$PROJECT_DIR/docs/.gdignore"

# ── .gitkeep files ────────────────────────────────────────────────────────
touch "$PROJECT_DIR/docs/systems/.gitkeep"
touch "$PROJECT_DIR/docs/plans/active/.gitkeep"
touch "$PROJECT_DIR/docs/plans/completed/.gitkeep"

# ── project.godot (new projects only) ────────────────────────────────────
if [ "$EXISTING_PROJECT" = false ]; then
cat > "$PROJECT_DIR/project.godot" << GODOT_EOF
; Engine configuration file.
; It's best edited using the editor UI and not directly,
; since the parameters that go here are not all obvious.
;
; Format:
;   [section]
;   key=value

config_version=5

[application]

config/name="$PROJECT_NAME"
config/features=PackedStringArray("4.4")

[rendering]

renderer/rendering_method="forward_plus"
GODOT_EOF
fi

# ── .gitignore (new projects only, or append if missing key entries) ─────
if [ "$EXISTING_PROJECT" = false ]; then
    cat > "$PROJECT_DIR/.gitignore" << 'GITIGNORE_EOF'
# Godot 4
.godot/
*.import
export_presets.cfg

# OS
.DS_Store
Thumbs.db

# IDE
.vscode/
*.swp
*.swo

# Builds
build/
export/
GITIGNORE_EOF
else
    # For existing projects, ensure .godot/ is ignored
    if [ -f "$PROJECT_DIR/.gitignore" ]; then
        if ! grep -q '\.godot/' "$PROJECT_DIR/.gitignore" 2>/dev/null; then
            echo "" >> "$PROJECT_DIR/.gitignore"
            echo "# Godot 4 (added by godot-kickoff.sh)" >> "$PROJECT_DIR/.gitignore"
            echo ".godot/" >> "$PROJECT_DIR/.gitignore"
        fi
    fi
fi

# ── docs/architecture.md ─────────────────────────────────────────────────
# Only write if it doesn't already exist — never clobber user's docs
if [ ! -f "$PROJECT_DIR/docs/architecture.md" ]; then
    if [ -n "$DESCRIPTION" ]; then
        OVERVIEW_PLACEHOLDER="$DESCRIPTION"
    elif [ "$EXISTING_PROJECT" = true ]; then
        OVERVIEW_PLACEHOLDER="<!-- Describe your game in 2-3 sentences. Run the planning session to fill this in from the existing codebase. -->"
    else
        OVERVIEW_PLACEHOLDER="<!-- Describe your game in 2-3 sentences: genre, player role, core loop -->"
    fi

cat > "$PROJECT_DIR/docs/architecture.md" << ARCH_EOF
# $PROJECT_NAME — Architecture
<!-- TEMPLATE: Fill this out during the planning session. Delete this comment when done. -->

## Overview
$OVERVIEW_PLACEHOLDER

## Scene Flow
\`\`\`
Main Menu ──→ Game World ──→ Game Over
                 │
                 └──→ Pause Menu
\`\`\`
<!-- Replace with your actual scene flow. Keep it simple. -->

## Systems
| System | Status | Description |
|--------|--------|-------------|
| <!-- e.g. Player --> | planned | <!-- one sentence --> |

## Autoloads
| Name | Purpose |
|------|---------|
| <!-- e.g. GameManager --> | <!-- one sentence --> |

## Key Decisions
<!-- Numbered list. Record the decision AND the reason. -->
<!-- 1. Using CharacterBody2D for player — need precise platformer physics, not RigidBody -->

## Constraints
<!-- Scope, platform, performance, or team constraints -->

## Active Plans
<!-- Links to docs/plans/active/*.md as they're created -->
ARCH_EOF
    echo "  Created docs/architecture.md"
else
    echo "  docs/architecture.md already exists — skipped"
fi

# ── docs/changelog.md ─────────────────────────────────────────────────────
if [ ! -f "$PROJECT_DIR/docs/changelog.md" ]; then
    if [ "$EXISTING_PROJECT" = true ]; then
        CHANGELOG_INIT="- AI-assisted dev scaffolding added by godot-kickoff.sh"
    else
        CHANGELOG_INIT="- Project created with godot-kickoff.sh"
    fi
cat > "$PROJECT_DIR/docs/changelog.md" << CHANGELOG_EOF
# $PROJECT_NAME — Changelog

## $(date +%Y-%m-%d)
$CHANGELOG_INIT
CHANGELOG_EOF
    echo "  Created docs/changelog.md"
else
    echo "  docs/changelog.md already exists — skipped"
fi

# ── docs/templates/system-spec.md ─────────────────────────────────────────
if [ ! -f "$PROJECT_DIR/docs/templates/system-spec.md" ]; then
cat > "$PROJECT_DIR/docs/templates/system-spec.md" << 'TEMPLATE_SYSTEM_EOF'
# {System Name}

## Purpose
<!-- One sentence: what does this system DO for the player? -->

## Shape
```
ParentScene
├── KeyNode (Type)
│   ├── ChildA
│   └── ChildB
└── OtherNode (Type)
```
<!-- Scene tree structure. Which .tscn files. Key scripts and what they own. -->

## Decisions
<!-- Why this approach? What alternatives were rejected? -->

## Constraints & Edge Cases
<!-- What breaks? What's tricky? Performance concerns? -->

## Open Questions
<!-- Unresolved design questions for this system -->
TEMPLATE_SYSTEM_EOF
fi

# ── docs/templates/plan.md ────────────────────────────────────────────────
if [ ! -f "$PROJECT_DIR/docs/templates/plan.md" ]; then
cat > "$PROJECT_DIR/docs/templates/plan.md" << 'TEMPLATE_PLAN_EOF'
# Plan: {Feature Name}
**Status**: draft | ready | building | done
**Created**: {date}

## Goal
<!-- What does the player get when this is done? One paragraph max. -->

## Scene Tree
```
<!-- ASCII art of the node hierarchy this plan creates or modifies -->
```

## Files
| File | Action | Purpose |
|------|--------|---------|
| <!-- path --> | create/modify | <!-- one sentence --> |

## Implementation Order
1. <!-- First thing to build (least dependencies) -->
2. <!-- Next -->

## Acceptance Criteria
- [ ] <!-- Observable outcome, not implementation detail -->

## Playtest Checkpoints
<!-- After each visible milestone, add a checkpoint for human testing -->

### Checkpoint 1: {after what}
**Launch:** runtime/run_project
**User tests:**
- [ ] <!-- specific action — which control to use -->
- [ ] <!-- specific action — which control to use -->
**Interview:**
- "<!-- question about feel/experience -->"
- "Anything feel off?"

## Notes
<!-- Anything the builder agent needs to know -->
TEMPLATE_PLAN_EOF
fi

# ── .claude/hooks/load-architecture.sh ────────────────────────────────────
cat > "$PROJECT_DIR/.claude/hooks/load-architecture.sh" << 'HOOK_ARCH_EOF'
#!/bin/bash
# Claude Code hook: inject game architecture docs into context at session start.
# Stdout from this script becomes context that Claude sees.

ARCH_FILE="docs/architecture.md"

if [ -f "$ARCH_FILE" ]; then
    # Check if it's still a template
    if grep -q "<!-- TEMPLATE:" "$ARCH_FILE" 2>/dev/null; then
        echo "=== GAME ARCHITECTURE (TEMPLATE — not yet filled in) ==="
        echo "This project's architecture.md is still a template."
        if [ -f "UpgradePlanningPrompt.md" ]; then
            echo "This is an existing project. Read UpgradePlanningPrompt.md and follow it to document the current architecture."
        elif [ -f "KickoffPlanningPrompt.md" ]; then
            echo "If starting a planning session, read KickoffPlanningPrompt.md and follow it."
        fi
        echo "=== END ==="
    else
        echo "=== GAME ARCHITECTURE ==="
        cat "$ARCH_FILE"
        echo ""
        echo "=== END ARCHITECTURE ==="
    fi
else
    echo "No docs/architecture.md found."
    echo "Run project/init_docs or follow KickoffPlanningPrompt.md to create one."
fi
HOOK_ARCH_EOF
chmod +x "$PROJECT_DIR/.claude/hooks/load-architecture.sh"

# ── .claude/settings.json ─────────────────────────────────────────────────
# Only write if it doesn't exist — don't clobber user's custom settings
if [ ! -f "$PROJECT_DIR/.claude/settings.json" ]; then
cat > "$PROJECT_DIR/.claude/settings.json" << 'SETTINGS_EOF'
{
  "hooks": {
    "SessionStart": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "bash \"$CLAUDE_PROJECT_DIR/.claude/hooks/load-architecture.sh\"",
            "statusMessage": "Loading game architecture..."
          }
        ]
      }
    ],
    "PostToolUse": [
      {
        "matcher": "Write|Edit",
        "hooks": [
          {
            "type": "command",
            "command": "echo '{\"hookSpecificOutput\":{\"additionalContext\":\"If this change affects game architecture (new systems, new scenes, structural refactors), remember to update docs/ when done.\"}}'"
          }
        ]
      }
    ],
    "Stop": [
      {
        "hooks": [
          {
            "type": "prompt",
            "prompt": "Review this session. Did you create new scenes, add new game systems, implement a plan from docs/plans/, or make architectural changes?\n\nIf so, were docs/ files updated (architecture.md, system specs, changelog)?\n\nRespond {\"decision\": \"allow\"} if no doc updates needed or docs were already updated.\nRespond {\"decision\": \"block\", \"reason\": \"You added [X] but didn't update docs/\"} if structural changes were made without updating docs.",
            "timeout": 15
          }
        ]
      }
    ]
  }
}
SETTINGS_EOF
    echo "  Created .claude/settings.json"
else
    echo "  .claude/settings.json already exists — skipped"
fi

# ── CLAUDE.md ─────────────────────────────────────────────────────────────
# Only write if it doesn't exist — don't clobber user's custom instructions
if [ ! -f "$PROJECT_DIR/CLAUDE.md" ]; then
cat > "$PROJECT_DIR/CLAUDE.md" << CLAUDE_EOF
# $PROJECT_NAME

Godot 4 game project. GDScript only.

## Architecture Docs
This project uses a documentation-first workflow.

- \`docs/architecture.md\` — Master document. Read this first every session.
- \`docs/systems/*.md\` — One short spec per game system (~25-35 lines).
- \`docs/plans/active/*.md\` — Implementation plans. Created before building.
- \`docs/plans/completed/\` — Plans moved here after implementation.
- \`docs/changelog.md\` — Running log of what was built and when.
- \`docs/templates/\` — Copy these when creating new specs or plans.

### Workflow
1. **Read** docs/architecture.md — understand the current state
2. **Read** relevant system spec before modifying an existing system
3. **Plan** in docs/plans/active/ before building anything non-trivial
4. **Build** the feature
5. **Update** architecture.md, system specs, and changelog after building
6. **Move** completed plans to docs/plans/completed/

### First Session
If docs/architecture.md still contains \`<!-- TEMPLATE\`, the project hasn't been
planned yet. Read \`KickoffPlanningPrompt.md\` (or \`UpgradePlanningPrompt.md\` for
existing projects) and follow its instructions to run a planning session.

## Conventions
- Composition over inheritance — combine small scene nodes, don't extend deep hierarchies
- Signals for decoupling — emitter never knows who listens
- \`@export\` for public API — the Inspector is the configuration interface
- Resources (\`.tres\`) for data — not dictionaries in code
- Scenes are the architecture — if you'd instantiate it twice, it's a \`.tscn\`
- One script, one responsibility — describable in one sentence
- Keep files small — many small obvious files over few clever large ones

## Documentation Principles
- Document shape, intent, decisions, constraints — not API surfaces
- Don't repeat what well-named code already says
- architecture.md is the entry point — any agent should understand the game from it alone
- System specs are ~25-35 lines — enough to orient, not enough to go stale
CLAUDE_EOF
    echo "  Created CLAUDE.md"
else
    echo "  CLAUDE.md already exists — skipped"
fi

# ── Planning prompt ───────────────────────────────────────────────────────
if [ "$EXISTING_PROJECT" = true ]; then
    # Existing project: generate UpgradePlanningPrompt.md
    if [ ! -f "$PROJECT_DIR/UpgradePlanningPrompt.md" ]; then
cat > "$PROJECT_DIR/UpgradePlanningPrompt.md" << 'UPGRADE_EOF'
# Architecture Discovery Session

This is an **existing Godot 4 project** that just got AI-assisted development
scaffolding. Your job is to **explore what's already here** and document it —
not to start from scratch.

## Your role

You are a game architect who reverse-engineers existing projects. You'll read
the code, understand the structure, and fill in the architecture docs so that
future sessions (yours or another agent's) can work with full context.

## What's already set up

The documentation structure is ready but empty:

```
docs/
├── architecture.md      ← TEMPLATE — needs to be filled from what exists
├── systems/             ← One spec per game system (empty)
├── plans/active/        ← Implementation plans (empty)
├── plans/completed/     ← Done plans (empty)
├── changelog.md         ← Build log
└── templates/           ← Copy these for new specs/plans
```

## How this works

### Phase 1 — Scan the project
Explore the existing codebase systematically:
- Read `project.godot` for project settings, autoloads, input mappings
- List all `.tscn` scene files — understand the scene hierarchy
- List all `.gd` scripts — identify systems by `class_name`, autoloads, key scripts
- Check for `addons/`, `resources/`, data files (`.tres`, `.json`)
- Look at the main scene and trace the flow from there
- Note the rendering method, target platforms, any special config

Don't just list files — **understand the relationships**. Which scripts attach
to which scenes? What signals connect systems? What's the player's path through
the game?

### Phase 2 — Document the architecture
Open `docs/architecture.md` and replace the template with real content:
- **Overview**: What IS this game? Genre, player role, core loop — from what the code shows
- **Scene Flow**: Real scene transitions based on actual scene files
- **Systems table**: Each real system found, status = "implemented" or "partial", one-line description
- **Autoloads**: Actual autoloads from project.godot with their purposes
- **Key Decisions**: Architectural choices visible in the code (node types used, patterns, etc.)
- **Constraints**: Anything apparent (target platform, rendering method, etc.)

Show me the draft. Iterate until I say it's right.

### Phase 3 — System specs
For each system in the table, create `docs/systems/{name}.md` using the template
in `docs/templates/system-spec.md`. Fill in from what the code actually does:
- **Purpose**: What does this system do for the player?
- **Shape**: Actual scene tree structure from the `.tscn` files
- **Decisions**: Architectural choices visible in the implementation
- **Constraints & Edge Cases**: Known limitations or tricky areas
- **Open Questions**: Things that seem unfinished or unclear

Keep these lean. ~25-35 lines. They orient — they don't spec every method.

### Phase 4 — What's next?
Ask me: what are you working on next? Create an implementation plan in
`docs/plans/active/` for the next feature or improvement.

## Rules for this session

- **Read before writing.** Scan thoroughly. Don't guess what a system does — read the code.
- **Document what IS, not what should be.** Describe the current reality, not ideals.
- **Flag gaps.** If something looks incomplete, unfinished, or questionable, note it.
- **Ask when uncertain.** If you can't tell the intent from code alone, ask me.
- **Stay lean.** Docs should be concise. If a spec feels long, cut it.
- **Don't refactor.** This session is documentation only. No code changes.

## When we're done

At the end of this session, the project should have:
1. A filled-in `docs/architecture.md` that accurately reflects the existing codebase
2. A system spec in `docs/systems/` for each identified system
3. An updated `docs/changelog.md` entry
4. Optionally, an implementation plan for the next piece of work

Future sessions will load the architecture automatically and work with full context.

---

**Let's start. I'll tell you about this project, then you explore the code.**
UPGRADE_EOF
        echo "  Created UpgradePlanningPrompt.md"
    else
        echo "  UpgradePlanningPrompt.md already exists — skipped"
    fi
else
    # New project: generate KickoffPlanningPrompt.md
cat > "$PROJECT_DIR/KickoffPlanningPrompt.md" << 'KICKOFF_EOF'
# Kickoff Planning Session

You are starting a new Godot 4 game project. This is a **planning-only session**.
No GDScript. No scenes. No code. Just architecture.

## Your role

You are a game architect who thinks in Godot's building blocks: scenes, nodes,
signals, @exports, and Resources. Your job is to understand what I want to build
and help me design it before a single line of code is written.

## What's already set up

This project has a documentation structure ready to fill in:

```
docs/
├── architecture.md      ← The master doc (currently a template)
├── systems/             ← One spec per game system
├── plans/active/        ← Implementation plans
├── plans/completed/     ← Done plans
├── changelog.md         ← Build log
└── templates/           ← Copy these for new specs/plans
```

Your goal is to fill these in through conversation with me.

## How this works

### Phase 1 — Understand the game
Interview me. Don't dump 20 questions — have a conversation. Start with:
- What kind of game is this?
- What does a typical play session feel like from the player's perspective?

Then dig deeper based on my answers:
- What are the 2-5 core systems? (movement, combat, inventory, progression...)
- What's the visual style? (pixel art, 3D low-poly, vector, hand-drawn...)
- What's the scope? (game jam, prototype, vertical slice, full release?)
- Any hard constraints? (platform, performance, multiplayer, accessibility?)

Don't move on until you can describe my game back to me and I agree.

### Phase 2 — Draft the architecture
Open `docs/architecture.md` and fill it in:
- **Overview**: 2-3 sentences capturing the game's identity
- **Scene Flow**: ASCII diagram of major scene transitions
- **Systems table**: Each identified system, status = "planned", one-line description
- **Autoloads**: Global singletons needed (keep this minimal)
- **Key Decisions**: Design choices with reasoning (e.g., "CharacterBody2D for player — need precise platformer physics, not RigidBody2D")
- **Constraints**: From our conversation

Show me the draft. Iterate until I say it's right.

### Phase 3 — System specs
For each system in the table, create `docs/systems/{name}.md` using the template
in `docs/templates/system-spec.md`. Each spec covers:
- **Purpose**: One sentence — what does this do for the player?
- **Shape**: Scene tree structure (ASCII art), key scripts, what they own
- **Decisions**: Why this approach? What was rejected?
- **Constraints & Edge Cases**: What breaks? What's tricky?
- **Open Questions**: Things we haven't decided yet

Keep these lean. ~25-35 lines. They orient — they don't spec every method.

### Phase 4 — First implementation plan
Create `docs/plans/active/001-project-foundation.md` using the template in
`docs/templates/plan.md`. This plan covers the first buildable slice:
- Project folder structure
- Main scene skeleton
- First 1-2 systems to implement
- Scene tree (ASCII art)
- Files to create with purpose
- Implementation order (dependencies first)
- Acceptance criteria (observable outcomes, not implementation details)

## Rules for this session

- **No code.** Not even pseudocode. If you catch yourself writing GDScript, stop.
- **Think in scenes.** Every design answer should be a node tree you can visualize in the Godot editor.
- **Challenge me.** If something won't work well in Godot 4, say so. Suggest the Godot-native way.
- **Stay lean.** If a doc feels long, it IS too long. Cut it.
- **Ask, don't assume.** If you're unsure about my intent, ask before designing around a guess.
- **Match complexity to scope.** A jam game doesn't need an ECS. A prototype doesn't need save/load. Don't over-engineer.
- **Prefer Godot primitives.** Before designing a custom system, check if a built-in node already does it (NavigationAgent, AnimationTree, TileMapLayer, etc.).

## When we're done

At the end of this session, the project should have:
1. A filled-in `docs/architecture.md` that any developer (or AI agent) could read and understand the game
2. A system spec in `docs/systems/` for each core system
3. An implementation plan in `docs/plans/active/001-project-foundation.md` ready for a builder agent
4. An updated `docs/changelog.md` entry

The next session can pick up the plan and start building with full context.

---

**Let's start. What are we building?**
KICKOFF_EOF
    echo "  Created KickoffPlanningPrompt.md"
fi

# ── Git init & commit ─────────────────────────────────────────────────────
if [ ! -d "$PROJECT_DIR/.git" ]; then
    git init -q "$PROJECT_DIR"
fi

git -C "$PROJECT_DIR" add -A

# Check if there's anything to commit
if git -C "$PROJECT_DIR" diff --cached --quiet 2>/dev/null; then
    echo ""
    echo "No changes to commit (scaffolding already in place)."
else
    if [ "$EXISTING_PROJECT" = true ]; then
        git -C "$PROJECT_DIR" commit -q -m "$(cat <<'COMMIT_EOF'
Add AI-assisted development scaffolding

Sets up documentation-first workflow for existing project:
- docs/ structure with architecture template, system specs, plans, changelog
- .claude/ hooks for architecture awareness (SessionStart, PostToolUse, Stop)
- CLAUDE.md with project conventions
- UpgradePlanningPrompt.md for architecture discovery session

Run a planning session to document the existing codebase.
COMMIT_EOF
)"
    else
        git -C "$PROJECT_DIR" commit -q -m "$(cat <<'COMMIT_EOF'
Bootstrap project with architecture docs and Claude Code config

Sets up documentation-first workflow:
- docs/ structure with architecture template, system specs, plans, changelog
- .claude/ hooks for architecture awareness (SessionStart, PostToolUse, Stop)
- CLAUDE.md with project conventions
- KickoffPlanningPrompt.md for the first planning session
- Minimal project.godot and .gitignore

No game code yet — run a planning session first.
COMMIT_EOF
)"
    fi
fi

# ── Self-clean ────────────────────────────────────────────────────────────
# Remove the kickoff script itself from the project — it's done its job.
if [ -f "$PROJECT_DIR/$(basename "$0")" ]; then
    rm "$PROJECT_DIR/$(basename "$0")"
fi

# ── Done ──────────────────────────────────────────────────────────────────
echo ""
echo "================================================"
if [ "$EXISTING_PROJECT" = true ]; then
echo "  $PROJECT_NAME — upgraded"
else
echo "  $PROJECT_NAME — ready"
fi
echo "================================================"
echo ""
if [ "$EXISTING_PROJECT" = true ]; then
    PROMPT_FILE="UpgradePlanningPrompt.md"
    echo "  Next steps:"
    echo "    1. Open Claude Code in this directory"
    echo "    2. Say: \"Read $PROMPT_FILE and follow it\""
    echo "    3. The agent will scan your code and document the architecture"
    echo "    4. Every future agent session loads docs automatically"
else
    PROMPT_FILE="KickoffPlanningPrompt.md"
    echo "  Next steps:"
    echo "    1. Open Claude Code in this directory"
    echo "    2. Say: \"Read $PROMPT_FILE and follow it\""
    echo "    3. Plan your game architecture through conversation"
    echo "    4. Every future agent session loads docs automatically"
fi
echo ""
echo "  Structure:"
echo "    docs/architecture.md        ← fill during planning"
echo "    docs/systems/*.md           ← one per game system"
echo "    docs/plans/active/*.md      ← implementation plans"
echo "    CLAUDE.md                   ← auto-loaded every session"
echo "    $PROMPT_FILE"
echo ""
