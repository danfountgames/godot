# macOS embedding spike — instructions for an agent on a Mac

Everything below needs a Mac. It was written on Linux, where the macOS path cannot be
compiled or run, so **every claim here is from reading the source and must be confirmed
by running it.** Where this file says "expected", treat it as a hypothesis to test, not a
result.

Context: `.agent/DECISIONS.md` DEC-0011 and the `W` group in `.agent/EXPERIENCE_LEDGER.md`.

## What is already established (on Linux/X11)

Two game processes embed and render inside one editor window at once. The platform layer
was never single-instance: every backend keys embedding by process id. Evidence:
`.agent/evidence/spike_two_embedded_processes.png`, produced by
`.agent/evidence/spike_two_embedded_processes.py`.

Three editor-layer seams carried the limitation. Two are now open (`W1`, `W2`) and the
third is replaced from the module (`W3`, `W4`). None of that work is macOS-specific.

## Why macOS is expected to be different

X11 and Windows embed by **reparenting a native window**. The game is launched with
`--wid <native handle>` and parents itself; the editor never has to hear back from it.

macOS cannot reparent another process's `NSWindow`. It uses a **shared CALayer context**
instead, and that requires a handshake *from the game back to the editor*:

1. The editor launches the game with `--wid` and `--embedded`.
2. The game creates a `CAContext` and sends its id to the editor over the debugger
   channel as `game_view:set_context_id`.
3. `GameViewDebuggerMacOS::_msg_set_context_id()` applies it to an `EmbeddedProcessMacOS`.
4. Only then can `DisplayServerMacOS::embed_process_update()` attach the layer
   (`host.contextId = static_cast<CAContextID>(p_context_id)`).

`EmbeddedProcessMacOS::_try_embed_process()` refuses while
`current_process_id == 0 || script_debugger == nullptr || context_id == 0`, so **nothing
embeds on macOS until that message arrives**.

## The specific blocker, and why it is not the platform layer

`DisplayServerMacOS` is fine. Its store is
`HashMap<ProcessID, EmbeddedProcessData>` where each entry owns its own
`EmbeddedProcessMacOS *process`, `WindowData *wd` and `CALayer *layer_host`
(`platform/macos/display_server_macos.h:233`). That is per-process, exactly like X11.

The blocker is one level up, in `platform/macos/editor/embedded_game_view_plugin.{h,mm}`:

- `GameViewDebuggerMacOS` holds **one** `EmbeddedProcessMacOS *embedded_process`, passed
  to its constructor.
- Its handler type is `typedef bool (GameViewDebuggerMacOS::*ParseMessageFunc)(const Array &p_args);`
  — **no session parameter**.
- `virtual bool capture(const String &p_message, const Array &p_data, int p_session)`
  *does* receive `p_session`, and then drops it before dispatching.
- So `_msg_set_context_id()` applies whatever context id arrives to the single embedder
  it holds.

**Expected consequence with N games: N `set_context_id` messages all land on one
embedder. The last one wins; the other N-1 never embed.** The same applies to
`_msg_cursor_set_shape`, `_msg_mouse_set_mode`, `_msg_warp_mouse` and the rest — every
handler in that file reaches through `embedded_process`.

This is a genuine macOS-only problem. X11 needs no equivalent fix because it needs no
handshake.

## Spike A — does the existing single-instance path still work?

Establish the baseline before changing anything.

```sh
scons platform=macos target=editor dev_build=yes debug_symbols=no scu_build=yes tests=yes -j$(sysctl -n hw.ncpu)
bin/godot.macos.editor.dev.arm64 --headless --test --test-case="*[godot_ai]*"
python3 tools/relay/tests/run_tests.py
python3 tools/relay/tests/run_editor_e2e.py
```

Expected: the module suite and relay suite pass as they do on Linux (116 cases / 763
assertions and 64/64 at the time of writing). The end-to-end script has only ever been
run on Linux and native macOS in earlier sessions; if it fails, find out whether it fails
for a macOS reason or for the same class of reason the Linux run did — a hardcoded
coordinate that assumed a particular window size. That bug is fixed, but look for
siblings before concluding anything about embedding.

## Spike B — do two processes embed at once?

Port `.agent/evidence/spike_two_embedded_processes.py`. It uses
`tools/virtual_display.py`, which is X11-only; on macOS drop it and let the editor open a
real window, keeping everything else (it drives the editor through the real relay, so it
needs no UI automation).

Then apply the same throwaway patch DEC-0011 describes:

1. `GameView::_update_arguments_for_instance()` — relax `p_idx != 0` to `p_idx > 1`.
2. Give `GameView` a second `EmbeddedProcess` beside the first.
3. Feed it the second launched pid from `EditorRun::pids`, which is public.

On macOS the second embedder must be an `EmbeddedProcessMacOS`, and
`GameViewPluginMacOS` (`embedded_game_view_plugin.mm:169`) is what constructs the first
one — so the patch belongs there rather than in the cross-platform plugin.

**Predicted result: one game embeds and the second stays blank or floats**, because both
`set_context_id` messages hit the same embedder. If instead both embed, the analysis
above is wrong and that is the single most valuable thing to report back.

Record what happens either way, with a screenshot, into `.agent/evidence/`.

## Spike C — the fix, if Spike B fails as predicted

Thread the session through, then resolve it to an instance:

1. Change `ParseMessageFunc` to `bool (GameViewDebuggerMacOS::*)(const Array &p_args, int p_session)`
   and pass `p_session` from `capture()` into every handler.
2. Resolve session → pid → embedder. The join already exists and is used twice in the
   tree: `GameView::_attach_script_debugger()` matches
   `script_debugger->get_remote_pid() == embedded_process->get_embedded_pid()`, and
   `MCPRuntimeInstances::debugger_for()` in this module does the same in reverse.
   `EditorDebuggerNode::get_singleton()->get_debugger(p_session)` gives the
   `ScriptEditorDebugger` for a session index; `get_remote_pid()` gives its pid.
3. Replace the single `embedded_process` member with a lookup keyed by pid.

Keep the change inside `platform/macos/editor/`. Do not add macOS-specific concepts to
the cross-platform `GameViewDebugger` — the fork-adoption risk in `.agent/NEXT.md` says
core changes should stay small and upstreamable, and this one is genuinely a macOS
implementation detail.

## What to report back

Write findings into `.agent/DECISIONS.md` as an amendment to DEC-0011, and update the
`W0` row in `.agent/EXPERIENCE_LEDGER.md`, which currently reads "Linux/X11 only".

Answer these:

- Does the single-instance path still work on macOS after the `W1`/`W2` extraction?
  (`embedded_process_apply_arguments()` and the callback list.) This is the regression
  risk from work already merged, and it matters more than the multi-instance question.
- Do two processes embed unpatched? Patched?
- Is the `set_context_id` collision real?
- How many instances stay usable before the CALayer path degrades?
- Does taking control of one embedded instance leave the others' input alone?
- Anything about focus, z-order or resize that differs from X11.

## Not in scope for this spike

Windows and Wayland are also unmeasured. Windows uses the same PID-keyed reparenting
shape as X11 (`DisplayServerWindows::embed_process`, `display_server_windows.cpp:3597`)
so it is expected to behave like X11, but expected is not measured.
