# MCP Documentation Tools — Design Document

## 1. Motivation

Context7 (https://github.com/upstash/context7) solves a real problem: LLMs hallucinate API details constantly. They confuse Godot 3 vs 4 syntax, invent non-existent methods, and guess at parameter types. Context7's approach is simple — give the LLM the actual docs in-context before it writes code.

Context7 exposes two MCP tools:
1. **resolve-library-id** — fuzzy-match a library name to an internal ID
2. **query-docs** — retrieve relevant documentation chunks for a query

We don't need the first tool — we **are** Godot. We know the exact version (4.6). The entire class reference is already loaded into memory by the editor at startup via `EditorHelp::generate_doc()`. The `DocTools` singleton holds a `HashMap<String, DocData::ClassDoc>` with ~1,059 classes (912 core + 147 module) fully parsed and ready.

**Our advantage over Context7**: zero network latency, zero crawling/indexing pipeline, version-perfect accuracy (the docs literally come from the same binary), and full access to the inheritance tree, method signatures, and descriptions.

## 2. Architecture Overview

```
LLM (Claude, etc.)
  │
  │  MCP tools/call
  ▼
┌─────────────────────────────────┐
│  MCPDocTools (new tool class)   │
│                                 │
│  doc/search_classes             │  ← "find me classes related to physics"
│  doc/get_class                  │  ← "give me full docs for RigidBody3D"
│  doc/search_methods             │  ← "how do I detect collisions?"
│  doc/get_method                 │  ← "what does Node.add_child() do?"
│  doc/get_property               │  ← "what is Control.anchor_left?"
│                                 │
│  All read from:                 │
│  EditorHelp::get_doc_data()     │
│  → DocTools::class_list         │
│  → HashMap<String, ClassDoc>    │
└─────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────┐
│  DocData (core/doc_data.h)      │
│  Already in memory at runtime   │
│                                 │
│  ClassDoc                       │
│    ├─ name, inherits            │
│    ├─ brief_description         │
│    ├─ description               │
│    ├─ methods[]   (MethodDoc)   │
│    ├─ properties[] (PropertyDoc)│
│    ├─ signals[]   (MethodDoc)   │
│    ├─ constants[] (ConstantDoc) │
│    ├─ constructors[]            │
│    ├─ operators[]               │
│    ├─ annotations[]             │
│    ├─ theme_properties[]        │
│    ├─ enums{}                   │
│    ├─ tutorials[]               │
│    └─ keywords                  │
└─────────────────────────────────┘
```

## 3. Tools

### 3.1 `doc/search_classes`

**Purpose**: Find classes matching a query. The LLM's first step when it needs to understand what's available.

**Parameters**:
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `query` | string | yes | Search terms (e.g., "physics body", "audio", "tween animation") |
| `limit` | integer | no | Max results (default 20, max 50) |
| `category` | string | no | Filter: "node", "resource", "refcounted", or empty for all |

**Matching Strategy** (no embeddings — fast substring/keyword matching):
1. Split query into lowercase terms
2. For each ClassDoc, compute a relevance score:
   - **+10**: Exact class name match (case-insensitive)
   - **+8**: Class name contains a query term
   - **+5**: `brief_description` contains a query term
   - **+3**: `keywords` field contains a query term
   - **+2**: `description` contains a query term
   - **+1**: Method/property/signal name contains a query term
3. All terms must match somewhere (AND logic) to be included
4. Sort by score descending, return top `limit` results

**Return Format**:
```
# Godot 4.6 — Class Search Results for "physics body"
# Found 8 matching classes (showing top 8)

## RigidBody3D (inherits PhysicsBody3D)
A 3D physics body that is moved by a physics simulation.

## CharacterBody3D (inherits PhysicsBody3D)
A 3D physics body specialized for characters moved by script.

## PhysicsBody3D (inherits CollisionObject3D)
Abstract base class for 3D physics bodies.
...
```

Structured content includes an array of `{name, inherits, brief_description, score}`.

### 3.2 `doc/get_class`

**Purpose**: Get complete documentation for a specific class. The core workhorse — equivalent to Context7's `query-docs` but for a single class.

**Parameters**:
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `class_name` | string | yes | Exact class name (e.g., "Node3D", "RigidBody3D") |
| `sections` | string | no | Comma-separated filter: "methods,properties,signals,constants,description" — default "all" |
| `include_inherited` | boolean | no | Include inherited members (default false — keeps output focused) |

**Output Sections** (in order, each section omitted if empty or filtered out):

1. **Header**: Class name, inherits chain, brief description, deprecation/experimental status
2. **Description**: Full class description (BBCode stripped to plain text)
3. **Tutorials**: Tutorial links
4. **Properties**: Table of name, type, default value, with descriptions
5. **Methods**: Signature + description for each method
6. **Signals**: Signal name + parameters + description
7. **Constants/Enums**: Grouped by enum, with values and descriptions
8. **Theme Properties**: Theme overrides
9. **Annotations**: GDScript annotations
10. **Operators**: Operator overloads

**Size Management**:
Large classes (Node, Control, RenderingServer) can produce enormous output. Strategy:
- When `sections` is "all" and estimated output exceeds ~8KB of text, return a **summary mode**: header + description + method/property/signal/constant **names only** (no descriptions), with a note telling the LLM to request specific sections or use `doc/get_method` for details.
- When specific sections are requested, return full detail for those sections.
- The `include_inherited` flag defaults to false to avoid pulling the entire Node chain into a RigidBody3D query.

### 3.3 `doc/search_methods`

**Purpose**: Search across all classes for methods matching a query. Critical for "how do I..." questions.

**Parameters**:
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `query` | string | yes | Search terms (e.g., "get children", "play animation", "load scene") |
| `limit` | integer | no | Max results (default 20, max 50) |
| `class_filter` | string | no | Restrict to a specific class and its ancestors |

**Matching**: Same term-based scoring as class search, but applied to method names, descriptions, parameter names, and keywords. Results grouped by class.

**Return Format**:
```
# Godot 4.6 — Method Search Results for "get children"

## Node.get_children(include_internal: bool = false) -> Array[Node]
Returns all children of this node. If include_internal is false, ...

## Node.get_child(idx: int, include_internal: bool = false) -> Node
Returns a child node by its index. ...

## Node.find_children(pattern: String, type: String = "", ...) -> Array[Node]
Finds descendants of this node whose name matches the given pattern. ...
```

### 3.4 `doc/get_method`

**Purpose**: Get detailed documentation for a specific method, signal, or property on a class.

**Parameters**:
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `class_name` | string | yes | The class name |
| `member_name` | string | yes | Method, signal, property, or constant name |
| `member_type` | string | no | Hint: "method", "property", "signal", "constant", "annotation" — auto-detected if omitted |

**Output**: Full detail including signature, return type, all parameters with types and defaults, description, deprecation info, related methods (by scanning description cross-references).

### 3.5 `doc/get_property`

**Purpose**: Convenience alias — equivalent to `doc/get_method` with `member_type=property`, but also includes setter/getter method names and default value.

**Parameters**:
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `class_name` | string | yes | The class name |
| `property_name` | string | yes | The property name |

## 4. Implementation Details

### 4.1 File Structure

```
modules/mcp_server/tools/mcp_doc_tools.h    — Class declaration
modules/mcp_server/tools/mcp_doc_tools.cpp  — Implementation
```

Follows the exact same pattern as `mcp_test_tools.h/.cpp`.

### 4.2 Registration

In `mcp_protocol.cpp`, add:
```cpp
#include "tools/mcp_doc_tools.h"
// ...
MCPDocTools::register_tools(&tool_registry);
```

### 4.3 Accessing Doc Data

```cpp
#include "editor/doc/editor_help.h"

DocTools *docs = EditorHelp::get_doc_data();
// docs->class_list is HashMap<String, DocData::ClassDoc>
```

**Thread Safety**: `EditorHelp::get_doc_data()` calls `_wait_for_thread()` internally, so it's safe to call from the MCP poll thread. The doc data is generated once at editor startup and is read-only thereafter (script docs can be added, but that's main-thread-only and won't race with our reads in practice).

**TOOLS_ENABLED guard**: The entire EditorHelp system is behind `#ifdef TOOLS_ENABLED`. Since MCP tools also only run in the editor, this is fine. The `mcp_doc_tools.cpp` should include the guard or the tool registration should be conditional.

### 4.4 BBCode to Plain Text

Godot doc descriptions use BBCode-like markup: `[b]`, `[i]`, `[code]`, `[url]`, `[method name]`, `[member name]`, `[param name]`, `[signal name]`, `[constant name]`, `[enum name]`, `[annotation name]`, `[theme_item name]`, `[codeblock]...[/codeblock]`, `[codeblocks][gdscript]...[/gdscript][/codeblocks]`.

We need a `_bbcode_to_text()` helper that:
- Strips formatting tags (`[b]`, `[i]`, `[/b]`, `[/i]`)
- Converts `[code]x[/code]` → `` `x` `` (backticks)
- Converts `[codeblock]` and `[gdscript]` blocks → fenced code blocks (```gdscript)
- Converts `[method name]` → `name()`
- Converts `[member name]` → `name`
- Converts `[param name]` → `name`
- Converts `[url=...]text[/url]` → `text (URL)`
- Preserves `[csharp]` blocks but labels them
- Handles `[br]` → newline
- Handles `[b]Note:[/b]` → `**Note:**`

### 4.5 Inheritance Resolution

For `include_inherited=true`:
```cpp
void _collect_inherited_members(const String &p_class, DocTools *p_docs,
    Vector<DocData::MethodDoc> &r_methods, ...) {
    String current = p_class;
    while (!current.is_empty()) {
        const DocData::ClassDoc *cd = p_docs->class_list.getptr(current);
        if (!cd) break;
        for (const auto &m : cd->methods) r_methods.push_back(m);
        // ... same for properties, signals, constants
        current = cd->inherits;
    }
}
```

### 4.6 Search Index (Optional Optimization)

For the initial implementation, brute-force scanning of all ~1,059 ClassDocs is fine — the data is in memory and a full scan takes <1ms.

If profiling shows it's slow (unlikely), we can build a simple inverted index at startup:
```cpp
HashMap<String, Vector<Pair<String, int>>> word_to_classes;  // word → [(class_name, score)]
```

But I recommend **not** doing this initially. Keep it simple.

### 4.7 Output Size Management

The `doc/get_class` tool needs to handle massive classes gracefully. ProjectSettings.xml is 360KB of XML — the in-memory ClassDoc could produce 100KB+ of text output.

**Strategy**:
1. First pass: estimate output size by counting methods + properties + (avg description length ~100 chars)
2. If estimated > 8KB and `sections="all"`:
   - Return summary mode with just signatures
   - Append: "This class has N methods. Use `doc/get_class` with `sections` parameter or `doc/get_method` for specific member details."
3. If specific sections requested: return full detail regardless of size
4. Hard cap at 32KB text output (truncate with message)

### 4.8 Method Signature Formatting

```cpp
String _format_method_sig(const DocData::MethodDoc &p_method) {
    // "func method_name(param1: Type = default, param2: Type) -> ReturnType"
    String sig = p_method.name + "(";
    for (int i = 0; i < p_method.arguments.size(); i++) {
        if (i > 0) sig += ", ";
        const auto &arg = p_method.arguments[i];
        sig += arg.name + ": " + arg.type;
        if (!arg.default_value.is_empty()) {
            sig += " = " + arg.default_value;
        }
    }
    sig += ")";
    if (!p_method.return_type.is_empty() && p_method.return_type != "void") {
        sig += " -> " + p_method.return_type;
    }
    if (!p_method.qualifiers.is_empty()) {
        sig += "  [" + p_method.qualifiers + "]";
    }
    return sig;
}
```

## 5. Tool Descriptions (for LLM Discovery)

Tool descriptions are critical — they're what the LLM reads during `tools/list` to decide which tool to call. They need to be specific enough to trigger correct usage.

### doc/search_classes
```
Search the Godot 4.6 class reference for classes matching a query.

Use this FIRST when you need to find which class provides specific functionality.
Returns class names, inheritance, and brief descriptions ranked by relevance.

Examples:
- "physics body" → RigidBody3D, CharacterBody3D, StaticBody3D...
- "audio" → AudioStreamPlayer, AudioBus, AudioEffect...
- "tween" → Tween, PropertyTweener, MethodTweener...
- "file" → FileAccess, DirAccess, EditorFileSystem...
```

### doc/get_class
```
Get complete documentation for a specific Godot 4.6 class.

Returns the full class reference including description, methods, properties,
signals, constants, and more. Use the 'sections' parameter to limit output
for large classes.

This is the authoritative source — never guess at Godot API signatures.
Always check here before writing code that uses a class.

The class_name must be exact (case-sensitive): "Node3D" not "node3d".
```

### doc/search_methods
```
Search across ALL Godot 4.6 classes for methods matching a query.

Use this when you know WHAT you want to do but not WHICH class has the method.

Examples:
- "play animation" → AnimationPlayer.play(), AnimationTree.set_active()...
- "load scene" → ResourceLoader.load(), PackedScene.instantiate()...
- "get mouse position" → Viewport.get_mouse_position(), Input.get_last_mouse_velocity()...
```

### doc/get_method
```
Get detailed documentation for a specific method, signal, constant, or annotation.

Returns the full signature, parameter details, description, and related references.
Use this after doc/search_methods or doc/get_class to get complete details.
```

### doc/get_property
```
Get detailed documentation for a specific property on a Godot 4.6 class.

Returns the property type, default value, setter/getter methods, and full description.
```

## 6. Comparison with Context7

| Feature | Context7 | Our Approach |
|---------|----------|--------------|
| Data source | Web crawling + indexing | Already in memory (DocTools) |
| Search method | Embeddings + vector search (proprietary) | Keyword/substring scoring |
| Versioning | Multiple versions indexed | Exact version — same binary |
| Latency | Network round-trip to API | <1ms (memory access) |
| Accuracy | May have stale/incomplete docs | 100% accurate — same source as F1 help |
| Chunking | Proprietary chunking pipeline | Natural chunks: classes, methods, properties |
| Resolution step | `resolve-library-id` needed | Not needed — we ARE the library |
| Scope | Any library | Godot only (which is all we need) |

## 7. Implementation Plan

### Phase 1: Core (MVP)
1. Create `mcp_doc_tools.h` — class declaration with 5 tool handlers
2. Create `mcp_doc_tools.cpp`:
   - `_bbcode_to_text()` helper
   - `_format_method_sig()` helper
   - `_score_class()` search scoring
   - `handle_search_classes()`
   - `handle_get_class()` with size management
   - `register_tools()`
3. Wire into `mcp_protocol.cpp`
4. Build and test

### Phase 2: Search Methods
5. `handle_search_methods()` — cross-class method search
6. `handle_get_method()` — specific member lookup
7. `handle_get_property()` — property detail

### Phase 3: Polish
8. Test with real LLM queries
9. Tune scoring weights based on real usage
10. Add `@GlobalScope` built-in functions/constants as a searchable source
11. Consider adding GDScript language reference (keywords, syntax) as a pseudo-class

## 8. Open Questions

1. **Should `doc/get_class` support fuzzy matching?** E.g., "rigidbody3d" → "RigidBody3D". Probably yes — LLMs often get casing wrong. We can do case-insensitive lookup as fallback.

2. **Should we expose constructors and operators?** They're in the data. For now, include them in `doc/get_class` output but not in method search results (constructors are rarely searched for).

3. **Should we add a `doc/get_inheritance_tree` tool?** Could be useful for understanding class hierarchies. Low priority — `doc/get_class` already shows the `inherits` chain.

4. **Should we cache formatted output?** The BBCode-to-text conversion is cheap (<1ms per class), so caching is unnecessary unless profiling proves otherwise.

5. **Do we need `doc/search_all`?** A unified search across classes, methods, properties, and signals. This is what `EditorHelpSearch` does. Could be useful as a one-stop shop, but the separate tools give the LLM more control over what it retrieves.
