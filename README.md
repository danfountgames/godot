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

### MCP Server (`feature/mcp-server`)

172 files, +46,706 / -69 — 40 commits

Built-in [Model Context Protocol](https://modelcontextprotocol.io) server that
lets LLM coding agents control the Godot editor over HTTP. Starts automatically
when the editor opens — no external process or plugin needed.

- **71 MCP tools** across 17 categories — project filesystem, GDScript
  validation, scene editing, game lifecycle (run/stop), live scene-tree
  inspection, node property read/write, runtime expression evaluation, input
  simulation, UI automation, breakpoint management, signal introspection,
  shader compilation, doc lookup, export, memory profiling, performance timing,
  and code analysis.
- **10 `godot://` URI resources** — structured read-only access to project info,
  settings, file tree, input map, game status, live scene tree, output/error
  logs, file contents, and node properties.
- **SSE streaming** for long-running operations with progress notifications and
  cancellation.
- **Multi-instance support** — each editor instance gets its own port and
  discovery file so multiple projects can run side-by-side.
- **Security** — bearer-token auth, `res://`-only filesystem access, read-only
  resource URIs.

Auto-discovers on `http://127.0.0.1:6009/mcp`. See
[`modules/mcp_server/README.md`](modules/mcp_server/README.md) for client
configuration.

### Semantic debug console (`feature/debug-console`)

19 files, +7,236 — 4 commits

Native in-game developer console and semantic debug registry. Games declare
what's debuggable — CVars, Commands, Queries, Actions, Events, Interactables,
UI Pages — through a `Debug` singleton that is discoverable by both human
developers and LLM agents via MCP.

- **CVars** — persistent tuning variables with type inference, min/max clamping,
  and flags (`ARCHIVE`, `READONLY`, `CHEAT`, `HIDDEN`). Console shorthand:
  `player.speed` to read, `player.speed 500` to write.
- **Commands** — console-callable functions with tab-completion. Shorthand:
  `kill_all`.
- **Queries** — live-readable values from callables, pollable by the overlay
  watcher. Shorthand: `query.player.health`.
- **Actions** — parameterized operations with schemas, callable from MCP.
  Shorthand: `action.heal_player amount=50`.
- **Events** — auto-connected signal monitors with a 200-entry ring buffer,
  queryable via `get_recent_events()`.
- **Interactables** — semantic hints (ui, world_2d, world_3d, logic) that help
  agents discover interactive nodes.
- **UI Pages** — hierarchical navigation graph with visibility tracking.
  Console: `ui pages`, `ui where`, `ui go <page>`.
- **`auto_expose()`** — one-line bulk registration that scans `@export`
  properties into CVars and `debug_*()` methods into Commands, with auto-cleanup
  on tree exit.
- **Scene tree navigation** — filesystem-like browsing with `cd`/`ls`/`pwd`,
  bare child shortcuts (`Player.health`), relative paths (`../Boss:die`).
- **Console UI** — toggle with backtick or three-finger swipe on mobile.
  History, tab-completion, colored output, log filters, mini-badge popup.
- **MCP tools** — `console/execute` and `console/get_manifest` let agents
  interact with the console programmatically.
- **Release-safe** — all APIs are no-ops in release builds. Zero performance
  cost, no `#ifdef` needed in GDScript.

See [`SEMANTIC_DEBUG_README.md`](SEMANTIC_DEBUG_README.md) for the full API
reference.

### Debug time control (`feature/debug-time-control`)

6 files, +477 / -1 — 1 commit

Frame-stepping and time-scale commands for the debug console, with matching
editor UI buttons:

- **`pause`** / **`resume`** — suspend and resume game execution at engine
  level (console stays active).
- **`step [N]`** — advance N frames then re-suspend (default: 1).
- **`step_instant [N]`** — instant frame advance without rendering intermediate
  frames.
- **`timescale [value]`** — get/set `Engine.time_scale` (clamped 0.0–100.0).
- **`debug_pause(condition)`** — conditional breakpoint callable from GDScript.
  Pauses when the condition is true, no-op in release builds.
- **Editor UI** — pause/resume/step buttons in the Game view toolbar.

### Agent terminal (`feature/mcp-agent-terminal`)

34 files, +9,693 / -5 — 4 commits

Embedded Claude Code terminal panel in the Godot editor with two specialized
subagents for AI-assisted game development.

- **Terminal emulator** — full VT100/xterm-compatible terminal widget using
  libvterm. Runs Claude Code as a subprocess with PTY management. Launch, stop,
  and clear buttons in the editor toolbar.
- **MCP auto-config** — automatically generates MCP server config JSON with the
  running editor's endpoint and auth token so the agent connects instantly.
- **System prompt** — lightweight Godot project context (name, path, version,
  tool count, debug system availability) passed via `--append-system-prompt`.
- **Two subagents** passed via Claude Code's `--agents` flag:
  - **`godot-builder`** — instruments GDScript with semantic debug content.
    Knows the full `Debug` API, density guidelines (HIGH for player-facing
    systems, LOW for static infrastructure), naming conventions, and validation
    workflow.
  - **`godot-game-player`** — launches, tests, and debugs the running game.
    Knows console shorthand syntax, scene tree navigation, UI interaction
    commands, time control, and the MCP tool priority order. Acts rather than
    describes — OODA loop workflow.
- **`help` tool** — dynamic MCP tool that returns a categorized overview of all
  registered tools, or detailed parameter info for a single tool.

---

## Branch structure

```
4.6-stable (upstream tag)
│
├── feature/hdr-edr-output                    ← PR-ready, single commit
├── feature/scroll-container-directional-drag  ← PR-ready, single commit
├── feature/basebutton-deadzone               ← PR-ready, single commit
├── feature/ios-metal-cleanup                 ← PR-ready, single commit
│
├── feature/mcp-server                        ← MCP server module (40 commits)
│   └── feature/debug-console                 ← semantic debug console (4 commits)
│
├── feature/debug-time-control                ← time control commands (1 commit)
├── feature/mcp-agent-terminal                ← embedded Claude terminal (4 commits)
│
└── fi-build                                  ← all 8 features merged + FI branding
                                                (this branch — production use)
```

**`feature/*`** branches for PRs 1–4 each contain a single commit on top of
`4.6-stable`. They are designed to be submitted as upstream PRs independently.

**`feature/mcp-server`** is a multi-commit branch containing the full MCP server
module. **`feature/debug-console`** branches from it and adds the semantic debug
system. **`feature/debug-time-control`** and **`feature/mcp-agent-terminal`**
branch independently from the fi-build base.

**`fi-build`** (this branch) merges all eight feature branches plus FI branding
and build tooling. This is the branch you check out to build and ship with.

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
# feature/debug-console must rebase after feature/mcp-server

# Rebuild fi-build
git checkout -B fi-build 4.7-stable
git merge feature/hdr-edr-output
git merge feature/scroll-container-directional-drag
git merge feature/basebutton-deadzone
git merge feature/ios-metal-cleanup
git merge feature/mcp-server
git merge feature/debug-console
git merge feature/debug-time-control
git merge feature/mcp-agent-terminal
# Cherry-pick or reapply FI branding commits
```

If a feature gets accepted upstream, stop merging that branch. The rest rebase
independently.

---

## Companion: Lens Effects addon

A standalone CompositorEffect-based addon lives separately at `../lens-effects-addon/`.
Barrel distortion, bokeh, and vignette as a post-process compute shader. No
engine modifications required — works with upstream Godot 4.6+ or this fork.

---

Based on Godot 4.6-stable ([`89cea143`](https://github.com/godotengine/godot/commit/89cea14398)). MIT license.
