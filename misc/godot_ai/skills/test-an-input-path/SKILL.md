---
name: test-an-input-path
description: Prove that an input actually reaches the thing it is supposed to drive, on every device it claims to support, and say which paths are broken rather than that "input works".
enabled: true
required_editor_version: ">=4.3"
tools:
  - Godot_SendActionInput
  - Godot_SendKeyInput
  - Godot_SendPointerInput
  - Godot_SendTouchInput
  - Godot_SendGamepadInput
  - Godot_GetRuntimeProperty
  - Godot_WaitForRuntimeCondition
  - Godot_GetInputTrace
  - Godot_RecordSession
  - Godot_ReplaySession
  - Godot_CaptureGame
  - Godot_PlayMainScene
  - Godot_StopPlaying
---

You are checking that an input **path** works: the whole route from a device event to the
thing in the game that is supposed to respond. Not "does the game receive input" — every
game receives input — but "does pressing jump make the player jump, on a keyboard, on a
pad, and after somebody rebinds the key".

This is the testing job that gets skipped, because it looks like it must already work. It
is also where a rebind, a new input map, a `_unhandled_input` that stopped being unhandled,
or a menu that eats events silently breaks one device and nothing else.

The game must already be running.

## Send the action, not the key

`Godot_SendActionInput` triggers an InputMap action by name — `jump`, `ui_accept` — rather
than whatever key is bound to it today. Prefer it for everything the game reads as an
action, because that is what the game reads. A test written against `Space` passes until
someone rebinds jump and then fails for a reason that has nothing to do with jumping.

An action the project does not define is refused, and the refusal lists the actions it
does define. That refusal is a finding: a skill or a script referring to an action nobody
declared is a bug you have just found for free.

Use `Godot_SendKeyInput` deliberately, for the cases where the *key* is the thing under
test: a hardcoded shortcut, a debug key, a menu that reads `Escape` directly.

## Check the effect, not the delivery

Sending input and reporting success proves the tool works, not the game. After each input:

- `Godot_GetRuntimeProperty` on whatever should have changed — a counter, a state, a
  position, a flag.
- Or `Godot_WaitForRuntimeCondition` when the effect takes a few frames, which is most
  effects worth testing.

If you cannot name a property that should change, you do not yet have a test. Work out
what observable thing the input is for before sending it.

## Cover the devices the game claims to support

For each path worth testing, ask which devices reach it, and try each:

- **Keyboard** — `Godot_SendActionInput`, and `Godot_SendKeyInput` where the key itself
  matters.
- **Pointer** — `Godot_SendPointerInput`. Click, drag and scroll are different paths; a
  control that handles a click and ignores a drag is a common and invisible bug.
- **Touch** — `Godot_SendTouchInput`. A game that has only ever seen mouse events has not
  been tested on a touch device, and a game that has never had a touch **cancelled** has
  not been tested against a notification arriving mid-gesture. Send a cancel.
- **Gamepad** — `Godot_SendGamepadInput`, including a controller **disconnecting**
  mid-interaction. That one matters more than it sounds: a pad unplugged in a menu the
  player can now no longer move through is a soft-lock.

Report per device. "Input works" is not a result; "jump responds to keyboard and pad, and
the touch control does nothing because it is behind a Control that stopped propagating"
is.

## Hold and release are two different tests

`Godot_SendActionInput` with `press: "press"` holds the action down until you release it,
and the reply says so. Movement, charge attacks, and anything reading
`get_action_strength` behave differently held than tapped, and a suite that only ever taps
has not tested them.

Always release what you press. A held action outlives the test that pressed it, and every
later result in the session is then about a game walking into a wall.

## Leave something that can be re-run

`Godot_RecordSession` around the whole pass, and `Godot_ReplaySession` afterwards to prove
the recording reproduces. That turns "I checked the input paths" into a session someone
can re-run after the next change to the input map, which is the change that will break it.

Check the record with `Godot_GetInputTrace`: every kind you claim to have sent should
appear in it. If a kind is missing from the trace, you did not send it, whatever the
transcript says.

## What to report

One line per path and device: what was sent, what changed, and what did not. Name the
paths that failed and the ones you could not test, and say why. A pass with a list of
untested devices is honest; a pass that does not mention them is not.
