#!/usr/bin/env python3
"""Replay the PCSX2 reference run natively and compare game state.

The reference run (REFERENCE-RUN.md) was closed loop: pcsx2pad.py waited on
game state (mode, zgui panel, player position) between inputs. Natively the
same inputs cannot simply be replayed on game time, because loading takes
about 10 vsyncs natively and about 1250 in PCSX2. So the native script is the
exact recorded input (scripts/native/ref-run1-input.txt, vsync units) plus
SYNC anchors (runtime/pad_script.h) for the events the reference waited
on that are not what we are testing:

  - front end: the mode / zgui panel waits of tutorial-01-start.txt;
  - level: the level frame counter 0x231CAC reaching its value at the first
    in-level checkpoint (frame 352 at vsync 2826 in run 1). Some early level
    frames take two vsyncs, so an earlier frame is not a safe anchor;
  - every message box dismissal: "box waiting for X" just before the CROSS
    tap that closed the box (WAIT: only stands the clock if native is late). `tips` in pcsx2pad.py taps CROSS (6 frames,
    then up to 20 frames waiting for the box to close) from the moment a box
    starts opening; boxes ignore input for about 40 frames. A tap that did
    not close its box is followed by the next tap exactly 26 vsyncs later
    (IGNORED_GAP); after a tap that closed it, the next tap comes as soon as
    the next box starts opening (9 vsyncs or more, never exactly 26 in this
    run), or the chain ends.
    The ignored taps stay in the native script: the box ignores them, but
    the game's pad state does not (a CROSS tap during a box makes the
    skater jump when play resumes at the next section start, in PCSX2 and
    natively). LOGICCHECK_KEEP_IGNORED=0 leaves them out (the old
    behaviour, for comparison).

Goal completion, player motion and the boxes that end a goal are not
anchored: they are what is compared. Each anchor logs how far it moved the
native clock ("[pad] sync"), which is itself a timing comparison.

Native RAM is dumped at the script time of every reference checkpoint
(ZX_DUMP_WHEN script>=...), at a VBlank like the PCSX2 savestates; ref03,
which the reference took when the first tip's box was waiting for X, once
that box is waiting too (level_anchor_event: its opening is EE timing).

The record is not exactly what the game saw in run 1: pcsx2pad.py wrote
inputs over PINE closed loop, and a write lands on the next pad read or,
now and then, another one. From the fakie goal on, PCSX2 replaying the
record (tools/pcsx2replay.py, in-EE input schedule, per-frame state log)
also ends up elsewhere than run 1 (ref12, ref14-ref17, ref20), while every
section start matches again. So `compare` is against run 1, and
`pxcompare` / `frames` are against PCSX2 replaying the same input, which is
what native correctness is checked with.

  python3 tools/logiccheck.py script            # print the native pad script
  python3 tools/logiccheck.py run NAME [SECS]   # replay natively, dumps in recomp/dumps/logic/NAME-*.ram
  python3 tools/logiccheck.py compare NAME [--exact] [--off OFFNAME]  # table of reference vs native
                                                # (--exact: also bit-for-bit player/camera/time;
                                                # --off: the same input with readable off at the same
                                                # clock: a row that differs from the reference only as
                                                # OFFNAME's does, in the same fields and values, is a
                                                # MATCH and says so)
  python3 tools/logiccheck.py pxcompare NAME PXLOG  # native vs PCSX2 replaying the same input
  python3 tools/logiccheck.py frames NAME PXLOG [--from F] [--limit N] [--all]
                                                # first level frames where per-frame logs differ
  python3 tools/logiccheck.py anchors NAME      # anchor shifts from the run log
  python3 tools/logiccheck.py trajectory NAME   # where the native path passes each reference position
  python3 tools/logiccheck.py waypoints NAME    # reference positions read mid-motion vs native
  python3 tools/logiccheck.py diffruns A B      # first level frame where two native runs differ

The app's menus (ZX_GAME_SETTINGS_MENUS=1, what the app sets) are a route of
their own, the same input: the settings screen, Quit to Desktop and the other
lines and natives aggressive_inline_settings.cpp adds to FrontEnd.zsc and
InGame.zsc move the zgui panels on the heap (not the skater or the camera),
so the panels the front-end and box anchors wait on, and the panel the
compare reads, are the menus' own (MENUS_PANELS). A run says which route it
took in NAME.route beside its dumps, and `compare` reads each run's (its
own and --off's) from there.

Environment: RUNNER (zx-runner to use), extra ZX_* vars pass through.
Each run gets its own memory card and, unless HOME is already a scratch one,
its own HOME (the default settings, never the user's), so runs can overlap;
the run stops once its last checkpoint is dumped.
"""
import math
import os
import pwd
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORKSPACE = os.path.join(ROOT, "workspace")   # (the game's workspace: recomp/, its run/, dumps/ and logs/)
NATIVE_DIR = os.path.join(ROOT, "tools", "scripts", "native")
# ref-run1-seen.txt is the record with the input edges moved to the vsync
# the game saw them on in run 1 (see its header); ref-run1-input.txt is the
# record as written.
INPUT_LOG = os.environ.get("LOGICCHECK_INPUT") or next(
    p for p in (os.path.join(NATIVE_DIR, "ref-run1-seen.txt"), os.path.join(NATIVE_DIR, "ref-run1-input.txt"))
    if os.path.exists(p))
RUN_OUT = os.path.join(NATIVE_DIR, "ref-run1-commands.txt")
REF_RAM = os.path.join(WORKSPACE, "recomp", "dumps", "ref")
NATIVE_RAM = os.path.join(WORKSPACE, "recomp", "dumps", "logic")
LOGS = os.path.join(WORKSPACE, "recomp", "logs")

MODE = 0x276FE4
PANEL = 0x240A20
SECTION = 0x277488
GAUGE = 0x2732FC
VSYNC = 0x231CA4
FRAME = 0x231CAC
FPS = 0x23FD80
LEVEL_TIME = 0x276FF8                # f32: the level's seconds, the frame limiter's frame seconds summed
CTRL = 0x27E0C4
POS = 0x2A76F0
EYE = 0x24C9E0
LOOK = 0x24C9F0
# Heap objects: PCSX2 address, native offset.
SKIN_NODE = (0x6F9E00, -0xF80)
CAMERA_NODE = (0x735BBC, -0x1000)
PANEL_HEAP_DELTA = -0x1290           # zgui panels, native = PCSX2 - 0x1290
BOX_WAITING = 0x831BB0               # zgui panel: message box waiting for X (PCSX2)
# With the app's menus (ZX_GAME_SETTINGS_MENUS=1) the zgui panels the
# reference waits on and holds at its checkpoints, PCSX2 -> native: the main
# menu (+0x930 on the plain native one), the two screens after it, level
# select, the message box waiting for X (+0x920), and the box still
# opening (PCSX2 0x832CD0: never in the reference; natively at ref03's
# frame on both routes when EE time opens the box late, which is why ref03
# is dumped at the reference's event, level_anchor_event). Measured with ZX_READABLE=off and the
# panel anchors left value-free (tools21, run t21-menus1: every in-level
# checkpoint's player and camera as the reference's). Not a fixed delta:
# the added widgets sit between them.
MENUS_PANELS = {0x8323E0: 0x831A80, 0x840F90: 0x8424B0, 0x847700: 0x848C60, 0x857D40: 0x8592A0,
                0x831BB0: 0x831240, 0x832CD0: 0x832360}
MENUS = os.environ.get("ZX_GAME_SETTINGS_MENUS") == "1"   # this process's route (run, script)
# In the level PCSX2 runs one main-loop frame per vsync, as native does, so
# after the level-start anchor the recorded presses already fall on the same
# level frames. Box anchors only stand the clock when native is late (WAIT),
# which keeps the box durations, and so the level time, as in PCSX2. SYNC
# (also jump forward when native is early) was used before.
BOX_ANCHOR = os.environ.get("LOGICCHECK_BOX_ANCHOR", "WAIT")
IGNORED_GAP = (24, 28)               # tips: next tap this many vsyncs later = this tap was ignored
KEEP_IGNORED = os.environ.get("LOGICCHECK_KEEP_IGNORED", "1") == "1"
# 0x27E0C4 is word 1 of D_0027E0C0, the 64-byte aligned buffer the
# sceSifCallRpc wrapper func_00117EE8 copies an unaligned send buffer into:
# the payload of the last such IOP request (sound driver calls; the values
# look like voice masks), not player state. Compared only with
# LOGICCHECK_CTRL=1.
CTRL_COMPARED = os.environ.get("LOGICCHECK_CTRL", "0") == "1"

# pcsx2pad.py writes an input through PINE during vsync V and the game reads
# it at its next pad poll, in frame V+1; the record says V. Natively a press
# at script time V is read in frame V. So recorded presses are delayed by
# INPUT_DELAY vsyncs, which puts native level frames, level time and state on
# the reference's (calibrated on ref05: bit for bit, same level time).
INPUT_DELAY = int(os.environ.get("LOGICCHECK_INPUT_DELAY", "1"))
DUMP_OFFSET = int(os.environ.get("LOGICCHECK_DUMP_OFFSET", "0"))

BUTTONS = {
    "SELECT": 0x0001, "L3": 0x0002, "R3": 0x0004, "START": 0x0008,
    "UP": 0x0010, "RIGHT": 0x0020, "DOWN": 0x0040, "LEFT": 0x0080,
    "L2": 0x0100, "R2": 0x0200, "L1": 0x0400, "R1": 0x0800,
    "TRIANGLE": 0x1000, "CIRCLE": 0x2000, "CROSS": 0x4000, "SQUARE": 0x8000,
}


def native_panel(value, menus=None):
    """A PCSX2 zgui panel's native address on this route (menus: MENUS by default)."""
    if not 0x800000 <= value < 0x900000:
        return value
    if MENUS if menus is None else menus:
        return MENUS_PANELS[value]
    return value + PANEL_HEAP_DELTA


def native_panel_back(value, menus=None):
    """A native zgui panel in PCSX2 terms. With the menus, a panel that is none
    of MENUS_PANELS has no PCSX2 address: shown negated (never a reference's)."""
    if not 0x800000 <= value < 0x900000:
        return value
    if MENUS if menus is None else menus:
        back = {v: k for k, v in MENUS_PANELS.items()}
        return back.get(value, -value)
    return value - PANEL_HEAP_DELTA


# ---- reference run ---------------------------------------------------------

def read_inputs():
    """(firstpoll, [(start_vsync, button, duration_vsyncs)]) from the record."""
    first, events = None, []
    for line in open(INPUT_LOG):
        if line.startswith("#"):
            if "firstpoll" in line:
                first = int(line.rsplit(" ", 1)[1])
            continue
        v, kind, val = line.split()
        if kind == "B":
            events.append((int(v), int(val, 16)))
    presses, held = [], {}
    for v, mask in events + [(events[-1][0] + 1, 0)]:
        for name, bit in BUTTONS.items():
            if mask & bit and name not in held:
                held[name] = v
            elif not (mask & bit) and name in held:
                start = held.pop(name)
                presses.append((start, name, max(1, v - start)))
    presses.sort()
    return first, presses


def read_run_out():
    """Commands of the reference run with their outcome vsyncs."""
    cmds = []
    cur = None
    for line in open(RUN_OUT):
        m = re.match(r"\[\d+\] (.*?)\s+\(vsync (\d+)\)$", line.rstrip())
        if m:
            cur = {"cmd": m.group(1), "start": int(m.group(2)), "end": None, "result": None}
            cmds.append(cur)
            continue
        m = re.search(r"\(vsync (\d+)(?:, game frame (\d+))?\)$", line.rstrip())
        if m and cur is not None and line.startswith(("  ", "checkpoint ")):
            cur["end"] = int(m.group(1))
            cur["result"] = line.strip()
    return cmds


def checkpoints():
    """[(name, vsync)] in run order."""
    out = []
    for line in open(RUN_OUT):
        m = re.match(r"checkpoint (ref\S+): .*\(vsync (\d+), game frame (\d+)\)", line)
        if m:
            out.append((m.group(1), int(m.group(2))))
    return out


def u32(ram, addr):
    return struct.unpack_from("<I", ram, addr)[0]


def f32(ram, addr, n=1):
    return struct.unpack_from("<%df" % n, ram, addr)


def first_level_checkpoint():
    """(vsync, level frame) of the first in-level reference checkpoint."""
    for name, v in checkpoints():
        path = os.path.join(REF_RAM, name + ".ram")
        if os.path.exists(path):
            ram = open(path, "rb").read()
            if u32(ram, MODE) in (2, 3):
                return u32(ram, VSYNC), u32(ram, FRAME)
    raise SystemExit("no in-level reference RAM image")


def level_start_vsync():
    """PCSX2 vsync of level frame 0 if every level frame took one vsync
    (true from the first in-level checkpoint on; earlier frames can take
    two)."""
    v, f = first_level_checkpoint()
    return v - f


def tips_windows(cmds):
    return [(c["start"], c["end"]) for c in cmds if c["cmd"].startswith("tips") and c["end"]]


def box_taps(cmds, presses):
    """(effective, ignored): CROSS taps of `tips` chains, as (start, dur)."""
    effective, ignored = [], []
    for a, b in tips_windows(cmds):
        taps = [(s, d) for s, n, d in presses if n == "CROSS" and a <= s <= b]
        for i, (s, d) in enumerate(taps):
            gap = taps[i + 1][0] - s if i + 1 < len(taps) else None
            if gap is not None and IGNORED_GAP[0] <= gap <= IGNORED_GAP[1]:
                ignored.append((s, d))
            else:
                effective.append((s, d))
    return effective, ignored


def anchors():
    """(firstpoll, [(vsync, condition, why)]) in run order."""
    out = []
    cmds = read_run_out()
    first, presses = read_inputs()
    level0 = level_start_vsync()
    for c in cmds:
        argv = c["cmd"].split()
        if not argv or c["end"] is None:
            continue
        verb = argv[0]
        if verb in ("waitval", "pressuntil"):
            addr_i = 1 if verb == "waitval" else 2
            addr, typ, op, val = argv[addr_i], argv[addr_i + 1], argv[addr_i + 2], argv[addr_i + 3]
            addr = int(addr, 0)
            # Front-end waits only; in-level mode/panel waits are goal
            # outcomes (compared, not anchored).
            if typ != "u32" or addr not in (MODE, PANEL) or c["end"] >= level0:
                continue
            value = int(val, 0)
            if addr == PANEL:
                value = native_panel(value)
            out.append((c["end"], "0x%X%s0x%X" % (addr, op, value), c["cmd"], "SYNC"))
    # The level frame counter of the first in-level checkpoint, read from its
    # RAM image, at the vsync it was taken.
    anchor_vsync, anchor_frame = first_level_checkpoint()
    out.append((anchor_vsync, "0x%X==%d" % (FRAME, anchor_frame), "level frame %d" % anchor_frame, "SYNC"))
    effective, _ = box_taps(cmds, presses)
    for start, _ in effective:
        out.append((start - 1 + INPUT_DELAY, "0x%X==0x%X" % (PANEL, native_panel(BOX_WAITING)), "box closed by CROSS at %d" % start,
                    BOX_ANCHOR))
    out.sort(key=lambda a: a[0])
    return first, out


def native_script():
    first, presses = read_inputs()
    _, anch = anchors()
    _, ignored = box_taps(read_run_out(), presses)
    ignored = set(ignored)
    if KEEP_IGNORED:
        ignored = set()
    items = [(s - first + INPUT_DELAY, "%dv:%s:%dv" % (s - first + INPUT_DELAY, n, d)) for s, n, d in presses
             if not (n == "CROSS" and (s, d) in ignored)]
    items += [(v - first - 0.5, "%dv:%s=%s" % (v - first, kind, cond)) for v, cond, _, kind in anch]
    items.sort(key=lambda i: i[0])
    return ",".join(i for _, i in items)




def level_anchor_event():
    """(checkpoint name, native condition) for the first in-level checkpoint, or
    None. The reference took that checkpoint the moment its waitval on the zgui
    panel held (ref03: the first tip's box waiting for X). That box opens on a
    script timer (the tutorial's TutorialAssignerOne, TimerFinished) that counts
    frame seconds through the level's first frames, where the adaptive frame
    limiter's seconds follow EE time: PCSX2 and readable off at the PS2's clock
    open it at level frame 310, the defaults at that clock and every run at
    ZX_EE_SCALE=4096 at 332 (level time 22/60 s behind from then on, which no
    other checkpoint's fields see). At the level-frame anchor (352) the box is
    then waiting or still opening by timing alone, so the native dump is taken
    where the reference's was: at its script time once the same panel holds."""
    name = next(ck for ck, _ in checkpoints()
                if u32(open(os.path.join(REF_RAM, ck + ".ram"), "rb").read(), MODE) in (2, 3))
    cmds = read_run_out()
    at = next(i for i, c in enumerate(cmds) if c["cmd"].split()[:1] == ["checkpoint"] and c["cmd"].split()[-1] == name)
    argv = cmds[at - 1]["cmd"].split() if at else []
    if argv[:1] != ["waitval"] or int(argv[1], 0) != PANEL or argv[2:4] != ["u32", "=="]:
        return None
    return name, "0x%X==0x%X" % (PANEL, native_panel(int(argv[4], 0)))


def dump_spec():
    # LOGICCHECK_DUMPS=name[,name...]: dump only these checkpoints.
    only = set(filter(None, os.environ.get("LOGICCHECK_DUMPS", "").split(",")))
    first, _ = read_inputs()
    event = level_anchor_event()
    return ",".join("%s:script>=%d%s" % (name, v - first + DUMP_OFFSET, "&" + event[1] if event and event[0] == name else "")
                    for name, v in checkpoints() if not only or name in only)


# ---- native run ------------------------------------------------------------

def run(name, secs):
    os.makedirs(NATIVE_RAM, exist_ok=True)
    env = dict(os.environ)
    # This run's dumps only: an old one would stop the run early.
    only = set(filter(None, os.environ.get("LOGICCHECK_DUMPS", "").split(",")))
    names = [ck for ck, _ in checkpoints() if not only or ck in only]
    for ck in names:
        path = "%s-%s.ram" % (os.path.join(NATIVE_RAM, name), ck)
        if os.path.exists(path):
            os.remove(path)
    if names:
        env["RUN_UNTIL"] = "%s-%s.ram" % (os.path.join(NATIVE_RAM, name), names[-1])
    # A card and home of the run's own, so runs can overlap and the user's
    # settings (and Library) stay out of it.
    scratch = os.path.join("/tmp", "logiccheck", name)
    shutil.rmtree(scratch, ignore_errors=True)
    shutil.copytree(os.path.join(WORKSPACE, "recomp", "run", "mc0"), os.path.join(scratch, "mc0"))
    env.setdefault("ZX_MC_ROOT", os.path.join(scratch, "mc0"))
    if os.path.realpath(env.get("HOME", "")) == os.path.realpath(pwd.getpwuid(os.getuid()).pw_dir):
        os.makedirs(os.path.join(scratch, "home"))
        env["HOME"] = os.path.join(scratch, "home")
    env.setdefault("ZX_GS_NULL", "1")
    env.setdefault("ZX_GS_BACKEND", "cpu")  # pictures from the reference renderer
    # The memory card path: the reference's boot includes PCSX2's card
    # dialogs (native save files skip them).
    env.setdefault("ZX_SAVES", "card")
    env["ZX_PAD_SCRIPT_CLOCK"] = "vsync"
    env["ZX_PAD_SCRIPT"] = native_script()
    env["ZX_DUMP_WHEN"] = dump_spec()
    env["ZX_DUMP_PREFIX"] = os.path.join(NATIVE_RAM, name)
    with open(os.path.join(NATIVE_RAM, name + ".route"), "w") as f:
        f.write("menus\n" if MENUS else "plain\n")
    env.setdefault("ZX_STATE_LOG", ",".join("0x%X" % a for a in (MODE, PANEL, SECTION, GAUGE, FRAME, POS, POS + 4, POS + 8)))
    subprocess.call([os.path.join(WORKSPACE, "recomp", "run.sh"), str(secs), name], env=env)


# ---- comparison ------------------------------------------------------------

def yaw(r):
    # Forward = -row2 of the world matrix; yaw from +x towards +z.
    return math.degrees(math.atan2(-r[10], -r[8]))


def state(ram, native, menus=None):
    d = 1 if native else 0
    skin = SKIN_NODE[0] + (SKIN_NODE[1] if native else 0)
    cam = CAMERA_NODE[0] + (CAMERA_NODE[1] if native else 0)
    m = f32(ram, skin + 0x10, 16)
    panel = u32(ram, PANEL)
    if native:
        panel = native_panel_back(panel, menus)   # show in PCSX2 terms
    return {
        "mode": u32(ram, MODE),
        "panel": panel,
        "section": u32(ram, SECTION),
        "gauge": u32(ram, GAUGE),
        "frame": u32(ram, FRAME),
        "time": f32(ram, LEVEL_TIME)[0],
        "ctrl": u32(ram, CTRL),
        "pos": f32(ram, POS, 3),
        "node": m[12:15],
        "yaw": yaw(m),
        "eye": f32(ram, EYE, 3),
        "look": f32(ram, LOOK, 3),
        "fov": f32(ram, cam - 0x0C)[0],
        "view": f32(ram, cam + 0x10, 12),
    }


def vec(v):
    return "(%s)" % ", ".join("%.3f" % x for x in v)


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def diff_states(r, n, tol=0.01):
    diffs = []
    level = r["mode"] in (2, 3)
    for k in ("mode", "panel", "section", "gauge") + (("ctrl",) if level and CTRL_COMPARED else ()):
        if r[k] != n[k]:
            diffs.append("%s %x/%x" % (k, r[k], n[k]))
    if level:
        for k in ("pos", "node", "eye", "look"):
            e = dist(r[k], n[k])
            if e > tol:
                diffs.append("%s %s/%s (d=%.3f)" % (k, vec(r[k]), vec(n[k]), e))
        if abs(r["yaw"] - n["yaw"]) > 0.1:
            diffs.append("yaw %.1f/%.1f" % (r["yaw"], n["yaw"]))
        ve = max(abs(a - b) for a, b in zip(r["view"], n["view"]))
        if ve > tol:
            diffs.append("view max|d|=%.3f" % ve)
    return diffs


def print_rows(rows, event=None):
    """event: the checkpoint dumped at the reference's event (level_anchor_event):
    its row also says the level time of each side, the EE timing the event leaves
    out of the compared fields."""
    worst_first = None
    for ck, r, n, *off in rows:
        if n is None or r is None:
            print("%-20s %s missing" % (ck, "native dump" if n is None else "reference"))
            continue
        diffs = diff_states(r, n)
        note = ""
        # What readable code does not change: `off` at the same clock shows it
        # too, field for field. Nothing else is let through.
        if diffs and off and off[0] is not None and diff_states(r, off[0]) == diffs and not diff_states(off[0], n):
            note = "as off at the same clock: "
        status = "MATCH" if not diffs or note else "DIFF"
        if status == "DIFF" and worst_first is None:
            worst_first = ck
        at = ("at the reference's event, level time %.4f/%.4f%s" % (r["time"], n["time"], "; " if diffs else "")
              if ck == event else "")
        print("%-20s %-5s frame %5d/%-5d %s%s%s" % (ck, status, r["frame"], n["frame"], at, note, "; ".join(diffs)))
    if worst_first:
        print("first divergence: %s" % worst_first)


def run_route(name):
    """True if run NAME took the menus route (its NAME.route; a run from before
    routes: this process's)."""
    path = os.path.join(NATIVE_RAM, name + ".route")
    return open(path).read().strip() == "menus" if os.path.exists(path) else MENUS


def native_state(name, ck):
    np_ = os.path.join(NATIVE_RAM, "%s-%s.ram" % (name, ck))
    return state(open(np_, "rb").read(), True, run_route(name)) if os.path.exists(np_) else None


def compare(name, exact=False, off=None):
    rows = []
    for ck, _ in checkpoints():
        rp = os.path.join(REF_RAM, ck + ".ram")
        rows.append((ck, state(open(rp, "rb").read(), False), native_state(name, ck), native_state(off, ck) if off else None))
    event = level_anchor_event()
    print_rows(rows, event[0] if event else None)
    if exact:
        compare_exact(name)


# Words compared bit for bit by `compare --exact`: (label, PCSX2 address,
# native offset, words). Skin node rotation rows and translation (not the w
# column, which holds unrelated data), camera eye/look-at, level time, the
# sound trigger stamps.
EXACT_WORDS = [
    ("pos", POS, 0, 3),
    ("node", SKIN_NODE[0] + 0x10, SKIN_NODE[1], 3), ("node", SKIN_NODE[0] + 0x20, SKIN_NODE[1], 3),
    ("node", SKIN_NODE[0] + 0x30, SKIN_NODE[1], 3), ("node", SKIN_NODE[0] + 0x40, SKIN_NODE[1], 3),
    ("eye", EYE, 0, 3), ("look", LOOK, 0, 3),
    ("time", LEVEL_TIME, 0, 1), ("stamps", 0x276FF0, 0, 2),
]


def compare_exact(name):
    """Level checkpoints whose player, camera, level time and sound stamps
    are bit-identical to run 1."""
    for ck, _ in checkpoints():
        ref = open(os.path.join(REF_RAM, ck + ".ram"), "rb").read()
        np_ = os.path.join(NATIVE_RAM, "%s-%s.ram" % (name, ck))
        if u32(ref, MODE) not in (2, 3) or not os.path.exists(np_):
            continue
        nat = open(np_, "rb").read()
        bad = sorted({label for label, a, d, n in EXACT_WORDS
                      for i in range(n) if u32(ref, a + 4 * i) != u32(nat, a + d + 4 * i)})
        print("%-20s %s" % (ck, "EXACT" if not bad else "differs: " + " ".join(bad)))


def px_records(path):
    """{vsync: {address: u32}} from a tools/pcsx2replay.py log (first record per vsync)."""
    hdr = open(path).readline()
    watch = hdr.split("watch ")[1].split()[0]
    cols = []
    for item in watch.split(","):
        a, _, n = item.partition(":")
        cols += [int(a, 0) + 4 * i for i in range(int(n or "1", 0))]
    recs = {}
    for line in open(path):
        if line.startswith("#"):
            continue
        r = [int(x, 16) for x in line.split()]
        recs.setdefault(r[0], dict(zip(cols, r[2:])))
    return recs


def px_state(rec, prev):
    """state() at vsync V from the PCSX2 replay records of vsyncs V+1 (rec)
    and V (prev). A record is taken at the game's pad read. The frame logic
    runs between the pad read and the flip, so the read of V+1 holds what a
    savestate (or a native VBlank dump) at V holds, except what the main loop
    updates after the flip (0x100AD4): the frame counter (one less at V) and
    the zgui panel (func_00109AB8; still the value of the read of V)."""
    u = lambda a: rec[a]
    f = lambda a: struct.unpack("<f", struct.pack("<I", rec[a]))[0]
    skin = SKIN_NODE[0] + 0x10
    cam = CAMERA_NODE[0]
    m = [f(skin + 4 * i) for i in range(16)]
    return {
        "mode": u(MODE), "panel": prev[PANEL], "section": u(SECTION), "gauge": u(GAUGE),
        "frame": u(FRAME) - 1, "ctrl": u(CTRL),
        "pos": tuple(f(POS + 4 * i) for i in range(3)), "node": tuple(m[12:15]), "yaw": yaw(m),
        "eye": tuple(f(EYE + 4 * i) for i in range(3)), "look": tuple(f(LOOK + 4 * i) for i in range(3)),
        "fov": f(cam - 0x0C), "view": tuple(f(cam + 0x10 + 4 * i) for i in range(12)),
    }


def pxcompare(name, pxlog):
    """Native checkpoint dumps against PCSX2 replaying the same input
    (tools/pcsx2replay.py run boot ... --watch with every field state() reads):
    where the recorded input is not exactly what the closed-loop reference
    run did, this is the reference for native correctness."""
    recs = px_records(pxlog)
    import bisect
    keys = sorted(recs)
    rows = []
    for ck, v in checkpoints():
        # Front-end frames can take more than one vsync: the first read after
        # V and the last one at or before it.
        i = bisect.bisect_left(keys, v + 1)
        j = bisect.bisect_right(keys, v) - 1
        rec = recs[keys[i]] if i < len(keys) else None
        prev = recs[keys[j]] if j >= 0 else None
        rows.append((ck, px_state(rec, prev) if rec and prev else None, native_state(name, ck)))
    print_rows(rows)


def read_state_log(name):
    """[(tick, {addr: u32})] from the [state] lines of a run log."""
    rows = []
    for line in open(os.path.join(LOGS, name + ".log"), errors="replace"):
        if not line.startswith("[state] tick="):
            continue
        parts = line.split()
        try:
            tick = int(parts[1].split("=")[1])
            vals = {int(k, 16): int(v, 16) for k, v in (p.split("=") for p in parts[2:])}
        except ValueError:
            continue
        rows.append((tick, vals))
    return rows


def as_f32(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


def trajectory(name):
    """For every in-level checkpoint, the native frame whose position is
    nearest to the reference position: distance 0 at a frame offset means
    the motion matches and only the timing differs."""
    rows = read_state_log(name)
    if not rows or POS not in rows[-1][1]:
        raise SystemExit("run without the position in ZX_STATE_LOG")
    track = []
    for tick, v in rows:
        if v.get(MODE) == 2:
            track.append((tick, v[FRAME], tuple(as_f32(v[POS + i]) for i in (0, 4, 8))))
    for ck, _ in checkpoints():
        rp = os.path.join(REF_RAM, ck + ".ram")
        np_ = os.path.join(NATIVE_RAM, "%s-%s.ram" % (name, ck))
        ram = open(rp, "rb").read()
        if u32(ram, MODE) != 2 or not os.path.exists(np_):
            continue
        ref = f32(ram, POS, 3)
        nat = open(np_, "rb").read()
        nframe = u32(nat, FRAME)
        near = [t for t in track if abs(t[1] - nframe) <= 120]
        if not near:
            continue
        best = min(near, key=lambda t: dist(t[2], ref))
        print("%-20s ref %s  nearest native %s at frame %d (dump frame %d, offset %+d) d=%.4f" % (
            ck, vec(ref), vec(best[2]), best[1], nframe, best[1] - nframe, dist(best[2], ref)))


def diffruns(a, b):
    """First in-level frame where the player position of two native runs
    differs (bit for bit), and the largest distance after it."""
    def track(name):
        out, level = {}, False
        for tick, v in read_state_log(name):
            if FRAME not in v or POS not in v:
                continue
            if v.get(MODE) in (2, 3) and v[FRAME] < 100:
                level = True
            if level:
                out.setdefault(v[FRAME], tuple(v[POS + i] for i in (0, 4, 8)))
        return out
    ta, tb = track(a), track(b)
    common = sorted(set(ta) & set(tb))
    first = next((f for f in common if ta[f] != tb[f]), None)
    print("%d common level frames" % len(common))
    if first is None:
        print("positions identical in every common frame")
        return
    worst = max(common[common.index(first):], key=lambda f: dist([as_f32(x) for x in ta[f]], [as_f32(x) for x in tb[f]]))
    print("first difference at level frame %d: %s vs %s" % (first, vec([as_f32(x) for x in ta[first]]), vec([as_f32(x) for x in tb[first]])))
    print("largest at frame %d: d=%.4f" % (worst, dist([as_f32(x) for x in ta[worst]], [as_f32(x) for x in tb[worst]])))


def script_ticks(name):
    """[(tick, script_time)] at every anchor of a native run, from the
    "[pad] script origin" and "[pad] sync" log lines. Between anchors the
    script clock runs with the tick (it only jumps or stands at anchors)."""
    pts = []
    for line in open(os.path.join(LOGS, name + ".log"), errors="replace"):
        m = re.match(r"\[pad\] script origin at vsync tick (\d+)", line)
        if m:
            pts.append((int(m.group(1)), 0))
        m = re.match(r"\[pad\] sync \d+ at script (\d+)v \(tick (\d+)\)", line)
        if m:
            pts.append((int(m.group(2)), int(m.group(1))))
    return pts


def waypoints(name):
    """Reference positions read during the run (goto "reached", peekf) against
    the native position at the same script time, and the nearest native
    frame within +-30 ticks."""
    first, _ = read_inputs()
    anchors_ = script_ticks(name)
    pos_by_tick = {}
    last = None
    rows = read_state_log(name)
    for tick, v in rows:
        if POS in v:
            last = tuple(as_f32(v[POS + i]) for i in (0, 4, 8))
        pos_by_tick[tick] = last
    ticks = sorted(pos_by_tick)

    def at(tick):
        # State lines are only written on change: take the latest at or before tick.
        import bisect
        i = bisect.bisect_right(ticks, tick) - 1
        return pos_by_tick[ticks[i]] if i >= 0 else None

    for line in open(RUN_OUT):
        m = re.search(r"reached (\S+) (\S+) (\S+) \(vsync (\d+)", line) or \
            re.match(r"002a76f0: (\S+) (\S+) (\S+) \(vsync (\d+)", line)
        if not m:
            continue
        ref = tuple(float(m.group(i)) for i in (1, 2, 3))
        st = int(m.group(4)) - first
        base = [a for a in anchors_ if a[1] <= st]
        if not base:
            continue
        t0, s0 = base[-1]
        tick = t0 + (st - s0)
        nat = at(tick)
        if nat is None or tick > ticks[-1]:
            print("ref vsync %5d %s  native not reached" % (st + first, vec(ref)))
            continue
        best = min(range(-30, 31), key=lambda d: dist(at(tick + d) or (1e9, 1e9, 1e9), ref))
        print("ref vsync %5d %s  native %s d=%.3f; nearest %+d ticks d=%.3f" % (
            st + first, vec(ref), vec(nat), dist(nat, ref), best, dist(at(tick + best), ref)))


def show_anchors(name):
    log = os.path.join(LOGS, name + ".log")
    _, anch = anchors()
    shifts = {}
    for line in open(log, errors="replace"):
        m = re.match(r"\[pad\] sync (\d+) at script (\d+)v \(tick (\d+)\): clock ([+-]\d+) vsyncs", line)
        if m:
            shifts[int(m.group(1))] = (int(m.group(3)), int(m.group(4)))
    first, _ = read_inputs()
    for i, (v, cond, why, kind) in enumerate(anch):
        s = shifts.get(i)
        print("%3d ref %5d  %-22s %-40s %s" % (i, v, cond, why[:40],
                                              "tick %d shift %+d" % s if s else "not reached"))


# ---- per-frame comparison against a PCSX2 replay ---------------------------

def pad_state_log(name):
    """(addresses, [(tick, [u32])]) from the [pad-state] lines (ZX_PAD_LOG)."""
    cols, rows = None, []
    for line in open(os.path.join(LOGS, name + ".log"), errors="replace"):
        if not line.startswith("[pad-state] tick="):
            continue
        parts = line.split()
        try:
            tick = int(parts[1].split("=")[1])
            kv = [p.split("=") for p in parts[2:]]
            vals = [int(v, 16) for _, v in kv]
        except (ValueError, IndexError):
            continue          # a line interleaved with other output
        if cols is None:
            cols = [int(k, 16) for k, _ in kv]
        if len(vals) == len(cols):
            rows.append((tick, vals))
    return cols, rows


def px_log_rows(path):
    """(addresses, [(vsync, [u32])]) from a tools/pcsx2replay.py log."""
    hdr = open(path).readline()
    watch = hdr.split("watch ")[1].split()[0]
    cols = []
    for item in watch.split(","):
        a, _, n = item.partition(":")
        cols += [int(a, 0) + 4 * i for i in range(int(n or "1", 0))]
    rows = []
    for line in open(path):
        if not line.startswith("#"):
            r = [int(x, 16) for x in line.split()]
            rows.append((r[0], r[2:]))
    return cols, rows


def by_level_frame(cols, rows):
    """{level frame: (vsync or tick, values)} after the last frame counter reset."""
    fi, mi = cols.index(FRAME), cols.index(MODE)
    start = 0
    for i in range(1, len(rows)):
        if rows[i][1][fi] < rows[i - 1][1][fi] and rows[i][1][mi] in (0, 2, 3):
            start = i
    out = {}
    for k, v in rows[start:]:
        out.setdefault(v[fi], (k, v))
    return out


def frames(name, pxlog, args):
    """First level frames where native (ZX_PAD_LOG) and the PCSX2 replay
    (pcsx2replay.py --watch, same addresses in the same order; heap addresses
    may differ) disagree. Both are taken at the game's pad read, so a frame's
    values are directly comparable. The SIF RPC payload word CTRL is left out
    unless --all is given; zgui panel pointers are mapped to PCSX2 addresses."""
    pc, pr = px_log_rows(pxlog)
    nc, nr = pad_state_log(name)
    if len(pc) != len(nc):
        raise SystemExit("watch lists differ: %d vs %d words" % (len(pc), len(nc)))
    if PANEL in nc:
        pi = nc.index(PANEL)
        nr = [(k, [native_panel_back(x) if i == pi else x for i, x in enumerate(v)]) for k, v in nr]
    P, N = by_level_frame(pc, pr), by_level_frame(nc, nr)
    skip = set() if "--all" in args else {CTRL}
    first = int(args[args.index("--from") + 1]) if "--from" in args else 0
    limit = int(args[args.index("--limit") + 1]) if "--limit" in args else 20
    common = sorted(set(P) & set(N))
    print("level frames: pcsx2 %d..%d, native %d..%d" % (min(P), max(P), min(N), max(N)))
    shown = 0
    for fr in common:
        if fr < first:
            continue
        pv, nv = P[fr][1], N[fr][1]
        d = [(pc[i], pv[i], nv[i]) for i in range(len(pv)) if pv[i] != nv[i] and pc[i] not in skip]
        if d:
            print("%6d vsync %d tick %d: %s" % (fr, P[fr][0], N[fr][0],
                                                 " ".join("%x:%x/%x" % x for x in d)))
            shown += 1
            if shown >= limit:
                break
    if not shown:
        print("identical in %d common frames from %d" % (len([f for f in common if f >= first]), first))


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return
    verb = argv[1]
    if verb == "script":
        print(native_script())
    elif verb == "dumpspec":
        print(dump_spec())
    elif verb == "run":
        run(argv[2], int(argv[3]) if len(argv) > 3 else 900)
    elif verb == "compare":
        compare(argv[2], "--exact" in argv[3:], argv[argv.index("--off") + 1] if "--off" in argv else None)
    elif verb == "pxcompare":
        pxcompare(argv[2], argv[3])
    elif verb == "frames":
        frames(argv[2], argv[3], argv[4:])
    elif verb == "anchors":
        show_anchors(argv[2])
    elif verb == "trajectory":
        trajectory(argv[2])
    elif verb == "waypoints":
        waypoints(argv[2])
    elif verb == "diffruns":
        diffruns(argv[2], argv[3])
    else:
        raise SystemExit("unknown verb %s" % verb)


if __name__ == "__main__":
    main(sys.argv)
