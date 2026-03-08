# ResourceDomainManager

## Purpose

ResourceDomainManager handles eviction of project resources from Godot's `ResourceCache` during the unmount sequence. When a project is mounted, all loaded resources (scenes, textures, scripts, etc.) are cached by Godot's `ResourceCache` under their `res://` paths. When the project is unmounted, these cached entries must be purged so that mounting a different project does not accidentally reuse stale resources that happen to share the same `res://` path.

**Source files:** `modules/devplayer/resource_domain_manager.h`, `modules/devplayer/resource_domain_manager.cpp`

## Key APIs

| Method | Description |
|--------|-------------|
| `begin_tracking(project_root)` | Called at the start of a mount session. Records the project root and clears any previously tracked resource paths. |
| `register_loaded_resource(resource)` | Registers a resource's path in the tracked set. Can be called during the session to explicitly track resources. |
| `purge_project_resources()` | Iterates the entire `ResourceCache`, collects all resources with `res://` paths, and evicts them by calling `set_path("")` on each one. |
| `get_tracked_resource_count() -> int` | Returns the number of explicitly tracked resource paths. Used by DevPlayerDebug for metrics display. |

## Architecture

### The `set_path("")` Eviction Technique

The core of the resource eviction strategy is the call to `Resource::set_path("")` on each cached resource. This works because of how Godot's `ResourceCache` is implemented internally:

1. `ResourceCache` maintains a `HashMap<String, Resource*>` mapping resource paths to resource instances.
2. When `Resource::set_path(new_path)` is called, the resource's old path is erased from the cache via `ResourceCache::resources.erase(path_cache)`.
3. Setting the path to an empty string effectively removes the resource from the cache without deleting the resource object itself.
4. Once removed from the cache, the resource is no longer findable by `ResourceLoader` and will be garbage-collected when all remaining `Ref<>` handles are released.

### Purge Process

The `purge_project_resources()` method follows a two-phase approach:

**Phase 1: Collect** -- Calls `ResourceCache::get_cached_resources()` to get a list of all cached resources. Iterates through them and collects any resource whose path starts with `res://` into a `to_evict` vector. Engine built-in resources do not use `res://` paths in devplayer mode, so this filter safely targets only the mounted project's resources.

**Phase 2: Evict** -- Iterates the `to_evict` vector and calls `set_path("")` on each resource. This is done in a separate pass to avoid modifying the `ResourceCache` HashMap while iterating it.

After eviction, both the `to_evict` vector and the `tracked_resource_paths` set are cleared to release all held references.

### Tracking vs. Purging

The class maintains a `HashSet<String> tracked_resource_paths` for explicit tracking via `register_loaded_resource()`. However, the `purge_project_resources()` method does NOT rely on this tracked set. Instead, it scans the entire `ResourceCache` and evicts everything with a `res://` prefix. This is more robust because it catches resources loaded indirectly (via dependency chains, autoloads, etc.) that may not have been explicitly registered.

## Integration

- **LaunchController** calls `begin_tracking()` at step 6 during mount and `purge_project_resources()` at step 4 during unmount.
- **DevPlayerDebug** reads `get_tracked_resource_count()` for the status display.
- Depends on Godot core's `ResourceCache` and `Resource::set_path()` for the eviction mechanism.

## Critical Notes

- The purge evicts ALL `res://` resources from the cache, not just those explicitly tracked. This is by design -- it is the only way to guarantee no stale resources survive a project switch.
- The two-phase collect-then-evict pattern is mandatory. Modifying the cache during iteration would invalidate the iterator and cause undefined behavior.
- After `set_path("")`, the resource object still exists in memory until all `Ref<>` handles are released. If any subsystem holds a lingering `Ref<>` to a purged resource, that resource will not be freed. This is why the LaunchController uses `memdelete()` on the scene node before calling purge -- to release script and scene references first.
- Engine built-in resources (themes, fonts, etc.) loaded by the DevPlayer shell itself do NOT use `res://` paths and are therefore not affected by the purge.
