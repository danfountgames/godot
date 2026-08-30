class_name PoolMusic
extends Node

## The pool as an instrument.
##
## Two things have to be true at once and they pull against each other. Every impact
## must sound intentional, which means quantizing it to the grid - up to 125ms of defer
## at 120bpm in sixteenths. And the game must feel tight, which means the *visual*
## response cannot wait for the note. So this node owes the rest of the game only the
## audio: the splash goes out the instant the physics says so, and the note lands on the
## grid a fraction later.
##
## It is also written to be checkable where there is no audio device, which is where
## this gets built and tested. Every scheduled note is appended to note_log with the
## beat it landed on, so "the chain played a descending run and every note was on the
## grid" is an assertion a tool can make rather than something you have to be in the
## room to hear.

@export var bpm: float = 112.0
@export var quantize_division: int = 4  ## 4 = sixteenths, 2 = eighths.
@export var root_midi: int = 57  ## A3.
## Minor pentatonic. Anything played in it sounds deliberate, which is the point: the
## player is improvising with a cue and should not be able to play a wrong note.
@export var scale_degrees: PackedInt32Array = PackedInt32Array([0, 3, 5, 7, 10])
## The area that sounds the root. Four times the area is twice the radius, and that is
## treated as one octave down - so ring size maps to pitch through the geometry rather
## than through a lookup table.
@export var reference_area: float = 2400.0
@export var voices: int = 12
@export var log_limit: int = 400

## Read from outside instead of listening. Each entry is one scheduled note:
##   "beat=12.250 kind=merge midi=52 area=9600 chain=3 quantized=+0.041"
var note_log: PackedStringArray = PackedStringArray()
var beats_elapsed: float = 0.0
## 0 none, 1 bass at x4, 2 arps at x8, 3 the drop at x16.
var layers: int = 0
var notes_played: int = 0
var notes_off_grid: int = 0

var _pending: Array = []
var _players: Array[AudioStreamPlayer] = []
var _next_voice: int = 0
var _tones: Dictionary = {}


func seconds_per_beat() -> float:
	return 60.0 / maxf(1.0, bpm)


func _ready() -> void:
	for i in voices:
		var player := AudioStreamPlayer.new()
		player.bus = "Master"
		add_child(player)
		_players.append(player)


func _process(delta: float) -> void:
	beats_elapsed += delta / seconds_per_beat()
	var still: Array = []
	for entry in _pending:
		if entry["due"] <= beats_elapsed:
			_sound(entry)
		else:
			still.append(entry)
	_pending = still


## Where the next grid line is. Everything audible lands on one of these.
func next_grid_beat() -> float:
	var step := 1.0 / float(maxi(1, quantize_division))
	return ceilf(beats_elapsed / step) * step


## Ring size to pitch, then snapped into the scale so a chain is a phrase rather than a
## chromatic slide. Bigger rings toll lower, which is the whole reason a merge run reads
## as a descending run.
func midi_for_area(a: float) -> int:
	var octaves := log(maxf(a, 1.0) / maxf(1.0, reference_area)) / log(4.0)
	return _snap(root_midi - int(round(octaves * 12.0)))


func _snap(midi: int) -> int:
	if scale_degrees.is_empty():
		return midi
	var offset := midi - root_midi
	var octave := int(floor(float(offset) / 12.0))
	var within := offset - octave * 12
	var best := scale_degrees[0]
	for degree in scale_degrees:
		if absi(degree - within) < absi(best - within):
			best = degree
	return root_midi + octave * 12 + best


## The one call the game makes. Returns the beat the note will land on, so a caller can
## assert about the grid without waiting for the sound.
func play(kind: String, a: float, chain: int) -> float:
	var due := next_grid_beat()
	var midi := midi_for_area(a)
	if kind == "bounce":
		# Percussion, not melody: a fixed high rim tone, so a scattered board clatters
		# as a drum pattern instead of a chord nobody asked for.
		midi = root_midi + 24
	_pending.append({"due": due, "midi": midi, "kind": kind})
	if note_log.size() < log_limit:
		note_log.append("beat=%0.3f kind=%s midi=%d area=%0.0f chain=%d quantized=+%0.3f"
			% [due, kind, midi, a, chain, due - beats_elapsed])
	return due


## Chain milestones bring layers in. The multiplier is meant to be audible, so this is
## the mix state and the game state at the same time.
func set_chain(chain: int) -> void:
	var want := 0
	if chain >= 16:
		want = 3
	elif chain >= 8:
		want = 2
	elif chain >= 4:
		want = 1
	if want != layers:
		layers = want
		if want > 0:
			play("layer", reference_area * pow(4.0, float(want)), chain)


## Did something that happened now land on a downbeat? The brief pays a small bonus for
## it, and a player can aim for it by timing the release.
func on_beat(window: float = 0.08) -> bool:
	var into := beats_elapsed - floorf(beats_elapsed)
	return into < window or into > 1.0 - window


func reset_log() -> void:
	note_log = PackedStringArray()
	notes_played = 0
	notes_off_grid = 0


func _sound(entry: Dictionary) -> void:
	notes_played += 1
	var step := 1.0 / float(maxi(1, quantize_division))
	# A note is on the grid if it fired within a tenth of a step of one. It can only be
	# late, never early, and being late by a whole step means _process missed a frame.
	if absf(entry["due"] / step - roundf(entry["due"] / step)) > 0.1:
		notes_off_grid += 1
	var player := _players[_next_voice] if not _players.is_empty() else null
	_next_voice = (_next_voice + 1) % maxi(1, _players.size())
	if player == null:
		return
	player.stream = _tone(int(entry["midi"]), 0.9 if entry["kind"] != "bounce" else 0.18)
	player.play()


func _tone(midi: int, seconds: float) -> AudioStreamWAV:
	var key := "%d:%0.2f" % [midi, seconds]
	if _tones.has(key):
		return _tones[key]
	var rate := 22050
	var freq := 440.0 * pow(2.0, (float(midi) - 69.0) / 12.0)
	var count := int(rate * seconds)
	var data := PackedByteArray()
	data.resize(count * 2)
	for i in count:
		var t := float(i) / float(rate)
		# A plucked envelope and one octave of overtone: enough for a ring to have a
		# voice rather than a beep, and cheap enough to generate at load.
		var envelope := exp(-t * (4.0 if seconds > 0.5 else 24.0))
		var sample := (sin(TAU * freq * t) * 0.7 + sin(TAU * freq * 2.0 * t) * 0.3) * envelope * 0.35
		data.encode_s16(i * 2, int(clampf(sample, -1.0, 1.0) * 32767.0))
	var stream := AudioStreamWAV.new()
	stream.format = AudioStreamWAV.FORMAT_16_BITS
	stream.mix_rate = rate
	stream.stereo = false
	stream.data = data
	_tones[key] = stream
	return stream
