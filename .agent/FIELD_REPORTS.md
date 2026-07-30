# FIELD REPORTS

Findings from the fork's **first real-world use**: building a game (Wonderboard, a
tactile logic-toy collection for children) against this toolset rather than against its
own test suite.

Each entry is what the toolset felt like from the outside, with a reproducer. A report
here is not automatically a defect — several are cases where the tooling is behaving
correctly and the *documentation* or an argument name is what cost time.

---

## FR-001 — `Godot_GetRuntimeErrors` surfaces a benign engine error on every run with an autoload

**Severity:** low. Nothing malfunctions; it costs a consumer's clean-console claim.
**Layer:** engine (upstream `main/main.cpp`), surfaced by `modules/godot_ai`.
**Status:** reported, not fixed. See *Recommendation*.

### What the consumer saw

Every play session of a project with one GDScript autoload reported exactly one error:

```
Can't use get_node() with absolute paths from outside the active scene tree.
scene/main/node.cpp:1647  get_node_or_null
Condition "!data.inside_tree && p_path.is_absolute()" is true. Returning: nullptr
stack: res://<the autoload script>.gd:<line>  @implicit_new
```

`Godot_ReadOutputLog` showed **only** the two startup lines throughout, so the game
looked clean by that measure and was not.

### Minimal reproducer

A two-line autoload. Register any script as an autoload and play any scene:

```gdscript
extends Node
var _d: Dictionary = {}
```

Bisected across seven declaration forms — `var _d = {}`, `var _d: Dictionary`,
`var _d: Dictionary = {}`, `var _d: Array = []`, `var _d: int = 0`,
`var _d: String = ""`, `var _d := {}` — **all seven produce the error.** These do not:

```gdscript
extends Node                                    # 0 errors
```
```gdscript
extends Node
const P := "res://anything.json"                # 0 errors
```

So the trigger is *any member variable at all*, not a type, not an initialiser, and not
anything the consuming project is doing unusually.

### Where it comes from

`main/main.cpp` instantiates a script autoload and attaches the script while the node is
still outside the tree; the node is only parented afterwards:

```
main/main.cpp:3578   Object *obj = ClassDB::instantiate(ibt);
main/main.cpp:3583   n->set_script(script_res);      // <- implicit initialiser runs here
main/main.cpp:3589   to_add.push_back(n);
main/main.cpp:3600   sml->get_root()->add_child(E);  // <- only now is it in the tree
```

`set_script` runs GDScript's `@implicit_new`, which exists only when the script has
members — matching the bisect exactly. Something on that path resolves an absolute node
path, and `get_node_or_null` correctly refuses and returns `nullptr`.

This is upstream engine code. The fork's contribution is that
`MCPRuntimeAgent::install()` adds a global error handler
(`mcp_runtime_agent.cpp:155-160`) and therefore *reports* an error the Output panel
never shows. **That is the tooling working as intended**, and it is how the consumer
found this at all.

### Why it is worth a report anyway

`misc/godot_ai/project_template/AGENTS.md` tells an agent that console output must be
clean of relevant errors, and `Godot_GetRuntimeErrors` is the tool it points at. A
permanent, unavoidable, benign entry means every consuming project either carries a
known-bad count forever or learns to ignore the tool — and ignoring it is the worse
outcome, because it is the only place engine-internal errors appear.

### Recommendation

In rough order of preference:

1. **Leave the engine alone and document it.** Add a line to the runtime-errors tool
   description and to the template's *sharp edges*: an autoload with member variables
   costs one benign `get_node` error per run, it is upstream, and it is not the game's
   fault. Cheapest, and honest.
2. **Classify rather than filter.** Give each collected error an `origin` of
   `engine_internal` vs `project`, so a consumer can assert "zero project errors"
   without the tool hiding anything. Filtering silently would be worse than the current
   noise.
3. **Fix upstream.** Parent the autoload before `set_script`, or defer the implicit
   initialiser until the node is in the tree. Correct, and much the largest change, with
   ordering consequences for every existing autoload.

Do **not** simply suppress this error class in the collector: the whole value of the
collector is that it shows what the Output panel does not.

### A second, smaller finding on the same tool

`Godot_GetRuntimeErrors` called in the same batch as `Godot_PlayMainScene` returns `0`
because it runs before autoloads have executed. The consuming project recorded "zero
runtime errors" in three commits on the strength of such reads before noticing. Worth a
sentence in the tool description: read errors *after* waiting on a condition that proves
the scene is built.

**Reported by:** the Wonderboard project (`danfountgames/MathToy`), engine pointer
`96ff060`, editor `4.3.dev.custom_build.96ff060d1`, Linux/X11 under
`tools/virtual_display.py`.

---

# FR-002 — a deleted scene file comes back, and nothing could close a tab

**Severity:** MAJOR — silent data resurrection with no error anywhere.
**Layer:** AI tooling (`modules/godot_ai/`), plus one editor API gap.
**Status:** FIXED in this branch — `Godot_CloseScene` and
`EditorNode::close_scene_by_path()`.

## What happened in the consuming project

The project had a dead placeholder scene, `res://scenes/main.tscn`, left over from
bootstrap and no longer the main scene. An accessibility audit needed the game to contain
no text nodes at all, and that file held the last one, so it was deleted with the agent's
own filesystem tools and committed.

It came back. Twice. On disk, tracked, with its Label intact — and `Godot_ListScenes` went
on listing it.

## Why

The scene was still an **open tab** in the editor. `Godot_PlayMainScene` saves every dirty
scene before launching, which is documented and correct, so the editor wrote its stale
in-memory copy back over a path that no longer existed and recreated the file.

Nothing in this produces an error. The tool call succeeded, the game launched, the runtime
error count stayed at zero, and the only symptom was a file the agent had deleted being
present. That reads as a broken filesystem tool, or as a hallucinated deletion — both of
which cost more to investigate than the actual cause.

## The interface gap underneath it

`Godot_OpenScene` could open a tab and **no tool could close one**. Of the 62 tools, none
touches the editor's set of open scenes except by adding to it. So an agent that has
written or removed a scene file behind the editor's back has no way to make the editor let
go of its copy, and the template's existing warning —

> `Godot_PlayMainScene` saves the editor's dirty scenes first. A `.tscn` you wrote as
> *text* will be silently overwritten by the editor's stale in-memory copy. After editing
> a scene file directly, reopen it (`Godot_OpenScene`) before playing.

— only covers the *write* direction, where reopening works. For a **deleted** file there
is nothing to reopen, and the advice has no answer.

## Fix in this branch

`Godot_CloseScene` — `path` optional (defaults to the edited scene), `discard_unsaved`
defaulting false, capability `edit_scene`, returning the remaining open scenes so a caller
can assert the tab is gone rather than assume it.

It resolves through `MCPPaths::resolve`, **not** `resolve_existing`: the whole point is
closing a tab whose file has already been deleted, so requiring existence would refuse the
only case that matters.

`edit_scene` rather than `read_project` is deliberate. Closing a tab drops the editor's
copy of a scene, and with `discard_unsaved` it destroys edits; a read-only session must not
be able to do that. A test pins the capability so a later reviewer does not "simplify" it.

The editor had no dialog-free close. `_scene_tab_closed()` pops a confirmation the moment
the scene is dirty, which a non-interactive caller cannot answer, and `_remove_scene()` is
private. So `EditorNode::close_scene_by_path(path, discard_unsaved, r_error)` was added
beside `is_scene_open()`: it refuses on unsaved changes with a message naming the way out,
rather than asking a question nobody is there to hear.

## Recommendation beyond the fix

Extend the template's sharp-edge note to the delete direction, and say what to do:
after removing or replacing a scene file, `Godot_CloseScene` it. Reopening is not a
substitute and for a deleted file it is not available.

**Reported by:** the Wonderboard project (`danfountgames/MathToy`), engine pointer
`33d50d1`, editor `4.3.dev.custom_build.96ff060d1`, Linux/X11 under
`tools/virtual_display.py`.

---

# FR-003 — gestures were not gestures: six things the first real project needed

**Severity:** MAJOR for the pacing defect; the rest MINOR-to-NORMAL gaps.
**Layers:** AI tooling (`modules/godot_ai/`), the relay, and two small editor accessors.
**Status:** all six FIXED in this branch.

Everything here was found by building a game with the interface, and every one of them had
already been *worked around* in the game before it was reported — which is the failure mode
the fork's own instructions warn about. The workarounds are now deleted.

## 1. A multi-event gesture delivered inside one frame is a different gesture

The worst of the six, and the one that had a shell script written for it.

`Godot_SendPointerInput action:"drag"` pushed a press, N motion events and a release through
`Input::parse_input_event()` in a single call — so all of them landed in **one frame**. Same
for `click` and for touch `tap`. The comment above the code said "one call, because a drag
split across three calls is three chances for something else to move the pointer", which is
a good reason to keep it one *call* and no reason at all to keep it one *frame*.

Why it matters, concretely:

- A game polling `Input.is_action_just_pressed()` in `_process` **never sees** a press and a
  release that share a frame.
- Per-frame logic between motion events never runs. In the consuming project the snap target
  resolves its claim once per frame with hysteresis (its §10.3); with the whole sweep in one
  frame it never got a chance to claim, `end_press` found no target, and the peg returned
  home. The route looked broken. It was only unpaced.
- Any movement or gesture threshold measured per frame is untrippable.
- Camera smoothing, drag inertia and anything integrating over frames sees a teleport.

The project's answer was `scripts/touch-drag.sh`: one relay connection per motion step, so
the frames advanced in between. That is ~20 process launches and handshakes for one drag —
precisely the cost `--batch` exists to avoid — to obtain behaviour the tool should have
provided.

**Fix.** One shared mechanism: `MCPRuntimeWatcher::Gesture`. Any `send_pointer`, `send_touch`,
`send_key` or `send_gamepad` call that produces more than one event is queued and delivered
**one event per frame**, and the tool answers when the last one has gone. The builders did not
change; they emit through an `inject()` seam that either collects or delivers, so the pacing
decision lives in exactly one place. Single-event actions (`down`, `up`, `move`) stay
synchronous — there is no gesture to pace and making them wait a frame would only be slower.

The reply carries `paced`, `first_frame`, `last_frame` and `frames_spanned`, so what was sent
is checkable rather than assumed. `frames_per_step` slows a gesture down for something that
samples every few frames. When the game cannot schedule frame work the events are delivered
unpaced *and the result says so* — a note, not a silent downgrade.

Verified: one `Godot_SendTouchInput` call with `to_x`/`to_y`/`steps: 10` reports 12 events over
13 frames and lands the piece on its target, where the same drag unpaced left it at its origin.

## 2. `Godot_SendTouchInput` had no swept drag at all

The pointer had `to_x`/`to_y`/`steps` since the beginning; touch had only `relative_x` /
`relative_y`, one event per call. So the *touch* route — the one a tablet game actually needs
— was the one that had to be assembled by hand.

**Fix.** Touch now takes the same `to_x`/`to_y`/`steps`, releasing at the destination rather
than the origin (a release reported at the start is how a drag gets mistaken for a tap), with
`relative` measured against the previous *step* rather than the start. The by-hand form is
kept for a caller assembling a gesture itself.

Two smaller asymmetries found alongside and worth closing later: touch requires integer
coordinates where the pointer accepts floats, and `Godot_GetInputTrace` has no `limit` (only
`clear`), so a long trace is all-or-nothing.

## 3. A new `class_name` was invisible until the editor restarted

`Godot_CheckScript` reported `class_registered: false`, and every script naming the new type
failed with *"Could not find type X in the current scope"*. `Godot_ReimportAsset` does not help
— global class registration comes from the filesystem scan, not the importer — and no tool
triggered a scan. The only remedy was restarting the editor, mid-cycle, which the project did.

**Fix.** `Godot_ScanFilesystem`, with `sources_only` for the cheap path. Capability
`edit_files`, not `read_project`: it rewrites the global class cache under `.godot/`.
It says plainly that a scan is asynchronous and points at `Godot_GetImportStatus.scanning`
rather than pretending to be done.

This one matters more than it looks. Untyped `_node.call("some_method")` is how a whole class
of silent bug survives — calling a *property* as a method abandons the enclosing function with
no error where anyone is looking — and the remedy is a `class_name` so the parser catches it.
Making `class_name` expensive to add pushes projects toward the untyped form.

## 4. There was no way to delete a project file

`user://` had `Godot_DeleteUserFile`; the project had nothing. So removing a dead scene meant
reaching outside the interface entirely, which is the one thing the interface is built not to
require.

**Fix.** `Godot_DeleteProjectFile`, `confirm`-gated, **checkpointed** (so unlike user data it
is recoverable), removing the `.uid`/`.import` sidecars with the file, and refusing a scene
that is still open in the editor — because that scene would be written straight back, which is
FR-002 all over again. It is the natural counterpart to `Godot_CloseScene`.

## 5. Nothing reported how many games were running

Three game processes coexisted in the consuming project. Under software rendering they starve
each other, and a runtime read and an input injection do not necessarily reach the same one.
The symptoms — 1 fps, a `process_frames` counter apparently frozen, touch input accepted and
never delivered — all pointed at the game. None of it was the game. An hour went to it.

Counting them was a shell trick, and `pgrep -f` matches its own command line, so the trick had
its own trap (it killed the session's shell twice).

**Fix.** `Godot_PlayMainScene` and `Godot_PlayCurrentScene` report `game_pids` and
`game_process_count`, with a note when it is above one. Needed one read-only accessor on
`EditorRunBar`, since `is_playing()` cannot tell one game from three.

## 6. `Godot_GetRuntimeErrors`: FR-001's own recommendation cannot be implemented as written

FR-001 asked for errors to be classified `engine_internal` vs `project`, so a project could
assert "zero project errors" while ignoring engine noise it cannot fix. **I implemented that,
tested it, and it got the motivating case backwards.** Reporting the correction here rather than
shipping the classifier.

Two candidate signals, and each fails on one of the two errors that matter:

| | FR-001's unavoidable autoload error | a deliberate `push_error()` from project script |
|---|---|---|
| raise site (`file`) | `./scene/main/node.cpp` → engine | `./core/variant/variant_utility.cpp` → **engine** |
| project frames on the stack | `res://…/design_tokens.gd` → **project** | `res://tests/errprobe.gd` → project |

So "was project script involved" labels the unavoidable engine error a *project* error — exactly
the number FR-001 wanted clean — and "where was it raised" labels a project's own `push_error` an
*engine* error. Verified live: both errors in one run, both misattributed by whichever single
signal you pick. The only remaining option is matching the error's signature, which is the silent
suppression FR-001 explicitly warned against.

**What shipped instead.** Two descriptive fields and no verdict: `raised_in` (`engine` or
`script`) and `project_frames` (bool), plus `with_project_frames_count`,
`raised_in_script_count`, and the `frame` each error was raised on. The tool's own description
and the result's `note` both say plainly that neither field tells you whether the project could
have avoided the error, and point the caller at the stack.

**Recommendation for FR-001.** Its option 2 ("classify rather than filter") should be retired in
favour of its option 1 (document the known-benign error) or option 3 (fix the engine ordering).
The distinction the classifier needed does not exist in the error data, and a field that claims
it is worse than no field: a project would assert on it and be wrong in both directions.

## And one thing in the relay: a batch is a sequence, not a bag

An argument error on one entry did **not** stop the rest. So a mistyped
`Godot_WaitForRuntimeCondition` gate — `timeout_ms` for `timeout_seconds`, say — was refused,
the gate never ran, and every later call proceeded as though the game had reached a state it
had not. That happened twice in one session and both times the capture afterwards succeeded by
luck. The exit code was already non-zero, but nothing named the entry, and a caller reading
selected fields out of the results array never saw an `error` key it was not looking for.

**Fix.** The failing entry's index, name and message go to stderr, and the batch **stops** by
default, saying how many later calls were abandoned. `--continue-on-error` keeps the old
behaviour for a batch of genuinely independent calls. Two relay tests cover both paths.

**Reported by:** the Wonderboard project (`danfountgames/MathToy`), engine pointer `601e239`,
Linux/X11 under `tools/virtual_display.py`.

---

# FR-004 — `Godot_SetRuntimeProperty` cannot set a null Resource property from a path

**Severity:** NORMAL. **Layer:** AI tooling (`modules/godot_ai/mcp_runtime_agent.cpp`).
**Status:** OPEN — reported, not yet fixed.

Setting an `AudioStream`-typed property that is currently `null`, from a `res://` path:

```
Godot_SetRuntimeProperty path=".../Voice" property="stream" value="res://assets/audio/peg_click.wav"
→ '/root/.../Voice.stream' is a Nil: cannot use a String as a Nil
```

The refusal is accurate about what it did and wrong about what it should have done. `coerce()`
converts the incoming JSON to *the type of the value already there*, and the value already there is
`null`, so the target type is `Nil`. But the property is declared `var stream: AudioStream`, and
`Object::get_property_list()` reports it as `TYPE_OBJECT` with `hint_string: "AudioStream"`.

**Every optional resource reference starts null**, which makes this the common case rather than an
edge one: a stream that has not been chosen, a texture not yet assigned, a `PackedScene` slot. The
consuming project hit it trying to attach a recording so a replay affordance could be driven, and
used a scene test instead — the right layer, but the tool should not have been the reason.

**Suggested fix.** Take the target type from the property list rather than from the current value,
falling back to the current value's type only when the property is not declared. When the target is
`TYPE_OBJECT` with a Resource hint and the incoming value is a String, `ResourceLoader::load()` it
and refuse with the hint name if the loaded resource is not of that class — so
`"res://x.png"` into an `AudioStream` slot still refuses, and says which class it wanted.

Worth noting alongside: there is deliberately **no** method-call tool, so a scene test is the only
way to reach an object's behaviour. That is a reasonable boundary and the consuming project's rules
already point at it — but it does mean property setting carries more weight than it looks, because
it is the only lever from outside.

**Reported by:** the Wonderboard project (`danfountgames/MathToy`), engine pointer `6c46c87`.
