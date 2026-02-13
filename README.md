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
├── feature/mcp-server                  ← MCP protocol, debugger bridge, tools
├── feature/mcp-agent-terminal          ← embedded terminal + multi-tab AI panel
│
├── verify/all-prs-combined             ← upstream PRs merged, no branding
│                                         (compile verification only)
│
└── fi-build                            ← everything merged + FI branding
                                          (this branch — production use)
```

**`feature/*`** branches each contain focused changes on top of `4.6-stable`.
The first four are designed to be submitted as upstream PRs independently.

**`feature/mcp-server`** and **`feature/mcp-agent-terminal`** are FI-specific
modules (MCP server for LLM integration, embedded terminal for Claude Code).

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

## Companion: Lens Effects addon

A standalone CompositorEffect-based addon lives separately at `../lens-effects-addon/`.
Barrel distortion, bokeh, and vignette as a post-process compute shader. No
engine modifications required — works with upstream Godot 4.6+ or this fork.

---

## MCP Server Module

Built-in [Model Context Protocol](https://modelcontextprotocol.io) server that lets LLM coding agents control the editor — read/write files, run and debug games, inspect the scene tree, evaluate expressions, and automate UI. Starts automatically when the editor opens. See [`modules/mcp_server/README.md`](modules/mcp_server/README.md) for setup and LLM client configuration.

---

Based on Godot 4.6-stable ([`89cea143`](https://github.com/godotengine/godot/commit/89cea14398)). MIT license.
