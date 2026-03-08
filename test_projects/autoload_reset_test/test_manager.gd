extends Node

## Autoload singleton for testing mount/unmount lifecycle.
## Tracks how many times it has been instantiated and exposes its instance identity.

static var creation_count: int = 0

var _my_instance_id: int = 0

func _init() -> void:
	creation_count += 1
	_my_instance_id = creation_count
	print("TestManager created (instance #%d, object id %d)" % [_my_instance_id, get_instance_id()])

func _exit_tree() -> void:
	print("TestManager destroyed (instance #%d, object id %d)" % [_my_instance_id, get_instance_id()])

func get_creation_count() -> int:
	return creation_count

func get_my_instance_number() -> int:
	return _my_instance_id
