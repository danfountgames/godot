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
