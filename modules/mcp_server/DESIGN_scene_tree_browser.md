# Design: Broad Scene Tree Browser Tool

## Overview and Rationale

### The Problem

The existing `debug/get_scene_tree` tool returns the **full hierarchical tree** with every node's name, type, ID, scene file path, and children array. For a game with 500+ nodes, this produces a large JSON payload that:

1. Consumes significant tokens in the LLM's context window.
2. Returns more data than needed when the LLM just wants to understand the scene's *structure*.
3. Has no way to page through results -- it is all-or-nothing (with only a `max_depth` truncation knob).
4. Cannot filter by type, group, visibility, or name pattern at fetch time -- the LLM must either search with `debug/search_scene_tree` (which returns flat matches without structural context) or download the entire tree and mentally filter.

### The Solution

A new **`debug/browse_scene_tree`** tool that acts as a lightweight scene tree navigator. Think of it as a "table of contents" for the scene tree:

- **Minimal per-node data**: name, type, path, child count, and a few key indicator flags.
- **Depth control**: browse N levels deep from any subtree root.
- **Inline filtering**: by type, group, visibility, or name pattern -- applied server-side to reduce payload.
- **Cursor-based pagination**: for massive procedural scenes with thousands of siblings.
- **Summary statistics**: total node count, type distribution, group membership counts.

The LLM's workflow becomes:

1. Call `debug/browse_scene_tree` with depth 2 to get the top-level layout.
2. Identify an interesting subtree (e.g., `"/root/Main/World/Enemies"`).
3. Call `debug/browse_scene_tree` again with `root_path` set to that subtree.
4. Once a specific node is identified, call `debug/get_node_properties` for full details.

This two-phase approach keeps context window usage low while giving the LLM full navigational capability.

### Relationship to Existing Tools

| Existing Tool | What It Does | Limitation This Addresses |
|---|---|---|
| `debug/get_scene_tree` | Full tree dump with name/type/id/scene_file per node | Too verbose for broad navigation; no filtering; no pagination |
| `debug/search_scene_tree` | Flat list of nodes matching name/type filters | No structural context (parent-child relationships); no subtree browsing |
| `debug/get_node_properties` | All properties of a single node | Only works once you know the exact path |
| `debug/get_session_summary` | Tree at depth 2 + output/errors/perf | Hardcoded depth; no filtering; bundled with unrelated data |

The new `debug/browse_scene_tree` fills the gap between "dump everything" and "search for specific names."

---

## Proposed Tool

### Tool Name: `debug/browse_scene_tree`

**Title:** Browse Scene Tree

**Description:**
> Browse the running game's scene tree with lightweight summary data. Returns minimal per-node information (name, type, child count, key indicators) -- ideal for understanding scene structure before drilling into specific nodes with debug/get_node_properties. Supports depth control, filtering, subtree browsing, and pagination for large scenes. Uses cached tree by default; set refresh: true to fetch fresh data.

**Annotations:**
- `readOnlyHint`: true
- `destructiveHint`: false
- `idempotentHint`: false (scene tree can change between calls)
- `openWorldHint`: false

### Input Schema

```json
{
  "type": "object",
  "properties": {
    "root_path": {
      "type": "string",
      "description": "Subtree root path to browse from (default: '/root'). Use paths from previous browse results to drill into subtrees. Example: '/root/Main/World/Enemies'"
    },
    "max_depth": {
      "type": "integer",
      "description": "Maximum depth of children to return relative to root_path (default: 2). Use 1 for immediate children only, 0 for just the root node info, -1 for unlimited."
    },
    "type_filter": {
      "type": "string",
      "description": "Only include nodes whose type matches this name exactly (e.g., 'CharacterBody2D', 'Sprite2D'). Their ancestors up to root_path are always included for context."
    },
    "name_pattern": {
      "type": "string",
      "description": "Glob pattern to filter node names (e.g., '*Enemy*', 'Level??'). Case-insensitive. Ancestors are always included for structural context."
    },
    "include_indicators": {
      "type": "boolean",
      "description": "Include key indicator flags per node: has_script, is_visible, group_count (default: true). Set false for maximum compactness."
    },
    "include_stats": {
      "type": "boolean",
      "description": "Include summary statistics: total descendant count, type distribution, group list (default: true). Set false to reduce payload."
    },
    "offset": {
      "type": "integer",
      "description": "Skip this many direct children of root_path before returning results (default: 0). Used for pagination of wide nodes."
    },
    "limit": {
      "type": "integer",
      "description": "Maximum number of direct children of root_path to include (default: 50, max: 200). Use with offset for pagination."
    },
    "refresh": {
      "type": "boolean",
      "description": "Fetch fresh tree from the game before browsing (default: false). Uses cached tree by default for speed."
    }
  },
  "required": []
}
```

### Response Format

#### Structured Content

```json
{
  "root_path": "/root",
  "node": {
    "name": "root",
    "type": "Window",
    "path": "/root",
    "child_count": 3,
    "descendant_count": 247,
    "has_script": false,
    "is_visible": true,
    "group_count": 0,
    "scene_file": "",
    "children": [
      {
        "name": "Main",
        "type": "Node2D",
        "path": "/root/Main",
        "child_count": 4,
        "descendant_count": 198,
        "has_script": true,
        "is_visible": true,
        "group_count": 0,
        "scene_file": "res://scenes/main.tscn",
        "children": [
          {
            "name": "Player",
            "type": "CharacterBody2D",
            "path": "/root/Main/Player",
            "child_count": 5,
            "descendant_count": 12,
            "has_script": true,
            "is_visible": true,
            "group_count": 1,
            "scene_file": "res://scenes/player.tscn",
            "children": "_truncated"
          },
          {
            "name": "Enemies",
            "type": "Node2D",
            "path": "/root/Main/Enemies",
            "child_count": 45,
            "descendant_count": 135,
            "has_script": true,
            "is_visible": true,
            "group_count": 0,
            "scene_file": "",
            "children": "_truncated"
          }
        ]
      },
      {
        "name": "UI",
        "type": "CanvasLayer",
        "path": "/root/UI",
        "child_count": 8,
        "descendant_count": 42,
        "has_script": true,
        "is_visible": true,
        "group_count": 0,
        "scene_file": "res://scenes/ui.tscn",
        "children": "_truncated"
      }
    ]
  },
  "stats": {
    "total_nodes": 247,
    "type_distribution": {
      "Node2D": 45,
      "Sprite2D": 62,
      "CharacterBody2D": 12,
      "CollisionShape2D": 57,
      "AnimatedSprite2D": 12,
      "Label": 15,
      "Button": 8,
      "CanvasLayer": 2,
      "Window": 1,
      "Other": 33
    },
    "groups": {
      "enemies": 45,
      "pickups": 12,
      "persistent": 3
    }
  },
  "pagination": {
    "offset": 0,
    "limit": 50,
    "total_children": 3,
    "has_more": false
  },
  "filters_applied": {
    "type_filter": "",
    "name_pattern": ""
  },
  "cached": true
}
```

#### Text Content

The text representation is a compact indented tree view for LLMs that do not use structured content:

```
Scene Tree Browser: /root (247 nodes total)

root (Window) [3 children, 247 descendants]
  Main (Node2D) [4 ch, 198 desc] [script] [res://scenes/main.tscn]
    Player (CharacterBody2D) [5 ch, 12 desc] [script] [1 group] [res://scenes/player.tscn]
    Enemies (Node2D) [45 ch, 135 desc] [script]
    World (Node2D) [2 ch, 48 desc]
    Items (Node2D) [0 ch] [script]
  UI (CanvasLayer) [8 ch, 42 desc] [script] [res://scenes/ui.tscn]
  AudioManager (Node) [3 ch, 7 desc] [script]

Stats: 247 nodes | Top types: Sprite2D(62) CollisionShape2D(57) Node2D(45) | Groups: enemies(45) pickups(12) persistent(3)
Page: 1/1 (all 3 children shown)
```

---

## Example Request/Response Payloads

### Example 1: Initial Overview (Default Call)

**Request:**
```json
{
  "name": "debug/browse_scene_tree",
  "arguments": {}
}
```

**Response text:**
```
Scene Tree Browser: /root (247 nodes total)

root (Window) [3 children, 247 descendants]
  Main (Node2D) [4 ch, 198 desc] [script] [res://scenes/main.tscn]
    Player (CharacterBody2D) [5 ch, 12 desc] [script] [1 group]
    Enemies (Node2D) [45 ch, 135 desc] [script]
    World (Node2D) [2 ch, 48 desc]
    Items (Node2D) [0 ch] [script]
  UI (CanvasLayer) [8 ch, 42 desc] [script] [res://scenes/ui.tscn]
  AudioManager (Node) [3 ch, 7 desc] [script]

Stats: 247 nodes | Top types: Sprite2D(62) CollisionShape2D(57) Node2D(45)
Groups: enemies(45) pickups(12) persistent(3)
Page: showing children 0-2 of 3
```

### Example 2: Drill Into a Subtree

**Request:**
```json
{
  "name": "debug/browse_scene_tree",
  "arguments": {
    "root_path": "/root/Main/Enemies",
    "max_depth": 1
  }
}
```

**Response text:**
```
Scene Tree Browser: /root/Main/Enemies (135 nodes in subtree)

Enemies (Node2D) [45 children, 135 descendants] [script]
  Goblin_001 (CharacterBody2D) [3 ch, 3 desc] [script] [1 group]
  Goblin_002 (CharacterBody2D) [3 ch, 3 desc] [script] [1 group]
  ...
  Goblin_045 (CharacterBody2D) [3 ch, 3 desc] [script] [1 group]

Stats: 135 nodes | Top types: CharacterBody2D(45) CollisionShape2D(45) Sprite2D(45)
Groups: enemies(45)
Page: showing children 0-44 of 45
```

### Example 3: Paginating Wide Nodes

**Request:**
```json
{
  "name": "debug/browse_scene_tree",
  "arguments": {
    "root_path": "/root/Main/Enemies",
    "max_depth": 1,
    "offset": 10,
    "limit": 5
  }
}
```

**Response text:**
```
Scene Tree Browser: /root/Main/Enemies (135 nodes in subtree)

Enemies (Node2D) [45 children, 135 descendants] [script]
  Goblin_011 (CharacterBody2D) [3 ch, 3 desc] [script] [1 group]
  Goblin_012 (CharacterBody2D) [3 ch, 3 desc] [script] [1 group]
  Goblin_013 (CharacterBody2D) [3 ch, 3 desc] [script] [1 group]
  Goblin_014 (CharacterBody2D) [3 ch, 3 desc] [script] [1 group]
  Goblin_015 (CharacterBody2D) [3 ch, 3 desc] [script] [1 group]

Page: showing children 10-14 of 45 (35 more)
```

### Example 4: Filtering by Type

**Request:**
```json
{
  "name": "debug/browse_scene_tree",
  "arguments": {
    "type_filter": "Button",
    "max_depth": -1,
    "include_stats": false
  }
}
```

**Response text (filtered view with ancestor context):**
```
Scene Tree Browser: /root (filtered: type = 'Button')

root (Window) [1 matching subtree]
  UI (CanvasLayer)
    HUD (Control)
      PauseButton (Button) [0 ch]
      SettingsButton (Button) [0 ch]
    MainMenu (Control)
      StartButton (Button) [0 ch]
      QuitButton (Button) [0 ch]
    PauseMenu (Control)
      ResumeButton (Button) [0 ch]
      MainMenuButton (Button) [0 ch]
    SettingsMenu (Control)
      ApplyButton (Button) [0 ch]
      BackButton (Button) [0 ch]

8 nodes matched filter.
```

### Example 5: Name Pattern + Stats Only

**Request:**
```json
{
  "name": "debug/browse_scene_tree",
  "arguments": {
    "name_pattern": "*Player*",
    "include_indicators": false
  }
}
```

**Response text:**
```
Scene Tree Browser: /root (filtered: name matches '*Player*')

root (Window)
  Main (Node2D)
    Player (CharacterBody2D) [5 ch, 12 desc]
    UI (CanvasLayer)
      HUD (Control)
        PlayerHealth (Label) [0 ch]
        PlayerScore (Label) [0 ch]

3 nodes matched filter.
Stats: Matching types: CharacterBody2D(1) Label(2)
```

---

## Integration Notes

### Recommended LLM Workflow

1. **Start**: Call `debug/get_status` to confirm the game is running.
2. **Orient**: Call `debug/browse_scene_tree` (default params) to get a depth-2 overview with stats.
3. **Navigate**: Call `debug/browse_scene_tree` with `root_path` set to an interesting subtree, increasing `max_depth` as needed.
4. **Find**: If searching for specific nodes, use `type_filter` or `name_pattern` parameters.
5. **Inspect**: Once a specific node path is identified, call `debug/get_node_properties` for full property details.
6. **Act**: Use `debug/evaluate` to read/write specific properties, or `debug/click_control` for UI interaction.

### Complementary Tool Usage

- **`debug/browse_scene_tree`** replaces most uses of `debug/get_scene_tree` for initial orientation. The existing tool remains available for cases where the full hierarchical tree with IDs is needed (e.g., programmatic tree diffing).
- **`debug/search_scene_tree`** remains useful for quick "find all nodes of type X" queries where structural context is not needed. `debug/browse_scene_tree` with a `type_filter` provides a structural alternative.
- **`debug/get_session_summary`** continues to be the best single call for "what is happening right now" (status + tree + output + errors + perf in one round trip).

### Caching Strategy

The tool leverages the existing `cached_scene_tree` from `MCPDebuggerBridge`. The `refresh` parameter triggers a new `request_scene_tree()` round trip. This is identical to how `debug/search_scene_tree` works today, keeping the caching behavior consistent.

All indicator data (has_script, is_visible, group_count) and statistics are derived from the cached tree by evaluating the `view_flags` field that is already present in the flat scene tree data from the game's `SceneDebuggerTree::serialize()`. This means **no additional game round-trips are required** beyond the existing scene tree request.

### What Requires Game-Side Changes

The current `SceneDebuggerTree::serialize()` sends 6 fields per node: `child_count`, `name`, `type_name`, `id`, `scene_file_path`, `view_flags`. The `view_flags` field already encodes visibility and editability flags.

To support the full indicator set, the game-side MCP plugin should be extended to send two additional fields per node:

| Field | Type | Source |
|---|---|---|
| `has_script` | bool | `node->get_script().is_null()` |
| `group_count` | int | `node->get_groups().size()` (excluding internal groups) |

This can be done either by:
- **(Option A -- Preferred)** Adding a new MCP-specific message handler (`mcp:get_scene_tree_browse`) that sends the extended fields. This avoids modifying the existing scene tree protocol.
- **(Option B)** Appending the two fields to the existing flat array format (fields 7 and 8). The bridge's `_flat_tree_to_hierarchical` already parses by stepping in increments; changing from 6 to 8 fields is a localized change.

If game-side changes are deferred, the tool can launch with a **degraded mode** where `has_script` and `group_count` are reported as `null` (unknown), while `is_visible` is derived from the existing `view_flags`.

---

## Implementation Approach

### Files to Modify

| File | Changes |
|---|---|
| `tools/mcp_debug_tools.h` | Add `handle_browse_scene_tree` static method; add private helper methods |
| `tools/mcp_debug_tools.cpp` | Implement handler and registration; add helpers for browse-specific logic |
| `mcp_debugger_bridge.h` | (Optional) Expose view_flags in hierarchical tree if not already accessible |
| `mcp_tool_registry.cpp` | Add `"debug/browse_scene_tree"` to `is_long_running_tool` list |

### New Methods in MCPDebugTools

```cpp
// --- Public ---
static Dictionary handle_browse_scene_tree(const Dictionary &p_args);

// --- Private helpers ---

// Find a subtree by path in the hierarchical tree Dictionary.
// Returns empty Dictionary if not found.
static Dictionary _find_subtree(const Dictionary &p_tree,
        const String &p_path, const String &p_current_path = String());

// Build a browse-format node from a full tree node.
// Includes only: name, type, path, child_count, descendant_count,
// and optional indicators. Recurses up to p_remaining_depth.
static Dictionary _build_browse_node(const Dictionary &p_tree_node,
        const String &p_current_path, int p_remaining_depth,
        bool p_include_indicators, int p_child_offset = 0,
        int p_child_limit = 50);

// Count total descendants of a tree node.
static int _count_descendants(const Dictionary &p_tree);

// Build type distribution map from a subtree.
static Dictionary _build_type_distribution(const Dictionary &p_tree);

// Build group membership counts from a subtree.
// NOTE: Requires extended scene tree data. Returns empty dict if unavailable.
static Dictionary _build_group_stats(const Dictionary &p_tree);

// Apply type/name filters to a tree, returning a pruned copy that
// retains ancestor paths for structural context.
static Dictionary _filter_tree(const Dictionary &p_tree,
        const String &p_type_filter, const String &p_name_pattern);

// Render a browse-format tree as compact text.
static String _browse_tree_to_text(const Dictionary &p_browse_node,
        int p_indent = 0, bool p_include_indicators = true);
```

### Registration Code

```cpp
// debug/browse_scene_tree
{
    Dictionary props;
    props["root_path"] = make_prop("string",
            "Subtree root path to browse from (default: '/root'). "
            "Use paths from previous browse results to drill in.");
    props["max_depth"] = make_prop("integer",
            "Max depth relative to root_path (default: 2). "
            "1 = immediate children. 0 = root only. -1 = unlimited.");
    props["type_filter"] = make_prop("string",
            "Exact type name filter (e.g., 'Button'). Ancestors always shown.");
    props["name_pattern"] = make_prop("string",
            "Glob pattern for node names (e.g., '*Enemy*'). Case-insensitive.");
    props["include_indicators"] = make_prop("boolean",
            "Include has_script/is_visible/group_count per node (default: true).");
    props["include_stats"] = make_prop("boolean",
            "Include summary statistics (default: true).");
    props["offset"] = make_prop("integer",
            "Skip N direct children of root (default: 0). For pagination.");
    props["limit"] = make_prop("integer",
            "Max direct children to return (default: 50, max: 200).");
    props["refresh"] = make_prop("boolean",
            "Fetch fresh tree from game (default: false, uses cache).");
    Array required;
    p_registry->register_tool(
            "debug/browse_scene_tree", "Browse Scene Tree",
            "Browse the running game's scene tree with lightweight summary data. "
            "Returns minimal per-node info (name, type, path, child count, indicators) "
            "for understanding scene structure. Supports subtree browsing, depth control, "
            "type/name filtering, and pagination. Use debug/get_node_properties for "
            "full details on specific nodes. Uses cached tree by default.",
            make_schema(props, required),
            make_annotations(/*readOnly=*/true, /*destructive=*/false,
                    /*idempotent=*/false),
            callable_mp_static(&MCPDebugTools::handle_browse_scene_tree));
}
```

### Handler Implementation Sketch

```cpp
Dictionary MCPDebugTools::handle_browse_scene_tree(const Dictionary &p_args) {
    // 1. Require game running.
    Dictionary guard = _require_game_running();
    if (!guard.is_empty()) return guard;

    // 2. Parse parameters with defaults.
    String root_path = p_args.get("root_path", "/root");
    int max_depth = (int)p_args.get("max_depth", 2);
    String type_filter = p_args.get("type_filter", "");
    String name_pattern = p_args.get("name_pattern", "");
    bool include_indicators = (bool)p_args.get("include_indicators", true);
    bool include_stats = (bool)p_args.get("include_stats", true);
    int offset = (int)p_args.get("offset", 0);
    int limit = (int)p_args.get("limit", 50);
    bool refresh = (bool)p_args.get("refresh", false);

    // 3. Clamp pagination values.
    offset = MAX(offset, 0);
    limit = CLAMP(limit, 1, 200);

    // 4. Get the scene tree (cached or fresh).
    MCPDebuggerBridge *bridge = _get_bridge();
    Dictionary full_tree;

    if (refresh) {
        Dictionary result = bridge->request_scene_tree();
        if (!(bool)result.get("success", false)) {
            return make_tool_error("Failed to fetch scene tree: " +
                    String(result.get("error", "Unknown")));
        }
        full_tree = result.get("tree", Dictionary());
    } else {
        full_tree = bridge->get_cached_scene_tree();
        if (full_tree.is_empty()) {
            // Auto-refresh on cache miss.
            Dictionary result = bridge->request_scene_tree();
            if (!(bool)result.get("success", false)) {
                return make_tool_error("No cached tree and failed to fetch: " +
                        String(result.get("error", "Unknown")));
            }
            full_tree = result.get("tree", Dictionary());
        }
    }

    // 5. Navigate to the requested subtree root.
    Dictionary subtree = _find_subtree(full_tree, root_path);
    if (subtree.is_empty()) {
        return make_tool_error("Subtree not found: " + root_path +
                "\n\nUse debug/browse_scene_tree with no root_path to see the full tree.");
    }

    // 6. Apply filters if requested.
    Dictionary working_tree = subtree;
    if (!type_filter.is_empty() || !name_pattern.is_empty()) {
        working_tree = _filter_tree(subtree, type_filter, name_pattern);
    }

    // 7. Build the browse-format tree with pagination.
    Dictionary browse_node = _build_browse_node(working_tree, root_path,
            max_depth, include_indicators, offset, limit);

    // 8. Build statistics if requested.
    Dictionary stats;
    if (include_stats) {
        stats["total_nodes"] = _count_descendants(subtree) + 1;
        stats["type_distribution"] = _build_type_distribution(subtree);
        stats["groups"] = _build_group_stats(subtree);
    }

    // 9. Build pagination info.
    Array all_children = subtree.get("children", Array());
    Dictionary pagination;
    pagination["offset"] = offset;
    pagination["limit"] = limit;
    pagination["total_children"] = all_children.size();
    pagination["has_more"] = (offset + limit) < all_children.size();

    // 10. Build text representation.
    int total_nodes = _count_descendants(subtree) + 1;
    String text = "Scene Tree Browser: " + root_path +
            " (" + itos(total_nodes) + " nodes";
    if (!type_filter.is_empty()) text += ", filtered: type='" + type_filter + "'";
    if (!name_pattern.is_empty()) text += ", filtered: name='" + name_pattern + "'";
    text += ")\n\n";
    text += _browse_tree_to_text(browse_node, 0, include_indicators);

    if (include_stats && !stats.is_empty()) {
        // Append top-N types.
        Dictionary type_dist = stats.get("type_distribution", Dictionary());
        if (!type_dist.is_empty()) {
            text += "\nTop types: ";
            // (Sort by count descending, show top 5.)
            // ... formatting logic ...
        }
    }

    if (all_children.size() > limit) {
        text += "\nPage: showing children " + itos(offset) + "-" +
                itos(MIN(offset + limit, all_children.size()) - 1) +
                " of " + itos(all_children.size());
    }

    // 11. Build structured response.
    Dictionary structured;
    structured["root_path"] = root_path;
    structured["node"] = browse_node;
    if (include_stats) structured["stats"] = stats;
    structured["pagination"] = pagination;

    Dictionary filters;
    filters["type_filter"] = type_filter;
    filters["name_pattern"] = name_pattern;
    structured["filters_applied"] = filters;
    structured["cached"] = !refresh;

    return make_tool_result(text, structured);
}
```

### Bridge-Level Changes

The existing `_flat_tree_to_hierarchical` stores `name`, `type`, `id`, `scene_file_path`, and `children` per node. The `view_flags` field (already parsed from the flat data at index +5) is currently **discarded** during hierarchical conversion.

**Minimal change:** Store `view_flags` in the hierarchical Dictionary so the browse tool can derive `is_visible` without a game round-trip:

```cpp
// In _flat_tree_to_hierarchical, in the make_dict lambda:
auto make_dict = [](const NodeInfo &info) -> Dictionary {
    Dictionary d;
    d["name"] = info.name;
    d["type"] = info.type;
    d["id"] = info.id;
    d["scene_file_path"] = info.scene_file_path;
    d["view_flags"] = info.view_flags;  // NEW: preserve for browse tool
    d["children"] = Array();
    return d;
};
```

The `view_flags` encode visibility (bit 0 is visible, from `SceneDebuggerTree::RemoteNode::view_flags`). The browse tool can check `(view_flags & 1) != 0` for `is_visible`.

For `has_script` and `group_count`, either:
- **(Phase 1)** Return `null` for these fields and note them as "unavailable without game-side extension" in the response.
- **(Phase 2)** Extend the game-side MCP plugin to send these in the scene tree data.

---

## Edge Cases and Error Handling

### 1. Game Not Running
Return the standard `_require_game_running()` error with guidance to use `debug/run_project` or `debug/run_scene`.

### 2. Invalid `root_path`
```
Error: Subtree not found: /root/NonExistent/Path

Use debug/browse_scene_tree with no root_path to see the full tree,
or debug/search_scene_tree to find a node by name.
```

### 3. `root_path` Character Validation
Apply the same safe-character validation used by `debug/get_node_properties`:
- Allow: alphanumeric, `_`, `/`, `.`, `@`, `:`, `-`, space
- Reject anything else with "Invalid root_path: contains disallowed character"

### 4. Empty Cached Tree
If the cache is empty and `refresh` is false, automatically fetch a fresh tree (same behavior as `debug/search_scene_tree`). If the fetch also fails, return an error.

### 5. Extremely Wide Nodes (1000+ Children)
Pagination with `offset`/`limit` handles this. The default limit of 50 prevents runaway payloads. The `total_children` and `has_more` fields tell the LLM to paginate.

### 6. Extremely Deep Trees
The `max_depth` parameter (default 2) prevents unbounded recursion. When unlimited depth is requested (`max_depth: -1`), an internal safety cap of 100 levels prevents stack overflow (matching the `_tree_to_text` pattern in the bridge).

### 7. Filters Match Nothing
Return a valid response with an empty node tree and a clear message:
```
No nodes matching type='NonExistentType' found under /root.

Try debug/search_scene_tree to search by name, or call debug/browse_scene_tree
without filters to see the full structure.
```

### 8. `offset` Beyond Total Children
Return the root node with an empty children array and `has_more: false`. Not an error -- the LLM may be paginating past the end.

### 9. Scene Tree Changes Between Calls
The cached tree may become stale if the game adds/removes nodes dynamically. The `refresh` parameter addresses this. The response includes `cached: true/false` so the LLM knows whether it is looking at fresh data.

### 10. Concurrent Requests
The existing `request_scene_tree()` mechanism handles one pending scene tree request at a time (new requests supersede old ones via `_create_pending`). The browse tool should not make concurrent scene tree requests. When using the cache, no game round-trip is needed, so concurrency is a non-issue.

### 11. Filter + Pagination Interaction
When filters are active, pagination applies to the *filtered* children of the root node. The `total_children` count reflects the filtered count, not the unfiltered count. The unfiltered total is available in `stats.total_nodes`.

### 12. Type Inheritance
`type_filter` matches the exact type name as reported by the debugger (e.g., `"CharacterBody2D"`). It does **not** match parent classes. This is consistent with `debug/search_scene_tree`. A future enhancement could add an `include_subtypes` flag.

---

## Performance Considerations

### Token Budget

A key goal is minimizing LLM context window consumption. Approximate per-node sizes:

| Format | Bytes per node (approx) |
|---|---|
| `debug/get_scene_tree` (existing) | ~120 bytes (name, type, id, scene_file, children structure) |
| `debug/browse_scene_tree` (text) | ~70 bytes (compact text line) |
| `debug/browse_scene_tree` (structured) | ~150 bytes (more fields, but shallower trees) |

The real savings come from **depth limiting** and **pagination**:
- A 500-node tree at depth 2 typically shows ~20-30 nodes instead of 500.
- Pagination ensures no single response exceeds ~50 nodes.
- Filtering can reduce a 500-node tree to just the 8 Button nodes the LLM cares about.

### Processing Cost

All operations are performed on the cached tree Dictionary (in-memory tree walking). No additional game round-trips are needed for the browse operation itself. The computational cost is O(N) where N is the number of nodes in the subtree, which is acceptable even for trees with thousands of nodes.

### Why Not a Separate Tool File?

The browse tool shares significant infrastructure with the existing debug tools: `_get_bridge()`, `_require_game_running()`, `_count_tree_nodes()`, the cached tree access pattern, and the text formatting style. Keeping it in `mcp_debug_tools.cpp` avoids duplicating these helpers and is consistent with how `debug/search_scene_tree` is already co-located with `debug/get_scene_tree`.

If the debug tools file grows too large (it is currently ~1100 lines), the browse-specific helpers can be extracted to a separate internal file later.

---

## Future Extensions

1. **Group browsing**: A `group` parameter that shows all nodes in a specific group, organized by their tree position.
2. **Diff mode**: Compare the current tree against a previous snapshot to detect added/removed/moved nodes.
3. **Bookmark paths**: The LLM can "bookmark" interesting paths in one call and retrieve updates on just those paths later.
4. **Type inheritance filter**: An `include_subtypes` flag that matches `CharacterBody2D` when filtering for `PhysicsBody2D`.
5. **Editor scene tree**: A variant that browses the editor's scene tree (the .tscn being edited) rather than the running game's tree. This would use Godot's `EditorInterface::get_edited_scene_root()` instead of the debugger bridge.
