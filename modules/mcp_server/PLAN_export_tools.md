# PLAN: Export Tools (Web, iOS, All Platforms)

**Branch:** `mcp-server`
**Priority:** P1
**Effort:** Small-Medium (2-3 days)
**Dependencies:** Export presets configured in project, export templates installed

---

## Motivation

The LLM can write code, run tests, and debug — but can't build the final artifact.
struktured-labs has `godot_export`. We need this to close the loop.

Web export is particularly valuable because it creates a playable build the user
can share instantly. iOS export is more complex due to signing but triggerable.

---

## API Surface (from codebase exploration)

```cpp
// editor/export/editor_export.h
class EditorExport {
    static EditorExport *get_singleton();
    int get_export_preset_count();
    Ref<EditorExportPreset> get_export_preset(int p_idx);
    int get_export_platform_count();
    Ref<EditorExportPlatform> get_export_platform(int p_idx);
};

// editor/export/editor_export_platform.h
class EditorExportPlatform {
    Error export_project(
        const Ref<EditorExportPreset> &p_preset,
        bool p_debug,
        const String &p_path,
        BitField<DebugFlags> p_flags);
};
```

---

## New Tools (3 tools)

### Tool 1: `editor/export/list_presets`

**Purpose:** List all configured export presets and their platforms.

**Parameters:** None

**Implementation:**
```cpp
Dictionary MCPExportTools::handle_list_presets(const Dictionary &p_args) {
    EditorExport *exporter = EditorExport::get_singleton();
    ERR_FAIL_NULL_V(exporter, make_tool_error("Export system not available."));

    Array presets;
    for (int i = 0; i < exporter->get_export_preset_count(); i++) {
        Ref<EditorExportPreset> preset = exporter->get_export_preset(i);
        Dictionary p;
        p["index"] = i;
        p["name"] = preset->get_name();
        p["platform"] = preset->get_platform()->get_name();
        p["runnable"] = preset->is_runnable();
        p["export_path"] = preset->get_export_path();
        p["custom_features"] = preset->get_custom_features();
        presets.push_back(p);
    }

    String text = "Export presets:\n";
    for (int i = 0; i < presets.size(); i++) {
        Dictionary p = presets[i];
        text += "  [" + itos(i) + "] " + String(p["name"]) +
                " (" + String(p["platform"]) + ")" +
                (bool(p["runnable"]) ? " [runnable]" : "") + "\n";
    }
    if (presets.is_empty()) {
        text = "No export presets configured.\n\n"
               "Create presets in Project > Export... before using this tool.\n"
               "Common presets: Web, Linux/X11, Windows Desktop, macOS, iOS, Android.";
    }

    Dictionary structured;
    structured["presets"] = presets;
    structured["count"] = presets.size();
    return make_tool_result(text, structured);
}
```

**Returns:**
```json
{
  "presets": [
    {"index": 0, "name": "Web", "platform": "Web", "runnable": true, "export_path": "build/web/index.html"},
    {"index": 1, "name": "Linux", "platform": "Linux/X11", "runnable": false, "export_path": "build/linux/game.x86_64"},
    {"index": 2, "name": "iOS", "platform": "iOS", "runnable": false, "export_path": "build/ios/game.ipa"}
  ],
  "count": 3
}
```

**LLM description:**
> List all configured export presets and their target platforms. Each preset
> specifies a platform, export path, and configuration. Presets must be configured
> in Project > Export before exporting. Use editor/export/run to execute an export.

**Annotations:** `readOnly=true, destructive=false, idempotent=true`

---

### Tool 2: `editor/export/run`

**Purpose:** Execute an export for a specific preset.

**Parameters:**
- `preset` (string or integer, required): Preset name or index
- `debug` (boolean, optional, default false): Debug or release export
- `output_path` (string, optional): Override the export path

**Implementation:**
```cpp
Dictionary MCPExportTools::handle_run_export(const Dictionary &p_args) {
    EditorExport *exporter = EditorExport::get_singleton();
    ERR_FAIL_NULL_V(exporter, make_tool_error("Export system not available."));

    // Resolve preset by name or index
    Ref<EditorExportPreset> preset;
    Variant preset_id = p_args.get("preset", Variant());

    if (preset_id.get_type() == Variant::INT) {
        int idx = (int)preset_id;
        if (idx >= 0 && idx < exporter->get_export_preset_count()) {
            preset = exporter->get_export_preset(idx);
        }
    } else {
        String name = (String)preset_id;
        for (int i = 0; i < exporter->get_export_preset_count(); i++) {
            if (exporter->get_export_preset(i)->get_name() == name) {
                preset = exporter->get_export_preset(i);
                break;
            }
        }
    }

    if (preset.is_null()) {
        return make_tool_error(
            "Export preset not found: " + String(preset_id) + "\n\n"
            "Use editor/export/list_presets to see available presets.");
    }

    bool debug = (bool)p_args.get("debug", false);
    String export_path = (String)p_args.get("output_path", "");
    if (export_path.is_empty()) {
        export_path = preset->get_export_path();
    }
    if (export_path.is_empty()) {
        return make_tool_error(
            "No export path configured for preset '" + preset->get_name() + "'.\n"
            "Set an output_path parameter or configure the path in Project > Export.");
    }

    // Ensure output directory exists
    String dir = export_path.get_base_dir();
    Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
    if (!da->dir_exists(dir)) {
        da->make_dir_recursive(dir);
    }

    // Execute export (this blocks — should use progress handler)
    Ref<EditorExportPlatform> platform = preset->get_platform();
    Error err = platform->export_project(preset, debug, export_path, 0);

    if (err != OK) {
        return make_tool_error(
            "Export failed with error code " + itos(err) + ".\n\n"
            "Common causes:\n"
            "- Export templates not installed (Editor > Manage Export Templates)\n"
            "- Invalid export path\n"
            "- Missing signing credentials (iOS/Android)\n"
            "- Missing platform SDK");
    }

    String text = "Export completed: " + preset->get_name() + "\n"
                  "Platform: " + platform->get_name() + "\n"
                  "Mode: " + (debug ? "debug" : "release") + "\n"
                  "Output: " + export_path;

    Dictionary structured;
    structured["preset"] = preset->get_name();
    structured["platform"] = platform->get_name();
    structured["debug"] = debug;
    structured["output_path"] = export_path;
    structured["success"] = true;
    return make_tool_result(text, structured);
}
```

**LLM description:**
> Export the project using a configured preset. Specify preset by name or index
> (use editor/export/list_presets to discover them). Exports can be debug or
> release mode. Export templates must be installed first (Editor > Manage Export
> Templates). For Web exports, the output is an HTML5 build. For iOS, ensure
> signing credentials are configured.

**Annotations:** `readOnly=false, destructive=false, idempotent=true`
**Progress handler:** Yes (long-running)

**Note on threading:**
`export_project()` is potentially blocking and slow (minutes for large projects).
This MUST use the progress handler pattern to:
1. Run on a background thread or with deferred callbacks
2. Stream progress via SSE
3. Support cancellation

Investigate whether `export_project()` is thread-safe or must run on main thread
via `call_deferred()`. If main-thread only, we need the pending request + semaphore
pattern.

---

### Tool 3: `editor/export/check_templates`

**Purpose:** Verify export templates are installed for each platform.

**Parameters:** None

**Implementation:**
```cpp
// Iterate all platforms, check if templates exist.
// EditorExportPlatform has methods to check template availability.
// Return structured report of which platforms are ready.
```

**Returns:**
```json
{
  "templates_installed": {
    "Web": true,
    "Linux/X11": true,
    "Windows Desktop": true,
    "macOS": false,
    "iOS": false,
    "Android": false
  },
  "template_version": "4.6.stable",
  "install_path": "/home/user/.local/share/godot/export_templates/4.6.stable/"
}
```

**LLM description:**
> Check which export templates are installed. Export templates are required for
> building. If a template is missing, use Editor > Manage Export Templates to
> download and install it. Returns availability per platform.

**Annotations:** `readOnly=true, destructive=false, idempotent=true`

---

## Web Export Specifics

For web builds, the output is typically:
```
build/web/
  ├── index.html          # Entry point
  ├── index.js            # Godot runtime
  ├── index.wasm          # WebAssembly binary
  ├── index.pck           # Game data
  └── index.audio.worklet.js
```

The LLM could additionally:
1. Serve it locally (outside MCP scope)
2. Deploy to itch.io via butler (CI territory)

---

## iOS Export Specifics

iOS export generates an Xcode project. Key considerations:
- Requires Apple signing identity + provisioning profile
- Preset must have Team ID, Bundle Identifier configured
- Output is typically a `.xcodeproj` folder or `.ipa`
- Full build usually requires `xcodebuild` command after export

The MCP tool triggers the export step. The user/CI handles Xcode signing.

---

## File Changes Summary

| File | Change |
|------|--------|
| `tools/mcp_export_tools.h` | **NEW** — class MCPExportTools |
| `tools/mcp_export_tools.cpp` | **NEW** — 3 tools + handlers |
| `mcp_protocol.cpp` | Add `MCPExportTools::register_tools(&tool_registry);` |
| `tests/test_export_tools.py` | **NEW** — export tool tests |
| `README.md` | Update tool count, add export section |
