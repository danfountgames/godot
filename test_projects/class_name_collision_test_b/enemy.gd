class_name Enemy
extends RefCounted

## Enemy class from Project B - passive behavior variant.
## Used to test class_name isolation between mounted projects.

func get_behavior() -> String:
	return "Project B Enemy - Passive"
