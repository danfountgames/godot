# Godot Engine [FI]

**Fountain Interactive's fork of Godot 4.6-stable.**

Small, focused engine patches maintained as individual upstream-submittable
branches, plus an integrated debug/AI system (MCP server, in-game console,
embedded Claude Code terminal). The `fi-build` branch merges everything with
FI branding for production use. If upstream accepts a PR, it gets dropped from
this fork. If not, it rebases cleanly onto the next stable release.

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
├── feature/debug-ai                    ← MCP server, debug console, introspection,
│                                         embedded terminal, AI agents
│
├── verify/all-prs-combined             ← upstream PRs merged, no branding
│                                         (compile verification only)
│
└── fi-build                            ← everything merged + FI branding
                                          (this branch — production use)
```

**`feature/*`** branches each contain focused changes on top of `4.6-stable`.
The first five are designed to be submitted as upstream PRs independently.

**`feature/debug-ai`** is the integrated debug/AI system: MCP server (96 tools),
in-game debug console, embedded Claude Code terminal, and runtime introspection
via `DebugIntrospector`. Consolidated from the earlier `feature/debug-console`,
`feature/mcp-server`, and `feature/mcp-agent-terminal` branches.

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
# Repeat for other feature/* branches (including feature/debug-ai)

# Rebuild fi-build
git checkout -B fi-build 4.7-stable
git merge feature/hdr-edr-output
git merge feature/scroll-container-directional-drag
git merge feature/basebutton-deadzone
git merge feature/ios-metal-cleanup
git merge feature/debug-ai
# Cherry-pick or reapply FI branding commits
```

If a feature gets accepted upstream, stop merging that branch. The rest rebase
independently.

---

## Debug / AI System

Integrated debug and AI tooling built directly into the editor: an MCP server,
in-game debug console, embedded Claude Code terminal, and runtime introspection
— all sharing the same `DebugIntrospector` infrastructure.

### MCP server

Built-in [Model Context Protocol](https://modelcontextprotocol.io) server that
lets LLM coding agents control the editor — read/write files, run and debug
games, inspect the scene tree, evaluate expressions, and automate UI. Starts
automatically when the editor opens. See
[`modules/mcp_server/README.md`](modules/mcp_server/README.md) for setup and
LLM client configuration.

**96 tools** across 17 categories: project, editor, scene editing, runtime
lifecycle / inspection / evaluate / input / UI / time / signals, debug,
memory, analysis, docs, testing, shader.

### Embedded terminal

Claude Code runs inside the editor with full MCP access. The terminal launches
from the project directory so `CLAUDE.md` is picked up automatically for
per-project instructions, while the compiled system prompt provides universal
Godot context.

Four specialized subagents handle different phases of development:

| Agent | Role |
|-------|------|
| `godot-planner` | Architecture, scene trees, signal wiring, @exports |
| `godot-builder` | Scripts, scenes, UI, gameplay — scene-first, validates compilation |
| `godot-game-player` | Launches, tests, debugs — evidence-based, never speculates |
| `godot-refactor` | Code health — splits monoliths, extracts duplication, never changes behavior |

Agent prompts are maintained as `prompts/*.txt` files and compiled into the
binary at build time via `prompt_builders.py`.

### Debug introspection

The core idea behind the MCP integration: **any Godot game can be inspected
and manipulated at runtime without any game-side registration code.**

The `DebugIntrospector` singleton (debug builds only) uses the GDScript parser's
own DocData and the live scene tree to provide structured access to any game:

| MCP Tool | Purpose | Example |
|----------|---------|---------|
| `debug/describe_class` | Parsed class reference (properties, methods, signals, docs) | `describe_class("Board")` |
| `debug/browse_tree` | Live scene tree with types, scripts, groups, child counts | `browse_tree("/root/Main", depth=3)` |
| `debug/get` | Read properties — single node or glob with summarization | `get("/root/Main/BrickGrid/*", "hit_points", summarize=true)` |
| `debug/set` | Set properties — single or glob with where filter | `set("/root/Main/BrickGrid/*", "hit_points", 1, where="hit_points > 1")` |
| `debug/call` | Call methods — single or glob with where filter | `call("/root/Main/BrickGrid/*", "hit", where="hit_points == 1")` |

**No game code required.** The system reads what already exists: GDScript doc
comments, variable/method declarations, type annotations, and the live scene
tree. Public members (no underscore prefix) are shown by default; private
members (`_foo`) require `include_private: true`.

This introspection serves two consumers:

1. **MCP agents** — the `debug/*` tools give agents a structured API to explore
   game structure, read state, modify properties, and invoke methods without
   crafting raw GDScript. Glob patterns and where clauses enable batch operations
   across arrays of nodes.

2. **In-game debug console** — a runtime console overlay with `$Path.property`
   syntax for reading/writing values, `$Path.method()` for calling functions,
   scene tree navigation (`cd`, `ls`, `pwd`), and time control (pause, step,
   slow-mo). Available in debug builds via a configurable hotkey.

### Other features

- **Tool aliases & error recovery** — common misspellings auto-resolve (13
  aliases). Unknown tools suggest the closest match by name similarity.
- **Screenshot save** — `runtime/get_screenshot` and `editor/get_screenshot`
  accept an optional `save_path` to write the PNG directly to disk.
- **Time control** — suspend, resume, frame-step, advance N frames, time scale.
  Agents use freeze→act→inspect→step→inspect for frame-level testing precision.
- **Memory profiling** — snapshots, diffs, trend tracking, leak detection,
  class breakdowns, orphan node detection.
- **Static analysis** — dead code, complexity, signal flow, dependencies,
  duplication, project health, scene validation, unused files.

---

### DevPlayer — Dynamic project mounting module (`GodotBeamDev` branch)

Runtime shell that mounts, runs, and unmounts Godot projects without
recompiling. Built as `modules/devplayer/` with 10 singleton subsystems.
Proven on Linux; iOS unbuilt.

| Doc | What it covers |
|-----|---------------|
| [WALKTHROUGH](GodotBeamDocs/WALKTHROUGH.md) | **Start here.** Build, launch shell, mount projects, git branch switch, live sync — step by step. |
| [LINUX_BASELINE](GodotBeamDocs/LINUX_BASELINE.md) | Frozen baseline: binary hash, build env, what's proven, what's not. |
| [ARCHITECTURE](GodotBeamDocs/ARCHITECTURE.md) | System design: 10 singletons, mount/unmount sequences, design decisions, milestone table. |
| [REGRESSION_CHECKS](GodotBeamDocs/REGRESSION_CHECKS.md) | All 38 regression demo checks explained: grep pattern, what each asserts. |
| [TESTING](GodotBeamDocs/TESTING.md) | Test projects, test scripts, how to add new tests. |
| [IOS_HANDOFF](GodotBeamDocs/IOS_HANDOFF.md) | For the first iOS attempt: patches, build commands, blockers, 5-phase test plan. |
| [IOS_BUILD](GodotBeamDocs/IOS_BUILD.md) | iOS blocker analysis (code review only, untested). |

Quick start:
```bash
git checkout GodotBeamDev
scons platform=linuxbsd target=editor module_devplayer_enabled=yes -j$(nproc)
./bin/godot.linuxbsd.editor.x86_64 --devplayer
```

---

Based on Godot 4.6.1-stable. MIT license.
