extends Control

@onready var label: Label = $Label

# This variable changes per branch to test isolation.
const BRANCH_MARKER := "main"

func _ready() -> void:
	print("Branch Switch Test loaded - branch marker: %s" % BRANCH_MARKER)
	label.text = "Branch Switch Test\nBranch: %s\nProject: %s" % [
		BRANCH_MARKER,
		ProjectSettings.get_setting("application/config/name")
	]
