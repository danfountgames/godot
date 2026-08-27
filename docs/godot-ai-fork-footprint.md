# What this fork actually changes

Teams are cautious about a custom engine branch, and they are right to be. Before anyone
adopts one they will ask how far it has diverged, whether their projects still open,
whether their exports still work, whether they can get out later, and what happens if the
person maintaining it stops. Those are the questions this document answers, with numbers
that can be re-derived from the repository rather than with reassurance.

Every figure below is a `git diff` against the upstream commit this fork merged from —
`457470a8d0`, "Merge pull request #120607". Claims are labelled **measured** only where
something was actually run, and **not yet measured** where it has not been. The commands
are given either way, so you can check any of it yourself.

## The short version

**Nothing in the engine runtime is touched.** Not `core/`, not `scene/`, not `servers/`,
not `drivers/`, not `main/`, not any platform backend. The whole fork is one self-contained
module plus sixteen small, almost entirely additive changes in `editor/`.

```
$ git diff --name-only 457470a8d0 HEAD -- core scene servers drivers main platform \
    | grep -v macos
(nothing)
```

| Where | Files | Lines added | Lines removed |
|---|---|---|---|
| `modules/godot_ai/` — the whole feature | 153 | 46,578 | 0 |
| `editor/` — the seams it needs | 16 | 285 | 76 |
| macOS branding (icons, plists, an icon script) | 6 | 182 | 14 |
| Everything else outside the module | 0 | 0 | 0 |

Sixty-eight thousand lines sounds like a lot until you notice that 46,578 of them are a
module you can delete, most of the rest is documentation and tooling that never compiles
into the engine, and the part that touches Godot itself is **285 added lines across
sixteen editor files**.

## Will my project open unchanged?

Yes, and there is nothing in the fork that could make it otherwise. Project files, scene
format, resource format, the script languages, the servers and the export pipeline are
untouched — see the table above. The engine reads and writes exactly what upstream 4.8-dev
reads and writes, because none of that code differs by a single line.

## Will my exports still work?

**Measured**, on this branch, at commit `ac65b36f12`. A release template built here
contains none of the tooling:

```sh
$ scons platform=linuxbsd target=template_release   # 14m20s on 4 cores
$ python3 tools/tests/run_tests.py -k export_template
PASS an_export_template_contains_none_of_the_tooling
```

The check scans every `bin/*template*` binary for `Godot_ManageNode`,
`Godot_CaptureViewport`, `MCPService` and `godot_ai`. Scanning the 88 MB binary directly for
those, plus `Godot_OfferVariants`, `Godot_ProposeChange` and `MCPRuntimeAgent`, finds zero
occurrences of any of them. The AI stack is compiled under `TOOLS_ENABLED` and
`modules/godot_ai/config.py` reports `can_build` only for editor builds, so none of it is
even offered to the linker.

One caveat about where this runs. The check is part of the standard tooling suite, so CI
executes it — but CI does not build a template, so **it skips there**, and a skipped check
is not a result. Until CI builds one, this has to be run by hand after a template build.

## Can I remove the module later?

**Not yet measured on this branch.** `scons module_godot_ai_enabled=no` is Godot's own
per-module switch and nothing here opts out of it; the module lives entirely under
`modules/godot_ai/` and registers itself through the ordinary
`EditorNode::add_init_callback` path, so there is no reason it should not simply be absent.
That reasoning is not a measurement, and the way to settle it is:

```sh
scons platform=linuxbsd target=editor module_godot_ai_enabled=no tests=yes
./bin/godot.linuxbsd.editor.x86_64 --headless --test
```

What does *not* disappear with the module is the sixteen editor seams — they are compiled
into the editor either way. They are listed below precisely so you can see how little
would remain.

## What are the sixteen editor changes?

Fourteen of the sixteen are purely additive: an accessor, a getter, a hook. Nothing changes
existing behaviour, and nothing is deleted.

| File | What it adds | Could it go upstream? |
|---|---|---|
| `editor/file_system/editor_file_system.cpp` | Clears the `singleton` pointer in the destructor | **Yes, and it should.** A plain bug: leaving it set hands every later `get_singleton()` freed memory. It caused an intermittent SIGSEGV in the engine's *own* test suite, roughly one run in four |
| `editor/editor_log.h` | Read access to the collected output | Plausibly. Anything wanting the log currently has to scrape a `RichTextLabel` |
| `editor/debugger/editor_debugger_node.h` | The remote scene tree, and node lookup by path | Plausibly. The remote inspector otherwise exposes only what the user selected |
| `editor/debugger/editor_debugger_tree.{h,cpp}` | Path lookup in the remote tree | With the above |
| `editor/debugger/editor_debugger_inspector.h` | A getter for the stack variables | Trivially |
| `editor/debugger/script_editor_debugger.{h,cpp}` | Read access to session state | Plausibly |
| `editor/editor_node.{h,cpp}` | `close_scene_by_path`, which refuses rather than popping a dialog a non-interactive caller cannot answer | Plausibly; it is the non-interactive half of an existing operation |
| `editor/run/editor_run.{h,cpp}` | Read access to which processes are running | Plausibly. `is_playing()` cannot tell one game from three |
| `editor/run/editor_run_bar.h` | The same, one level up | With the above |
| `editor/run/embedded_process.{h,cpp}` | Embedding logic **moved out of** `game_view_plugin.cpp` so more than one game can embed | **Yes.** This is a refactor, not a feature: the platform layer was never single-instance, and this is the editor-side seam that assumed it was |
| `editor/run/game_view_plugin.cpp` | The other side of that move (−71 lines) | With the above |

The only file that loses more than a handful of lines is `game_view_plugin.cpp`, and those
lines moved into `embedded_process.cpp` rather than disappearing.

## How fast does it follow upstream?

Honestly: this has been done once, and it went well. The fork was built against 4.3 and
merged up to 4.8-dev in commit `117870273`. The merge required no changes to the module's
logic — only include paths, because 4.8 nests the editor sources (`editor/docks/`,
`editor/run/`, `editor/file_system/`).

That is one data point, not a track record, and it should be read as one. What makes the
next merge likely to be similar is structural rather than a promise: 285 lines in sixteen
files is a small conflict surface, and the module itself only depends on editor APIs that
are public and stable.

## What if the maintainer stops?

The honest answer is that this is a real risk and no document can remove it. What can be
said concretely:

- The feature is one module. If it stops being maintained, `module_godot_ai_enabled=no`
  gets you a stock editor, and the sixteen seams above are what you would carry.
- Nothing in your project depends on the fork. Projects, scenes and exports are stock
  Godot, so leaving costs you the tooling and nothing else.
- The parts most likely to rot — the relay's bridge protocol, the tool schemas — are
  version-checked at the boundary rather than assumed, so a mismatch is reported instead of
  misbehaving.

## Checking any of this yourself

```sh
# Nothing in the engine runtime.
git diff --name-only 457470a8d0 HEAD -- core scene servers drivers main platform | grep -v macos

# The whole footprint outside the fork's own directories.
git diff --stat 457470a8d0 HEAD -- . ':(exclude)modules/godot_ai' ':(exclude)tools' \
  ':(exclude).agent' ':(exclude)docs' ':(exclude)misc/godot_ai' ':(exclude).github'

# An editor with the module removed.
scons platform=linuxbsd target=editor module_godot_ai_enabled=no tests=yes
./bin/godot.linuxbsd.editor.x86_64 --headless --test

# An export template, and the check that it carries none of the tooling.
scons platform=linuxbsd target=template_release
python3 tools/tests/run_tests.py -k export_template
```
