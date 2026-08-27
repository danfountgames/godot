# macOS embedding spike — instructions for an agent on a Mac

Everything below needs a Mac. It was written on Linux, where the macOS path cannot be
compiled or run, so **every claim here is from reading the source and must be confirmed
by running it.** Where this file says "expected", treat it as a hypothesis to test, not a
result.

Context: `.agent/DECISIONS.md` DEC-0011 and the `W` group in `.agent/EXPERIENCE_LEDGER.md`.

## FIXED, 2026-08-27, native arm64 — three games embed at once on macOS

`.agent/evidence/spike_macos_embedding.png` shows three agent-launched games rendering
simultaneously in three workspace tiles on a Mac. The spike that was written to fail now
passes all eleven of its checks, and no embed is refused. What follows is what was in the
way, kept because the diagnosis is the useful part.

**Two defects, stacked. The one this document predicted was the second of them.**

*First:* `MCPWorkspaceTile` named `EmbeddedProcess` — the reparenting implementation —
so on macOS every tile asked `DisplayServer::embed_process()`, which `DisplayServerMacOS`
does not implement, and the base class warned once per tile. Nothing embedded, not even
one game. Fixed by giving `EmbeddedProcessBase` a `create()` seam that platforms register
into (`editor/run/embedded_process.{h,cpp}`), with macOS registering
`EmbeddedProcessMacOS` from its game-view plugin at static initialization so it cannot
depend on which editor plugin is constructed first. Without a registration the seam
returns the reparenting embedder, so X11 and Windows are untouched.

*Second:* the collision described further down, and it is exactly as described.
`GameViewDebuggerMacOS::capture()` took `p_session`, checked the session existed, and
dispatched without it, so every game's `set_context_id` reached the one embedder the
debugger was constructed with. `ParseMessageFunc` now carries the session and every
handler resolves session → pid → embedder, through a pid-keyed registry on
`EmbeddedProcessMacOS`. When a session resolves to no registered embedder the handler
falls back to the plugin's own, which is what every message went to before — so the
ordinary single-game Game workspace path behaves exactly as it did.

The tile also has to hand its game's `ScriptEditorDebugger` to its embedder, because
`_try_embed_process()` refuses while it has no debugger, no pid or no context id, and the
session does not exist yet at launch. The workspace's existing half-second poll retries
it. On the reparenting platforms `set_script_debugger()` is an empty base method, so this
is free rather than conditional.

**One thing to know before reading a screenshot.** macOS composites the embedded game as
a CALayer owned by the window server, so it is *not* in the editor's render target. A
control-level capture of a tile shows the tile's chrome and a flat rectangle where the
game is. Only a screen-level capture shows the game. A blank tile capture is not evidence
that embedding failed — this cost time once already.

Verified: 1693/1693 engine cases (425,056 assertions), 276/276 module cases, 64/64 relay,
the native end-to-end, and the spike's eleven checks.

## The run that found it, 2026-08-27 — and the prediction below was wrong

Everything from here down was written on Linux and marked "expected". It has now been
run on a Mac. **Spike A passes and Spike B fails harder than predicted.** Read this
section first; the analysis below is kept because it is still correct about
`GameViewDebuggerMacOS`, but it is no longer the *first* thing in the way.

Evidence: `.agent/evidence/spike_macos_embedding.py`, `spike_macos_embedding.png`,
`spike_macos_tile_{0,1,2}.png`, `spike_macos_embedding_editor.log`.

**Spike A — the regression risk from W1/W2 — is clean.** The macOS editor builds (after
one real fix, below), the module suite passes 276 cases / 3575 assertions, the relay
suite 64/64, and `run_editor_e2e.py` passes 124 checks natively. In particular
`embedded_process_apply_arguments()`, the W1 extraction, is macOS-correct already: it
strips `--display-driver` and adds `--embedded` under `MACOS_ENABLED`.

**Spike B — the prediction was "one game embeds and the second stays blank". Measured:
none embed.** Three instances launch, all three processes run, all three tiles lay out,
and all three tiles are blank. The editor says why, three times, once per instance:

```
WARNING: Embedded process not supported by this display server.
     at: embed_process (./servers/display/display_server.cpp:1162)
```

That is `DisplayServer`'s **base-class** `embed_process`, not a macOS override — because
**`DisplayServerMacOS` does not override `embed_process` at all.** X11 declares
`virtual Error embed_process(WindowID, ProcessID, Rect2i, bool, bool) override`; macOS
declares only `Error embed_process_update(WindowID, EmbeddedProcessMacOS *)`, a different
signature that takes the macOS embedder itself. macOS cannot implement the generic
pid-keyed call, because at that moment it does not yet have the CALayer context id.

So the blocker is one level earlier than this document assumed, and it is in **our
module, not in the platform**: `MCPWorkspaceTile` does `memnew(EmbeddedProcess)`
unconditionally (`modules/godot_ai/mcp_workspace.cpp:88`). That is the reparenting
implementation. Upstream never picks that class directly on macOS — it picks it through
the plugin seam, where `editor/run/game_view_plugin.cpp:1799` constructs `EmbeddedProcess`
and `platform/macos/editor/embedded_game_view_plugin.mm:169` constructs
`EmbeddedProcessMacOS` instead. The workspace bypassed that seam.

**The `set_context_id` collision described below is real but currently unreachable.**
Confirmed by reading it here: `capture()` receives `p_session`, uses it only to null-check
the session, then calls `(this->**fn_ptr)(p_data)` and drops it. It will matter the moment
a tile holds a real `EmbeddedProcessMacOS` — but no tile does yet, so it is the *second*
thing to fix, not the first.

**A flag that disagrees with its implementation.** `DisplayServerMacOS::has_feature()`
returns **true** for `FEATURE_WINDOW_EMBEDDING`, and `EmbeddedProcess::embed_process()`
gates on exactly that flag before calling a method the platform does not implement. So
the gate passes and the call then warns. On macOS the feature is real but is reached
through a different API than the flag advertises. This is upstream engine behaviour, not
something this fork introduced, and it is a plausible upstream report.

**What does work on macOS, measured.** Everything except the picture: three instances
launch with their own ids and pids, `debugger_connected` is true, `Godot_ListInstances`
sees three live, pausing one reports `applied` and stops none of the others, and
`Godot_StopAllInstances` clears exactly the agent's three. The W3/W4/W9 routing layer is
platform-independent and behaves here as it does on Linux.

**Also found on the way in:** the agent terminal did not compile on macOS at all.
`mcp_pty.cpp` called `execvpe`, a glibc extension absent on macOS and the BSDs, so the
whole editor build stopped there and nothing downstream had ever been measured. Fixed.

### Answers to "What to report back"

| Question | Answer |
|---|---|
| Does the single-instance path still work after W1/W2? | The argument builder is correct; the embed itself never happens, for the reason above. Not a W1/W2 regression. |
| Do two processes embed unpatched? | No — and neither does one. |
| Is the `set_context_id` collision real? | Real in source, unreachable today. |
| How many instances before the CALayer path degrades? | Unanswerable until a tile holds a macOS embedder. |
| Does taking control of one leave the others alone? | Yes — pause targeted exactly one, three times over. |
| Focus, z-order, resize vs X11? | The games are ordinary floating windows on macOS today, so none of it is comparable yet. |

### Revised order of work

1. Give `MCPWorkspaceTile` the platform's embedder instead of hardcoding `EmbeddedProcess`
   — a seam in the module, resolved per platform, mirroring what the game-view plugin does.
2. Then Spike C below: thread `p_session` through `GameViewDebuggerMacOS` and key the
   embedders by pid, because with step 1 done the collision becomes reachable.

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
