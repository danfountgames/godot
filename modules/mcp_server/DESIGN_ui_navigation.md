# Design: UI Navigation & Interaction Tools

## Status

**Draft** -- Proposed for implementation in the MCP server module.

## Overview

This document describes a set of MCP tools that allow an LLM client to discover, inspect, and interact with Godot UI controls in a running game. The LLM operates like an accessibility agent: it perceives the UI through the scene tree and control metadata, then manipulates controls by targeting them via node path.

### Why not just use `debug/evaluate`?

The existing `debug/evaluate` tool can technically set properties on any node. However:

1. **Expression syntax is fragile.** The LLM must construct GDScript expressions that correctly chain method calls, handle quoting, and deal with type coercion. A dedicated tool with typed parameters is far less error-prone.
2. **No validation.** `debug/evaluate` cannot check whether a control is visible, enabled, or even the right type before modifying it. Dedicated tools can validate preconditions and return meaningful errors.
3. **No state reporting.** After setting a slider value via expression, the LLM gets back `"Nil: null"`. Dedicated tools return the new state of the control, confirming the interaction succeeded.
4. **Focus management.** Many controls require focus before they accept input. Dedicated tools handle focus automatically.
5. **Security.** Expression evaluation is a broad attack surface. Typed tools constrain the LLM to well-defined operations.

### Design principles

- **Discover, then interact.** The LLM uses `debug/search_scene_tree` (already implemented) to find controls, then uses these new tools to interact with them. The two systems compose naturally.
- **Node path targeting.** Every tool takes a `node_path` parameter. The LLM never needs to construct expressions or guess coordinates.
- **Precondition validation.** Every tool validates that the target node exists, is the correct type, is visible, and is not disabled before attempting the interaction. Errors include actionable guidance.
- **State echoing.** Every tool returns the post-interaction state of the control, so the LLM can verify the effect without a separate property read.
- **Minimal new message types.** Rather than adding one debugger message per tool, the design uses a single generic `mcp:ui_interact` message with a sub-action field. This keeps the game-side handler compact and extensible.

---

## Architecture

### Communication flow

```
LLM Client                MCP Server (Editor)           Running Game
    |                           |                            |
    |-- tools/call ------------>|                            |
    |   "debug/ui_interact"     |                            |
    |   {node_path, action, ..} |                            |
    |                           |-- validate params -------->|
    |                           |-- bridge.send_ui_interact  |
    |                           |   "mcp:ui_interact"        |
    |                           |   [node_path, action, ...] |
    |                           |        (blocks on semaphore)
    |                           |                            |-- find node
    |                           |                            |-- validate type
    |                           |                            |-- validate visible/enabled
    |                           |                            |-- perform action
    |                           |                            |-- read post-state
    |                           |                            |
    |                           |<-- "mcp:ui_interact_result"|
    |                           |   [success, data_dict]     |
    |                           |        (semaphore posted)  |
    |<-- tool result -----------|                            |
    |   {state after interact}  |                            |
```

### Layering

The system is split into three layers, matching the existing architecture:

| Layer | File(s) | Responsibility |
|-------|---------|---------------|
| **MCP Tool Handlers** | `tools/mcp_ui_tools.cpp`, `tools/mcp_ui_tools.h` | Parameter validation, schema definitions, result formatting. One static handler per MCP tool name. |
| **Debugger Bridge** | `mcp_debugger_bridge.cpp`, `mcp_debugger_bridge.h` | Async request/response. New method: `send_ui_interact()`. New capture handler for `mcp:ui_interact_result`. |
| **Game-Side Handler** | `scene/debugger/scene_debugger.cpp` | Node lookup, type checking, precondition validation, action execution, state readback. Single `_mcp_ui_interact()` function with action dispatch. |

---

## Tool Design

### Naming convention

All tools in this category are under the `debug/` prefix (they require a running game):

- `debug/ui_get_control_info` -- Read-only inspection of any Control
- `debug/ui_set_text` -- TextEdit / LineEdit text manipulation
- `debug/ui_get_text` -- TextEdit / LineEdit text reading
- `debug/ui_set_range_value` -- HSlider / VSlider / SpinBox / ProgressBar value setting
- `debug/ui_get_range_value` -- HSlider / VSlider / SpinBox / ProgressBar value reading
- `debug/ui_select_option` -- OptionButton / PopupMenu item selection
- `debug/ui_get_options` -- OptionButton / PopupMenu item listing
- `debug/ui_set_tab` -- TabContainer / TabBar tab switching
- `debug/ui_get_tabs` -- TabContainer / TabBar tab listing
- `debug/ui_set_checked` -- CheckBox / CheckButton state setting
- `debug/ui_tree_select` -- Tree control item selection
- `debug/ui_tree_get_items` -- Tree control item listing
- `debug/ui_itemlist_select` -- ItemList item selection
- `debug/ui_itemlist_get_items` -- ItemList item listing
- `debug/ui_scroll_to` -- ScrollContainer scrolling
- `debug/ui_focus` -- Focus management (focus/unfocus a control)

### Why separate tools instead of one mega-tool?

MCP tool schemas serve as the LLM's "API documentation." Separate tools with tailored schemas make it obvious what parameters are available for each control type. A single `debug/ui_interact` tool with a union schema would be harder for the LLM to use correctly.

However, the **game-side** implementation uses a single message type (`mcp:ui_interact`) with an action discriminator to avoid message-type proliferation in the debugger protocol. The MCP tool handlers map their typed parameters into this generic format.

---

## Tool Schemas and Payloads

### 1. `debug/ui_get_control_info`

**Purpose:** Read accessibility-like metadata from any Control node. This is the LLM's primary way to "see" a control's current state before interacting with it.

**Schema:**

```json
{
  "type": "object",
  "properties": {
    "node_path": {
      "type": "string",
      "description": "Path to the Control node (e.g., '/root/Main/UI/StartButton')"
    }
  },
  "required": ["node_path"]
}
```

**Annotations:** `readOnly: true, destructive: false, idempotent: true`

**Example request:**

```json
{
  "method": "tools/call",
  "params": {
    "name": "debug/ui_get_control_info",
    "arguments": {
      "node_path": "/root/Main/UI/LoginForm/UsernameField"
    }
  }
}
```

**Example response:**

```json
{
  "content": [{
    "type": "text",
    "text": "Control: /root/Main/UI/LoginForm/UsernameField\nType: LineEdit\nVisible: true\nEnabled: true\nFocused: false\nRect: (120, 200, 300, 40)\nText: \"\"\nPlaceholder: \"Enter username\"\nMax Length: 64\nEditable: true"
  }],
  "structuredContent": {
    "node_path": "/root/Main/UI/LoginForm/UsernameField",
    "class": "LineEdit",
    "visible": true,
    "visible_in_tree": true,
    "enabled": true,
    "focused": false,
    "rect": {"x": 120, "y": 200, "width": 300, "height": 40},
    "control_data": {
      "text": "",
      "placeholder": "Enter username",
      "max_length": 64,
      "editable": true,
      "secret": false,
      "caret_column": 0
    }
  }
}
```

**Game-side `control_data` varies by type:**

| Control Type | Fields in `control_data` |
|---|---|
| `LineEdit` | `text`, `placeholder`, `max_length`, `editable`, `secret`, `caret_column` |
| `TextEdit` | `text`, `line_count`, `editable`, `caret_line`, `caret_column`, `has_selection`, `selected_text` |
| `Button` / `BaseButton` | `text`, `pressed`, `toggle_mode`, `disabled`, `icon_name` |
| `CheckBox` / `CheckButton` | (inherits BaseButton) + `checked` (alias for `pressed` in toggle_mode) |
| `HSlider` / `VSlider` / `Range` | `value`, `min_value`, `max_value`, `step`, `ratio` (0.0-1.0), `editable` |
| `SpinBox` | (inherits Range) + `prefix`, `suffix` |
| `OptionButton` | `selected_index`, `selected_text`, `item_count` |
| `TabContainer` | `current_tab`, `tab_count`, `tab_names` |
| `TabBar` | `current_tab`, `tab_count`, `tab_names` |
| `Tree` | `selected_item_text`, `column_count`, `column_titles`, `root_item_count` |
| `ItemList` | `item_count`, `selected_indices`, `max_columns`, `select_mode` |
| `ScrollContainer` | `scroll_h`, `scroll_v`, `scroll_h_max`, `scroll_v_max` |
| `ProgressBar` | `value`, `min_value`, `max_value`, `ratio` |
| `Label` | `text`, `visible_characters`, `autowrap_mode` |
| Other `Control` | (base fields only -- rect, visible, enabled, focused) |

---

### 2. `debug/ui_set_text`

**Purpose:** Set, clear, append, or insert text in a TextEdit or LineEdit. Optionally focus the control first.

**Schema:**

```json
{
  "type": "object",
  "properties": {
    "node_path": {
      "type": "string",
      "description": "Path to a TextEdit or LineEdit node"
    },
    "text": {
      "type": "string",
      "description": "The text to set, insert, or append"
    },
    "mode": {
      "type": "string",
      "description": "How to apply the text: 'replace' (default, replaces all text), 'append' (adds to end), 'insert' (at caret position), 'clear' (ignores text param)"
    },
    "focus": {
      "type": "boolean",
      "description": "Whether to focus the control before modifying (default: true)"
    }
  },
  "required": ["node_path"]
}
```

**Annotations:** `readOnly: false, destructive: false, idempotent: false`

**Example -- replace text in a LineEdit:**

```json
{
  "arguments": {
    "node_path": "/root/Main/UI/LoginForm/UsernameField",
    "text": "player1",
    "mode": "replace"
  }
}
```

**Response:**

```json
{
  "structuredContent": {
    "success": true,
    "node_path": "/root/Main/UI/LoginForm/UsernameField",
    "class": "LineEdit",
    "mode": "replace",
    "previous_text": "",
    "current_text": "player1",
    "focused": true
  }
}
```

**Example -- clear text:**

```json
{
  "arguments": {
    "node_path": "/root/Main/UI/ChatBox/InputField",
    "mode": "clear"
  }
}
```

**Example -- append to TextEdit:**

```json
{
  "arguments": {
    "node_path": "/root/Main/UI/NoteEditor",
    "text": "\nNew line appended",
    "mode": "append"
  }
}
```

---

### 3. `debug/ui_get_text`

**Purpose:** Read the current text content from a TextEdit or LineEdit. Optionally read only the selected text.

**Schema:**

```json
{
  "type": "object",
  "properties": {
    "node_path": {
      "type": "string",
      "description": "Path to a TextEdit or LineEdit node"
    },
    "selection_only": {
      "type": "boolean",
      "description": "If true, return only the currently selected text (default: false)"
    }
  },
  "required": ["node_path"]
}
```

**Annotations:** `readOnly: true, destructive: false, idempotent: true`

**Response:**

```json
{
  "structuredContent": {
    "node_path": "/root/Main/UI/NoteEditor",
    "class": "TextEdit",
    "text": "Hello world\nSecond line",
    "line_count": 2,
    "caret_line": 1,
    "caret_column": 11,
    "has_selection": false,
    "selected_text": ""
  }
}
```

---

### 4. `debug/ui_set_range_value`

**Purpose:** Set the value of any Range-based control (HSlider, VSlider, SpinBox, ProgressBar, ScrollBar).

**Schema:**

```json
{
  "type": "object",
  "properties": {
    "node_path": {
      "type": "string",
      "description": "Path to a Range-based node (HSlider, VSlider, SpinBox, ProgressBar, ScrollBar)"
    },
    "value": {
      "type": "number",
      "description": "The absolute value to set. Mutually exclusive with 'ratio'."
    },
    "ratio": {
      "type": "number",
      "description": "Value as a 0.0-1.0 ratio of the range. Mutually exclusive with 'value'. 0.0 = min, 1.0 = max."
    },
    "delta": {
      "type": "number",
      "description": "Relative change. Added to current value. Mutually exclusive with 'value' and 'ratio'."
    }
  },
  "required": ["node_path"]
}
```

**Annotations:** `readOnly: false, destructive: false, idempotent: false`

**Example -- set slider to 75%:**

```json
{
  "arguments": {
    "node_path": "/root/Main/UI/Settings/VolumeSlider",
    "ratio": 0.75
  }
}
```

**Response:**

```json
{
  "structuredContent": {
    "success": true,
    "node_path": "/root/Main/UI/Settings/VolumeSlider",
    "class": "HSlider",
    "previous_value": 50.0,
    "current_value": 75.0,
    "min_value": 0.0,
    "max_value": 100.0,
    "step": 1.0,
    "ratio": 0.75
  }
}
```

**Example -- increment SpinBox by one step:**

```json
{
  "arguments": {
    "node_path": "/root/Main/UI/Settings/FontSizeSpinBox",
    "delta": 1.0
  }
}
```

---

### 5. `debug/ui_get_range_value`

**Purpose:** Read the current value and range of any Range-based control.

**Schema:**

```json
{
  "type": "object",
  "properties": {
    "node_path": {
      "type": "string",
      "description": "Path to a Range-based node"
    }
  },
  "required": ["node_path"]
}
```

**Annotations:** `readOnly: true, destructive: false, idempotent: true`

**Response:**

```json
{
  "structuredContent": {
    "node_path": "/root/Main/UI/Settings/VolumeSlider",
    "class": "HSlider",
    "value": 75.0,
    "min_value": 0.0,
    "max_value": 100.0,
    "step": 1.0,
    "ratio": 0.75,
    "editable": true
  }
}
```

---

### 6. `debug/ui_select_option`

**Purpose:** Select an item in an OptionButton (dropdown). Can target by index or by text match.

**Schema:**

```json
{
  "type": "object",
  "properties": {
    "node_path": {
      "type": "string",
      "description": "Path to an OptionButton node"
    },
    "index": {
      "type": "integer",
      "description": "Item index to select (0-based). Mutually exclusive with 'text'."
    },
    "text": {
      "type": "string",
      "description": "Item text to match (case-insensitive, first match wins). Mutually exclusive with 'index'."
    }
  },
  "required": ["node_path"]
}
```

**Annotations:** `readOnly: false, destructive: false, idempotent: true`

**Example -- select by text:**

```json
{
  "arguments": {
    "node_path": "/root/Main/UI/Settings/LanguageDropdown",
    "text": "English"
  }
}
```

**Response:**

```json
{
  "structuredContent": {
    "success": true,
    "node_path": "/root/Main/UI/Settings/LanguageDropdown",
    "class": "OptionButton",
    "previous_index": 0,
    "previous_text": "French",
    "selected_index": 2,
    "selected_text": "English",
    "item_count": 5
  }
}
```

---

### 7. `debug/ui_get_options`

**Purpose:** List all items in an OptionButton, including which is currently selected.

**Schema:**

```json
{
  "type": "object",
  "properties": {
    "node_path": {
      "type": "string",
      "description": "Path to an OptionButton node"
    }
  },
  "required": ["node_path"]
}
```

**Annotations:** `readOnly: true, destructive: false, idempotent: true`

**Response:**

```json
{
  "structuredContent": {
    "node_path": "/root/Main/UI/Settings/LanguageDropdown",
    "class": "OptionButton",
    "selected_index": 2,
    "selected_text": "English",
    "items": [
      {"index": 0, "text": "French", "disabled": false, "separator": false},
      {"index": 1, "text": "German", "disabled": false, "separator": false},
      {"index": 2, "text": "English", "disabled": false, "separator": false},
      {"index": 3, "text": "Spanish", "disabled": false, "separator": false},
      {"index": 4, "text": "Japanese", "disabled": true, "separator": false}
    ]
  }
}
```

---

### 8. `debug/ui_set_tab`

**Purpose:** Switch the active tab on a TabContainer or TabBar.

**Schema:**

```json
{
  "type": "object",
  "properties": {
    "node_path": {
      "type": "string",
      "description": "Path to a TabContainer or TabBar node"
    },
    "index": {
      "type": "integer",
      "description": "Tab index to activate (0-based). Mutually exclusive with 'title'."
    },
    "title": {
      "type": "string",
      "description": "Tab title to match (case-insensitive, first match wins). Mutually exclusive with 'index'."
    }
  },
  "required": ["node_path"]
}
```

**Annotations:** `readOnly: false, destructive: false, idempotent: true`

**Example:**

```json
{
  "arguments": {
    "node_path": "/root/Main/UI/Settings/SettingsTabs",
    "title": "Audio"
  }
}
```

**Response:**

```json
{
  "structuredContent": {
    "success": true,
    "node_path": "/root/Main/UI/Settings/SettingsTabs",
    "class": "TabContainer",
    "previous_tab": 0,
    "current_tab": 2,
    "current_tab_title": "Audio",
    "tab_count": 4,
    "tab_titles": ["General", "Video", "Audio", "Controls"]
  }
}
```

---

### 9. `debug/ui_get_tabs`

**Purpose:** List all tabs and the current selection.

**Schema:**

```json
{
  "type": "object",
  "properties": {
    "node_path": {
      "type": "string",
      "description": "Path to a TabContainer or TabBar node"
    }
  },
  "required": ["node_path"]
}
```

**Annotations:** `readOnly: true, destructive: false, idempotent: true`

**Response:**

```json
{
  "structuredContent": {
    "node_path": "/root/Main/UI/Settings/SettingsTabs",
    "class": "TabContainer",
    "current_tab": 2,
    "tab_count": 4,
    "tabs": [
      {"index": 0, "title": "General", "disabled": false, "hidden": false},
      {"index": 1, "title": "Video", "disabled": false, "hidden": false},
      {"index": 2, "title": "Audio", "disabled": false, "hidden": false},
      {"index": 3, "title": "Controls", "disabled": true, "hidden": false}
    ]
  }
}
```

---

### 10. `debug/ui_set_checked`

**Purpose:** Set or toggle the checked state of a CheckBox or CheckButton.

**Schema:**

```json
{
  "type": "object",
  "properties": {
    "node_path": {
      "type": "string",
      "description": "Path to a CheckBox or CheckButton node"
    },
    "checked": {
      "type": "boolean",
      "description": "Desired checked state. Omit to toggle."
    }
  },
  "required": ["node_path"]
}
```

**Annotations:** `readOnly: false, destructive: false, idempotent: false`

**Example -- toggle:**

```json
{
  "arguments": {
    "node_path": "/root/Main/UI/Settings/FullscreenCheck"
  }
}
```

**Response:**

```json
{
  "structuredContent": {
    "success": true,
    "node_path": "/root/Main/UI/Settings/FullscreenCheck",
    "class": "CheckBox",
    "previous_checked": false,
    "current_checked": true,
    "text": "Fullscreen"
  }
}
```

---

### 11. `debug/ui_tree_select`

**Purpose:** Select an item in a Godot Tree control by path or index, and optionally expand/collapse.

**Schema:**

```json
{
  "type": "object",
  "properties": {
    "node_path": {
      "type": "string",
      "description": "Path to a Tree node"
    },
    "item_path": {
      "type": "string",
      "description": "Slash-separated path of item text labels from root to target (e.g., 'Weapons/Swords/Excalibur'). Uses first matching child at each level."
    },
    "item_index_path": {
      "type": "array",
      "items": {"type": "integer"},
      "description": "Path of child indices from root to target (e.g., [0, 2, 1] = root's child 0, then its child 2, then that child's child 1). Alternative to item_path."
    },
    "column": {
      "type": "integer",
      "description": "Column index to select (default: 0)"
    },
    "expand": {
      "type": "boolean",
      "description": "If true, expand (uncollapse) the selected item. If false, collapse it. Omit to leave unchanged."
    },
    "scroll_to": {
      "type": "boolean",
      "description": "Whether to scroll the tree to make the selected item visible (default: true)"
    }
  },
  "required": ["node_path"]
}
```

**Annotations:** `readOnly: false, destructive: false, idempotent: true`

**Example:**

```json
{
  "arguments": {
    "node_path": "/root/Main/UI/Inventory/ItemTree",
    "item_path": "Weapons/Swords/Excalibur",
    "expand": true
  }
}
```

**Response:**

```json
{
  "structuredContent": {
    "success": true,
    "node_path": "/root/Main/UI/Inventory/ItemTree",
    "class": "Tree",
    "selected_item_text": "Excalibur",
    "selected_item_path": "Weapons/Swords/Excalibur",
    "selected_column": 0,
    "is_collapsed": false,
    "child_count": 0
  }
}
```

---

### 12. `debug/ui_tree_get_items`

**Purpose:** List items in a Tree control. Returns a hierarchical structure up to a maximum depth.

**Schema:**

```json
{
  "type": "object",
  "properties": {
    "node_path": {
      "type": "string",
      "description": "Path to a Tree node"
    },
    "max_depth": {
      "type": "integer",
      "description": "Maximum depth to traverse (default: 3). Use -1 for unlimited."
    },
    "root_path": {
      "type": "string",
      "description": "Slash-separated path to start listing from (default: '' = tree root)"
    }
  },
  "required": ["node_path"]
}
```

**Annotations:** `readOnly: true, destructive: false, idempotent: true`

**Response:**

```json
{
  "structuredContent": {
    "node_path": "/root/Main/UI/Inventory/ItemTree",
    "class": "Tree",
    "column_count": 2,
    "column_titles": ["Name", "Qty"],
    "selected_item_text": "Excalibur",
    "items": {
      "text": ["Inventory", ""],
      "collapsed": false,
      "children": [
        {
          "text": ["Weapons", ""],
          "collapsed": false,
          "selectable": true,
          "children": [
            {
              "text": ["Swords", ""],
              "collapsed": false,
              "children": [
                {"text": ["Excalibur", "1"], "collapsed": false, "children": [], "selected": true},
                {"text": ["Iron Sword", "3"], "collapsed": false, "children": []}
              ]
            }
          ]
        },
        {
          "text": ["Potions", ""],
          "collapsed": true,
          "children_count": 5,
          "children": []
        }
      ]
    }
  }
}
```

When an item is collapsed and `max_depth` has not been reached, `children_count` is included to indicate there are hidden children. When `max_depth` is reached, children are omitted and `children_count` is included.

---

### 13. `debug/ui_itemlist_select`

**Purpose:** Select one or more items in an ItemList by index or text.

**Schema:**

```json
{
  "type": "object",
  "properties": {
    "node_path": {
      "type": "string",
      "description": "Path to an ItemList node"
    },
    "index": {
      "type": "integer",
      "description": "Item index to select (0-based). Mutually exclusive with 'text'."
    },
    "text": {
      "type": "string",
      "description": "Item text to match (case-insensitive, first match). Mutually exclusive with 'index'."
    },
    "scroll_to": {
      "type": "boolean",
      "description": "Whether to scroll to make the selected item visible (default: true)"
    }
  },
  "required": ["node_path"]
}
```

**Annotations:** `readOnly: false, destructive: false, idempotent: true`

**Response:**

```json
{
  "structuredContent": {
    "success": true,
    "node_path": "/root/Main/UI/FileList",
    "class": "ItemList",
    "selected_index": 3,
    "selected_text": "level_02.tscn",
    "item_count": 12
  }
}
```

---

### 14. `debug/ui_itemlist_get_items`

**Purpose:** List all items in an ItemList.

**Schema:**

```json
{
  "type": "object",
  "properties": {
    "node_path": {
      "type": "string",
      "description": "Path to an ItemList node"
    },
    "offset": {
      "type": "integer",
      "description": "Start index for pagination (default: 0)"
    },
    "limit": {
      "type": "integer",
      "description": "Maximum items to return (default: 100, max: 500)"
    }
  },
  "required": ["node_path"]
}
```

**Annotations:** `readOnly: true, destructive: false, idempotent: true`

**Response:**

```json
{
  "structuredContent": {
    "node_path": "/root/Main/UI/FileList",
    "class": "ItemList",
    "item_count": 12,
    "selected_indices": [3],
    "items": [
      {"index": 0, "text": "main.tscn", "selectable": true, "disabled": false},
      {"index": 1, "text": "player.tscn", "selectable": true, "disabled": false}
    ],
    "has_more": true
  }
}
```

---

### 15. `debug/ui_scroll_to`

**Purpose:** Scroll a ScrollContainer to a specific position or to bring a child node into view.

**Schema:**

```json
{
  "type": "object",
  "properties": {
    "node_path": {
      "type": "string",
      "description": "Path to a ScrollContainer node"
    },
    "h": {
      "type": "integer",
      "description": "Horizontal scroll position in pixels. Omit to leave unchanged."
    },
    "v": {
      "type": "integer",
      "description": "Vertical scroll position in pixels. Omit to leave unchanged."
    },
    "child_path": {
      "type": "string",
      "description": "Path to a child Control to scroll into view. Mutually exclusive with h/v. Path is relative to the ScrollContainer."
    }
  },
  "required": ["node_path"]
}
```

**Annotations:** `readOnly: false, destructive: false, idempotent: true`

**Example -- scroll to child:**

```json
{
  "arguments": {
    "node_path": "/root/Main/UI/SettingsScroll",
    "child_path": "AudioSection"
  }
}
```

**Response:**

```json
{
  "structuredContent": {
    "success": true,
    "node_path": "/root/Main/UI/SettingsScroll",
    "class": "ScrollContainer",
    "scroll_h": 0,
    "scroll_v": 420,
    "scroll_h_max": 0,
    "scroll_v_max": 1200
  }
}
```

---

### 16. `debug/ui_focus`

**Purpose:** Explicitly focus or unfocus a Control. Useful before keyboard input or to verify focus state.

**Schema:**

```json
{
  "type": "object",
  "properties": {
    "node_path": {
      "type": "string",
      "description": "Path to a Control node"
    },
    "action": {
      "type": "string",
      "description": "'grab' (default) to grab focus, 'release' to release focus"
    }
  },
  "required": ["node_path"]
}
```

**Annotations:** `readOnly: false, destructive: false, idempotent: true`

**Response:**

```json
{
  "structuredContent": {
    "success": true,
    "node_path": "/root/Main/UI/LoginForm/UsernameField",
    "class": "LineEdit",
    "focused": true,
    "focus_mode": "all"
  }
}
```

---

## Control Targeting

### Primary: Node path

All tools accept a `node_path` parameter. This is the scene tree path as returned by `debug/get_scene_tree` and `debug/search_scene_tree`. Paths are absolute from the root viewport, e.g., `/root/Main/UI/StartButton`.

### Discovery workflow

The expected LLM workflow is:

1. **Browse:** `debug/get_scene_tree` with `max_depth: 3` to see the scene structure.
2. **Search:** `debug/search_scene_tree` with `type: "Button"` (or `name_pattern: "*Login*"`) to find specific controls.
3. **Inspect:** `debug/ui_get_control_info` to read the control's current state.
4. **Interact:** `debug/ui_set_text`, `debug/ui_set_range_value`, etc.
5. **Verify:** The interaction response includes post-state, or use `debug/ui_get_control_info` again, or take a `debug/get_screenshot`.

### Path validation

Same character validation as existing tools (`handle_click_control`, `handle_get_node_properties`): only alphanumeric, underscore, forward slash, period, at-sign, colon, hyphen, and space characters are permitted. This prevents expression injection since the path is embedded in game-side code.

---

## Game-Side Implementation

### Single message type: `mcp:ui_interact`

**Data format:** `[action: String, node_path: String, params: Dictionary]`

- `action` -- one of: `"get_info"`, `"set_text"`, `"get_text"`, `"set_range"`, `"get_range"`, `"select_option"`, `"get_options"`, `"set_tab"`, `"get_tabs"`, `"set_checked"`, `"tree_select"`, `"tree_get_items"`, `"itemlist_select"`, `"itemlist_get_items"`, `"scroll_to"`, `"focus"`
- `node_path` -- absolute scene tree path
- `params` -- action-specific parameters as a Dictionary

**Response message:** `mcp:ui_interact_result`

**Data format:** `[success: bool, result: Dictionary]`

The `result` Dictionary contains the action-specific response data.

### Game-side handler pseudocode

```cpp
// In scene_debugger.cpp, inside _mcp_capture():

if (p_msg == "ui_interact") {
    ERR_FAIL_COND_V(p_data.size() < 3, ERR_INVALID_DATA);

    String action = p_data[0];
    String node_path_str = p_data[1];
    Dictionary params = p_data[2];

    // 1. Find the node.
    Node *node = scene_tree->get_root()->get_node_or_null(NodePath(node_path_str));
    if (!node) {
        _send_ui_result(false, {{"error", "Node not found: " + node_path_str}});
        return OK;
    }

    // 2. Cast to Control (all UI interactions require Control).
    Control *control = Object::cast_to<Control>(node);
    if (!control) {
        _send_ui_result(false, {{"error", "Node is not a Control: " + node_path_str}});
        return OK;
    }

    // 3. Build base info (shared by all actions).
    Dictionary base;
    base["class"] = node->get_class();
    base["visible"] = control->is_visible();
    base["visible_in_tree"] = control->is_visible_in_tree();
    base["enabled"] = !control->is_read_only_or_disabled(); // custom helper
    base["focused"] = control->has_focus();
    Rect2 rect = control->get_global_rect();
    Dictionary rect_dict;
    rect_dict["x"] = rect.position.x;
    rect_dict["y"] = rect.position.y;
    rect_dict["width"] = rect.size.x;
    rect_dict["height"] = rect.size.y;
    base["rect"] = rect_dict;

    // 4. Dispatch by action.
    if (action == "get_info") {
        _handle_ui_get_info(control, base);
    } else if (action == "set_text") {
        _handle_ui_set_text(control, params, base);
    }
    // ... etc for each action ...
    else {
        _send_ui_result(false, {{"error", "Unknown UI action: " + action}});
    }

    return OK;
}
```

### Precondition checking

Each write action checks before proceeding:

```cpp
// Visibility check (warn but allow -- the LLM may be interacting
// with an off-screen control intentionally).
if (!control->is_visible_in_tree()) {
    result["warning"] = "Control is not visible in tree";
}

// Disabled check (hard error -- disabled controls should not be modified).
if (control->is_read_only()) {
    _send_ui_result(false, {{"error", "Control is read-only/disabled"}});
    return;
}
```

### Example: `_handle_ui_set_text`

```cpp
void _handle_ui_set_text(Control *p_control, const Dictionary &p_params, Dictionary &p_base) {
    String mode = p_params.get("mode", "replace");
    String text = p_params.get("text", "");
    bool do_focus = p_params.get("focus", true);

    Dictionary result = p_base;

    LineEdit *line_edit = Object::cast_to<LineEdit>(p_control);
    TextEdit *text_edit = Object::cast_to<TextEdit>(p_control);

    if (!line_edit && !text_edit) {
        _send_ui_result(false, {{"error", "Node is not a LineEdit or TextEdit"}});
        return;
    }

    if (line_edit) {
        result["previous_text"] = line_edit->get_text();

        if (do_focus) {
            line_edit->grab_focus();
        }

        if (mode == "replace") {
            line_edit->set_text(text);
            line_edit->emit_signal("text_changed", text);
        } else if (mode == "clear") {
            line_edit->set_text("");
            line_edit->emit_signal("text_changed", "");
        } else if (mode == "append") {
            String current = line_edit->get_text();
            line_edit->set_text(current + text);
            line_edit->emit_signal("text_changed", current + text);
        } else if (mode == "insert") {
            int caret = line_edit->get_caret_column();
            String current = line_edit->get_text();
            String new_text = current.insert(caret, text);
            line_edit->set_text(new_text);
            line_edit->set_caret_column(caret + text.length());
            line_edit->emit_signal("text_changed", new_text);
        }

        result["current_text"] = line_edit->get_text();
        result["caret_column"] = line_edit->get_caret_column();
        result["focused"] = line_edit->has_focus();
    }

    // Similar for TextEdit...

    result["mode"] = mode;
    result["success"] = true;
    _send_ui_result(true, result);
}
```

**Signal emission note:** When programmatically setting text, Godot does not always emit the `text_changed` signal. The game-side handler explicitly emits it so that game scripts respond as if the user had typed. This is critical -- without the signal, the game's validation logic, auto-complete, or other listeners would not fire.

---

## Focus Management Strategy

### Automatic focus

Write tools (`set_text`, `set_range_value`, `select_option`, `set_checked`) automatically call `grab_focus()` on the target control before the interaction, unless the LLM passes `focus: false`. This matches how a user would click on a field before typing.

### Explicit focus

The `debug/ui_focus` tool allows the LLM to explicitly manage focus. This is useful for:

- Verifying which control has focus before sending keyboard input via `debug/send_input`.
- Tabbing through a form (use `debug/send_input` with `ui_focus_next` / `ui_focus_prev`).
- Releasing focus before taking a screenshot (to avoid cursor blink artifacts).

### Focus mode validation

If a control's `focus_mode` is `FOCUS_NONE`, `grab_focus()` is a no-op. The tool detects this and warns:

```json
{
  "structuredContent": {
    "success": true,
    "warning": "Control has focus_mode=NONE; focus was not changed",
    "focused": false,
    "focus_mode": "none"
  }
}
```

---

## Integration with Scene Tree Browser

The UI tools are designed to compose with the existing scene tree inspection tools:

### Discover controls by type

```json
{"name": "debug/search_scene_tree", "arguments": {"type": "LineEdit"}}
```

Returns all LineEdit nodes with their paths. The LLM picks the one it needs.

### Discover controls by name

```json
{"name": "debug/search_scene_tree", "arguments": {"name_pattern": "*password*"}}
```

### Inspect before interacting

```json
{"name": "debug/ui_get_control_info", "arguments": {"node_path": "/root/Main/UI/LoginForm/PasswordField"}}
```

Returns whether it is editable, visible, what placeholder text says, etc.

### Full login form example workflow

```
1. debug/search_scene_tree { "type": "LineEdit" }
   -> finds /root/Main/Login/UsernameField, /root/Main/Login/PasswordField

2. debug/ui_get_control_info { "node_path": "/root/Main/Login/UsernameField" }
   -> editable: true, placeholder: "Username", text: ""

3. debug/ui_set_text { "node_path": "/root/Main/Login/UsernameField", "text": "testuser" }
   -> current_text: "testuser"

4. debug/ui_set_text { "node_path": "/root/Main/Login/PasswordField", "text": "password123" }
   -> current_text: "password123"

5. debug/search_scene_tree { "name_pattern": "*Login*", "type": "Button" }
   -> finds /root/Main/Login/LoginButton

6. debug/click_control { "node_path": "/root/Main/Login/LoginButton" }
   -> Button pressed

7. debug/wait_frames { "frames": 30 }
   -> Waited 30 frames

8. debug/get_screenshot {}
   -> Shows the post-login screen
```

---

## Edge Cases and Error Handling

### Node not found

```json
{
  "isError": true,
  "content": [{
    "type": "text",
    "text": "Node not found: /root/Main/UI/OldButton\n\nThe node may have been removed from the scene. Use debug/search_scene_tree to find the correct path."
  }]
}
```

### Wrong control type

```json
{
  "isError": true,
  "content": [{
    "type": "text",
    "text": "Expected LineEdit or TextEdit but found Button at /root/Main/UI/SubmitButton\n\nUse debug/ui_get_control_info to check the control type, or debug/click_control to click a Button."
  }]
}
```

### Control is hidden

For read-only tools, a warning is included but the data is still returned:

```json
{
  "structuredContent": {
    "warning": "Control is not visible in tree",
    "visible_in_tree": false,
    "text": "hidden content"
  }
}
```

For write tools, a warning is included but the operation proceeds. The LLM is told the control is hidden so it can decide whether to continue.

### Control is disabled

Write operations on disabled controls fail with an error:

```json
{
  "isError": true,
  "content": [{
    "type": "text",
    "text": "Control is disabled: /root/Main/UI/Settings/LockedSlider\n\nThe control's 'editable' property is false. It cannot be modified."
  }]
}
```

### Control in a popup (not in tree)

A common pattern: OptionButton's popup menu exists but is not visible. The `debug/ui_select_option` tool operates on the OptionButton itself (not its popup), so this is transparent. The tool calls `OptionButton::select()` directly rather than trying to simulate clicking the popup.

### Index out of range

```json
{
  "isError": true,
  "content": [{
    "type": "text",
    "text": "Index 5 is out of range for OptionButton with 3 items (valid: 0-2) at /root/Main/UI/Settings/LanguageDropdown"
  }]
}
```

### Text match not found

```json
{
  "isError": true,
  "content": [{
    "type": "text",
    "text": "No item with text matching 'Klingon' found in OptionButton at /root/Main/UI/Settings/LanguageDropdown\n\nAvailable items: French, German, English, Spanish, Japanese\n\nUse debug/ui_get_options to see all items."
  }]
}
```

### Mutually exclusive parameters

When the LLM provides both `value` and `ratio`:

```json
{
  "isError": true,
  "content": [{
    "type": "text",
    "text": "Parameters 'value', 'ratio', and 'delta' are mutually exclusive. Provide exactly one."
  }]
}
```

### Missing required parameters

When `node_path` is missing (unlikely, since it is `required`, but the LLM might send an empty string):

```json
{
  "isError": true,
  "content": [{
    "type": "text",
    "text": "Missing required parameter: node_path\n\nProvide the scene tree path of a Control node.\nUse debug/search_scene_tree to find Control nodes."
  }]
}
```

### Game not running

All tools check `_require_game_running()` first and return the standard error with guidance on how to start the game.

### Timeout

The bridge request uses the standard `_wait_for_pending()` timeout mechanism (10 seconds default). If the game is unresponsive:

```json
{
  "isError": true,
  "content": [{
    "type": "text",
    "text": "UI interaction timed out after 10000ms.\n\nThe game may be frozen or processing a heavy frame. Try debug/get_status to check game state."
  }]
}
```

---

## Bridge Extension

### New method in `MCPDebuggerBridge`

```cpp
// mcp_debugger_bridge.h
Dictionary send_ui_interact(const String &p_action, const String &p_node_path,
                            const Dictionary &p_params, int p_timeout_msec = 10000);
```

### New capture in `MCPDebuggerBridge::capture()`

```cpp
// --- ui_interact_result ---
if (sub_msg == "ui_interact_result") {
    ERR_FAIL_COND_V(p_data.size() < 2, false);
    Dictionary result;
    result["success"] = (bool)p_data[0];
    result["data"] = p_data[1]; // Dictionary with action-specific result
    _complete_pending("ui_interact", result);
    return true;
}
```

### Long-running tool registration

`debug/ui_get_control_info` and write tools are async round-trips, so they should be added to the `long_running_tools` list in `MCPToolRegistry::is_long_running_tool()`.

---

## File Organization

### New files

| File | Purpose |
|------|---------|
| `modules/mcp_server/tools/mcp_ui_tools.h` | Class declaration for `MCPUITools` |
| `modules/mcp_server/tools/mcp_ui_tools.cpp` | Tool registration and handlers |

### Modified files

| File | Changes |
|------|---------|
| `modules/mcp_server/mcp_debugger_bridge.h` | Add `send_ui_interact()` method declaration |
| `modules/mcp_server/mcp_debugger_bridge.cpp` | Add `send_ui_interact()` implementation, `ui_interact_result` capture |
| `modules/mcp_server/mcp_tool_registry.cpp` | Add `debug/ui_*` tools to `long_running_tools` list |
| `scene/debugger/scene_debugger.h` | Add `_mcp_handle_ui_interact()` declaration |
| `scene/debugger/scene_debugger.cpp` | Add `ui_interact` message handler with sub-action dispatch |

### Registration

In the module's initialization (where `MCPAutomationTools::register_tools` and others are called), add:

```cpp
#include "tools/mcp_ui_tools.h"
// ...
MCPUITools::register_tools(registry);
```

---

## Security Considerations

1. **Path validation.** Same character whitelist as `handle_click_control` and `handle_get_node_properties`. No expression injection risk.

2. **No arbitrary method calls.** The game-side handler only calls specific, known methods on known control types. The LLM cannot call arbitrary methods through these tools.

3. **Signal emission.** The handler explicitly emits signals (like `text_changed`) to trigger game-side listeners. This is intentional -- it simulates real user interaction. However, it means the LLM can trigger game logic. This is equivalent to what `debug/click_control` already does.

4. **Read-only controls.** Write operations are blocked on disabled/read-only controls to prevent unintended state modification.

5. **No file system access.** These tools interact only with in-memory UI state. They do not read or write files.

---

## Performance Considerations

1. **Single round-trip.** Each tool call results in exactly one debugger message round-trip (same as `debug/evaluate` or `debug/click_control`). No multi-message protocols.

2. **Data size.** The `tree_get_items` and `itemlist_get_items` tools support pagination to prevent excessive data transfer for large collections.

3. **No polling.** All tools are synchronous request/response. The LLM does not need to poll for completion.

4. **Caching.** Control info is not cached -- each call gets fresh data from the game. This is correct because UI state can change between frames. The scene tree cache (existing) still helps with node path discovery.

---

## Future Extensions

These are explicitly out of scope for the initial implementation but noted for completeness:

- **FileDialog interaction.** Navigating file dialogs is complex (involves OS-level dialogs in some configurations). Deferred to a future design.
- **Drag and drop.** Simulating drag-and-drop between UI elements requires multi-frame input sequences. Could be built on top of `debug/send_input` with coordinate targeting.
- **Custom controls.** Game-specific custom controls cannot be generically handled. The LLM can fall back to `debug/evaluate` for these, or use `debug/click_control` for basic click interaction.
- **Animated transitions.** Some games animate UI transitions. The LLM should use `debug/wait_frames` between interactions to allow animations to complete.
- **Rich text (RichTextLabel).** Reading BBCode content and interacting with embedded links is complex. Deferred.
- **ColorPicker / ColorPickerButton.** Specialized controls that could benefit from dedicated tools. Deferred.
