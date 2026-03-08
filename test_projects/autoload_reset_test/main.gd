extends Control

@onready var label: Label = $Label

func _ready() -> void:
	var tm = TestManager
	var info := "Autoload Reset Test\n"
	info += "TestManager object id: %d\n" % tm.get_instance_id()
	info += "TestManager instance #: %d\n" % tm.get_my_instance_number()
	info += "Total creations: %d" % tm.get_creation_count()
	label.text = info
	print("Autoload test scene ready - TestManager instance #%d (object id %d)" % [tm.get_my_instance_number(), tm.get_instance_id()])
