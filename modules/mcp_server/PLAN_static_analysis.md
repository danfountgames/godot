# PLAN: Static Analysis Tools

**Branch:** `mcp-server`
**Priority:** P1
**Effort:** Medium (3-5 days)
**Dependencies:** None — operates on project files via GDScript parser

---

## Motivation

godot-mcp-ultimate has 8 code quality tools (dead code, complexity, signal flow,
duplication, dependency graph, linting). Our `testing/check_script` and
`testing/check_all_scripts` do parse/syntax checking only — they use
`GDScriptLanguage::validate()` which finds compilation errors but can't answer:

- "Which functions are never called?"
- "Where does this signal chain lead?"
- "How complex is this function?"
- "Are there circular autoload dependencies?"

These questions make the LLM a **code reviewer**, not just a writer.

---

## New File

```
tools/mcp_analysis_tools.h
tools/mcp_analysis_tools.cpp
```

---

## Architecture: The Script Database

All analysis tools share a common data structure: a project-wide script index
built by parsing every `.gd` file in the project.

```cpp
// Internal data structure, built on first use and cached.
struct ScriptSymbol {
    String name;
    String type;            // "function", "variable", "signal", "class"
    String file;            // res:// path
    int line;               // Line number
    bool is_exported;       // @export
    bool is_static;
    String return_type;     // For functions
    Vector<String> parameters;
};

struct ScriptFile {
    String path;            // res://
    String class_name;      // class_name if declared
    String extends;         // Base class
    bool is_tool;           // @tool
    bool is_autoload;       // Registered as autoload
    Vector<ScriptSymbol> functions;
    Vector<ScriptSymbol> variables;
    Vector<ScriptSymbol> signals;
    Vector<String> signal_connections;  // connect("sig", callable) calls
    Vector<String> signal_emissions;   // sig.emit() calls
    Vector<String> references;         // Identifiers used (potential call targets)
    int total_lines;
    HashMap<String, int> function_complexities;  // Cyclomatic complexity per function
};

struct ProjectIndex {
    HashMap<String, ScriptFile> files;  // path -> ScriptFile
    HashMap<String, Vector<String>> symbol_definitions;  // name -> [defining files]
    HashMap<String, Vector<String>> symbol_references;   // name -> [referencing files]
    bool is_built = false;
    uint64_t build_time_msec = 0;
};
```

### Building the Index

```cpp
// Uses GDScriptLanguage::validate() + manual parsing for semantic info.
// GDScript's parser gives us:
//   - functions (names, parameters, return types)
//   - variables (names, types, exports)
//   - signals (names, parameters)
//
// We additionally grep for:
//   - .connect("signal_name", ...) calls
//   - signal_name.emit() calls
//   - Function/method calls by name
//   - preload() / load() references

void MCPAnalysisTools::_build_index(const String &p_root = "res://") {
    // 1. Find all .gd files recursively
    // 2. For each file:
    //    a. Read source code
    //    b. Parse with GDScriptParser (if available) or regex
    //    c. Extract symbols, connections, emissions, calls
    //    d. Compute cyclomatic complexity per function
    // 3. Build cross-reference maps
    // 4. Cache result
}
```

---

## New Tools (5 tools)

### Tool 1: `analysis/dead_code`

**Purpose:** Find unused functions, variables, and signals.

**Parameters:**
- `path` (string, optional, default "res://"): Scope to analyze
- `include_private` (boolean, optional, default true): Include _-prefixed names
- `ignore_virtual` (boolean, optional, default true): Skip _ready, _process, etc.

**Implementation:**
```
For each function defined in the project:
  1. Is it a virtual override (_ready, _process, _input, etc.)? → Not dead
  2. Is it referenced by name anywhere in any other file? → Not dead
  3. Is it connected to a signal via connect()? → Not dead
  4. Is it in a @tool script's _run()? → Not dead
  5. Is it @export or used in EditorProperty? → Not dead
  Otherwise → Dead code candidate

For each signal defined:
  1. Is it connected to anywhere? → Not dead
  2. Is it emitted anywhere? → Possibly dead (defined but never emitted)

For each variable:
  1. Is it read anywhere outside its declaration? → Not dead
  2. Is it @export? → Not dead (editor-set)
```

**Returns:**
```json
{
  "dead_functions": [
    {"name": "calculate_damage_old", "file": "res://scripts/combat.gd", "line": 145},
    {"name": "_debug_draw", "file": "res://scripts/enemy.gd", "line": 89}
  ],
  "dead_signals": [
    {"name": "combo_achieved", "file": "res://scripts/player.gd", "line": 12,
     "reason": "defined but never emitted or connected"}
  ],
  "dead_variables": [
    {"name": "old_speed", "file": "res://scripts/player.gd", "line": 23}
  ],
  "summary": {
    "dead_functions": 5,
    "dead_signals": 2,
    "dead_variables": 3,
    "total_functions": 234,
    "total_signals": 45,
    "percentage_dead": "3.5%"
  }
}
```

**LLM description:**
> Find unused functions, signals, and variables in the project. Reports code that
> is defined but never referenced, connected, or called from anywhere in the project.
> Virtual overrides (_ready, _process, etc.) and @export variables are excluded by
> default. Dead code is safe to remove and reduces maintenance burden.

---

### Tool 2: `analysis/signal_flow`

**Purpose:** Trace signal connections across the project.

**Parameters:**
- `signal_name` (string, optional): Trace a specific signal
- `file` (string, optional): Scope to a specific file
- `direction` (string, optional, default "both"): "emitters", "receivers", or "both"

**Implementation:**
```
Build a signal graph:
  For each file:
    - Signals defined (signal xxx)
    - Signals emitted (xxx.emit() or emit_signal("xxx"))
    - Signals connected (connect("xxx", callable) or .xxx.connect(callable))
    - Signal handler methods (methods that are connect targets)

Return as a directed graph: emitter → signal → receiver
```

**Returns:**
```json
{
  "signals": [
    {
      "name": "health_changed",
      "defined_in": "res://scripts/player.gd:12",
      "emitters": [
        {"file": "res://scripts/player.gd", "line": 45, "context": "take_damage()"},
        {"file": "res://scripts/player.gd", "line": 67, "context": "heal()"}
      ],
      "receivers": [
        {"file": "res://scripts/ui/health_bar.gd", "line": 20, "method": "_on_health_changed"},
        {"file": "res://scripts/audio_manager.gd", "line": 55, "method": "_on_player_health_changed"}
      ]
    }
  ],
  "orphan_signals": [
    {"name": "combo_achieved", "file": "res://scripts/player.gd", "line": 15,
     "issue": "defined but never emitted"}
  ],
  "orphan_connections": [
    {"signal": "enemy_died", "receiver": "res://scripts/score.gd:_on_enemy_died",
     "issue": "connected to signal that doesn't exist in the emitting class"}
  ]
}
```

**LLM description:**
> Trace signal flow across the project. Shows where each signal is defined,
> emitted, and connected. Identifies orphan signals (defined but never emitted)
> and orphan connections (connected to non-existent signals). Use to understand
> event-driven communication patterns or debug signal wiring issues.

---

### Tool 3: `analysis/complexity`

**Purpose:** Calculate cyclomatic complexity for functions.

**Parameters:**
- `path` (string, optional): Scope to analyze
- `threshold` (integer, optional, default 10): Only report functions above this complexity
- `sort_by` (string, optional, default "complexity"): "complexity" or "file"

**Implementation:**
```
Cyclomatic complexity = 1 + number of decision points

Decision points in GDScript:
  if, elif, else    → +1 each
  for, while        → +1 each
  match (per arm)   → +1 each
  and, or           → +1 each (in conditions)
  ternary (x if c else y) → +1
  assert            → +1

Parse each function body, count decision points.
```

**Returns:**
```json
{
  "functions": [
    {"name": "parse_save_file", "file": "res://scripts/save_manager.gd", "line": 45,
     "complexity": 23, "grade": "D", "suggestion": "Consider breaking into smaller functions"},
    {"name": "update_ai_state", "file": "res://scripts/enemy_ai.gd", "line": 89,
     "complexity": 15, "grade": "C", "suggestion": "Consider using a state machine pattern"}
  ],
  "summary": {
    "total_functions": 234,
    "average_complexity": 4.2,
    "above_threshold": 8,
    "grade_distribution": {"A": 180, "B": 38, "C": 8, "D": 6, "F": 2}
  },
  "grading": "A: 1-5, B: 6-10, C: 11-15, D: 16-25, F: 26+"
}
```

**LLM description:**
> Calculate cyclomatic complexity for every function in the project. Complex
> functions (many branches/conditions) are harder to test and maintain. Default
> threshold of 10 — functions above this are reported. Grades: A (1-5 simple),
> B (6-10 moderate), C (11-15 complex), D (16-25 very complex), F (26+ untestable).

---

### Tool 4: `analysis/dependencies`

**Purpose:** Map autoload dependencies and detect circular references.

**Parameters:**
- `include_preloads` (boolean, optional, default true): Include preload() references

**Implementation:**
```
1. Read project.godot for autoload declarations.
2. For each autoload script:
   - What other autoloads does it reference?
   - What does it preload()?
   - What signals does it connect to from other autoloads?
3. Build directed dependency graph.
4. Run cycle detection (DFS with coloring).
5. Report graph + any cycles.
```

**Returns:**
```json
{
  "autoloads": [
    {"name": "GameManager", "path": "res://scripts/game_manager.gd",
     "depends_on": ["SaveManager", "AudioManager"]},
    {"name": "SaveManager", "path": "res://scripts/save_manager.gd",
     "depends_on": ["GameManager"]},
    {"name": "AudioManager", "path": "res://scripts/audio_manager.gd",
     "depends_on": []}
  ],
  "cycles": [
    ["GameManager", "SaveManager", "GameManager"]
  ],
  "warnings": [
    {
      "severity": "HIGH",
      "message": "Circular dependency: GameManager ↔ SaveManager",
      "fix": "Extract shared state into a third autoload or use signals instead of direct references."
    }
  ]
}
```

**LLM description:**
> Map autoload dependencies and detect circular references. Circular dependencies
> between autoloads cause initialization order bugs and are hard to debug. Also
> shows the full dependency graph for understanding project architecture.

---

### Tool 5: `analysis/project_health`

**Purpose:** Comprehensive project quality dashboard.

**Parameters:** None

**Implementation:**
```
Aggregate results from all other analysis tools:
1. Run dead code detection → dead code percentage
2. Run complexity analysis → average complexity, worst functions
3. Run dependency analysis → cycle count
4. Run signal flow → orphan signal count
5. Count: total scripts, total lines, test coverage estimate
6. Check: naming conventions, file organization
7. Compute overall grade
```

**Returns:**
```json
{
  "grade": "B+",
  "scores": {
    "dead_code": {"score": 9, "detail": "2.1% dead code (good)"},
    "complexity": {"score": 7, "detail": "Average 6.3 (moderate)"},
    "dependencies": {"score": 10, "detail": "No circular dependencies"},
    "signal_hygiene": {"score": 8, "detail": "2 orphan signals"},
    "test_coverage": {"score": 5, "detail": "~30% of scripts have tests"},
    "script_errors": {"score": 10, "detail": "0 parse errors"}
  },
  "top_issues": [
    "parse_save_file() has complexity 23 — refactor into smaller functions",
    "combo_achieved signal is defined but never emitted — remove or implement",
    "Test coverage is low — add tests for combat and AI scripts"
  ],
  "stats": {
    "total_scripts": 67,
    "total_lines": 12450,
    "total_functions": 234,
    "total_signals": 45,
    "total_autoloads": 5
  }
}
```

**LLM description:**
> Comprehensive project health dashboard. Scores the project on dead code,
> complexity, dependencies, signal hygiene, test coverage, and syntax errors.
> Returns an overall grade (A-F) and the top issues to fix. Use this for a
> high-level project review.

**Progress handler:** Yes (runs all analyses, takes several seconds)

---

## Implementation Notes

### GDScript Parsing Approach

**Option A: Use GDScriptParser directly (C++)**
```cpp
#include "modules/gdscript/gdscript_parser.h"

GDScriptParser parser;
parser.parse(source_code, path, false);
// Walk the parse tree for functions, signals, variables, etc.
```

**Pro:** Most accurate, understands all GDScript syntax.
**Con:** GDScriptParser API is internal and may change between versions.

**Option B: Regex-based extraction**
```cpp
// Parse using line-by-line regex:
// func (\w+)\((.*)\)       → function definition
// signal (\w+)             → signal definition
// var (\w+)                → variable definition
// \.connect\("(\w+)"       → signal connection
// \.emit\(\)               → signal emission (preceding word is signal name)
// (if|elif|for|while|match) → complexity decision points
```

**Pro:** Simpler, no dependency on parser internals.
**Con:** Can miss edge cases (multiline signatures, comments, etc.).

**Recommended:** Start with regex (Option B) for speed. Add GDScriptParser
(Option A) as an accuracy improvement later.

### Caching

The project index should be cached and invalidated when files change:
```cpp
// Listen to EditorFileSystem::filesystem_changed signal
// Rebuild index only when .gd files are added/removed/modified
```

---

## File Changes Summary

| File | Change |
|------|--------|
| `tools/mcp_analysis_tools.h` | **NEW** — class MCPAnalysisTools + ProjectIndex structs |
| `tools/mcp_analysis_tools.cpp` | **NEW** — 5 tools + index builder + parsers |
| `mcp_protocol.cpp` | Add `MCPAnalysisTools::register_tools(&tool_registry);` |
| `tests/test_analysis_tools.py` | **NEW** — analysis tool tests |
| `README.md` | Update tool count, add analysis section |
