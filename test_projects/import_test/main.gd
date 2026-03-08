extends Node2D

func _ready():
	print("Import test project loaded")
	# Check if the texture resource loaded successfully.
	var sprite = $Sprite2D
	if sprite and sprite.texture:
		print("Texture loaded: " + str(sprite.texture.get_size()))
	else:
		print("WARNING: Texture not loaded (missing import?)")
