# TACTICS

Reusable technical discoveries. Not a diary — see `LOG.md` for chronology.

## Environment setup (bare container)

```sh
pip install scons
apt-get update            # REQUIRED first; without it libasound2-dev 404s on a stale index
apt-get install -y --no-install-recommends \
    libxcursor-dev libxinerama-dev libxi-dev libxrandr-dev libasound2-dev
```
Verify with `pkg-config --exists xcursor xinerama xrandr xi alsa`.

## Builds

- Editor: `scons platform=linuxbsd target=editor dev_build=yes debug_symbols=no scu_build=yes tests=yes -j$(nproc)`
  - `scu_build=yes` is the difference between a tolerable and an intolerable build.
  - Measured: **7m42s** clean on 4 cores. Incremental module-only rebuilds are seconds.
  - Output: `bin/godot.linuxbsd.editor.dev.x86_64`.
  - Full doctest baseline (unmodified tree): 878 cases, 2,394,660 assertions, passing.
- Relay: `tools/relay/build.sh` — seconds, no engine dependency.

## Tests

- Module doctest: `bin/godot.linuxbsd.editor.dev.x86_64 --headless --test --test-case="*[godot_ai]*"`
  — runs from any directory.
- **Full engine suite must run from the repository root.** The GDScript runner,
  completion and LSP suites resolve their test data relative to the working directory
  and fail with "Invalid test directory" / "Could not open specified root directory"
  when run elsewhere. That is not a regression; check it from the root before
  concluding anything broke. Baseline with this module: 922 cases, ~2.4M assertions.
- Module test headers are auto-discovered from `modules/<name>/tests/test_*.h` via
  `modules/modules_tests.gen.h` when `tests=yes`; no manual registration.
- Relay: `python3 tools/relay/tests/run_tests.py` — the fastest meaningful signal.
- Whole stack: `python3 tools/relay/tests/run_editor_e2e.py` — launches a headless
  editor on a scratch project and drives it through the real relay (~30s).

## In-tree patterns worth copying

- **Editor-side protocol server**: `editor/debugger/debug_adapter/debug_adapter_server.cpp`
  — `start()` on `NOTIFICATION_ENTER_TREE`, `stop()` on `NOTIFICATION_EXIT_TREE`,
  `poll()` from `NOTIFICATION_INTERNAL_PROCESS` behind a `polling` re-entrancy flag.
  The main loop can be re-entered during request processing; our service needs the
  same guard because tools call editor operations.
- **Module registering an editor plugin**: `modules/gdscript/register_types.cpp`
  `_editor_init()` → `EditorNode::get_singleton()->add_editor_plugin(...)`, hooked up
  with `EditorNode::add_init_callback(&_editor_init)` under `TOOLS_ENABLED`.
- **Editor settings**: `_EDITOR_DEF(...)` in the constructor, `EDITOR_GET`/`_EDITOR_GET`
  to read, react to `EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED` with
  `check_changed_settings_in_group()`.

## Test-fixture safety

- `DirAccess::create(DirAccess::ACCESS_FILESYSTEM)` starts at the **process working
  directory**. `erase_contents_recursive()` on that handle deletes the repository.
  Use `mcp_test_remove_tree()` from `modules/godot_ai/tests/test_mcp_fs_helpers.h`,
  which refuses paths outside the cache dir or without the `godot_ai_test_` marker.
- Python test doubles: `socket.close()` does **not** send FIN while another thread is
  blocked in `recv()` on the same fd. Call `shutdown(SHUT_RDWR)` first, or the peer
  only notices the disconnect when it next writes. The same applies to a listening
  socket and `accept()`.

## Engine gotchas

- `Dictionary::operator[]` **inserts** a null for a missing key even when the
  dictionary is reached through a `const Dictionary &`: the private data pointer does
  not propagate constness, so the const overload ends up calling the mutating HashMap
  operator. Reading an optional argument this way before validation makes the call
  fail with a confusing "must be of type" error about a key the caller never sent.
  Use `has()` or `get(key, default)`.
- `EditorInterface::get_singleton()` is non-null in any editor build, including the
  headless unit-test binary, because `register_editor_types()` creates it. Its methods
  dereference `EditorNode`, which is null there. Guard on both.

## Working with a display where there is none

- `tools/virtual_display.py` starts `Xvfb`, waits until it actually answers, and hands
  back the environment (`DISPLAY` plus software-GL variables). `ensure()` reuses a
  working `DISPLAY`, starts one otherwise, and returns an unusable display rather than
  raising when it cannot. `run_editor_e2e.py` calls it, so the visual checks run
  everywhere `Xvfb` is installed.
  Packages: `xvfb x11-utils libgl1-mesa-dri`.
- **Do not trust `DISPLAY` from the environment.** A container image can export one
  that was never started; the editor then fails in a way that looks like a rendering
  bug. Probe it (`xdpyinfo -display`) before using it.
- **llvmpipe has no Vulkan.** The editor needs `--rendering-driver opengl3` plus
  `LIBGL_ALWAYS_SOFTWARE=1`.
- **The game process inherits the *project's* renderer, not the editor's command
  line.** An editor started with `--rendering-driver opengl3` still launches a game
  that tries Vulkan and dies immediately, taking runtime inspection with it. Put
  `renderer/rendering_method="gl_compatibility"` in `project.godot`.
- **The editor only requests the remote scene tree while the Remote panel is
  visible.** Nothing else asks for it, and nothing signals when it arrives, so a tool
  that wants it must call `request_remote_tree()` and then poll —
  `MCPDeferred::begin_polled()` exists for exactly this shape of answer.
- A capture that succeeds returns an **image** block and no text. Test helpers that
  format a failure message from `content[0]["text"]` run eagerly and crash on success.

## Driving the editor's GUI from a test

- `xdotool` (package `xdotool`) works against a virtual display. Find the dialog by
  diffing `xdotool search --onlyvisible --name .` before and after the call that opens
  it; `getwindowgeometry --shell` then gives coordinates to aim at.
- **Typing works, but only after clicking the field first.** Characters go to whatever
  holds *Godot* focus, and a freshly opened dialog or palette usually focuses a button
  instead of its text field - so `Return` submits an empty value and the typing appears
  to have vanished. Click the field, then type.
- **Nothing restores X input focus when a popup closes**, because there is no window
  manager. After a dialog is dismissed, keystrokes go to a destroyed window and are
  lost, which looks exactly like a keyboard shortcut that stopped working. Call
  `xdotool windowfocus --sync <editor window>` before each keystroke sequence.
- Running `openbox` does not help, and its decorations shift the geometry that
  `getwindowgeometry` reports, so clicks computed from it miss. Leave the display bare.
- Take a screenshot when a UI check fails (`xwd -root | convert`), **before** the
  teardown that closes the editor — otherwise the evidence is a black screen.
- Do not pin a click to a fixed pixel offset: walk fractions of the dialog's height
  until one lands. With a single choice, whatever answer arrives is unambiguous.

## Editor behaviour that trips tests

- **Running the game clears the editor's Output panel.** Any test that reads back log
  messages must run before the play-lifecycle checks, or it will find an empty log.
- A headless game with an empty scene may exit almost immediately, so
  `Godot_StopPlaying`'s `was_playing` cannot be pinned down here; assert the
  postcondition (nothing running afterwards) instead.
- The remote scene tree does not populate reliably for a headless game, so
  `Godot_GetRuntimeSceneTree` is probed and reported rather than asserted in e2e.

## Cross-platform

- `tools/relay/build.sh --windows` cross-compiles the relay with mingw
  (`apt-get install g++-mingw-w64-x86-64`). It cannot be run here, but it keeps the
  Windows backend compiling.
- Windows cannot wait on a console/pipe handle and a socket in one call: `WSAPoll`
  takes sockets only. The Windows backend therefore reads stdin on a thread, which is
  why `platform::wait_for_input()` exists rather than a bare `poll()`.
- `_dupenv_s` and friends are MSVC-only and do not exist under mingw; use the Win32
  API (`GetEnvironmentVariableA`) instead.

## Harness gotchas

- Foreground `sleep` is blocked; use background commands or `until <cond>; do sleep 2; done`.
- **Do not `pgrep -f` / `pkill -f` a pattern that appears in your own command line.**
  The wrapper shell's argv contains the whole script, so the pattern matches the shell
  itself and kills it mid-command (exit 144), silently skipping everything after it.
  Kill by recorded pid, or match on the binary name only.
- End-to-end runs need `GODOT_AI_HOME` (private instance registry) and
  `GODOT_AI_AUTO_APPROVE=1` (skips the first-connection approval). A SIGTERM-killed
  editor leaves its instance descriptor behind; the relay prunes it on the next
  connection attempt.
- Do not chain a long build with `&&` to a check — start it in the background, record
  it as the in-flight operation, and poll the log.
