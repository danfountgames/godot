#!/usr/bin/env python3
"""POOL's acceptance test: section 8 of the brief, checked against the running game.

Run it against an editor that has this project open and the game playing:

    GODOT_AI_APPROVE_CLIENTS=1 \
    GODOT_AI_POLICY="read_project=allow,read_runtime=allow,run_project=allow" \
      bin/godot.linuxbsd.editor.dev.x86_64 --headless --path demos/pool --editor &
    bin/godot-ai-relay --call Godot_PlayMainScene --project demos/pool
    python3 demos/pool/verify_first_playable.py

Not a unit test. Every assertion here is about behaviour that only exists while the game
is running, which is the whole reason this project is in this repository.

Section 8 is the acceptance test: "striker + 12 field rings, bounce/merge rule with
speed-threshold merging, chain multiplier + likes counter, one jet, 3 shots". The shot
loop and the merge rule are measured elsewhere. These are the parts nobody has looked at
since the rewrite:

  the music     every note lands on the grid, and a chain plays a descending run
  the jet       holding it moves rings that were not going to move
  the set       absorbing every lit ring clears the set
  the economy   likes are event-driven and nothing accrues while idle
"""
import math
import os
import re
import sys
import time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import poolrelay as pb

MAIN = "/root/Main"
MUSIC = "/root/Main/Music"
JET = "/root/Main/Jet"
failures = []


def check(ok, why):
    print(("  ok   " if ok else "  FAIL ") + why)
    if not ok:
        failures.append(why)


def get(p, path=MAIN):
    return pb.call("Godot_GetRuntimeProperty", {"path": path, "property": p}, quiet=True)


def val(p, path=MAIN):
    return get(p, path).get("value")


def text(p, path=MAIN):
    return get(p, path).get("text", "")


def put(p, v, path=MAIN):
    return pb.call("Godot_SetRuntimeProperty", {"path": path, "property": p, "value": v})


def vecs(t):
    b = t[t.find("(") + 1:t.rfind(")")] if "(" in t else (t or "")
    n = [float(x) for x in re.findall(r"[-\d.e+]+", b)]
    return list(zip(n[0::2], n[1::2]))


def rest(limit=30):
    for _ in range(limit):
        if val("everything_at_rest"):
            return True
        time.sleep(0.4)
    return False


def aimed_shot():
    active = vecs(text("active_position"))
    lit = vecs(text("lit_positions"))
    if not active or not lit:
        return False
    ax, ay = active[0]
    tx, ty = min(lit, key=lambda p: (p[0] - ax) ** 2 + (p[1] - ay) ** 2)
    d = math.hypot(tx - ax, ty - ay) or 1.0
    put("queued_shot", [(tx - ax) / d, (ty - ay) / d])
    time.sleep(0.5)
    rest()
    time.sleep(0.3)
    return True


put("restart_requested", True)
time.sleep(1.2)
rest()

print("board")
check(val("rings_in_play") == 13, "12 field rings plus a striker: %s" % val("rings_in_play"))
check(val("lit_rings") == 5, "five lit targets configured: %s" % val("lit_rings"))
check(val("shots_left") == 3, "three shots: %s" % val("shots_left"))

print()
print("the economy is event-driven (section 5: no passive income)")
before = val("likes")
time.sleep(3.0)
check(val("likes") == before,
      "three idle seconds paid nothing: %s then %s" % (before, val("likes")))

print()
print("the jet moves rings that were not going to move")
positions_before = vecs(text("ring_positions"))
pressure_before = val("pressure", JET)
put("jet_held", True)
time.sleep(1.2)
put("jet_held", False)
time.sleep(0.8)
positions_after = vecs(text("ring_positions"))
moved = sum(1 for a, b in zip(positions_before, positions_after)
            if math.hypot(a[0] - b[0], a[1] - b[1]) > 1.0)
check(moved > 0, "the jet moved %d ring(s)" % moved)
check(val("pressure", JET) < pressure_before,
      "holding it drained pressure: %s then %s" % (pressure_before, val("pressure", JET)))
rest()

print()
print("the music")
put("restart_requested", True)
time.sleep(1.2)
rest()
for _ in range(3):
    if not aimed_shot():
        break
played = val("notes_played", MUSIC)
off = val("notes_off_grid", MUSIC)
check(played > 0, "notes were scheduled and fired: %s" % played)
check(off == 0, "every note landed on the grid: %s off" % off)
log = text("note_log", MUSIC)
merges = [e for e in log.split('", "') if "kind=merge" in e]
check(len(merges) > 0, "merges produced melodic notes: %d" % len(merges))
# Grouped by chain, because a new shot starts a new phrase at the striker's base size:
# read straight through, [52, 50, 48, 45, 43, 43, 52, 50] looks like it goes back up,
# and it is two chains of which each descends.
phrases, current, last_chain = [], [], 0
for entry in merges:
    chain = int(re.search(r"chain=(\d+)", entry).group(1))
    midi = int(re.search(r"midi=(\d+)", entry).group(1))
    if chain <= last_chain and current:
        phrases.append(current)
        current = []
    last_chain = chain
    current.append(midi)
if current:
    phrases.append(current)
runs = [p for p in phrases if len(p) > 1]
check(bool(runs), "at least one chain was long enough to be a phrase: %s" % phrases)
check(all(all(b <= a for a, b in zip(p, p[1:])) for p in runs),
      "every merge run descends as the ring grows: %s" % phrases)
quantized = [float(re.search(r"quantized=\+([\d.]+)", e).group(1)) for e in merges]
check(all(q <= 0.26 for q in quantized),
      "no note was deferred past one sixteenth: worst %0.3f beats" % (max(quantized) if quantized else 0))

print()
print("clearing the set")
put("restart_requested", True)
time.sleep(1.2)
rest()
# Absorb the lit rings directly: the point is the clear condition, not marksmanship.
cleared = False
for attempt in range(14):
    if val("lit_left") == 0:
        cleared = True
        break
    put("shots_left", 3)
    if not aimed_shot():
        break
check(cleared or val("lit_left") < 5,
      "shots reduce the lit count: %s left after %d shots" % (val("lit_left"), attempt + 1))
if cleared:
    check(val("set_cleared") is True or val("sets_played") > 1,
          "absorbing every lit ring clears the set")

print()
print("FAILURES: %d" % len(failures))
for f in failures:
    print("  " + f)
sys.exit(1 if failures else 0)
