# Godot Engine [FI]

**Fountain Interactive's fork of Godot 4.6-stable.**

Small, focused engine patches maintained as individual upstream-submittable
branches. The `fi-build` branch merges them all with FI branding for production
use. If upstream accepts a PR, it gets dropped from this fork. If not, it
rebases cleanly onto the next stable release.

---

## What's changed

### PR 1 — HDR / EDR output (`feature/hdr-edr-output`)

14 files, +81 / -7

Enables true HDR output on Apple platforms via Metal:

- **Metal pixel format** — `CAMetalLayer` uses `RGBA16Float` instead of
  `BGRA8Unorm` on iOS 16+ and macOS 10.11+, with
  `wantsExtendedDynamicRangeContent` and `kCGColorSpaceExtendedSRGB`.
- **sRGB clamp removal** — Removes the `clamp(color.rgb, vec3(0.0), vec3(1.0))`
  after sRGB conversion in `copy_to_fb.glsl`, `sdfgi_debug.glsl`, `blit.glsl`,
  and `tonemap.glsl`. Without this, HDR values above 1.0 are crushed.
- **`OS.get_hdr_headroom()` API** — Returns the display's current EDR headroom
  (iOS: `UIScreen.currentEDRHeadroom`, macOS:
  `NSScreen.maximumExtendedDynamicRangeColorComponentValue`). Falls back to `1.0`
  on non-HDR displays or unsupported platforms. Exposed to GDScript via ClassDB.

### PR 2 — ScrollContainer directional drag (`feature/scroll-container-directional-drag`)

1 file, +12 / -3

Fixes touch UX when a `ScrollContainer` only scrolls in one axis. If the user
drags perpendicular to the scroll direction and exceeds the deadzone, the drag is
cancelled instead of consumed. This lets parent controls (another ScrollContainer,
swipe gestures, etc.) pick up the event. Also tightens `accept_event()` to only
fire when the deadzone is actually exceeded.

### PR 3 — BaseButton scroll deadzone (`feature/basebutton-deadzone`)

3 files, +39

Adds a `scroll_deadzone` property to `BaseButton`. When a button is inside a
`ScrollContainer`, small finger movements during a tap can trigger a scroll
instead of a press. This property tracks drag distance in `gui_input` and
suppresses the button action if the finger moves beyond the threshold. Reads the
global `gui/common/default_scroll_deadzone` by default, or can be overridden
per-button in the inspector.

### PR 4 — iOS Metal-only export cleanup (`feature/ios-metal-cleanup`)

1 file, +12 / -1

Wraps MoltenVK framework substitution in the iOS export plugin behind
`#ifdef VULKAN_ENABLED`. When building Godot with Metal-only (no Vulkan), the
export plugin no longer references MoltenVK at all instead of substituting an
empty path, which previously caused warnings.

### PR 5 — Git branch name in window title (`feature/git-branch-title`)

3 files, +96

Shows the current git branch name in both the editor and debug game window
titles as `[branch-name]`. Supports normal repos, detached HEAD (shows short
SHA), and git worktrees (reads the `gitdir:` pointer from the `.git` file to
find the actual HEAD).

---

## Branch structure

```
4.6-stable (upstream tag)
│
├── feature/hdr-edr-output              ← PR-ready, single commit
├── feature/scroll-container-directional-drag  ← PR-ready, single commit
├── feature/basebutton-deadzone         ← PR-ready, single commit
├── feature/ios-metal-cleanup           ← PR-ready, single commit
├── feature/git-branch-title            ← PR-ready
│
├── feature/mcp-server                  ← MCP server, tools, embedded terminal, AI panel
│
├── verify/all-prs-combined             ← upstream PRs merged, no branding
│                                         (compile verification only)
│
└── fi-build                            ← everything merged + FI branding
                                          (this branch — production use)
```

**`feature/*`** branches each contain focused changes on top of `4.6-stable`.
The first four are designed to be submitted as upstream PRs independently.

**`feature/mcp-server`** is an FI-specific module (MCP server, 96 tools,
embedded Claude Code terminal, 5 specialized subagents).

**`verify/all-prs-combined`** merges upstream-submittable feature branches with
no other changes. Exists purely to verify the patches compile and don't conflict.

**`fi-build`** (this branch) merges all feature branches plus FI branding and
build tooling. This is the branch you check out to build and ship with.

---

## FI branding

The fork identifies itself everywhere version information is surfaced:

| Where | Value | How |
|-------|-------|-----|
| Window title / CLI | `Godot Engine [FI]` | `version.py` `name` field |
| Version string | `4.6.stable.fi` | `BUILD_NAME=fi` env var at build time |
| Editor binary names | `*.x86_64.fi` | `extra_suffix=fi` scons option (editors only) |
| macOS .app bundle | `Godot [FI]` | `CFBundleName` in `Info.plist` |
| Linux desktop entry | `Godot Engine [FI]` | `.desktop` and `.appdata.xml` |

`BUILD_NAME` is set on all builds (editor + templates) so the version string
always reads `fi`. `extra_suffix` is only used on **editor** builds to brand
the binary filename. Templates are built **without** `extra_suffix` so the
editor's export system finds them at the standard paths automatically — no
custom template configuration needed.

---

## Build scripts

All scripts live in the repo root. Editor scripts use `BUILD_NAME=fi` +
`extra_suffix=fi`. Template scripts use `BUILD_NAME=fi` only (no extra_suffix)
so the editor finds them automatically.

| Script | Platform | Output |
|--------|----------|--------|
| `make_linux_editor.sh` | Linux x86_64 | `bin/godot-fi` (symlink) |
| `make_macos_editor.sh` | macOS arm64 (dev) | `Godot FI.app` |
| `make_macos_release.sh` | macOS arm64 (release) | `Godot FI.app` |
| `make_windows_editor.sh` | Windows x86_64 (cross-compile) | `bin/godot.windows.editor.dev.x86_64.fi.exe` |
| `make_ios_templates.sh` | iOS arm64 + simulator | `templates/ios.zip` |
| `make_visionos_templates.sh` | visionOS arm64 + simulator | `templates/visionos.zip` |
| `make_apple_templates.sh` | iOS + visionOS combined | `apple_embedded_xcode/` |
| `make_android_templates.sh` | Android arm64 + x86_64 | scons output (then Gradle) |
| `make_web_templates.sh` | Web / Emscripten | `bin/godot.web.template_*.wasm32.zip` |

### Prerequisites

- **Linux editor:** `scons`, `pkg-config`, standard build deps
- **macOS editor:** Xcode command line tools, scons
- **Windows editor:** `mingw-w64` cross-compiler
- **iOS / visionOS:** macOS + Xcode with appropriate SDKs
- **Android:** `ANDROID_SDK_ROOT` and `ANDROID_NDK_ROOT` set
- **Web:** Emscripten SDK sourced (`source emsdk_env.sh`)

---

## Rebasing onto new releases

When a new stable ships:

```bash
git fetch upstream

# Rebase each feature branch
git checkout feature/hdr-edr-output
git rebase upstream/4.7-stable

# Repeat for other feature branches...

# Rebuild fi-build
git checkout -B fi-build 4.7-stable
git merge feature/hdr-edr-output
git merge feature/scroll-container-directional-drag
git merge feature/basebutton-deadzone
git merge feature/ios-metal-cleanup
# Cherry-pick or reapply FI branding commits
```

If a feature gets accepted upstream, stop merging that branch. The rest rebase
independently.

---

## MCP Server Module

Built-in [Model Context Protocol](https://modelcontextprotocol.io) server that
lets LLM coding agents control the editor — read/write files, run and debug
games, inspect the scene tree, evaluate expressions, and automate UI. Starts
automatically when the editor opens. See
[`modules/mcp_server/README.md`](modules/mcp_server/README.md) for setup and
LLM client configuration.

**96 tools** across 17 categories: project, editor, scene editing, runtime
lifecycle / inspection / evaluate / input / UI / time / signals, debug, console,
memory, analysis, docs, testing, shader.

### Embedded terminal

Claude Code runs inside the editor with full MCP access. The terminal launches
from the project directory so `CLAUDE.md` is picked up automatically for
per-project instructions, while the compiled system prompt provides universal
Godot context.

Five specialized subagents handle different phases of development:

| Agent | Role |
|-------|------|
| `godot-planner` | Architecture, scene trees, signal wiring, @exports |
| `godot-builder` | Scripts, scenes, UI, gameplay — scene-first, validates compilation |
| `godot-semantic-contexter` | Adds `Debug.register_*` calls for observability |
| `godot-game-player` | Launches, tests, debugs — evidence-based, never speculates |
| `godot-refactor` | Code health — splits monoliths, extracts duplication, never changes behavior |

Agent prompts are maintained as `prompts/*.txt` files and compiled into the
binary at build time via `prompt_builders.py`.

### Semantic debug context

The core idea behind the MCP integration: **any Godot game can be made
machine-readable without changing its behavior.**

Games register semantic context through a `Debug` autoload singleton
(`DebugSemanticRegistry`). All calls are no-ops in release builds — zero
performance cost, no `#ifdef` needed, the game plays identically with or without
context. What it provides:

| Primitive | Purpose | Example |
|-----------|---------|---------|
| **CVars** | Live-tunable values with min/max clamping | `player.speed`, `gravity`, `god_mode` |
| **Queries** | Readable state, polled each frame when watched | `player.health`, `enemy.count`, `fps` |
| **Actions** | Parameterized operations callable from MCP/console | `give_item`, `teleport`, `spawn_enemy` |
| **Commands** | String-arg functions, auto-registered from `debug_*()` methods | `kill_all`, `noclip`, `reset_level` |
| **Events** | Signal monitors that auto-log when they fire | `player_died`, `item_collected` |
| **UI Pages** | Navigation graph of game screens | `main_menu` → `settings` → `settings.audio` |
| **Interactables** | Semantic hints about interactive nodes | buttons, NPCs, chests with actions |

The fastest way to add context is `Debug.auto_expose(self)` in `_ready()` —
one line scans `@export` properties into live-bound CVars and `debug_*()`
methods into commands. Manual `register_query`, `register_event`,
`register_action` calls add richer metadata.

This context serves two consumers:

1. **MCP agents** — the `console/*` tools (`get_manifest`, `query`,
   `batch_query`, `invoke`, `get_cvar`, `set_cvar`, `get_events`) give agents a
   structured, typed API to read state, tweak values, and trigger operations
   without crafting raw GDScript. An agent that can read the manifest can
   understand, test, and debug any game project autonomously.

2. **In-game debug console** — the same registry powers a runtime console
   overlay with CVar editing, query watches pinned to an on-screen overlay,
   command execution, scene tree navigation (`cd`, `ls`, `pwd`), UI interaction,
   and time control (pause, step, slow-mo). The console is available in debug
   builds via a configurable hotkey.

A game with no context still works — MCP agents fall back to `runtime/evaluate`
and raw scene tree inspection. But a well-contexted game gives agents
structured, named, documented access to everything they need, turning a
black-box game into a transparent system.

### Other features

- **Tool aliases & error recovery** — common misspellings auto-resolve (13
  aliases). Unknown tools suggest the closest match by name similarity.
- **Manifest filtering** — `console/get_manifest` supports `names_only` and
  `sections` parameters for lightweight discovery plus human-readable summaries.
- **Screenshot save** — `runtime/get_screenshot` and `editor/get_screenshot`
  accept an optional `save_path` to write the PNG directly to disk.
- **Time control** — suspend, resume, frame-step, advance N frames, time scale.
  Agents use freeze→act→inspect→step→inspect for frame-level testing precision.
- **Memory profiling** — snapshots, diffs, trend tracking, leak detection,
  class breakdowns, orphan node detection.
- **Static analysis** — dead code, complexity, signal flow, dependencies,
  duplication, project health, scene validation, unused files.

---

Based on Godot 4.6-stable ([`89cea143`](https://github.com/godotengine/godot/commit/89cea14398)). MIT license.
