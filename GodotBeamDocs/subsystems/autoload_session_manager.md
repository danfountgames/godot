# AutoloadSessionManager

## Purpose

AutoloadSessionManager manages the lifecycle of autoload nodes for mounted projects. Godot projects can define autoload singletons in their `project.godot` (under `[autoload]`), which are nodes that are instantiated once and added to the SceneTree root before the main scene loads. In the DevPlayer, autoloads must be dynamically created when a project is mounted and destroyed when it is unmounted, since the engine was not originally started with the project's autoload configuration.

**Source files:** `modules/devplayer/autoload_session_manager.h`, `modules/devplayer/autoload_session_manager.cpp`

## Key APIs

| Method | Description |
|--------|-------------|
| `build_autoloads_from_project()` | Reads the autoload list from `ProjectSettings::get_autoload_list()`, loads each autoload as either a PackedScene or a Script, instantiates it, adds it to the SceneTree root, and optionally registers it as a named global constant for singleton access. |
| `destroy_autoloads()` | Removes all global constant registrations, detaches each autoload node from its parent, calls `queue_free()` on each node, and clears the internal tracking vector. |
| `get_active_autoload_names() -> PackedStringArray` | Returns the names of all currently active autoload nodes. |
| `get_active_autoload_count() -> int` | Returns the number of active autoload nodes. Used by DevPlayerDebug for metrics. |

## Architecture

### Building Autoloads

The `build_autoloads_from_project()` method processes each entry in `ProjectSettings::get_autoload_list()`:

1. **Load the resource** -- Calls `ResourceLoader::load(path)` with the autoload's path (e.g., `res://globals.gd` or `res://ui_manager.tscn`).

2. **Determine type and instantiate**:
   - If the resource is a `PackedScene`, calls `scene->instantiate()` to create the node tree.
   - If the resource is a `Script`, instantiates a node of the script's base type via `ClassDB::instantiate(script->get_instance_base_type())` and then calls `node->set_script(script)` to attach the script.
   - If the resource is neither, the entry is skipped with an error.

3. **Add to SceneTree** -- Sets the node's name to the autoload's configured name and adds it as a child of `SceneTree::get_root()`.

4. **Register as global constant** -- If the autoload is marked as a singleton (`is_singleton`), iterates all script languages and calls `add_named_global_constant(name, node)` on each. This allows GDScript code to reference the autoload by name (e.g., `GameManager.some_method()`).

5. **Track** -- Pushes the node pointer into the `active_autoloads` vector.

### Destroying Autoloads

The `destroy_autoloads()` method performs teardown in reverse:

1. **Remove global constants** -- Iterates the autoload list from ProjectSettings and removes each singleton registration from all script languages via `remove_named_global_constant()`.

2. **Remove and free nodes** -- Iterates `active_autoloads` in reverse order (last-added first). For each node:
   - Removes it from its parent via `parent->remove_child(node)`.
   - Calls `node->queue_free()` to schedule deletion.

3. **Clear tracking** -- Clears the `active_autoloads` vector.

## Integration

- **LaunchController** calls `build_autoloads_from_project()` at step 8 during mount and `destroy_autoloads()` at step 2 during unmount.
- Depends on **ProjectSettings** for the autoload list (`get_autoload_list()`).
- Depends on **ScriptServer** for global constant registration/removal.
- **DevPlayerDebug** reads `get_active_autoload_count()` for the status display.
- The DevPlayerShell automated tests specifically exercise autoload creation/destruction in cycles 5 and 6 to verify no autoload state leaks across mount sessions.

## Critical Notes

- Autoloads are destroyed using `queue_free()`, not `memdelete()`. This differs from the mounted scene node (which uses `memdelete()`). The rationale is that autoload nodes are detached from the tree first and their deferred deletion is safe since the cache purge happens afterward in the unmount sequence.
- The global constant registration is critical for GDScript interop. Without it, scripts that reference autoloads by name (e.g., `GameManager`) will fail with "Identifier not found" errors.
- Autoloads are loaded using the default cache mode, not `CACHE_MODE_REPLACE_DEEP`. This means if an autoload script has already been cached from a previous project mount with the same `res://` path, it may return a stale resource. The `ScriptDomainManager::clear_script_caches()` call during the prior unmount is what prevents this scenario.
- The destroy order (reverse iteration) ensures that autoloads which might depend on other autoloads are removed in the correct order (last-in, first-out).
