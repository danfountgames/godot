class_name Enemy
extends RefCounted

## Enemy class from Project A - aggressive behavior variant.
## Used to test class_name isolation between mounted projects.

func get_behavior() -> String:
	return "Project A Enemy - Aggressive"
