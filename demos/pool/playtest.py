#!/usr/bin/env python3
"""Play POOL through the relay and report what the boards were actually like.

    python3 demos/pool/playtest.py [boards] [hoard|shield|wave] [bot|good|ok|poor]

Needs an editor open on this project; it starts the game itself. Every design number in
`README.md` came out of this, and it exists so the next person does not rebuild it.

Three things in here are the whole method, and each cost a wrong conclusion to learn:

**Ask the game what state it is in; do not keep your own copy.** This tracked "is a
striker live" with a local flag, missed the re-serve whenever the sample loop stepped over
the frame the old ball vanished on, and then never launched the new one - producing two
200-second "stalls" with 96% dead time that were entirely this file. One of them was very
nearly fixed in the game, by clamping the ball away from horizontal to break an orbit that
was never happening.

**Play badly on purpose.** A bot that predicts the crossing point exactly loses no
strikers ever, so it cannot tell you anything about danger, difficulty, or whether the
loss condition exists at all. `SKILLS` gives it the two things a person has: a reaction
delay, and an error in where they think the ball is going. That single change turned "the
loss condition is decoration" into a difficulty curve.

**Measure the gaps, not the totals.** A board's problem is almost never its length; it is
how much of that length contains nothing. `dead_time_pct` is the number that moved POOL,
and `quiet_seconds` in the game exists because of it.

One persistent MCP session, not `poolrelay.py`'s one-shot calls: a control loop needs to
read and write several times a second, and half a second per call is not a control loop.
"""
import json, math, os, random, subprocess, sys, time

PROJECT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(PROJECT))
RELAY = os.environ.get("GODOT_AI_RELAY", os.path.join(REPO, "bin/godot-ai-relay"))
HOME = os.environ.get("GODOT_AI_HOME", "")
MAIN = "/root/Main"
POOL_W, POOL_H = 840.0, 560.0
PADDLE_Y, THICK = 526.0, 22.0
BALL_R = 6.5
CONTACT_Y = PADDLE_Y - THICK - BALL_R - 1.0
LOW, HIGH = 66.0, POOL_W - 66.0


class Session:
    def __init__(self):
        env = dict(os.environ)
        if HOME:
            env["GODOT_AI_HOME"] = HOME
        env["GODOT_AI_APPROVE_CLIENTS"] = "1"
        self.p = subprocess.Popen(
            [RELAY, "--mcp", "--project", PROJECT, "--approval-mode", "allow",
             "--client-name", "playtest"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            env=env)
        self.id = 0
        self._rpc("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                 "clientInfo": {"name": "playtest", "version": "1"}})
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})

    def _send(self, obj):
        self.p.stdin.write((json.dumps(obj) + "\n").encode())
        self.p.stdin.flush()

    def _rpc(self, method, params):
        self.id += 1
        mine = self.id
        self._send({"jsonrpc": "2.0", "id": mine, "method": method, "params": params})
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("relay closed")
            try:
                msg = json.loads(line.decode())
            except json.JSONDecodeError:
                continue
            if msg.get("id") == mine:
                return msg

    @staticmethod
    def _unwrap(msg):
        if "error" in msg:
            return {"_error": msg["error"]}
        res = msg.get("result", {})
        if res.get("isError"):
            return {"_error": " ".join(c.get("text", "") for c in res.get("content", []))}
        if "structuredContent" in res:
            return res["structuredContent"]
        try:
            return json.loads(res["content"][0]["text"])
        except Exception:
            return res

    def call(self, tool, args=None):
        return self._unwrap(self._rpc("tools/call", {"name": tool, "arguments": args or {}}))

    def multi(self, calls):
        ids = []
        for tool, args in calls:
            self.id += 1
            ids.append(self.id)
            self._send({"jsonrpc": "2.0", "id": self.id, "method": "tools/call",
                        "params": {"name": tool, "arguments": args or {}}})
        want, got = set(ids), {}
        while want:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("relay closed")
            try:
                msg = json.loads(line.decode())
            except json.JSONDecodeError:
                continue
            if msg.get("id") in want:
                want.discard(msg["id"])
                got[msg["id"]] = msg
        return [self._unwrap(got[i]) for i in ids]

    def gets(self, props, path=MAIN):
        rs = self.multi([("Godot_GetRuntimeProperty", {"path": path, "property": q})
                         for q in props])
        return [r.get("value", r.get("text")) if "_error" not in r else None for r in rs]

    def get(self, prop, path=MAIN):
        r = self.call("Godot_GetRuntimeProperty", {"path": path, "property": prop})
        return None if "_error" in r else r.get("value", r.get("text"))

    def set(self, prop, value, path=MAIN):
        return self.call("Godot_SetRuntimeProperty",
                         {"path": path, "property": prop, "value": value})

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=3)
        except Exception:
            self.p.kill()


def num(v, default=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def fold(x, lo=BALL_R, hi=POOL_W - BALL_R):
    span = hi - lo
    t = (x - lo) % (2 * span)
    if t < 0:
        t += 2 * span
    return lo + (t if t <= span else 2 * span - t)


READS = ("striker_position", "striker_in_play",
         "targets_left", "strikers_left", "score", "multiplier",
         "meter", "threads", "rim_hits", "ring_impacts", "targets_destroyed",
         "likes_collected", "likes_loose", "board_seconds", "quiet_seconds",
         "drain_open", "board_cleared", "board_failed", "run_over", "choosing_cocktail")


# How good the player is. The default bot predicts the striker's crossing point exactly,
# through the side walls, and moves the lounger there every loop - so it loses no strikers
# and can say nothing at all about danger or difficulty. Each level below adds the two
# things a person actually has: a reaction delay before the hand follows the eye, and an
# error in where they think the ball is going.
SKILLS = {
    "bot": (0.0, 0.0),
    "good": (0.15, 22.0),
    "ok": (0.30, 45.0),
    "poor": (0.50, 80.0),
}


def play_board(s, cap=200.0, current=-1.0, spend=None, skill="bot"):
    """One board. Returns a trace of samples plus a summary.

    `spend` is the meter policy: None hoards, "shield" raises the barrier whenever it is
    affordable and the ball is coming down, "wave" fires a Party Wave whenever the meter
    fills. Both count the times the policy wanted to spend and could not, which is the
    only direct measure of whether the two costs actually compete.
    """
    s.set("current", current)
    samples = []
    t0 = time.time()
    last, vel, launched = None, None, False
    meter_max = num(s.get("meter_max"), 100.0)
    shield_cost = num(s.get("shield_cost"), 35.0)
    wave_cost = num(s.get("party_wave_cost"), 100.0)
    denied = {"shield": 0, "wave": 0}
    spent = {"shield": 0, "wave": 0}
    wanted_at = 0.0
    react, sigma = SKILLS[skill]
    moved_at = -99.0
    aim_error = 0.0
    strikers_at_start = num(s.get("strikers_left"), 3.0)
    while time.time() - t0 < cap:
        v = s.gets(READS)
        pos = v[0]
        if not isinstance(pos, list) or len(pos) < 2:
            break
        x, y = pos[0], pos[1]
        now = time.time() - t0
        row = dict(t=now, x=x, y=y)
        for i, key in enumerate(READS[1:], start=1):
            row[key] = v[i]
        samples.append(row)
        if row["board_cleared"] or row["board_failed"] or row["run_over"] \
                or row["choosing_cocktail"]:
            break

        if last is not None:
            dt = now - last[0]
            if dt > 0:
                vx, vy = (x - last[1]) / dt, (y - last[2]) / dt
                sp = math.hypot(vx, vy)
                if sp > 50:
                    vel = (vx, vy)
        last = (now, x, y)

        # Whether a striker is live is the game's own answer, not a flag kept out here.
        # Tracking it locally - set on launch, cleared when the ball is seen below the
        # pool - misses the re-serve whenever the sample loop steps over that window,
        # after which the bot never launches again and the board sits untouched. That
        # produced two 200-second "stalls" with 96% dead time that were entirely this
        # harness, and one of them was very nearly diagnosed as a defect in the game.
        launched = bool(row["striker_in_play"])
        if not launched:
            last = vel = None
            s.set("launch_requested", True)
            continue
        if vel is None:
            continue
        vx, vy = vel
        if vy > 1.0 and now - moved_at >= react:
            t = (CONTACT_Y - y) / vy
            if t >= 0:
                if sigma > 0.0:
                    # A fresh guess per commitment, not per frame: a person decides where
                    # the ball is going and then moves, they do not jitter around the
                    # right answer at nine hertz, which would average out to no error.
                    aim_error = random.gauss(0.0, sigma)
                moved_at = now
                s.set("paddle_x", min(max(fold(x + vx * t) + aim_error, LOW), HIGH))

        if spend == "shield" and vy > 1.0 and y > 380.0 and now - wanted_at > 2.0:
            wanted_at = now
            if num(row["meter"]) >= shield_cost:
                s.set("shield_requested", True)
                spent["shield"] += 1
            else:
                denied["shield"] += 1
        elif spend == "wave" and now - wanted_at > 2.0:
            wanted_at = now
            if num(row["meter"]) >= wave_cost:
                s.set("party_wave_requested", True)
                spent["wave"] += 1
            else:
                denied["wave"] += 1
    lost = 0
    if samples:
        lost = int(max(0.0, strikers_at_start
                       - num(samples[-1]["strikers_left"], strikers_at_start)))
    return samples, meter_max, spent, denied, lost


def summarise(samples, meter_max):
    if not samples:
        return {}
    total = samples[-1]["board_seconds"]
    total = num(total) if num(total) > 0 else samples[-1]["t"]

    def ev(row):
        return (num(row["threads"]) + num(row["rim_hits"]) + num(row["ring_impacts"])
                + num(row["targets_destroyed"]) + num(row["likes_collected"]))

    # Dead time: consecutive samples with no scoring event between them, over 3s apart
    # from the last one that had one.
    dead = 0.0
    quiet_run = 0.0
    last_ev = ev(samples[0])
    for a, b in zip(samples, samples[1:]):
        dt = b["t"] - a["t"]
        if ev(b) > last_ev:
            last_ev = ev(b)
            quiet_run = 0.0
        else:
            quiet_run += dt
            if quiet_run > 3.0:
                dead += dt
    at_max = sum(b["t"] - a["t"] for a, b in zip(samples, samples[1:])
                 if num(b["meter"]) >= meter_max - 0.01)
    start = num(samples[0]["targets_left"])
    quarters = []
    for q in range(4):
        want = start * (1.0 - (q + 1) / 4.0)
        hit = next((r["t"] for r in samples if num(r["targets_left"]) <= want), None)
        quarters.append(hit)
    span = samples[-1]["t"] - samples[0]["t"]
    # How much of the board the ball spent on a near-horizontal line, estimated from
    # consecutive positions. A sustained shallow orbit between the side walls is the
    # failure `Striker.min_vertical` exists to prevent, and this is the measurement that
    # says whether it happens at all.
    shallow = moving = 0
    for a, b in zip(samples, samples[1:]):
        dx, dy = b["x"] - a["x"], b["y"] - a["y"]
        step = math.hypot(dx, dy)
        if step < 10.0:
            continue
        moving += 1
        if abs(dy) / step < 0.18:
            shallow += 1
    return dict(
        shallow_pct=round(100.0 * shallow / max(1, moving), 1),
        seconds=round(span, 1),
        rings_at_start=int(start),
        cleared=bool(samples[-1]["board_cleared"]),
        failed=bool(samples[-1]["board_failed"]),
        score=int(num(samples[-1]["score"])),
        multiplier_peak=round(max(num(r["multiplier"]) for r in samples), 2),
        multiplier_end=round(num(samples[-1]["multiplier"]), 2),
        threads=int(num(samples[-1]["threads"])),
        rim_hits=int(num(samples[-1]["rim_hits"])),
        ring_impacts=int(num(samples[-1]["ring_impacts"])),
        destroyed=int(num(samples[-1]["targets_destroyed"])),
        likes_collected=int(num(samples[-1]["likes_collected"])),
        likes_left_in_water=int(num(samples[-1]["likes_loose"])),
        strikers_left=int(num(samples[-1]["strikers_left"])),
        dead_time_pct=round(100.0 * dead / max(0.001, span), 1),
        meter_at_max_pct=round(100.0 * at_max / max(0.001, span), 1),
        quarter_times=[None if q is None else round(q, 1) for q in quarters],
        drain_used=any(bool(r["drain_open"]) for r in samples),
        max_quiet=round(max(num(r["quiet_seconds"]) for r in samples), 1),
    )


def main():
    boards = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    spend = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] != "hoard" else None
    skill = sys.argv[3] if len(sys.argv) > 3 else "bot"
    s = Session()
    r = s.call("Godot_PlayMainScene", {})
    print("play:", json.dumps(r)[:200])
    time.sleep(3.0)
    s.set("restart_requested", True)
    time.sleep(1.0)
    out = []
    for i in range(boards):
        samples, meter_max, spent, denied, lost = play_board(s, spend=spend, skill=skill)
        summary = summarise(samples, meter_max)
        summary["board"] = i + 1
        summary["skill"] = skill
        summary["strikers_lost"] = lost
        summary["policy"] = spend or "hoard"
        summary["spent"] = spent
        summary["denied"] = denied
        summary["last_event"] = s.get("last_event")
        out.append(summary)
        print(json.dumps(summary))
        sys.stdout.flush()
        offer = s.get("cocktail_offer")
        if isinstance(offer, list) and offer:
            s.set("cocktail_choice", offer[0])
            time.sleep(0.8)
        elif num(s.get("run_over")):
            s.set("restart_requested", True)
            time.sleep(1.0)
    # The verdict, which is the point. A list of per-board JSON is evidence; these four
    # numbers are what a design decision is actually made on.
    done = [b for b in out if b]
    if done:
        lengths = sorted(b["seconds"] for b in done)
        median = lengths[len(lengths) // 2]
        print("\n%d boards as '%s': median %.0fs, dead time %.0f%%, "
              "%.1f strikers lost a board, %d/%d cleared, meter at max %.0f%% of the time"
              % (len(done), skill, median,
                 sum(b["dead_time_pct"] for b in done) / len(done),
                 sum(b["strikers_lost"] for b in done) / len(done),
                 sum(1 for b in done if b["cleared"]), len(done),
                 sum(b["meter_at_max_pct"] for b in done) / len(done)))

    # Never into the project: Godot would import it, and a measurement is not an asset.
    out_path = os.environ.get("POOL_PLAYTEST_OUT")
    if out_path:
        with open(out_path, "w") as f:
            json.dump(out, f, indent=1)
        print("wrote %s" % out_path)
    s.close()


if __name__ == "__main__":
    main()
