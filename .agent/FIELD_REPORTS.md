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
