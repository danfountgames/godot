extends Node
## A structured event trace, so "did that actually fire?" is a log read rather than a playtest.
##
## Autoloaded as `Trace`. Announce anything a test might gate on:
##
##     Trace.note(&"demo", {"played": true, "stage": stage_id})
##     Trace.note(&"demo", {"played": false, "because": "board_already_complete"})
##
## `Godot_ReadOutputLog` takes a `contains` filter, so `contains: "TRACE demo"` then answers
## "did the demonstration play, and if not why not" in one call against a running game.
##
## **The `because=` field is the point.** "demo skipped" is a fact; "demo skipped
## because=already_complete" is a diagnosis, and the difference is the gap between reading a log
## and running a playtest.
##
## This exists because of a real and expensive mistake. In the project this template came from, a
## specified opening demonstration was silently suppressed by a persisted flag — and that was
## discovered by a twenty-four minute black-box playtest whose entire purpose was to judge whether
## the demonstration *taught* anything. It never played. The tester correctly reported learning
## nothing from it: a true finding about a feature that was not running, and unrepeatable.
##
## Mechanism questions are cheap and come first. Meaning questions are expensive and irreplaceable.
## Never spend the second on the first.
##
## Not a debug *surface*: this draws nothing, and is off in a release build unless switched on.

const SETTING := "game/debug/trace_events"
const PREFIX := "TRACE"
const KEEP := 200

## Also kept in memory, so a test that has just driven a route can read it straight off the node
## instead of parsing the output panel, which it shares with the editor's own chatter.
var events: Array[String] = []

var _on: bool = false


func _ready() -> void:
	_on = bool(ProjectSettings.get_setting(SETTING, OS.is_debug_build()))


func note(topic: StringName, detail: Dictionary = {}) -> void:
	if not _on:
		return
	var line := "%s %s" % [PREFIX, topic]
	for key in detail:
		line += " %s=%s" % [key, detail[key]]
	events.append(line)
	if events.size() > KEEP:
		events = events.slice(events.size() - KEEP)
	print(line)


## Every trace line whose topic matches, oldest first.
func since(topic: StringName) -> Array[String]:
	var out: Array[String] = []
	var needle := "%s %s" % [PREFIX, topic]
	for line in events:
		if line.begins_with(needle):
			out.append(line)
	return out


func clear() -> void:
	events.clear()
