# Input Simulation System -- MCP Tool Design

## 1. Overview and Rationale

The existing MCP automation tooling provides `runtime/input/send_input` for action-based input injection. While functional for simple presses, it has significant limitations:

- **No raw keyboard input.** Many games bind keys directly (e.g., via `_input()` or `_unhandled_input()`) without defining named actions. Testing these requires injecting `InputEventKey` events.
- **No gamepad/joystick support.** There is no way to simulate analog stick movement, trigger presses, or gamepad button input -- critical for testing controller-driven games.
- **No key combos.** Modifier combinations like Ctrl+S, Shift+Click, or multi-key chords cannot be expressed.
- **No text entry.** Typing into `LineEdit` or `TextEdit` controls requires sending character-by-character key events with proper Unicode handling.
- **No input sequences.** Complex automation scenarios (e.g., "hold right for 500ms, then press jump") require multiple tool calls with manual `runtime/wait_frames` interleaving. A sequence primitive would allow expressing these as a single atomic operation.
- **No state visibility.** There is no way to query what inputs are currently held, or to perform an emergency reset of all held inputs.

This design introduces a suite of five MCP tools that comprehensively cover keyboard, gamepad, action, text, and sequence-based input simulation, plus state management and safety mechanisms.

### Design Principles

1. **Complement, don't replace.** The existing `runtime/input/send_input` tool remains unchanged. The new tools are additive.
2. **Follow established patterns.** All tools use the same registration, handler, bridge, and game-side message patterns as the existing automation tools.
3. **LLM-friendly parameter design.** String-based key/button names (not integer keycodes), sensible defaults, and rich error messages with suggestions.
4. **Safety first.** Automatic hold expiry, a global release-all mechanism, and held-state tracking prevent stuck inputs.
5. **Atomic sequences.** A single tool call can express multi-step input choreography, reducing round-trip latency and timing jitter.

---

## 2. Proposed Tools

### Tool Summary

| Tool Name | Purpose |
|---|---|
| `runtime/input/send_key` | Press/release/hold a keyboard key with optional modifiers |
| `runtime/input/send_joypad` | Press/release gamepad buttons or set analog axis values |
| `runtime/input/type_text` | Type a string of characters into the running game |
| `runtime/input/send_input_sequence` | Execute a timed sequence of input steps atomically |
| `runtime/input/get_held_inputs` | Query currently held inputs and optionally release all |

---

## 3. Tool Schemas

### 3.1 `runtime/input/send_key`

Send a keyboard key event to the running game. Supports modifier keys, hold duration, and physical/logical key specification.

**Input Schema:**

```json
{
  "type": "object",
  "properties": {
    "key": {
      "type": "string",
      "description": "Key name matching Godot's Key enum (e.g., 'A', 'space', 'escape', 'f1', 'shift', 'ctrl', 'up', 'down', 'enter'). Case-insensitive."
    },
    "pressed": {
      "type": "boolean",
      "description": "Press (true) or release (false) the key. Default: true."
    },
    "hold_frames": {
      "type": "integer",
      "description": "Frames to hold before auto-release. 0 = no auto-release (stays held until explicit release). Default: 1. Max: 600."
    },
    "modifiers": {
      "type": "array",
      "items": { "type": "string" },
      "description": "Modifier keys to hold during this key event. Values: 'shift', 'ctrl', 'alt', 'meta'. Example: ['ctrl', 'shift'] for Ctrl+Shift+key."
    },
    "echo": {
      "type": "boolean",
      "description": "Whether this is a key repeat/echo event. Default: false."
    }
  },
  "required": ["key"]
}
```

**Response (structured):**

```json
{
  "success": true,
  "key": "space",
  "keycode": 32,
  "pressed": true,
  "hold_frames": 1,
  "modifiers": ["ctrl"],
  "held_count": 3
}
```

**Example Request:**

```json
{
  "method": "tools/call",
  "params": {
    "name": "runtime/input/send_key",
    "arguments": {
      "key": "space",
      "pressed": true,
      "hold_frames": 30,
      "modifiers": ["shift"]
    }
  }
}
```

**Example Response:**

```
Key 'space' pressed with modifiers [shift] (hold: 30 frames)
```

**Registration (C++):**

```cpp
// runtime/input/send_key
{
    Dictionary props;
    props["key"] = make_prop("string",
            "Key name matching Godot's Key enum (e.g., 'A', 'space', 'escape', "
            "'f1', 'shift', 'ctrl', 'up', 'down', 'enter'). Case-insensitive.");
    props["pressed"] = make_prop("boolean",
            "Press (true) or release (false) the key (default: true)");
    props["hold_frames"] = make_prop("integer",
            "Frames to hold before auto-release. 0 = stay held. (default: 1, max: 600)");
    props["modifiers"] = make_prop("array",
            "Modifier keys: 'shift', 'ctrl', 'alt', 'meta'. Example: ['ctrl', 'shift']");
    props["echo"] = make_prop("boolean",
            "Whether this is a key repeat event (default: false)");
    Array required;
    required.push_back("key");
    p_registry->register_tool(
            "runtime/input/send_key", "Send Key Input",
            "Send a keyboard key event to the running game. Supports modifier combos "
            "(Ctrl+Shift+S), hold duration, and all Godot Key enum names. Use for games "
            "that bind raw keys rather than input actions. Game must be running.",
            make_schema(props, required),
            make_annotations(false, false, false),
            callable_mp_static(&MCPInputTools::handle_send_key));
}
```

---

### 3.2 `runtime/input/send_joypad`

Send a gamepad button press or analog axis value to the running game.

**Input Schema:**

```json
{
  "type": "object",
  "properties": {
    "type": {
      "type": "string",
      "enum": ["button", "axis"],
      "description": "Whether to send a button event or an axis motion event."
    },
    "button": {
      "type": "string",
      "description": "Joypad button name (for type='button'). Values: 'a', 'b', 'x', 'y', 'back', 'guide', 'start', 'left_stick', 'right_stick', 'left_shoulder', 'right_shoulder', 'dpad_up', 'dpad_down', 'dpad_left', 'dpad_right'. Case-insensitive."
    },
    "axis": {
      "type": "string",
      "description": "Joypad axis name (for type='axis'). Values: 'left_x', 'left_y', 'right_x', 'right_y', 'trigger_left', 'trigger_right'. Case-insensitive."
    },
    "value": {
      "type": "number",
      "description": "Axis value from -1.0 to 1.0 (for sticks) or 0.0 to 1.0 (for triggers). Default: 1.0 for buttons."
    },
    "pressed": {
      "type": "boolean",
      "description": "Press (true) or release (false) the button (for type='button'). Default: true."
    },
    "hold_frames": {
      "type": "integer",
      "description": "Frames to hold before auto-release/reset. 0 = stay held. Default: 1. Max: 600."
    },
    "device": {
      "type": "integer",
      "description": "Joypad device index. Default: 0."
    }
  },
  "required": ["type"]
}
```

**Response (structured):**

```json
{
  "success": true,
  "type": "axis",
  "axis": "left_x",
  "value": 0.75,
  "hold_frames": 30,
  "device": 0,
  "held_count": 2
}
```

**Example Request (analog stick):**

```json
{
  "method": "tools/call",
  "params": {
    "name": "runtime/input/send_joypad",
    "arguments": {
      "type": "axis",
      "axis": "left_x",
      "value": 1.0,
      "hold_frames": 60
    }
  }
}
```

**Example Response:**

```
Joypad axis 'left_x' set to 1.0 on device 0 (hold: 60 frames)
```

**Example Request (button):**

```json
{
  "method": "tools/call",
  "params": {
    "name": "runtime/input/send_joypad",
    "arguments": {
      "type": "button",
      "button": "a",
      "pressed": true,
      "hold_frames": 5
    }
  }
}
```

**Example Response:**

```
Joypad button 'a' pressed on device 0 (hold: 5 frames)
```

---

### 3.3 `runtime/input/type_text`

Type a string of characters into the running game. Each character is sent as an `InputEventKey` press+release pair with the appropriate Unicode value. Useful for filling `LineEdit` and `TextEdit` controls.

**Input Schema:**

```json
{
  "type": "object",
  "properties": {
    "text": {
      "type": "string",
      "description": "The text to type. Each character is sent as a key press+release pair."
    },
    "interval_frames": {
      "type": "integer",
      "description": "Frames to wait between each character. Default: 0 (as fast as possible). Max: 60."
    }
  },
  "required": ["text"]
}
```

**Response (structured):**

```json
{
  "success": true,
  "text": "Hello World",
  "char_count": 11,
  "interval_frames": 2
}
```

**Example Request:**

```json
{
  "method": "tools/call",
  "params": {
    "name": "runtime/input/type_text",
    "arguments": {
      "text": "Hello World",
      "interval_frames": 2
    }
  }
}
```

**Example Response:**

```
Typed 11 characters: "Hello World" (interval: 2 frames between chars)
```

---

### 3.4 `runtime/input/send_input_sequence`

Execute a sequence of input steps with precise frame-based timing. Each step can be a key press, action, joypad event, or a wait. The entire sequence executes atomically on the game side.

**Input Schema:**

```json
{
  "type": "object",
  "properties": {
    "steps": {
      "type": "array",
      "description": "Ordered list of input steps to execute.",
      "items": {
        "type": "object",
        "properties": {
          "type": {
            "type": "string",
            "enum": ["key", "action", "joypad_button", "joypad_axis", "wait"],
            "description": "The type of input step."
          },
          "key": {
            "type": "string",
            "description": "Key name (for type='key')."
          },
          "action": {
            "type": "string",
            "description": "Action name (for type='action')."
          },
          "button": {
            "type": "string",
            "description": "Joypad button name (for type='joypad_button')."
          },
          "axis": {
            "type": "string",
            "description": "Joypad axis name (for type='joypad_axis')."
          },
          "value": {
            "type": "number",
            "description": "Axis value -1.0 to 1.0, or action strength 0.0 to 1.0."
          },
          "pressed": {
            "type": "boolean",
            "description": "Press (true) or release (false). Default: true."
          },
          "modifiers": {
            "type": "array",
            "items": { "type": "string" },
            "description": "Modifier keys for key events."
          },
          "frames": {
            "type": "integer",
            "description": "For type='wait': frames to wait before next step. For other types: frames to hold before auto-release. Default: 0."
          }
        },
        "required": ["type"]
      }
    }
  },
  "required": ["steps"]
}
```

**Response (structured):**

```json
{
  "success": true,
  "steps_executed": 4,
  "total_frames": 90,
  "steps": [
    { "index": 0, "type": "key", "key": "right", "pressed": true, "status": "ok" },
    { "index": 1, "type": "wait", "frames": 30, "status": "ok" },
    { "index": 2, "type": "key", "key": "right", "pressed": false, "status": "ok" },
    { "index": 3, "type": "action", "action": "jump", "pressed": true, "status": "ok" }
  ]
}
```

**Example Request -- "walk right for 30 frames, then jump":**

```json
{
  "method": "tools/call",
  "params": {
    "name": "runtime/input/send_input_sequence",
    "arguments": {
      "steps": [
        { "type": "key", "key": "right", "pressed": true },
        { "type": "wait", "frames": 30 },
        { "type": "key", "key": "right", "pressed": false },
        { "type": "action", "action": "jump", "pressed": true, "frames": 5 }
      ]
    }
  }
}
```

**Example Response:**

```
Sequence completed: 4 steps executed over ~30 frames.
  [0] key 'right' pressed
  [1] wait 30 frames
  [2] key 'right' released
  [3] action 'jump' pressed (hold: 5 frames)
```

**Limits:**

- Maximum 50 steps per sequence.
- Maximum 1800 total frames (30 seconds at 60fps).
- Sequence timeout: 60 seconds.

---

### 3.5 `runtime/input/get_held_inputs`

Query the current held-input state on the game side, and optionally release all held inputs.

**Input Schema:**

```json
{
  "type": "object",
  "properties": {
    "release_all": {
      "type": "boolean",
      "description": "If true, release all currently held inputs before returning. Default: false."
    }
  },
  "required": []
}
```

**Response (structured):**

```json
{
  "success": true,
  "released": false,
  "held_keys": [
    { "key": "right", "keycode": 16777233, "held_frames": 45 }
  ],
  "held_actions": [
    { "action": "move_right", "strength": 1.0, "held_frames": 45 }
  ],
  "held_joypad_buttons": [],
  "held_joypad_axes": [
    { "axis": "left_x", "value": 0.75, "device": 0, "held_frames": 20 }
  ],
  "total_held": 3
}
```

**Example Request (emergency reset):**

```json
{
  "method": "tools/call",
  "params": {
    "name": "runtime/input/get_held_inputs",
    "arguments": {
      "release_all": true
    }
  }
}
```

**Example Response:**

```
Released 3 held inputs:
  Key 'right' (held 45 frames)
  Action 'move_right' (held 45 frames)
  Joypad axis 'left_x' = 0.75 on device 0 (held 20 frames)
```

---

## 4. Godot InputEvent Mapping Details

This section describes how MCP tool parameters map to Godot's `InputEvent` subclasses.

### 4.1 Key Names to Keycodes

The `key` parameter in `runtime/input/send_key` accepts string names that map to Godot's `Key` enum values. The game-side handler performs a case-insensitive lookup.

| MCP Key Name | Godot Key Enum | Keycode |
|---|---|---|
| `"a"` - `"z"` | `KEY_A` - `KEY_Z` | 65-90 |
| `"0"` - `"9"` | `KEY_0` - `KEY_9` | 48-57 |
| `"f1"` - `"f12"` | `KEY_F1` - `KEY_F12` | 4194332-4194343 |
| `"space"` | `KEY_SPACE` | 32 |
| `"enter"` | `KEY_ENTER` | 4194309 |
| `"escape"`, `"esc"` | `KEY_ESCAPE` | 4194305 |
| `"tab"` | `KEY_TAB` | 4194306 |
| `"backspace"` | `KEY_BACKSPACE` | 4194308 |
| `"up"` | `KEY_UP` | 4194320 |
| `"down"` | `KEY_DOWN` | 4194322 |
| `"left"` | `KEY_LEFT` | 4194319 |
| `"right"` | `KEY_RIGHT` | 4194321 |
| `"shift"` | `KEY_SHIFT` | 4194325 |
| `"ctrl"` | `KEY_CTRL` | 4194326 |
| `"alt"` | `KEY_ALT` | 4194327 |
| `"meta"` | `KEY_META` | 4194328 |
| `"delete"` | `KEY_DELETE` | 4194312 |
| `"home"` | `KEY_HOME` | 4194313 |
| `"end"` | `KEY_END` | 4194314 |

The full lookup table is built from `find_keycode_name()` / Godot's keycode utilities at compile time. Unrecognized key names return an error with suggestions.

### 4.2 InputEventKey Construction

```
MCP Parameters          ->  InputEventKey Properties
---------------------------------------------------------
key: "space"            ->  keycode = KEY_SPACE (32)
                            physical_keycode = KEY_SPACE
pressed: true           ->  pressed = true
modifiers: ["ctrl"]     ->  ctrl_pressed = true
echo: false             ->  echo = false
                            unicode = 0 (set from keycode for printable chars)
```

For `runtime/input/type_text`, each character is sent as:
```
InputEventKey {
    keycode = <inferred from char, or KEY_NONE>
    unicode = <char Unicode codepoint>
    pressed = true / false (press then release)
    echo = false
}
```

### 4.3 Joypad Button Names to Enums

| MCP Button Name | Godot JoyButton Enum | Value |
|---|---|---|
| `"a"` | `JoyButton::A` | 0 |
| `"b"` | `JoyButton::B` | 1 |
| `"x"` | `JoyButton::X` | 2 |
| `"y"` | `JoyButton::Y` | 3 |
| `"back"` | `JoyButton::BACK` | 4 |
| `"guide"` | `JoyButton::GUIDE` | 5 |
| `"start"` | `JoyButton::START` | 6 |
| `"left_stick"` | `JoyButton::LEFT_STICK` | 7 |
| `"right_stick"` | `JoyButton::RIGHT_STICK` | 8 |
| `"left_shoulder"`, `"lb"` | `JoyButton::LEFT_SHOULDER` | 9 |
| `"right_shoulder"`, `"rb"` | `JoyButton::RIGHT_SHOULDER` | 10 |
| `"dpad_up"` | `JoyButton::DPAD_UP` | 11 |
| `"dpad_down"` | `JoyButton::DPAD_DOWN` | 12 |
| `"dpad_left"` | `JoyButton::DPAD_LEFT` | 13 |
| `"dpad_right"` | `JoyButton::DPAD_RIGHT` | 14 |

### 4.4 Joypad Axis Names to Enums

| MCP Axis Name | Godot JoyAxis Enum | Value | Range |
|---|---|---|---|
| `"left_x"` | `JoyAxis::LEFT_X` | 0 | -1.0 to 1.0 |
| `"left_y"` | `JoyAxis::LEFT_Y` | 1 | -1.0 to 1.0 |
| `"right_x"` | `JoyAxis::RIGHT_X` | 2 | -1.0 to 1.0 |
| `"right_y"` | `JoyAxis::RIGHT_Y` | 3 | -1.0 to 1.0 |
| `"trigger_left"`, `"lt"` | `JoyAxis::TRIGGER_LEFT` | 4 | 0.0 to 1.0 |
| `"trigger_right"`, `"rt"` | `JoyAxis::TRIGGER_RIGHT` | 5 | 0.0 to 1.0 |

### 4.5 InputEvent Construction for Joypad

**Button:**
```
InputEventJoypadButton {
    device = <device param, default 0>
    button_index = <JoyButton enum from name>
    pressed = <pressed param>
    pressure = <pressed ? 1.0 : 0.0>
}
```

**Axis:**
```
InputEventJoypadMotion {
    device = <device param, default 0>
    axis = <JoyAxis enum from name>
    axis_value = <value param, clamped to appropriate range>
}
```

---

## 5. Game-Side Implementation

All input injection runs in the game process via the existing `_mcp_capture` message handler in `scene_debugger.cpp`. New message types are added to the `mcp:` capture namespace.

### 5.1 New Debugger Messages

| Editor -> Game Message | Data Array | Game -> Editor Response |
|---|---|---|
| `mcp:inject_key` | `[key_name, pressed, hold_frames, modifier_flags, echo]` | `mcp:key_done` `[success, message]` |
| `mcp:inject_joypad_button` | `[button_name, pressed, hold_frames, device]` | `mcp:joypad_done` `[success, message]` |
| `mcp:inject_joypad_axis` | `[axis_name, value, hold_frames, device]` | `mcp:joypad_done` `[success, message]` |
| `mcp:type_text` | `[text, interval_frames]` | `mcp:type_text_done` `[success, chars_typed]` |
| `mcp:run_input_sequence` | `[steps_array]` | `mcp:sequence_done` `[success, steps_executed, results_array]` |
| `mcp:get_held_inputs` | `[release_all]` | `mcp:held_inputs_result` `[held_data_dict]` |

### 5.2 Held Input Tracking (Game-Side)

A new static structure in `scene_debugger.cpp` tracks MCP-injected held inputs:

```cpp
struct MCPHeldInput {
    String name;         // Key/button/axis/action name for identification.
    int type;            // 0=key, 1=action, 2=joypad_button, 3=joypad_axis
    int device;          // Joypad device index (0 for keys/actions).
    float value;         // Axis value or strength.
    int frames_held;     // Incremented each process frame.
    int auto_release_at; // Frame count at which to auto-release. -1 = manual.
};

static Vector<MCPHeldInput> _mcp_held_inputs;
```

A process-frame callback increments `frames_held` for each entry and handles auto-release when `frames_held >= auto_release_at`.

### 5.3 Key Injection Implementation (Game-Side Pseudocode)

```cpp
// In SceneDebugger::_mcp_capture, handling "inject_key":

if (p_msg == "inject_key") {
    String key_name = p_data[0];
    bool pressed = p_data[1];
    int hold_frames = p_data[2];
    int modifier_flags = p_data[3];
    bool echo = p_data[4];

    // Resolve key name to Key enum.
    Key keycode = _mcp_key_name_to_keycode(key_name);
    if (keycode == Key::NONE) {
        // Send error with suggestions.
        send_error("Unknown key: " + key_name);
        return OK;
    }

    // Construct InputEventKey.
    Ref<InputEventKey> ev;
    ev.instantiate();
    ev->set_keycode(keycode);
    ev->set_physical_keycode(keycode);
    ev->set_pressed(pressed);
    ev->set_echo(echo);

    // Set unicode for printable characters.
    char32_t unicode = _keycode_to_unicode(keycode);
    if (modifier_flags & MOD_SHIFT) {
        unicode = _shift_unicode(unicode);
    }
    ev->set_unicode(unicode);

    // Set modifiers.
    ev->set_shift_pressed(modifier_flags & MOD_SHIFT);
    ev->set_ctrl_pressed(modifier_flags & MOD_CTRL);
    ev->set_alt_pressed(modifier_flags & MOD_ALT);
    ev->set_meta_pressed(modifier_flags & MOD_META);

    Input::get_singleton()->parse_input_event(ev);

    // Track hold state.
    if (pressed && hold_frames > 0) {
        _mcp_add_held_input("key:" + key_name, 0, 0, 1.0, hold_frames);
        // Schedule release via process_frame counter.
    } else if (pressed && hold_frames == 0) {
        _mcp_add_held_input("key:" + key_name, 0, 0, 1.0, -1); // Manual release.
    } else if (!pressed) {
        _mcp_remove_held_input("key:" + key_name);
    }

    // Send success response.
    Array result;
    result.push_back(true);
    result.push_back("");
    EngineDebugger::get_singleton()->send_message("mcp:key_done", result);
    return OK;
}
```

### 5.4 Joypad Injection Implementation (Game-Side Pseudocode)

```cpp
// Button:
if (p_msg == "inject_joypad_button") {
    String button_name = p_data[0];
    bool pressed = p_data[1];
    int hold_frames = p_data[2];
    int device = p_data[3];

    JoyButton button = _mcp_button_name_to_enum(button_name);
    if (button == JoyButton::INVALID) {
        send_error("Unknown joypad button: " + button_name);
        return OK;
    }

    Ref<InputEventJoypadButton> ev;
    ev.instantiate();
    ev->set_device(device);
    ev->set_button_index(button);
    ev->set_pressed(pressed);
    ev->set_pressure(pressed ? 1.0f : 0.0f);

    Input::get_singleton()->parse_input_event(ev);
    // Track hold state, schedule release...
}

// Axis:
if (p_msg == "inject_joypad_axis") {
    String axis_name = p_data[0];
    float value = p_data[1];
    int hold_frames = p_data[2];
    int device = p_data[3];

    JoyAxis axis = _mcp_axis_name_to_enum(axis_name);
    if (axis == JoyAxis::INVALID) {
        send_error("Unknown joypad axis: " + axis_name);
        return OK;
    }

    Ref<InputEventJoypadMotion> ev;
    ev.instantiate();
    ev->set_device(device);
    ev->set_axis(axis);
    ev->set_axis_value(value);

    Input::get_singleton()->parse_input_event(ev);
    // Track hold state, schedule reset to 0.0...
}
```

### 5.5 Text Typing Implementation (Game-Side Pseudocode)

```cpp
if (p_msg == "type_text") {
    String text = p_data[0];
    int interval_frames = p_data[1];

    if (interval_frames <= 0) {
        // Send all characters immediately in the same frame.
        for (int i = 0; i < text.length(); i++) {
            _mcp_send_char_key(text[i], true);   // Press.
            _mcp_send_char_key(text[i], false);  // Release.
        }
        Array result;
        result.push_back(true);
        result.push_back(text.length());
        EngineDebugger::get_singleton()->send_message("mcp:type_text_done", result);
    } else {
        // Queue characters with frame-based spacing.
        // Store pending chars in a static queue, dispatch via process_frame.
        _mcp_type_queue = text;
        _mcp_type_index = 0;
        _mcp_type_interval = interval_frames;
        _mcp_type_frame_counter = 0;
        // Connect to process_frame if not already connected.
    }
    return OK;
}
```

### 5.6 Sequence Execution (Game-Side)

Sequences are executed by a state machine connected to `process_frame`:

```cpp
struct MCPSequenceState {
    Array steps;                // The full step list.
    int current_step = 0;       // Index of the step being executed.
    int wait_frames_remaining;  // Countdown for current wait/hold step.
    Array results;              // Per-step result for the response.
    bool active = false;
};

static MCPSequenceState _mcp_sequence;
```

On `mcp:run_input_sequence`:
1. Parse and validate all steps upfront. Reject the entire sequence if any step has invalid parameters.
2. Set `_mcp_sequence.active = true` and connect to `process_frame`.
3. Each frame, the tick function:
   - If `wait_frames_remaining > 0`, decrement and continue.
   - Otherwise, execute the current step (inject input event, or start a wait).
   - Advance to next step.
   - When all steps are complete, send `mcp:sequence_done` and disconnect.

---

## 6. Editor-Side Implementation

### 6.1 New Bridge Methods (`mcp_debugger_bridge.h/.cpp`)

```cpp
// --- New Async Request Methods ---

Dictionary send_inject_key(
    const String &p_key_name,
    bool p_pressed,
    int p_hold_frames,
    int p_modifier_flags,
    bool p_echo,
    int p_timeout_msec = 10000
);

Dictionary send_inject_joypad_button(
    const String &p_button_name,
    bool p_pressed,
    int p_hold_frames,
    int p_device,
    int p_timeout_msec = 10000
);

Dictionary send_inject_joypad_axis(
    const String &p_axis_name,
    float p_value,
    int p_hold_frames,
    int p_device,
    int p_timeout_msec = 10000
);

Dictionary send_type_text(
    const String &p_text,
    int p_interval_frames,
    int p_timeout_msec = 30000
);

Dictionary send_input_sequence(
    const Array &p_steps,
    int p_timeout_msec = 60000
);

Dictionary send_get_held_inputs(
    bool p_release_all,
    int p_timeout_msec = 10000
);
```

Each method follows the established `_create_pending` / `MCP_BRIDGE_SEND_OR_FAIL` / `_wait_for_pending` pattern.

### 6.2 New Capture Handlers in `MCPDebuggerBridge::capture()`

```cpp
// --- key_done ---
if (sub_msg == "key_done") {
    ERR_FAIL_COND_V(p_data.size() < 2, false);
    Dictionary result;
    result["success"] = (bool)p_data[0];
    result["message"] = p_data[1];
    _complete_pending("inject_key", result);
    return true;
}

// --- joypad_done ---
if (sub_msg == "joypad_done") {
    ERR_FAIL_COND_V(p_data.size() < 2, false);
    Dictionary result;
    result["success"] = (bool)p_data[0];
    result["message"] = p_data[1];
    _complete_pending("inject_joypad", result);  // Shared for button and axis.
    return true;
}

// --- type_text_done ---
if (sub_msg == "type_text_done") {
    ERR_FAIL_COND_V(p_data.size() < 2, false);
    Dictionary result;
    result["success"] = (bool)p_data[0];
    result["chars_typed"] = p_data[1];
    _complete_pending("type_text", result);
    return true;
}

// --- sequence_done ---
if (sub_msg == "sequence_done") {
    ERR_FAIL_COND_V(p_data.size() < 3, false);
    Dictionary result;
    result["success"] = (bool)p_data[0];
    result["steps_executed"] = p_data[1];
    result["step_results"] = p_data[2];
    _complete_pending("input_sequence", result);
    return true;
}

// --- held_inputs_result ---
if (sub_msg == "held_inputs_result") {
    ERR_FAIL_COND_V(p_data.is_empty(), false);
    Dictionary result;
    result["success"] = true;
    result["data"] = p_data[0];
    _complete_pending("get_held_inputs", result);
    return true;
}
```

### 6.3 Tool Handler Class (`mcp_input_tools.h/.cpp`)

A new file pair `tools/mcp_input_tools.h` and `tools/mcp_input_tools.cpp` following the same pattern as `mcp_automation_tools.*`:

```cpp
// mcp_input_tools.h
#pragma once

#include "core/variant/dictionary.h"

class MCPToolRegistry;

class MCPInputTools {
public:
    static void register_tools(MCPToolRegistry *p_registry);

    static Dictionary handle_send_key(const Dictionary &p_args);
    static Dictionary handle_send_joypad(const Dictionary &p_args);
    static Dictionary handle_type_text(const Dictionary &p_args);
    static Dictionary handle_send_input_sequence(const Dictionary &p_args);
    static Dictionary handle_get_held_inputs(const Dictionary &p_args);

private:
    static class MCPDebuggerBridge *_get_bridge();
    static Dictionary _require_game_running();
    static int _parse_modifier_flags(const Array &p_modifiers);
    static bool _validate_key_name(const String &p_key);
    static bool _validate_button_name(const String &p_button);
    static bool _validate_axis_name(const String &p_axis);
};
```

Registration is invoked from `MCPToolRegistry` initialization (or wherever `MCPAutomationTools::register_tools` is called):

```cpp
MCPInputTools::register_tools(registry);
```

### 6.4 Parameter Validation (Editor-Side)

Key/button/axis name validation happens on the editor side before sending to the game, providing immediate error feedback (matching the pattern used by `_action_exists()` in `mcp_automation_tools.cpp`).

A static lookup table maps string names to integer enum values:

```cpp
static HashMap<String, Key> key_name_map;
static HashMap<String, JoyButton> button_name_map;
static HashMap<String, JoyAxis> axis_name_map;

// Populated once at first use (lazy init).
static void _ensure_lookup_tables();
```

---

## 7. Hold State Management and Safety Mechanisms

### 7.1 Hold Lifecycle

Every input injected by MCP with `hold_frames > 0` or `hold_frames == 0` (indefinite hold) is tracked in the game-side `_mcp_held_inputs` vector.

**Auto-release (hold_frames > 0):**
- A `process_frame` callback increments `frames_held` for each held input.
- When `frames_held >= auto_release_at`, the corresponding release event is injected and the entry is removed.

**Manual release (hold_frames == 0):**
- The input stays held until an explicit release call (`pressed: false`) or `runtime/input/get_held_inputs` with `release_all: true`.

### 7.2 Maximum Hold Duration

Even "indefinite" holds have an absolute ceiling of **1800 frames (30 seconds at 60fps)**. This prevents a disconnected or crashed LLM client from leaving inputs stuck forever.

When the ceiling is hit:
1. The input is auto-released.
2. A warning message is sent to the editor output: `[MCP] Auto-released held input 'key:right' after 1800 frames (safety limit)`.

### 7.3 Session Cleanup

When the game stops (`_on_session_stopped`), all game-side state is destroyed naturally (the process exits). No explicit cleanup is needed.

When a new game session starts, the editor-side bridge resets its state. The game-side `_mcp_held_inputs` vector starts empty because it lives in the new game process.

### 7.4 Release-All Mechanism

`runtime/input/get_held_inputs` with `release_all: true` iterates `_mcp_held_inputs` and injects the appropriate release event for each entry:
- Keys: `InputEventKey` with `pressed = false`.
- Actions: `InputEventAction` with `pressed = false`.
- Joypad buttons: `InputEventJoypadButton` with `pressed = false`.
- Joypad axes: `InputEventJoypadMotion` with `axis_value = 0.0`.

Then the vector is cleared.

### 7.5 Concurrency Safety

The game-side held-input tracking is accessed only on the game's main thread (from `process_frame` and from MCP message handlers, which both run on the main thread). No mutex is needed.

The editor-side bridge methods are called from MCP HTTP threads and use the existing `_create_pending` / `_wait_for_pending` pattern, which is already thread-safe.

---

## 8. Sequence Execution Model

### 8.1 Execution Semantics

A sequence is a linearly ordered list of steps. Steps execute one at a time:

1. **Input steps** (`key`, `action`, `joypad_button`, `joypad_axis`) are injected immediately (in the current frame). If the step has `frames > 0`, the input is held for that many frames before auto-release, but execution proceeds to the next step immediately (the hold runs in parallel).
2. **Wait steps** (`wait`) pause sequence execution for the specified number of frames. The next step executes after the wait completes.

This means holds and waits interact as follows:

```
Step 0: key "right" pressed, frames=30   <-- injected frame 0, auto-releases at frame 30
Step 1: wait, frames=15                  <-- sequence pauses for 15 frames
Step 2: action "jump" pressed, frames=5  <-- injected frame 15, auto-releases at frame 20
Step 3: wait, frames=30                  <-- sequence pauses for 30 more frames
                                             (right key releases during this wait at frame 30)
Step 4: (sequence complete at frame 45)
```

### 8.2 Atomicity

The entire sequence runs on the game side. The editor sends a single `mcp:run_input_sequence` message and receives a single `mcp:sequence_done` response when all steps have executed. This eliminates HTTP round-trip latency between steps.

### 8.3 Error Handling Within Sequences

If any step fails validation (e.g., unknown key name), the sequence is **rejected entirely before execution** -- no steps run, and the error response identifies the failing step.

If an error occurs during execution (unlikely, since validation is upfront), the sequence stops at the failing step and returns a partial result.

### 8.4 Sequence Cancellation

If a new `mcp:run_input_sequence` message arrives while one is already active, the old sequence is cancelled (all its pending holds are released) and the new one starts. This matches the existing `_create_pending` supersession behavior.

Similarly, if the game stops mid-sequence, the bridge's `_wake_all_pending` mechanism returns an error to the waiting MCP thread.

---

## 9. Edge Cases and Error Handling

### 9.1 Game Not Running

All five tools call `_require_game_running()` first. If the game is not running, they return the standard guidance message directing the user to `runtime/run_project` or `runtime/run_scene`.

### 9.2 Unknown Key/Button/Axis Names

Each tool validates names before sending to the game. On failure, the error response includes:
1. The exact invalid name.
2. A "did you mean?" list of similar valid names (fuzzy matching).
3. A reference to the valid name list.

Example error:
```
Unknown key name: 'spacebar'.

Did you mean one of these?
  space
  backspace

Valid key names include: a-z, 0-9, f1-f12, space, enter, escape, tab, backspace,
up, down, left, right, shift, ctrl, alt, meta, delete, home, end, ...
```

### 9.3 Conflicting Holds

If the LLM sends `key: "right", pressed: true` while "right" is already held:
- The existing hold is replaced (its auto-release timer resets).
- Only one held-input entry per unique key/button/axis exists at a time.
- A note in the response indicates the hold was refreshed: `"note": "replaced existing hold"`.

### 9.4 Modifier Handling for send_key

Modifier flags are packed as a bitmask:
```
MOD_SHIFT = 1
MOD_CTRL  = 2
MOD_ALT   = 4
MOD_META  = 8
```

The game-side handler sets the corresponding `InputEventKey` modifier properties. Modifiers in the `modifiers` array that are also physical keys (e.g., sending `key: "a"` with `modifiers: ["shift"]`) do NOT inject separate key events for the modifier keys. The modifier state is set on the `InputEventKey` directly. This mirrors how Godot's own input system works: `InputEventKey.shift_pressed` is a property of the key event, not a separate event.

If the user wants to actually press/hold the Shift key itself (e.g., for a game that checks `Input.is_key_pressed(KEY_SHIFT)`), they should send `runtime/input/send_key` with `key: "shift"` separately.

### 9.5 Text with Special Characters

`runtime/input/type_text` handles:
- **Printable ASCII** (a-z, 0-9, symbols): Sent as `InputEventKey` with appropriate `keycode` and `unicode`.
- **Unicode** (e.g., accented characters, CJK): Sent with `keycode = KEY_NONE` and `unicode = <codepoint>`. Godot's `LineEdit`/`TextEdit` accept input via the Unicode field.
- **Newline** (`\n`): Sent as `KEY_ENTER`.
- **Tab** (`\t`): Sent as `KEY_TAB`.
- **Backspace** (not in text, but can be typed via `runtime/input/send_key`).

Maximum text length: 1000 characters per call.

### 9.6 Joypad Axis Reset

When a joypad axis hold expires, the axis is reset to `0.0` (neutral position), not left at the last value. This is critical because `InputEventJoypadMotion` with a non-zero value is a persistent state in Godot -- the engine treats it as "the stick is being held there" until a new motion event resets it.

### 9.7 Sequence Limits

| Limit | Value | Rationale |
|---|---|---|
| Max steps per sequence | 50 | Prevents unbounded execution time |
| Max total wait frames | 1800 | 30 seconds at 60fps -- reasonable for any test scenario |
| Sequence timeout (editor-side) | 60000ms | Generous timeout for sequences with long waits |
| Max text length (type_text) | 1000 chars | Prevents extremely long typing sequences |

### 9.8 Device Index Validation

Joypad device indices are validated to be in range 0-7. Godot supports up to 8 joypad devices. Out-of-range values return an error.

Note: MCP input simulation does not require a physical joypad to be connected. Godot's `Input.parse_input_event()` processes synthetic joypad events regardless of whether a real device is present.

### 9.9 Race Condition: Input During Frame Boundary

`Input::parse_input_event()` is safe to call from the main thread at any time during a frame. Events are queued internally and processed during the next input flush. Since MCP message handlers run on the main thread (dispatched by the debugger protocol), there is no risk of race conditions.

### 9.10 Echo/Repeat Events

`runtime/input/send_key` supports an `echo` parameter for generating key repeat events. This is useful for simulating held-key repeat behavior (e.g., scrolling through a list). The game's `_input()` handler can distinguish between initial presses (`echo == false`) and repeats (`echo == true`).

---

## 10. File Layout

### New Files

| File | Purpose |
|---|---|
| `modules/mcp_server/tools/mcp_input_tools.h` | Header for `MCPInputTools` class |
| `modules/mcp_server/tools/mcp_input_tools.cpp` | Tool registration and handler implementations |

### Modified Files

| File | Changes |
|---|---|
| `modules/mcp_server/mcp_debugger_bridge.h` | Add 6 new `send_*` method declarations |
| `modules/mcp_server/mcp_debugger_bridge.cpp` | Add 6 new `send_*` implementations, 5 new `capture()` handlers |
| `scene/debugger/scene_debugger.h` | Add static state variables for held inputs, typing queue, sequence state |
| `scene/debugger/scene_debugger.cpp` | Add 6 new `_mcp_capture` message handlers, held-input tick, sequence tick |
| `modules/mcp_server/mcp_tool_registry.cpp` | Add `mcp_input_tools.h` include, call `MCPInputTools::register_tools()` |
| `modules/mcp_server/SCsub` (or equivalent build file) | Add `mcp_input_tools.cpp` to compilation |

### Approximate Code Size Estimates

| Component | Estimated Lines |
|---|---|
| `mcp_input_tools.h` | ~40 |
| `mcp_input_tools.cpp` (editor-side tools) | ~500 |
| `mcp_debugger_bridge.*` additions | ~150 |
| `scene_debugger.cpp` additions (game-side) | ~600 |
| **Total** | **~1300** |

---

## 11. Summary of Debugger Wire Protocol

Complete list of new messages between editor and game:

```
EDITOR -> GAME (via EditorDebuggerSession::send_message):
  mcp:inject_key           [key_name, pressed, hold_frames, modifier_flags, echo]
  mcp:inject_joypad_button [button_name, pressed, hold_frames, device]
  mcp:inject_joypad_axis   [axis_name, value, hold_frames, device]
  mcp:type_text            [text, interval_frames]
  mcp:run_input_sequence   [steps_array]
  mcp:get_held_inputs      [release_all]

GAME -> EDITOR (via EngineDebugger::send_message):
  mcp:key_done             [success, message]
  mcp:joypad_done          [success, message]
  mcp:type_text_done       [success, chars_typed]
  mcp:sequence_done        [success, steps_executed, results_array]
  mcp:held_inputs_result   [held_data_dict]
```

All messages use the `mcp:` prefix and flow through the existing `EditorDebuggerPlugin` capture mechanism on the editor side and the `_mcp_capture` handler on the game side.
