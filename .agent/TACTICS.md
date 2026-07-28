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
