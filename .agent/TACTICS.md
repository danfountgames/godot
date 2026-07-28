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

- Engine doctest: `bin/godot.linuxbsd.editor.dev.x86_64 --headless --test --test-case="*[godot_ai]*"`.
  **Run it from outside the repository** (`cd /tmp && …`): the binary's working
  directory is what a misbehaving fixture would damage.
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
