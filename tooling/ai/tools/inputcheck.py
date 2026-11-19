#!/usr/bin/env python3
"""End-to-end check of the keyboard/mouse input layer in the native build.

Drives the game with PS2X_HOST_INPUT_SCRIPT (host key / mouse / focus events
on the pad script clock, mapped through controls.ini exactly like real input;
see z-axis runtime/include/runtime/ps2_host_input.h) on top of the
ref04-talk-go pad script, which reaches free play in the Tutorial. Each
scenario uses its own controls.ini (PS2X_CONTROLS_INI, never the user's) and
memory card folder, and checks game state:

  defaults  W moves the skater; mouse motion swings the camera (eye/look-at)
            and it springs back; W+D held across a focus loss: the pad report
            drops to idle while unfocused and comes back after refocus, and
            the skater stops turning while unfocused; after a recapture
            click, mouse up tilts the view up.
  rebind    controls.ini rebinds Forward to T and inverts mouse x: W does
            nothing, T moves, mouse right swings the camera the other way.
            (Both use [mouse] mode = stick: the mouse pushes the right stick.)
  look      mouse-look (the default mode) in the Tutorial: 600 px right turns
            the view 120 degrees (past the stick's 60), it stays there,
            skating through a turn keeps it at that angle to the skater, R
            recentres, mouse up tilts the view 20 degrees up, motion in the
            pause menu is dropped and the angle survives the pause, a lesson
            restart from the pause menu (scripted intro camera) recentres
            it and motion during the intro is dropped.
  lookcareer  mouse-look in MOVIE LOT, on the Career replay of
            careercheck.py: motion during the fly-thru is dropped (scripted
            camera), yaw 120 / pitch 20 at the level start, motion in the
            pause menu and save dialog is dropped, and on the grind rail the
            mouse turns the camera (grind = player state 5, a follow-camera
            state). The skater's state matches the Career reference up to
            the grind (mouse-look does not touch game logic).

Usage:
  RUNNER=<ps2EntryRunner> python3 tools/inputcheck.py run [defaults|rebind|look|lookcareer ...]
  python3 tools/inputcheck.py check [...]   # re-check logs

Logs: recomp/logs/<tag>input-check-<scenario>.log, screenshots (PS2X_SHOT_AT,
the presented picture at a script time) in recomp/logs/input/<tag><scenario>-*.png.
INPUTCHECK_TAG=<tag> prefixes those names (and the config / memory card
folders under recomp/run), so parallel agents do not overwrite each other.
"""
import math
import os
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECOMP = os.path.join(ROOT, "recomp")
LOGS = os.path.join(RECOMP, "logs")
sys.path.insert(0, os.path.dirname(ROOT))
import zpaths  # noqa: E402,F401  (the shared environment: no sound, ZAXIS_AUDIO)
SHOTS = os.path.join(LOGS, "input")
PAD_SCRIPT = os.path.join(ROOT, "tools", "scripts", "native", "ref04-talk-go.padscript")

TAG = os.environ.get("INPUTCHECK_TAG", "")
POS = (0x2A76F0, 0x2A76F8)              # player x, z
EYE = (0x24C9E0, 0x24C9E4, 0x24C9E8)    # camera eye x, y, z
LOOK = (0x24C9F0, 0x24C9F4, 0x24C9F8)   # camera look-at x, y, z
PAD_BUF = (0x282494, 0x282498)  # the game's scePadRead buffer: status/buttons, sticks RX RY LX LY
VPS = 59.94

MODE = 0x276FE4                          # 2 play, 3 paused / message box
STICK_MODE = "[mouse]\nmode = stick\n"  # the mouse pushes the right stick

# Career replay (careercheck.py): script times are reference vsyncs - 461.
# Natively loading is ~1240 vsyncs shorter: the fly-thru runs at script
# ~1112-2201 and the level-start anchor then moves the clock to 3047.
CAREER_FLYTHRU = (1112, 2201)
CAREER_LEVEL_START = 3047  # car05-level-start: play begins after the fly-thru
CAREER_PAUSE = 3082        # the pause menu opens (car06-pause is at 3117)
CAREER_GRIND = 4368        # car12-grind: on the rail

SCENARIOS = {
    "defaults": {
        "ini": STICK_MODE,
        "host": [
            "44:KEY=MOUSE_LEFT:0.1",        # click: captures the cursor, not sent to the game
            "46:KEY=W:1.5",                 # forward
            "49:KEY=S:2",                   # brake
            "52:MOUSE=300/0:0.5",           # mouse right
            "55:KEY=W:5", "55:KEY=D:5",     # forward + right turn ...
            "57:FOCUS=0", "58.5:FOCUS=1",   # ... across a focus loss
            "61:KEY=S:2",
            "63:KEY=MOUSE_LEFT:0.1",        # recapture after the focus loss
            "64:MOUSE=0/-200:0.4",          # mouse up: look up (invert_y = true)
        ],
        "shots": {"before-move": 45.9, "after-move": 47.6, "before-mouse": 51.9,
                  "mouse-peak": 52.75, "mouse-return": 54.5, "before-mouse-up": 63.9,
                  "mouse-up": 64.6},
        "secs": 140,
    },
    "rebind": {
        "ini": "[sticks]\nleft_up = T\n" + STICK_MODE + "invert_x = true\nsensitivity_x = 2.0\n",
        "host": [
            "44:KEY=MOUSE_LEFT:0.1",
            "46:KEY=W:1",                   # unbound now: nothing
            "48:KEY=T:1.5",                 # forward
            "51:KEY=S:2",
            "54:MOUSE=150/0:0.5",           # 150 px at sensitivity 2 = 300 px, inverted
        ],
        "shots": {"w-pressed": 47.2, "after-i": 49.6, "before-mouse": 53.9, "mouse-peak": 54.75},
        "secs": 120,
    },
    "look": {
        "ini": None,  # defaults: mode = look
        "host": [
            "44:KEY=MOUSE_LEFT:0.1",        # capture
            "46:MOUSE=600/0:0.5",           # 600 px * 0.2 = view 120 deg right
            "48:KEY=W:3", "48.5:KEY=D:2.5", # skate through a right turn
            "52:KEY=R:0.1",                 # recentre
            "54:MOUSE=0/-100:0.3",          # mouse up: view 20 deg up
            "56:KEY=ENTER:0.1",             # pause
            "57:MOUSE=300/0:0.3",           # ... motion in the pause menu: dropped
            "59:KEY=ENTER:0.1",             # resume
            "61:KEY=ENTER:0.1",             # pause again
            "62:KEY=DOWN:0.1",              # "Talking to people"
            "63:KEY=SPACE:0.1",             # restart that lesson: scripted intro
            "65:MOUSE=300/0:0.3",           # motion during the intro: dropped
        ],
        "shots": {"before": 45.9, "yaw120": 46.8, "held": 47.8, "turning": 49.5, "turned": 51.0,
                  "recentred": 53.5, "pitch-up": 54.6, "pause": 56.8, "pause-moved": 58.0,
                  "resumed": 60.0, "restart-intro": 65.5},
        "secs": 170,
    },
    "lookcareer": {
        "ini": None,
        "career": True,
        "host": [
            "1400v:KEY=MOUSE_LEFT:6v",      # capture during the fly-thru
            "1500v:MOUSE=400/0:40v",        # fly-thru: dropped
            "3052v:MOUSE=600/-100:20v",     # level start: yaw 120, pitch 20
            "3200v:MOUSE=-500/0:30v",       # pause menu: dropped
            "4340v:MOUSE=-450/0:30v",       # on the rail: yaw 120 -> 30
        ],
        "shots": {"flythru-before": "1490v", "flythru-moved": "1560v", "start": "3050v",
                  "yaw120": "3078v", "pause-moved": "3250v", "resumed": "4300v",
                  "grind-before": "4335v", "grind-turned": "4380v", "grind-late": "4420v"},
        "secs": 330,
    },
}


def f32(word):
    return struct.unpack("<f", struct.pack("<I", int(word, 16)))[0]


def log_name(name):
    return "%sinput-check-%s" % (TAG, name)


def shot_time(t):
    return t if isinstance(t, str) else "%s" % t


def run(name):
    sc = SCENARIOS[name]
    os.makedirs(SHOTS, exist_ok=True)
    cfg_dir = os.path.join(RECOMP, "run", TAG + "input-cfg")
    os.makedirs(cfg_dir, exist_ok=True)
    ini = os.path.join(cfg_dir, name + ".ini")
    if os.path.exists(ini):
        os.remove(ini)
    if sc["ini"] is not None:
        with open(ini, "w") as out:
            out.write(sc["ini"])
    mc = os.environ.get("INPUTCHECK_MC") or os.path.join(RECOMP, "run", TAG + "mc-input")
    mc = os.path.join(mc, name)
    if sc.get("career") and os.path.isdir(mc):
        shutil.rmtree(mc)  # the Career replay starts from an empty card and saves to it
    os.makedirs(mc, exist_ok=True)
    env = dict(os.environ)
    env.setdefault("PS2X_GS_BACKEND", "cpu")  # pictures from the reference renderer
    # The memory card path: the Career replay's inputs include the card's
    # save dialog (careercheck.py).
    env.setdefault("PS2X_SAVES", "card")
    if sc.get("career"):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import careercheck
        pad = careercheck.native_script()
    else:
        pad = open(PAD_SCRIPT).read().strip()
    env.update({
        "PS2X_MC_ROOT": mc,
        "PS2X_CONTROLS_INI": ini,
        "PS2X_PAD_SCRIPT_CLOCK": "vsync",
        "PS2X_PAD_SCRIPT": pad,
        "PS2X_HOST_INPUT_SCRIPT": ",".join(sc["host"]),
        "PS2X_SHOT_AT": ",".join("%s:%s" % (shot_time(t), os.path.join(SHOTS, "%s%s-%s.png" % (TAG, name, label)))
                                 for label, t in sc["shots"].items()),
        "PS2X_TRACE_HOST_INPUT": "1",
        "PS2X_TRACE_MOUSELOOK": "1",
        "PS2X_STATE_LOG": ",".join("0x%X" % a for a in POS + EYE + LOOK + PAD_BUF + (MODE,)),
    })
    subprocess.call([os.path.join(RECOMP, "run.sh"), str(sc["secs"]), log_name(name)], env=env)


def load(name):
    states, inputs, looks, placed, anchors = [], [], [], [], []
    origin = None
    for line in open(os.path.join(LOGS, log_name(name) + ".log"), errors="replace"):
        if line.startswith("[pad] script origin"):
            origin = int(line.split()[-1])
            anchors.append((origin, 0))
        elif line.startswith("[pad] sync"):
            m = re.match(r"\[pad\] sync \d+ at script (\d+)v \(tick (\d+)\)", line)
            if m:
                anchors.append((int(m.group(2)), int(m.group(1))))
        elif line.startswith("[input] look"):
            m = dict(re.findall(r"(\w+)=(-?[\d.]+)", line))
            looks.append({k: float(v) for k, v in m.items()})
        elif line.startswith("[mouselook]"):
            m = dict(re.findall(r"(\w+)=(\w+)", line))
            placed.append(m)
        elif line.startswith("[state]"):
            m = dict(re.findall(r"(\w+)=(\w+)", line))
            states.append((int(m["tick"]), m))
        elif line.startswith("[input] tick="):
            m = dict(re.findall(r"(\w+)=(-?\w+)", line))
            inputs.append(m)
    return origin, states, inputs, looks, placed, anchors


class Timeline:
    def __init__(self, name):
        self.origin, self.states, self.inputs, self.looks, self.placed, self.anchors = load(name)
        self.ticks = [t for t, _ in self.states]

    def tick_of(self, script_vsyncs):
        """Emulated tick at a script time: the script clock runs with the
        tick between anchors (pad script SYNC/WAIT entries move it)."""
        base = None
        for tick, script in self.anchors:
            if script <= script_vsyncs:
                base = (tick, script)
        if base is None:
            base = (self.origin, 0)
        return base[0] + int(round(script_vsyncs - base[1]))

    def at(self, seconds):
        """State at script time (seconds, or "<n>v" vsyncs): last [state]
        line at or before it."""
        tick = self.tick_of(vsyncs(seconds))
        lo, hi = 0, len(self.ticks)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.ticks[mid] <= tick:
                lo = mid + 1
            else:
                hi = mid
        return self.states[lo - 1][1] if lo else None

    def last_script_seconds(self):
        return (self.ticks[-1] - self.origin) / VPS if self.ticks else 0.0

    def mode(self, s):
        return int(self.at(s)["276fe4"], 16)

    def look(self, s):
        """Mouse-look state (yaw, pitch, active) at script time s: the last
        [input] look trace line at or before it (lines are logged on change)."""
        t = vsyncs(s)
        last = {"yaw": 0.0, "pitch": 0.0, "active": 0.0}
        for m in self.looks:
            if m["script"] <= t:
                last = m
        return last["yaw"], last["pitch"], bool(last["active"])

    def look_changes(self, a, b):
        """[input] look lines with script time in (a, b]."""
        ta, tb = vsyncs(a), vsyncs(b)
        return [m for m in self.looks if ta < m["script"] <= tb]

    def pos(self, s):
        m = self.at(s)
        return f32(m["2a76f0"]), f32(m["2a76f8"])

    def cam_yaw(self, s):
        m = self.at(s)
        ex, ez, lx, lz = (f32(m[k]) for k in ("24c9e0", "24c9e8", "24c9f0", "24c9f8"))
        return math.degrees(math.atan2(lz - ez, lx - ex))

    def cam_pitch(self, s):
        """Degrees, positive = looking up (PS2 y points down)."""
        m = self.at(s)
        ex, ey, ez, lx, ly, lz = (f32(m[k]) for k in ("24c9e0", "24c9e4", "24c9e8", "24c9f0", "24c9f4", "24c9f8"))
        return math.degrees(math.atan2(-(ly - ey), math.hypot(lx - ex, lz - ez)))

    def pad_sticks(self, s):
        """Game-side pad buffer: (RX, RY, LX, LY) bytes."""
        w = int(self.at(s)["282498"], 16)
        return w & 0xFF, (w >> 8) & 0xFF, (w >> 16) & 0xFF, (w >> 24) & 0xFF

    def report(self, s):
        """Last [input] trace line at or before script time s (seconds)."""
        t = s * VPS
        last = None
        for m in self.inputs:
            if int(m["script"]) <= t:
                last = m
        return last


def vsyncs(t):
    """Script time in vsyncs from seconds or "<n>v"."""
    if isinstance(t, str) and t.endswith("v"):
        return float(t[:-1])
    return float(t) * VPS


def wrap(deg):
    return (deg + 180.0) % 360.0 - 180.0


def heading(tl, a, b):
    (x0, z0), (x1, z1) = tl.pos(a), tl.pos(b)
    return math.degrees(math.atan2(z1 - z0, x1 - x0)), math.hypot(x1 - x0, z1 - z0)


def check(name):
    tl = Timeline(name)
    results = []

    def ok(cond, what):
        results.append((bool(cond), what))

    end = tl.last_script_seconds()
    if name == "defaults":
        ok(end > 65, "run reached script time 65 s (got %.1f)" % end)
        p0, p1 = tl.pos(45.9), tl.pos(47.6)
        ok(math.hypot(p1[0] - p0[0], p1[1] - p0[1]) > 3,
           "W moves the skater: (%.2f, %.2f) -> (%.2f, %.2f)" % (p0 + p1))
        ok(tl.pad_sticks(46.5)[3] < 0x20, "game pad buffer LY while W held: 0x%02X" % tl.pad_sticks(46.5)[3])
        y0, y1, y2 = tl.cam_yaw(51.9), tl.cam_yaw(52.75), tl.cam_yaw(54.5)
        ok(abs(y1 - y0) > 20, "mouse right swings the camera: yaw %.1f -> %.1f deg" % (y0, y1))
        ok(abs(y2 - y0) < 3, "camera springs back after the mouse stops: yaw %.1f deg" % y2)
        ok(tl.pad_sticks(52.4)[0] > 0xC0, "game pad buffer RX during mouse motion: 0x%02X" % tl.pad_sticks(52.4)[0])
        r_held, r_lost, r_back = tl.report(56.8), tl.report(58.0), tl.report(59.5)
        ok(r_held and r_held["ly"] == "01" and r_held["lx"] == "ff",
           "W+D held, focused: report lx=%s ly=%s" % (r_held and r_held["lx"], r_held and r_held["ly"]))
        ok(r_lost and r_lost["ly"] == "80" and r_lost["lx"] == "80" and r_lost["focused"] == "0"
           and r_lost["captured"] == "0",
           "focus lost: report idle and cursor released (lx=%s ly=%s captured=%s)"
           % (r_lost and r_lost["lx"], r_lost and r_lost["ly"], r_lost and r_lost["captured"]))
        ok(tl.pad_sticks(58.0)[2:] == (0x80, 0x80), "game pad buffer LX/LY while unfocused: %s"
           % (tuple("0x%02X" % v for v in tl.pad_sticks(58.0)[2:]),))
        ok(r_back and r_back["ly"] == "01" and r_back["focused"] == "1",
           "refocused with W+D still held: report ly=%s" % (r_back and r_back["ly"]))
        h_turn = heading(tl, 56.0, 56.9)[0] - heading(tl, 55.5, 56.4)[0]
        h_free = heading(tl, 57.6, 58.4)[0] - heading(tl, 57.2, 58.0)[0]
        ok(abs(h_turn) > abs(h_free) + 3,
           "skater turns while focused (heading %.1f deg in 0.5 s) and not while unfocused (%.1f)" % (h_turn, h_free))
        p0, p1 = tl.cam_pitch(63.9), tl.cam_pitch(64.6)
        ok(p1 - p0 > 10, "mouse up tilts the view up: pitch %.1f -> %.1f deg" % (p0, p1))
    elif name == "rebind":
        ok(end > 56, "run reached script time 56 s (got %.1f)" % end)
        p0, p1 = tl.pos(45.9), tl.pos(47.2)
        ok(math.hypot(p1[0] - p0[0], p1[1] - p0[1]) < 0.01,
           "W (unbound) does not move the skater: (%.2f, %.2f) -> (%.2f, %.2f)" % (p0 + p1))
        p2 = tl.pos(49.6)
        ok(math.hypot(p2[0] - p1[0], p2[1] - p1[1]) > 3,
           "T (rebound Forward) moves it: -> (%.2f, %.2f)" % p2)
        y0, y1 = tl.cam_yaw(53.9), tl.cam_yaw(54.75)
        ok(y1 - y0 > 20, "inverted mouse right swings the camera the other way: yaw %.1f -> %.1f deg" % (y0, y1))
        ok(tl.pad_sticks(54.4)[0] < 0x40, "game pad buffer RX: 0x%02X" % tl.pad_sticks(54.4)[0])
    elif name == "look":
        check_look(tl, ok)
    elif name == "lookcareer":
        check_lookcareer(tl, ok)
    for good, what in results:
        print("%s  %s: %s" % ("PASS" if good else "FAIL", name, what))
    return all(good for good, _ in results)


def check_look(tl, ok):
    end = tl.last_script_seconds()
    ok(end > 66, "run reached script time 66 s (got %.1f)" % end)
    # View yaw: atan2 of the view direction, from +x towards +z. Facing +x,
    # the skater's right is -z, so turning the view right lowers it.
    y0, y1, y2 = tl.cam_yaw(45.9), tl.cam_yaw(46.8), tl.cam_yaw(47.8)
    ok(abs(wrap(y1 - y0) + 120) < 6, "600 px right turns the view 120 deg right (past the stick's 60): %.1f -> %.1f"
       % (y0, y1))
    ok(abs(wrap(y2 - y1)) < 2, "the view stays there with the mouse still: %.1f deg one second later" % y2)
    ok(tl.pad_sticks(46.4)[:2] == (0x80, 0x80), "the mouse does not push the right stick: RX/RY %s"
       % (tuple("0x%02X" % v for v in tl.pad_sticks(46.4)[:2]),))
    rel = []
    for s in (49.6, 50.2, 50.8):
        h, d = heading(tl, s - 0.3, s)
        rel.append((wrap(tl.cam_yaw(s) - h), h, d))
    turned = wrap(rel[-1][1] - heading(tl, 47.7, 48.6)[0]) if rel else 0
    ok(all(d > 0.5 for _, _, d in rel) and abs(turned) > 30,
       "the skater skates through a turn: heading %.1f -> %.1f" % (heading(tl, 47.7, 48.6)[0], rel[-1][1]))
    # In the turn the game's follow camera lags behind the motion a little
    # (kept on purpose); coasting straight after it the angle is exact.
    ok(all(abs(wrap(r + 120)) < 20 for r, _, _ in rel),
       "through the turn the view keeps its angle to the skater's motion (with the game's follow lag): %s"
       % ", ".join("%.1f" % r for r, _, _ in rel))
    h, d = heading(tl, 51.6, 51.9)
    r = wrap(tl.cam_yaw(51.9) - h)
    ok(d > 0.3 and abs(r + 120) < 6, "coasting out of the turn: view at %.1f deg to the motion" % r)
    h, d = heading(tl, 53.1, 53.5)
    yaw, pitch, active = tl.look(53.0)
    ok(not active and abs(wrap(tl.cam_yaw(53.5) - h)) < 10 if d > 0.2 else not active,
       "R recentres: look active=%d, view %.1f vs motion %.1f" % (active, tl.cam_yaw(53.5), h))
    p0, p1, p2 = tl.cam_pitch(53.9), tl.cam_pitch(54.6), tl.cam_pitch(55.5)
    ok(abs(p1 - p0 - 20) < 4 and abs(p2 - p1) < 2,
       "mouse up 100 px tilts the view 20 deg up and it stays (camera pulled in, not into the floor): "
       "pitch %.1f -> %.1f -> %.1f" % (p0, p1, p2))
    ok(tl.mode(56.8) == 3 and tl.mode(58.0) == 3, "ENTER opens the pause menu (mode %d)" % tl.mode(56.8))
    moved = tl.look_changes(56.5, 59.0)
    ok(not moved, "motion in the pause menu is dropped (%d look changes)" % len(moved))
    yaw, pitch, active = tl.look(60.5)
    ok(tl.mode(60.0) == 2 and active and abs(pitch - 20) < 0.01,
       "after the pause the view keeps its angle (pitch %.1f, active %d)" % (pitch, active))
    yaw, pitch, active = tl.look(66.0)
    ok(not active, "a lesson restart (scripted intro camera) recentres the view (active %d)" % active)
    moved = [m for m in tl.look_changes(64.9, 66.0) if m["active"]]
    ok(not moved, "motion during the intro is dropped (%d look changes)" % len(moved))


def check_lookcareer(tl, ok):
    ok(tl.ticks and tl.anchors, "run log has states and pad anchors")
    ok(any(CAREER_FLYTHRU[0] < int(m["script"]) < CAREER_FLYTHRU[1] for m in tl.inputs),
       "the fly-thru ran at script %d-%dv" % CAREER_FLYTHRU)
    early = [m for m in tl.looks if m["script"] < CAREER_LEVEL_START and m["active"]]
    ok(not early, "fly-thru (scripted camera): motion dropped, no mouse-look (%d changes)" % len(early))
    yaw, pitch, active = tl.look("3078v")
    ok(active and abs(wrap(yaw - 120)) < 0.01 and abs(pitch - 20) < 0.01,
       "level start: 600 px right / 100 px up = yaw %.1f pitch %.1f (120 / 20)" % (yaw, pitch))
    y0, y1 = tl.cam_yaw("3050v"), tl.cam_yaw("3078v")
    ok(abs(wrap(y1 - y0) + 120) < 8, "the view turned 120 deg right: %.1f -> %.1f" % (y0, y1))
    paused = tl.look_changes("%dv" % CAREER_PAUSE, "4250v")
    ok(not paused, "pause menu / save dialog: motion dropped (%d look changes)" % len(paused))
    on_rail = [m for m in tl.placed if m.get("state") == "5"]
    ok(len(on_rail) > 10, "grind: the follow camera runs (player state 5) and mouse-look places it (%d frames)"
       % len(on_rail))
    y0, y1 = tl.look("4338v")[0], tl.look("4380v")
    ok(y1[2] and abs(wrap(y1[0] - y0) + 90) < 0.01, "on the rail 450 px left turn the view 90 deg: yaw %.1f -> %.1f"
       % (y0, y1[0]))
    g0, g1 = tl.cam_yaw("4338v"), tl.cam_yaw("4380v")
    ok(abs(abs(wrap(g1 - g0)) - 90) < 25, "camera on the rail turned about 90 deg: %.1f -> %.1f" % (g0, g1))
    x, z = tl.pos("%dv" % CAREER_GRIND)
    ok(abs(z + 148.338) < 0.3, "the skater is on the rail (z -148.34): (%.2f, %.2f)" % (x, z))


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("run", "check"):
        print(__doc__)
        return 2
    names = sys.argv[2:] or list(SCENARIOS)
    passed = True
    for name in names:
        if sys.argv[1] == "run":
            run(name)
        passed = check(name) and passed
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
