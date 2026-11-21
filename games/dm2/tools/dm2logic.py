#!/usr/bin/env python3
"""Small DM2 logic check: the native runner against the PCSX2 reference run.

Reference run 1 (tools/scripts/dm2-ref-run1.txt, driven by tools/dm2pcsx2.py
in PCSX2): fresh boot, START skips the intro, main menu, FREERIDE, Dave
Mirra, first bike, default modifiers, Woodward Camp, then about 8 s of
riding (pedal, turn left, jump, coast, turn right). The hook's seen log
(tools/scripts/dm2-ref-run1-seen.txt) holds every input change with the
vsync the game read it on; the six checkpoints are PCSX2 savestates whose
values are kept in tools/scripts/dm2-ref-run1-values.txt (no RAM dumps are
committed).

The native script is the seen log in vsync units from the first pad read
(the native ZX_PAD_SCRIPT origin), plus two SYNC anchors so loading
time does not matter:
  - main menu: sound mode 0x259770 becomes 1 (front-end music starts);
  - level: the rider task frame counter 0x25BCC4 reaches its value at the
    first in-level checkpoint (from that checkpoint's RAM image).
From the level anchor on, PCSX2 runs one level frame per vsync, so the
recorded presses fall on the same level frames natively.

Reference run 2 (`--run 2`: tools/scripts/dm2-ref-run2.txt, PCSX2 2.8.2):
fresh boot, ProQuest, Dave Mirra, first bike, default modifiers, Woodward
Camp, and a ride played natively by the closed-loop player (playbot dm2.py,
seed 3; its answers changed on even level frames only) replayed in PCSX2
from the hook's schedule (tools/scripts/dm2-ref-run2-schedule.txt: the front
end by VBlank count, the level by level frame, so PCSX2 takes it on exactly
those frames): the instructions and the challenge (CROSS each, the goal
given), a manual banked (1,400), an air trick into a bail, more bails, lines
landed (2,425, 3,475), the pause menu (START, DOWN, UP, CROSS on CONTINUE),
a grind banked (7,928), an air trick landed (10,128) completing the goal.
21 checkpoints. Its native script, as bmxlogic's: the front end at its
vsyncs with the main-menu anchor; in the level every input change at level
frame f anchored on the frame counter (`t:SYNC=0x25BCC4>=f-1`, effective
half a vsync after t: the read of frame f), sticks included (LX/LY items);
the schedule's changes are >= 2 frames apart, as an anchor needs. Its level
checkpoints are dumped natively at the VBlank where the frame counter reaches
the checkpoint's f (DM2LOGIC_FRAME_LEAD=1 dumps a frame later: the rider is
then one frame of motion ahead, so a PCSX2 savestate at counter f is the
native VBlank at counter f).

Compared (both runs): mode, game type, level cursor, session, the main menu's
cursor, the level frame (natively one more: FRAME_SKEW), the rider's
position, velocity and heading within TOL, its game state (physics, trick,
action levels, flags), scores, line state and the camera; and, where the
values file has them (run 2): the player's character's goals of the level
(LevelGoals, 0x44 bytes: each challenge's goals' flags and parts, the level's
flags, challenge, goals done), the character's respect, the game screen up
(g_gameScreen) and, with a screen up, the selected index of every UIBuilder
menu widget (the pause menu's cursor).

  python3 -I tools/dm2logic.py [--run 2] script            # the native ZX_PAD_SCRIPT
  python3 -I tools/dm2logic.py [--run 2] refvalues         # (re)write the values file from recomp/dumps/ref[2]/*.ram
  python3 -I tools/dm2logic.py [--run 2] run NAME [SECS]   # run natively, dumps in recomp/dumps/logic/NAME-*.ram
  python3 -I tools/dm2logic.py [--run 2] compare NAME [--off OFFNAME]
                                                 # reference values vs native dumps (--off: the same input with
                                                 # ZX_READABLE=off at the same clock: a checkpoint that differs
                                                 # from the reference only as OFFNAME's does is a MATCH and says so)
  python3 -I tools/dm2logic.py show RAMFILE      # the compared values of any EE RAM image

`--ref DIR` instead of `--run N`: a re-recorded reference in one folder
(DIR/seen.txt, commands.txt, values.txt, ram/), checked as run 1 is, or with
`--by-frame` as run 2 is (a seen log with the level frame column).

Environment: RUNNER (zx-runner), ELF (default recomp/disc-root/SLUS_201.59),
DM2_SCRATCH (default $ZAXIS_SCRATCH/dm2logic: HOME, app support, memory
cards, the run 2 script), ZX_GS_NULL (default 1) / ZX_VU1_SKIP and other
ZX_* pass through. DM2LOGIC_INPUT_DELAY / DM2LOGIC_DUMP_OFFSET shift presses /
dumps (vsyncs; run 2's level inputs: level frames, a negative test).
DM2LOGIC_KEEP=1 keeps the native dumps of a run that matches (else `compare`
deletes them; an --off run's are kept until it is compared itself).
"""
import math
import os
import re
import signal
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(ROOT)), "tools"))   # (tools/: zpaths.py)
import zpaths  # noqa: E402,F401  (the shared environment: no sound, ZX_AUDIO)
SCRIPTS = os.path.join(ROOT, "tools", "scripts")
# The reference runs: run 1 (the free ride: presses by vsync, level dumps by script time) and run 2 (the ProQuest
# ride: level input and dumps by level frame). select_run() points these at one.
RUNS = {1: dict(seen=os.path.join(SCRIPTS, "dm2-ref-run1-seen.txt"), commands=os.path.join(SCRIPTS, "dm2-ref-run1-commands.txt"),
                values=os.path.join(SCRIPTS, "dm2-ref-run1-values.txt"), ram=os.path.join(ROOT, "workspace", "recomp", "dumps", "ref"),
                by_frame=False),
        2: dict(seen=os.path.join(SCRIPTS, "dm2-ref-run2-seen.txt"), commands=os.path.join(SCRIPTS, "dm2-ref-run2-commands.txt"),
                values=os.path.join(SCRIPTS, "dm2-ref-run2-values.txt"), ram=os.path.join(ROOT, "workspace", "recomp", "dumps", "ref2"),
                by_frame=True)}
RUN = 1
SEEN, COMMANDS, VALUES, REF_RAM, BY_FRAME = (RUNS[1][k] for k in ("seen", "commands", "values", "ram", "by_frame"))
NATIVE_RAM = os.path.join(ROOT, "workspace", "recomp", "dumps", "logic")
LOGS = os.path.join(ROOT, "workspace", "recomp", "logs")
RUNNER = os.environ.get("RUNNER", os.path.join(zpaths.ZAXIS_WORKTREES, "dm2", "out", "build-dm2", "runtime", "zx-runner"))
ELF = os.environ.get("ELF", os.path.join(ROOT, "workspace", "recomp", "disc-root", "SLUS_201.59"))
SCRATCH = os.environ.get("DM2_SCRATCH", os.path.join(zpaths.ZAXIS_SCRATCH, "dm2logic"))

# DM2 anchors (tools/PCSX2-DM2.md)
MODE = 0x259770          # sound/game mode: 0 boot+movies, 1 front end, 2 level (and its loading)
GAME_TYPE = 0x25A1C0     # 0 in Freeride
LEVEL_CURSOR = 0x25A1A8  # Choose-a-Level cursor
SESSION = 0x2566C8       # bytes: +1 rider id, +2 level id, +3 player count
LEVEL_FRAME = 0x25BCC4   # rider task frames since level start
RIDER_TABLE = 0x2F4EA0   # rider object pointers; [0] = player 1
RIDER_POS, RIDER_VEL, RIDER_ROT = 0x1040, 0x10A0, 0x1000
VSYNC = 0x0F7018         # PCSX2 hook's VBlank counter (only in reference images)
MENU_WIDGET_TYPE = 0x1C  # UIBuilder menu widget: +0x08 type, +0x0C name ptr, +0x24 selected index
# (proof1) the rider's game state and the camera (games/dm2/rider/rider.h Rider, ScoreBlock; camera/camera.h
# PlayerCamera): flags +0x160, the action state levels +0x165, the trick +0x172, the score block +0x1A0 (turn score
# f32, game score, line points; +0x69C the line's state, +0x69D the lines, +0x69F the tricks), the physics state
# +0xFF0; player 1's camera g_playerCameras[0]: position +0x10, target +0x40, field of view +0x70
RIDER_FLAGS, RIDER_ACTIONS, RIDER_TRICK, RIDER_SCORE, RIDER_PHYSICS = 0x160, 0x165, 0x172, 0x1A0, 0xFF0
PLAYER_CAMERAS = 0x2BC070
# (run 2) the goals and the screens (games/dm2/game/players.h RiderCharacter, LevelGoals; overlay/screens.h):
# the characters' records g_riderCharacters (0x3C8 each: +0xB0 respect, +0x13C the levels' LevelGoals, 0x44 each),
# player 1's character g_playerSetups[0] +3, the session's level SESSION +2; the game screen up g_gameScreen
RIDER_CHARACTERS, CHARACTER_SIZE, CHARACTER_RESPECT, CHARACTER_LEVELS, LEVEL_GOALS_SIZE = 0x2174B4, 0x3C8, 0xB0, 0x13C, 0x44
PLAYER1_CHARACTER = 0x21C41C + 3
SESSION_LEVEL = SESSION + 2
GAME_SCREEN = 0x25BBE4

INPUT_DELAY = int(os.environ.get("DM2LOGIC_INPUT_DELAY", "0"))
FRAME_LEAD = int(os.environ.get("DM2LOGIC_FRAME_LEAD", "0"))   # (run 2) frames a level dump is taken after its checkpoint's
DUMP_OFFSET = int(os.environ.get("DM2LOGIC_DUMP_OFFSET", "0"))
TOL = 0.01
FRAME_SKEW = int(os.environ.get("DM2LOGIC_FRAME_SKEW", "1"))

BUTTONS = {
    "SELECT": 0x0001, "L3": 0x0002, "R3": 0x0004, "START": 0x0008,
    "UP": 0x0010, "RIGHT": 0x0020, "DOWN": 0x0040, "LEFT": 0x0080,
    "L2": 0x0100, "R2": 0x0200, "L1": 0x0400, "R1": 0x0800,
    "TRIANGLE": 0x1000, "CIRCLE": 0x2000, "CROSS": 0x4000, "SQUARE": 0x8000,
}


def u32(ram, a):
    return struct.unpack_from("<I", ram, a)[0]


def menu_cursor(ram, name="MainMenu"):
    """Selected index of the UIBuilder menu widget called `name` (heap
    object found by its name string, so heap layout does not matter)."""
    key = name.encode() + b"\0"
    i = 0
    while True:
        s = ram.find(key, i)
        if s < 0:
            return None
        i = s + 1
        if s & 3:
            continue
        p, j = struct.pack("<I", s), 0
        while True:
            a = ram.find(p, j)
            if a < 0:
                break
            j = a + 1
            o = a - 0x0C
            if a & 3 or o < 0 or u32(ram, o + 0x08) != MENU_WIDGET_TYPE:
                continue
            return u32(ram, o + 0x24)


def menus(ram):
    """The selected index of every UIBuilder menu widget with one (not -1), in address order: "Name:index,..."."""
    out = []
    for name in (b"Menu\0", b"MainMenu\0"):
        i = 0
        while True:
            s = ram.find(name, i)
            if s < 0:
                break
            i = s + 1
            if s & 3 or ram[s - 1:s].isalpha():
                continue
            p, j = struct.pack("<I", s), 0
            while True:
                a = ram.find(p, j)
                if a < 0:
                    break
                j = a + 1
                o = a - 0x0C
                if a & 3 or o < 0 or u32(ram, o + 0x08) != MENU_WIDGET_TYPE or u32(ram, o + 0x24) == 0xFFFFFFFF:
                    continue
                out.append((o, "%s:%d" % (name[:-1].decode(), u32(ram, o + 0x24))))
    return ",".join(t for _, t in sorted(out)) or "-"


def state(ram):
    s = {"mode": u32(ram, MODE), "gametype": u32(ram, GAME_TYPE), "lvlcursor": u32(ram, LEVEL_CURSOR),
         "session": u32(ram, SESSION) & 0xFFFFFF00, "frame": u32(ram, LEVEL_FRAME), "menu": menu_cursor(ram)}
    obj = u32(ram, RIDER_TABLE)
    if s["frame"] and 0x100000 <= obj < len(ram) - 0x2000:
        s["pos"] = struct.unpack_from("<3f", ram, obj + RIDER_POS)
        s["vel"] = struct.unpack_from("<3f", ram, obj + RIDER_VEL)
        s["fwd"] = struct.unpack_from("<3f", ram, obj + RIDER_ROT + 0x20)   # third rotation row
        s["posbits"] = struct.unpack_from("<3I", ram, obj + RIDER_POS)
        # (proof1) what the rider does and has scored, and where the camera is
        s["physics"] = ram[obj + RIDER_PHYSICS]
        s["trick"] = ram[obj + RIDER_TRICK]
        s["actions"] = tuple(ram[obj + RIDER_ACTIONS:obj + RIDER_ACTIONS + 4])
        s["rflags"] = u32(ram, obj + RIDER_FLAGS)
        s["score"] = struct.unpack_from("<f", ram, obj + RIDER_SCORE)[0]
        s["gamescore"] = u32(ram, obj + RIDER_SCORE + 4)
        s["linepoints"] = u32(ram, obj + RIDER_SCORE + 8)
        s["lines"] = tuple(ram[obj + RIDER_SCORE + 0x69C:obj + RIDER_SCORE + 0x6A0])
        s["eye"] = struct.unpack_from("<3f", ram, PLAYER_CAMERAS + 0x10)
        s["look"] = struct.unpack_from("<3f", ram, PLAYER_CAMERAS + 0x40)
        s["fov"] = struct.unpack_from("<f", ram, PLAYER_CAMERAS + 0x70)[0]
    # (run 2) the goals of the level for the player's character, its respect, the screen up and its menus
    character, level = ram[PLAYER1_CHARACTER], ram[SESSION_LEVEL]
    record = RIDER_CHARACTERS + character * CHARACTER_SIZE
    goals = record + CHARACTER_LEVELS + level * LEVEL_GOALS_SIZE
    s["goals"] = ram[goals:goals + LEVEL_GOALS_SIZE].hex()
    s["respect"] = u32(ram, record + CHARACTER_RESPECT)
    s["screen"] = ram[GAME_SCREEN]
    if s["screen"]:
        s["menus"] = menus(ram)
    return s


# ---- reference ---------------------------------------------------------------

def read_seen_entries():
    """(firstpoll, [(vsync, buttons, lx, ly, level frame or None)]) from the seen log (run 1's has no frame column)."""
    first, ents = None, []
    for line in open(SEEN):
        if line.startswith("#"):
            first = int(line.split("firstpoll")[1].split()[0])
            continue
        f = line.split()
        frame = int(f[4]) if len(f) > 5 and f[4].isdigit() else None
        ents.append((int(f[0]), int(f[1], 16), int(f[2], 16), int(f[3], 16), frame))
    return first, ents


def read_seen():
    first, full = read_seen_entries()
    ents = [(v, mask) for v, mask, _, _, _ in full]
    presses, held = [], {}
    for v, mask in ents + [(ents[-1][0] + 1, 0)]:
        for name, bit in BUTTONS.items():
            if mask & bit and name not in held:
                held[name] = v
            elif not (mask & bit) and name in held:
                start = held.pop(name)
                presses.append((start, name, max(1, v - start)))
    return first, sorted(presses)


def checkpoints():
    """[(name, vsync, frame)] from the reference run's command output."""
    out = []
    for line in open(COMMANDS):
        m = re.match(r"checkpoint (\S+): .*\(vsync (\d+), game frame (\d+)\)", line)
        if m:
            out.append((m.group(1), int(m.group(2)), int(m.group(3))))
    return out


def main_menu_vsync():
    lines = open(COMMANDS).read().splitlines()
    for i, line in enumerate(lines):
        if line.startswith("[") and "waitval 0x259770 u32 == 1" in line:
            return int(re.search(r"vsync (\d+)", lines[i + 1]).group(1))
    raise SystemExit("no main-menu wait in %s" % COMMANDS)


def fmt_state(s):
    out = "mode %d gametype %d lvlcursor %d session %06x frame %d menu %s" % (
        s["mode"], s["gametype"], s["lvlcursor"], s["session"] >> 8, s["frame"], s["menu"])
    if "pos" in s:
        out += " pos %.4f %.4f %.4f vel %.4f %.4f %.4f fwd %.4f %.4f %.4f posbits %08x %08x %08x" % (
            s["pos"] + s["vel"] + s["fwd"] + s["posbits"])
    if "physics" in s:
        out += " physics %d trick %d actions %s rflags %08x score %.4f gamescore %d linepoints %d lines %s" % (
            s["physics"], s["trick"], ",".join(str(x) for x in s["actions"]), s["rflags"], s["score"], s["gamescore"],
            s["linepoints"], ",".join(str(x) for x in s["lines"]))
        out += " eye %.4f %.4f %.4f look %.4f %.4f %.4f fov %.4f" % (s["eye"] + s["look"] + (s["fov"],))
    if "goals" in s:
        out += " goals %s respect %d screen %d" % (s["goals"], s["respect"], s["screen"])
    if "menus" in s:
        out += " menus %s" % s["menus"]
    return out


def parse_state(text):
    f = text.split()
    kv, i = {}, 0
    while i < len(f):
        k = f[i]
        if k in ("pos", "vel", "fwd", "eye", "look"):
            kv[k] = tuple(float(x) for x in f[i + 1:i + 4]); i += 4
        elif k in ("score", "fov"):
            kv[k] = float(f[i + 1]); i += 2
        elif k in ("actions", "lines"):
            kv[k] = tuple(int(x) for x in f[i + 1].split(",")); i += 2
        elif k in ("goals", "menus"):
            kv[k] = f[i + 1]; i += 2
        elif k == "rflags":
            kv[k] = int(f[i + 1], 16); i += 2
        elif k == "posbits":
            kv[k] = tuple(int(x, 16) for x in f[i + 1:i + 4]); i += 4
        elif k == "session":
            kv[k] = int(f[i + 1], 16) << 8; i += 2
        elif k == "menu":
            kv[k] = None if f[i + 1] == "None" else int(f[i + 1]); i += 2
        else:
            kv[k] = int(f[i + 1]); i += 2
    return kv


def refvalues():
    first, _ = read_seen()
    lines = ["# DM2 reference run %s (PCSX2 2.8.2, tools/scripts/dm2-ref-run%s.txt): values at each checkpoint" % (RUN, RUN),
             "# name vsync script(=vsync-firstpoll %d) | state" % first]
    for name, v, f in checkpoints():
        ram = open(os.path.join(REF_RAM, name + ".ram"), "rb").read()
        assert u32(ram, VSYNC) == v, (name, u32(ram, VSYNC), v)
        s = state(ram)
        if not BY_FRAME:
            for k in ("goals", "respect", "screen", "menus"):    # (run 1's values file has none of them)
                s.pop(k, None)
        lines.append("%s %d %d | %s" % (name, v, v - first, fmt_state(s)))
    open(VALUES, "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))


def ref_values():
    out = []
    for line in open(VALUES):
        if line.startswith("#"):
            continue
        head, _, rest = line.partition("|")
        name, v, t = head.split()
        out.append((name, int(v), int(t), parse_state(rest)))
    return out


# ---- native ----------------------------------------------------------------

def native_script():
    if BY_FRAME:
        return native_script_by_frame()
    first, presses = read_seen()
    items = [(s - first + INPUT_DELAY, "%dv:%s:%dv" % (s - first + INPUT_DELAY, n, d)) for s, n, d in presses]
    menu_t = main_menu_vsync() - first
    items.append((menu_t - 0.5, "%dv:SYNC=0x%X==1" % (menu_t, MODE)))
    # level anchor: the frame counter of the first in-level checkpoint
    name, v, t, s = next(r for r in ref_values() if r[3]["frame"] > 0)
    items.append((t - 0.5, "%dv:SYNC=0x%X==%d" % (t, LEVEL_FRAME, s["frame"])))
    items.sort(key=lambda i: i[0])
    return ",".join(i for _, i in items)


STICK_CENTRE = 0x80   # the native pad's idle sticks (PCSX2's are 0x7F; the schedule sets 0x80 in the level)


def stick_value(byte):
    """The pad script value whose axis byte (lround(128 + v * 127.5)) is `byte`."""
    return round((byte - 128) / 127.5, 4)


def native_script_by_frame():
    """(run 2) bmxlogic's script: the front end at its vsyncs with the main-menu anchor; in the level every input
    change at level frame f anchored on the frame counter and effective half a vsync after its anchor time, buttons
    and the left stick. A change at frame < 2 is not anchored (frame >= 0 is always true: an anchor needs its
    condition false once): it is the level's first read setting the sticks to the native centre already there."""
    first, ents = read_seen_entries()
    items = []
    menu_t = main_menu_vsync() - first
    items.append((menu_t - 0.25, "%dv:SYNC=0x%X==1" % (menu_t, MODE)))
    held, prev = {}, (0, STICK_CENTRE, STICK_CENTRE)
    last = ents[-1]
    for v, mask, lx, ly, frame in ents[1:] + [(last[0] + 1, 0, STICK_CENTRE, STICK_CENTRE, (last[4] or 0) + 2)]:
        t = v - first
        if frame and frame >= 2:
            items.append((t - 0.25, "%dv:SYNC=0x%X>=%d" % (t, LEVEL_FRAME, frame - 1 + INPUT_DELAY)))
            at = t + 0.5
        else:
            at = t
        if not frame:
            lx = ly = STICK_CENTRE     # (the front end: PCSX2's idle 0x7F, natively the centre as run 1)
        now = {n for n, bit in BUTTONS.items() if mask & bit}
        now |= {"LX=%g" % stick_value(lx)} if lx != STICK_CENTRE else set()
        now |= {"LY=%g" % stick_value(ly)} if ly != STICK_CENTRE else set()
        for key in list(held):
            if key not in now:
                start = held.pop(key)
                items.append((start, "%gv:%s:%gv" % (start, key, at - start)))
        for key in now:
            if key not in held:
                held[key] = at
    items.sort(key=lambda i: i[0])
    return ",".join(i for _, i in items)


def dump_spec():
    if BY_FRAME:
        return ",".join("%s:%s" % (name, "0x%X>=%d" % (LEVEL_FRAME, s["frame"] + FRAME_LEAD) if s["frame"] > 0
                                   else "script>=%d" % (t + DUMP_OFFSET)) for name, _, t, s in ref_values())
    return ",".join("%s:script>=%d" % (name, t + DUMP_OFFSET) for name, _, t, _ in ref_values())


def run(name, secs):
    os.makedirs(NATIVE_RAM, exist_ok=True)
    os.makedirs(LOGS, exist_ok=True)
    for d in ("home", "mc0", "mc1", "run"):
        os.makedirs(os.path.join(SCRATCH, d), exist_ok=True)
    for _, _, _, _ in ref_values():
        pass
    expected = [os.path.join(NATIVE_RAM, "%s-%s.ram" % (name, ck)) for ck, _, _, _ in ref_values()]
    for p in expected:
        if os.path.exists(p):
            os.remove(p)
    env = dict(os.environ)
    env["HOME"] = os.path.join(SCRATCH, "home")
    env["ZX_APP_SUPPORT_DIR"] = os.path.join(SCRATCH, "home", "support")
    env["ZX_APP_LOG_DIR"] = os.path.join(SCRATCH, "home", "logs")
    env.setdefault("ZX_MC_ROOT", os.path.join(SCRATCH, "mc0"))
    env.setdefault("ZX_DEBUG_UI", "0")
    env.setdefault("ZX_GS_BACKEND", "cpu")
    env.setdefault("ZX_GS_NULL", "1")
    env["ZX_PAD_SCRIPT_CLOCK"] = "vsync"
    script_path = os.path.join(SCRATCH, "run", "script-%s.txt" % name)
    open(script_path, "w").write(native_script())
    env["ZX_PAD_SCRIPT"] = "@" + script_path
    env["ZX_DUMP_WHEN"] = dump_spec()
    env["ZX_DUMP_PREFIX"] = os.path.join(NATIVE_RAM, name)
    env.setdefault("ZX_STATE_LOG", ",".join("0x%X" % a for a in (MODE, LEVEL_CURSOR, SESSION, LEVEL_FRAME, RIDER_TABLE)))
    log = os.path.join(LOGS, "logic-%s.log" % name)
    print("runner %s\nlog %s\nscript %s\ndumps %s" % (RUNNER, log, script_path, env["ZX_DUMP_WHEN"]))
    with open(log, "w") as f:
        proc = subprocess.Popen([RUNNER, ELF], stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                cwd=os.path.join(SCRATCH, "run"), env=env, start_new_session=True)
    t0 = time.time()
    try:
        while time.time() - t0 < secs and proc.poll() is None:
            if all(os.path.exists(p) for p in expected):
                time.sleep(1)
                break
            time.sleep(1)
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    got = sum(os.path.exists(p) for p in expected)
    print("%d/%d dumps after %.0f s (rc %s)" % (got, len(expected), time.time() - t0, proc.returncode))
    syncs = [l.rstrip() for l in open(log, errors="replace") if "[pad] sync" in l]
    late = [l for l in syncs if re.search(r"clock -\d", l)]
    print("anchors: %d hit, %d with the native clock standing (late)" % (len(syncs), len(late)))
    for l in syncs[:3] + late[:5]:
        print("  " + l)


def diffs_of(r, n):
    """(the differences of native state n from reference state r, the position note, whether pos is bit-exact)."""
    diffs = []
    for k in ("mode", "gametype", "lvlcursor", "session", "menu"):
        if r.get(k) != n.get(k):
            diffs.append("%s %s/%s" % (k, r.get(k), n.get(k)))
    # The frame counter is sampled one increment later natively: a PCSX2
    # savestate lands at the VBlank while the EE is still finishing the
    # frame (rider physics done, the rider task's counter increment not
    # yet), the native VBlank dump after the whole frame. Rider state at
    # native frame N+1 is PCSX2's at N (run n2 with DM2LOGIC_DUMP_OFFSET=-1
    # showed the counters equal and the rider one frame behind).
    if n["frame"] - r["frame"] not in (0, FRAME_SKEW):
        diffs.append("frame %d/%d" % (r["frame"], n["frame"]))
    exact, bit_exact = "", True
    if "pos" in r:
        if "pos" not in n:
            diffs.append("no rider natively")
        else:
            exact_d = 0.0
            for k in ("pos", "vel", "fwd"):
                e = math.sqrt(sum((a - b) ** 2 for a, b in zip(r[k], n[k])))
                if k == "pos":
                    exact_d = e
                if e > TOL:
                    diffs.append("%s (%s)/(%s) d=%.4f" % (k, " ".join("%.3f" % x for x in r[k]),
                                                         " ".join("%.3f" % x for x in n[k]), e))
            bit_exact = r["posbits"] == n["posbits"]
            # (proof1) the rider's game state exactly, the score and the camera within TOL (a values file from
            # before has none of them: not compared)
            for k in ("physics", "trick", "actions", "rflags", "gamescore", "linepoints", "lines"):
                if k in r and r[k] != n.get(k):
                    diffs.append("%s %s/%s" % (k, r[k], n.get(k)))
            if "score" in r and abs(r["score"] - n.get("score", float("nan"))) > TOL:
                diffs.append("score %.3f/%.3f" % (r["score"], n.get("score", float("nan"))))
            for k in ("eye", "look"):
                if k in r:
                    e = math.sqrt(sum((x - y) ** 2 for x, y in zip(r[k], n.get(k, (float("nan"),) * 3))))
                    if not e <= TOL:
                        diffs.append("%s (%s)/(%s) d=%.4f" % (k, " ".join("%.3f" % x for x in r[k]),
                                                             " ".join("%.3f" % x for x in n.get(k, ())), e))
            if "fov" in r and not abs(r["fov"] - n.get("fov", float("nan"))) <= TOL:
                diffs.append("fov %.3f/%.3f" % (r["fov"], n.get("fov", float("nan"))))
            exact = " pos bit-exact" if bit_exact else " pos |d|=%.4f" % exact_d
    # (run 2) the goals, respect, the screen and its menus exactly, wherever the values file has them
    for k in ("goals", "respect", "screen", "menus"):
        if k in r and r[k] != n.get(k):
            diffs.append("%s %s/%s" % (k, r[k], n.get(k)))
    return diffs, exact, bit_exact


def compare(name, off=None):
    """The reference values against NAME's dumps. With OFF (the same input with ZX_READABLE=off at the same
    clock, its dumps kept), a checkpoint that differs from the reference only as OFF's does, in the same fields
    and values, and whose compared state equals OFF's exactly, is a MATCH and says so (logiccheck's rule)."""
    first_diff = first_inexact = None
    matches = 0
    rows = ref_values()
    for ck, v, t, r in rows:
        p = os.path.join(NATIVE_RAM, "%s-%s.ram" % (name, ck))
        if not os.path.exists(p):
            print("%-24s missing native dump" % ck)
            first_diff = first_diff or ck
            continue
        n = state(open(p, "rb").read())
        diffs, exact, bit_exact = diffs_of(r, n)
        note = ""
        if diffs and off:
            po = os.path.join(NATIVE_RAM, "%s-%s.ram" % (off, ck))
            o = state(open(po, "rb").read()) if os.path.exists(po) else None
            if o is not None and diffs_of(r, o)[0] == diffs and o == n:
                note = "as off at the same clock: "
        if not bit_exact and first_inexact is None:
            first_inexact = ck
        print("%-24s %-5s frame %5d/%-5d%s %s%s" % (ck, "MATCH" if not diffs or note else "DIFF", r["frame"], n["frame"],
                                                  exact, note, "; ".join(diffs)))
        matches += not diffs or bool(note)
        if diffs and not note and first_diff is None:
            first_diff = ck
    print("%d/%d MATCH; first divergence (tolerance %.2f): %s; first rider position not bit-exact: %s"
          % (matches, len(rows), TOL, first_diff or "none", first_inexact or "none"))
    if matches == len(rows) and os.environ.get("DM2LOGIC_KEEP") != "1":   # a pass deletes its dumps (32 MB each)
        for ck, _, _, _ in rows:
            p = os.path.join(NATIVE_RAM, "%s-%s.ram" % (name, ck))
            if os.path.exists(p):
                os.remove(p)


def select_run(a):
    """--run N or --ref DIR out of the arguments: point SEEN, COMMANDS, VALUES, REF_RAM, BY_FRAME at that run."""
    global RUN, SEEN, COMMANDS, VALUES, REF_RAM, BY_FRAME
    if "--run" in a:
        i = a.index("--run")
        RUN = int(a[i + 1])
        run = RUNS[RUN]
        del a[i:i + 2]
    elif "--ref" in a:
        i = a.index("--ref")
        d = a[i + 1]
        RUN = os.path.basename(os.path.normpath(d))
        run = dict(seen=os.path.join(d, "seen.txt"), commands=os.path.join(d, "commands.txt"),
                   values=os.path.join(d, "values.txt"), ram=os.path.join(d, "ram"), by_frame="--by-frame" in a)
        del a[i:i + 2]
        if "--by-frame" in a:
            a.remove("--by-frame")
    else:
        return a
    SEEN, COMMANDS, VALUES, REF_RAM, BY_FRAME = (run[k] for k in ("seen", "commands", "values", "ram", "by_frame"))
    return a


def main():
    a = select_run(sys.argv[1:])
    if not a:
        print(__doc__)
        return
    if a[0] == "script":
        print(native_script())
    elif a[0] == "refvalues":
        refvalues()
    elif a[0] == "run":
        run(a[1], int(a[2]) if len(a) > 2 else 600)
    elif a[0] == "compare":
        compare(a[1], a[a.index("--off") + 1] if "--off" in a else None)
    elif a[0] == "show":
        print(fmt_state(state(open(a[1], "rb").read())))
    else:
        raise SystemExit("unknown command %r" % a[0])


if __name__ == "__main__":
    main()
