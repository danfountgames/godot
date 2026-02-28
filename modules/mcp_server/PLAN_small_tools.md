# PLAN: Small Tools Bundle

Five small features bundled into one plan. Each is tiny (< 1 day).

**Branch:** `mcp-server`

---

## 1. `runtime/clear_output` — Clear Output/Error Buffers

**Priority:** P2 | **Effort:** Tiny (30 min)

### Motivation

GDAI has `clear_output_logs`. The LLM needs to clear the output buffer before
running an action so it can cleanly see only the new output. Currently, the ring
buffer is append-only.

### Implementation

```cpp
// mcp_debugger_bridge.h — add public methods:
void clear_output_buffer();
void clear_error_buffer();

// mcp_debugger_bridge.cpp — implementation:
void MCPDebuggerBridge::clear_output_buffer() {
    MutexLock lock(output_mutex);  // Use existing mutex
    output_buffer.clear();  // Need to add clear() to OutputRingBuffer
}

// OutputRingBuffer — add clear method:
void OutputRingBuffer::clear() {
    entries.clear();
    entries.resize(CAPACITY);
    write_pos = 0;
    count = 0;
    next_seq = next_seq;  // Keep sequence monotonic — don't reset!
    // This way cursors held by clients become "expired" gracefully.
}
```

### Tool Registration

```cpp
// In mcp_debug_tools.cpp:
{
    Dictionary props;
    props["target"] = make_prop("string",
        "What to clear: 'output', 'errors', or 'all' (default: 'all')");
    Array required;
    p_registry->register_tool(
        "runtime/clear_output", "Clear Output",
        "Clear the output and/or error buffers. Use before performing an action "
        "to establish a clean baseline, then check runtime/get_output to see only "
        "the new output. Does not affect the running game — only clears the MCP "
        "server's captured buffer.",
        make_schema(props, required),
        make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
        callable_mp_static(&MCPDebugTools::handle_clear_output));
}
```

### Handler

```cpp
Dictionary MCPDebugTools::handle_clear_output(const Dictionary &p_args) {
    MCPDebuggerBridge *bridge = _get_bridge();
    ERR_FAIL_NULL_V(bridge, make_tool_error("Debugger bridge not available."));

    String target = ((String)p_args.get("target", "all")).to_lower();

    if (target == "output" || target == "all") {
        bridge->clear_output_buffer();
    }
    if (target == "errors" || target == "all") {
        bridge->clear_error_buffer();
    }

    return make_tool_result("Cleared " + target + " buffer(s).");
}
```

### File Changes
- `mcp_debugger_bridge.h/cpp` — Add `clear()` to OutputRingBuffer, add public clear methods
- `tools/mcp_debug_tools.h/cpp` — Add tool registration + handler

---

## 2. `scene/set_anchor_preset` — Anchor Presets for Controls

**Priority:** P3 | **Effort:** Tiny (1 hour)

### Motivation

GDAI has dedicated anchor tools. Setting anchors requires knowing the 4 values
(left, top, right, bottom). Presets like "full_rect" or "center" are much more
ergonomic.

### Implementation

```cpp
// Preset lookup table
static const HashMap<String, Vector4> ANCHOR_PRESETS = {
    {"top_left",      Vector4(0, 0, 0, 0)},
    {"top_right",     Vector4(1, 0, 1, 0)},
    {"bottom_left",   Vector4(0, 1, 0, 1)},
    {"bottom_right",  Vector4(1, 1, 1, 1)},
    {"center",        Vector4(0.5, 0.5, 0.5, 0.5)},
    {"center_left",   Vector4(0, 0.5, 0, 0.5)},
    {"center_right",  Vector4(1, 0.5, 1, 0.5)},
    {"center_top",    Vector4(0.5, 0, 0.5, 0)},
    {"center_bottom", Vector4(0.5, 1, 0.5, 1)},
    {"left_wide",     Vector4(0, 0, 0, 1)},
    {"right_wide",    Vector4(1, 0, 1, 1)},
    {"top_wide",      Vector4(0, 0, 1, 0)},
    {"bottom_wide",   Vector4(0, 1, 1, 1)},
    {"full_rect",     Vector4(0, 0, 1, 1)},
    {"vcenter_wide",  Vector4(0.5, 0, 0.5, 1)},
    {"hcenter_wide",  Vector4(0, 0.5, 1, 0.5)},
};
// Vector4 = (anchor_left, anchor_top, anchor_right, anchor_bottom)
```

### Tool Registration

```cpp
// In mcp_scene_tools.cpp:
{
    Dictionary props;
    props["node_path"] = make_prop("string", "Path to the Control node");
    props["preset"] = make_prop("string",
        "Anchor preset name: top_left, top_right, bottom_left, bottom_right, "
        "center, center_left, center_right, center_top, center_bottom, "
        "left_wide, right_wide, top_wide, bottom_wide, full_rect, "
        "vcenter_wide, hcenter_wide");
    Array required;
    required.push_back("node_path");
    required.push_back("preset");
    p_registry->register_tool(
        "scene/set_anchor_preset", "Set Anchor Preset",
        "Set the anchor of a Control node using a named preset. Presets set "
        "all four anchor values (left, top, right, bottom) at once. Commonly "
        "used presets: 'full_rect' (fills parent), 'center' (centered point), "
        "'top_wide' (top edge, full width). Modifies the scene file on disk.",
        make_schema(props, required),
        make_annotations(/*readOnly=*/false, /*destructive=*/false, /*idempotent=*/true),
        callable_mp_static(&MCPSceneTools::handle_set_anchor_preset));
}
```

### Handler

```cpp
Dictionary MCPSceneTools::handle_set_anchor_preset(const Dictionary &p_args) {
    String node_path = p_args.get("node_path", "");
    String preset = ((String)p_args.get("preset", "")).to_lower();

    if (!ANCHOR_PRESETS.has(preset)) {
        return make_tool_error(
            "Unknown anchor preset: '" + preset + "'\n\n"
            "Available presets: top_left, top_right, bottom_left, bottom_right, "
            "center, center_left, center_right, center_top, center_bottom, "
            "left_wide, right_wide, top_wide, bottom_wide, full_rect, "
            "vcenter_wide, hcenter_wide");
    }

    Vector4 anchors = ANCHOR_PRESETS[preset];

    // Get scene root, find node, verify it's a Control
    Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
    ERR_FAIL_NULL_V(root, make_tool_error("No scene open in editor."));
    Node *node = root->get_node_or_null(NodePath(node_path));
    if (!node) {
        return make_tool_error("Node not found: " + node_path);
    }
    Control *control = Object::cast_to<Control>(node);
    if (!control) {
        return make_tool_error("Node is not a Control: " + node_path +
                               " (type: " + node->get_class() + ")");
    }

    // Use UndoRedo for editor-undoable operation
    EditorUndoRedoManager *undo_redo = EditorInterface::get_singleton()->get_editor_undo_redo();
    undo_redo->create_action("Set Anchor Preset: " + preset);
    undo_redo->add_do_property(control, "anchor_left", anchors.x);
    undo_redo->add_do_property(control, "anchor_top", anchors.y);
    undo_redo->add_do_property(control, "anchor_right", anchors.z);
    undo_redo->add_do_property(control, "anchor_bottom", anchors.w);
    undo_redo->add_undo_property(control, "anchor_left", control->get_anchor(SIDE_LEFT));
    undo_redo->add_undo_property(control, "anchor_top", control->get_anchor(SIDE_TOP));
    undo_redo->add_undo_property(control, "anchor_right", control->get_anchor(SIDE_RIGHT));
    undo_redo->add_undo_property(control, "anchor_bottom", control->get_anchor(SIDE_BOTTOM));
    undo_redo->commit_action();

    return make_tool_result(
        "Set anchor preset '" + preset + "' on " + node_path + "\n"
        "Anchors: left=" + String::num(anchors.x) + " top=" + String::num(anchors.y) +
        " right=" + String::num(anchors.z) + " bottom=" + String::num(anchors.w));
}
```

### File Changes
- `tools/mcp_scene_tools.h/cpp` — Add tool + handler + preset table

---

## 3. `editor/get_open_scripts` — List Open Script Tabs

**Priority:** P2 | **Effort:** Tiny (1 hour)

### Motivation

GDAI has `get_open_scripts` which returns all open tabs with their contents.
We have `editor/focus_script` to open a script but no way to list what's open.

### API Surface

```cpp
// editor/script/script_editor_plugin.h
class ScriptEditor {
    static ScriptEditor *get_singleton();
    Vector<Ref<Script>> get_open_scripts() const;
    Ref<Script> _get_current_script();
};
```

### Implementation

```cpp
// In mcp_editor_nav_tools.cpp — add alongside get_open_scenes:
{
    Dictionary props;
    props["include_contents"] = make_prop("boolean",
        "If true, include the full source code of each script (default: false). "
        "Warning: can be large for many scripts.");
    Array required;
    p_registry->register_tool(
        "editor/get_open_scripts", "Get Open Scripts",
        "List all scripts currently open in the script editor tabs. Returns "
        "file paths, class names, and base classes. Optionally includes source "
        "code. The active (focused) script is marked.",
        make_schema(props, required),
        make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/true),
        callable_mp_static(&MCPEditorNavTools::handle_get_open_scripts));
}
```

### Handler

```cpp
Dictionary MCPEditorNavTools::handle_get_open_scripts(const Dictionary &p_args) {
    ScriptEditor *se = ScriptEditor::get_singleton();
    ERR_FAIL_NULL_V(se, make_tool_error("Script editor not available."));

    bool include_contents = (bool)p_args.get("include_contents", false);

    Vector<Ref<Script>> scripts = se->get_open_scripts();
    Ref<Script> current = se->_get_current_script();

    Array result_scripts;
    String text = "Open scripts (" + itos(scripts.size()) + "):\n";

    for (int i = 0; i < scripts.size(); i++) {
        Ref<Script> s = scripts[i];
        Dictionary entry;
        entry["path"] = s->get_path();
        entry["class_name"] = s->get_global_name();
        entry["base_class"] = s->get_instance_base_type();
        entry["is_active"] = (current.is_valid() && s == current);
        entry["is_tool"] = s->is_tool();

        if (include_contents) {
            entry["source"] = s->get_source_code();
        }

        result_scripts.push_back(entry);

        String marker = (bool)entry["is_active"] ? " [active]" : "";
        text += "  " + String(entry["path"]) + marker + "\n";
    }

    Dictionary structured;
    structured["scripts"] = result_scripts;
    structured["count"] = scripts.size();
    structured["active"] = current.is_valid() ? current->get_path() : "";
    return make_tool_result(text, structured);
}
```

### File Changes
- `tools/mcp_editor_nav_tools.h/cpp` — Add tool + handler
- Include `editor/script/script_editor_plugin.h`

---

## 4. `editor/execute_script` — Run Arbitrary Tool Script in Editor

**Priority:** P1 | **Effort:** Small (2-4 hours)

### Motivation

GDAI has `execute_editor_script`. `runtime/evaluate` runs expressions in the
game process. There's no way to run GDScript in the **editor** context —
e.g., batch-rename resources, automate editor menus, generate files
programmatically using engine APIs.

### API Surface

```cpp
// editor/script/editor_script.h
class EditorScript : public RefCounted {
    void run();  // Calls virtual _run()
    EditorInterface *get_editor_interface();
};
```

### Approach

Two options:
1. **Use EditorScript**: Write GDScript to a temp file, load it, instantiate, call `_run()`
2. **Direct eval**: Use `GDScriptLanguage` to evaluate code snippets in editor context

Option 2 is more flexible for MCP — the LLM sends a code string, we execute it.

### Implementation

```cpp
// New tool in mcp_editor_tools.cpp:
{
    Dictionary props;
    props["code"] = make_prop("string",
        "GDScript code to execute as a @tool script in the editor. "
        "Has access to EditorInterface, the edited scene, and all editor APIs. "
        "The code runs in the editor process, NOT in the running game. "
        "Use runtime/evaluate for game-side evaluation.");
    Array required;
    required.push_back("code");
    p_registry->register_tool(
        "editor/execute_script", "Execute Editor Script",
        "Run a GDScript code snippet in the editor context. The script runs as a "
        "@tool script with full access to EditorInterface, EditorFileSystem, the "
        "scene tree, and all editor APIs. Use this for batch operations, custom "
        "import logic, or anything that needs editor-side access.\n\n"
        "SAFETY: The code runs with full editor permissions. Avoid destructive "
        "operations without confirmation. The expression denylist from "
        "runtime/evaluate does NOT apply here (editor scripts need file access).\n\n"
        "Example: 'var ei = EditorInterface.get_singleton()\\n"
        "print(ei.get_edited_scene_root().get_children())'",
        make_schema(props, required),
        make_annotations(/*readOnly=*/false, /*destructive=*/true, /*idempotent=*/false),
        callable_mp_static(&MCPEditorTools::handle_execute_script));
}
```

### Handler

```cpp
Dictionary MCPEditorTools::handle_execute_script(const Dictionary &p_args) {
    String code = p_args.get("code", "");
    if (code.is_empty()) {
        return make_tool_error("Missing required parameter: code");
    }

    // Wrap the code in an EditorScript class
    String full_script = "@tool\n"
                         "extends EditorScript\n\n"
                         "func _run():\n";

    // Indent user code
    Vector<String> lines = code.split("\n");
    for (int i = 0; i < lines.size(); i++) {
        full_script += "\t" + lines[i] + "\n";
    }

    // Write to temporary file
    String temp_path = "res://addons/mcp_temp/editor_script.gd";
    {
        Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
        da->make_dir_recursive("res://addons/mcp_temp");
    }
    {
        Ref<FileAccess> fa = FileAccess::open(temp_path, FileAccess::WRITE);
        ERR_FAIL_COND_V(fa.is_null(), make_tool_error("Failed to write temp script."));
        fa->store_string(full_script);
    }

    // Load and execute
    Ref<Script> script = ResourceLoader::load(temp_path);
    if (script.is_null()) {
        _cleanup_temp();
        return make_tool_error("Failed to load editor script. Check for syntax errors.");
    }

    // Instantiate EditorScript
    Ref<EditorScript> es;
    es.instantiate();
    es->set_script(script);

    // Capture output (redirect print)
    // TODO: Hook into logger to capture print() output during _run()

    es->run();

    // Cleanup
    _cleanup_temp();

    return make_tool_result("Editor script executed successfully.");
}
```

**NOTE:** Capturing the script's `print()` output requires hooking the logger
or using a custom output handler. Alternative: use `Expression` class for
simpler evaluations that return a value.

### Security Considerations

- `editor/execute_script` is marked `destructive=true`
- It has FULL editor access — can delete files, modify scenes, etc.
- Consider: adding a confirmation prompt or denylist for known-dangerous calls
- The MCP auth token provides the first layer of security

### File Changes
- `tools/mcp_editor_tools.h/cpp` — Add tool + handler
- Include `editor/script/editor_script.h`, `core/io/resource_loader.h`

---

## 5. `editor/get_screenshot` — Editor Window Screenshot

**Priority:** P2 | **Effort:** Small (2-3 hours)

### Motivation

GDAI has `get_editor_screenshot`. The existing `runtime/get_screenshot` captures
the game viewport. But the LLM can't see what the user is looking at in the editor.

### Implementation

```cpp
// The editor is a Godot application. Its main window is accessible.
// We can capture the editor's viewport similar to how game screenshots work.

Dictionary MCPEditorTools::handle_get_editor_screenshot(const Dictionary &p_args) {
    // Get the editor's main window
    DisplayServer *ds = DisplayServer::get_singleton();
    ERR_FAIL_NULL_V(ds, make_tool_error("Display server not available."));

    // Get the main window viewport
    Window *main_window = EditorNode::get_singleton()->get_window();
    ERR_FAIL_NULL_V(main_window, make_tool_error("Editor window not available."));

    Viewport *vp = main_window->get_viewport();
    ERR_FAIL_NULL_V(vp, make_tool_error("Editor viewport not available."));

    // Capture the viewport texture
    Ref<ViewportTexture> tex = vp->get_texture();
    Ref<Image> img = tex->get_image();

    if (img.is_null() || img->is_empty()) {
        return make_tool_error("Failed to capture editor screenshot.");
    }

    // Encode as PNG
    PackedByteArray png_data = img->save_png_to_buffer();
    String base64 = CryptoCore::b64_encode(png_data.ptr(), png_data.size());

    // Build response with image content
    Dictionary content_item;
    content_item["type"] = "image";
    content_item["data"] = base64;
    content_item["mimeType"] = "image/png";
    Array content;
    content.push_back(content_item);

    Dictionary result;
    result["content"] = content;

    Dictionary structured;
    structured["width"] = img->get_width();
    structured["height"] = img->get_height();
    structured["size_bytes"] = png_data.size();
    result["structuredContent"] = structured;

    return result;
}
```

**NOTE:** Viewport capture from the editor thread may require `call_deferred()`
and synchronization. Test whether `get_image()` is safe to call from HTTP thread.
May need the pending request + semaphore pattern used by game screenshots.

### Tool Registration

```cpp
{
    Dictionary props;
    Array required;
    p_registry->register_tool(
        "editor/get_screenshot", "Get Editor Screenshot",
        "Capture the Godot editor window as a PNG image. Shows the current state "
        "of the editor including open panels, scene tree, inspector, and any "
        "visible errors. Use runtime/get_screenshot for the running game viewport.",
        make_schema(props, required),
        make_annotations(/*readOnly=*/true, /*destructive=*/false, /*idempotent=*/false),
        callable_mp_static(&MCPEditorTools::handle_get_editor_screenshot));
}
```

### File Changes
- `tools/mcp_editor_tools.h/cpp` — Add tool + handler

---

## Combined File Changes Summary

| File | Changes |
|------|---------|
| `tools/mcp_debug_tools.h/cpp` | Add `runtime/clear_output` |
| `tools/mcp_scene_tools.h/cpp` | Add `scene/set_anchor_preset` |
| `tools/mcp_editor_nav_tools.h/cpp` | Add `editor/get_open_scripts` |
| `tools/mcp_editor_tools.h/cpp` | Add `editor/execute_script`, `editor/get_screenshot` |
| `mcp_debugger_bridge.h/cpp` | Add `clear()` methods to ring buffers |
| `mcp_protocol.cpp` | No change (tools added to existing modules) |
| `tests/test_small_tools.py` | **NEW** — Tests for all 5 tools |
| `README.md` | Update tool count (+5) |
