#!/usr/bin/env python3
"""BMX XXX logic check: the native runner against the PCSX2 reference run (dm2logic.py's method).

Reference run 1 (tools/scripts/bmx-ref-run1.txt, driven by tools/bmxpcsx2.py in PCSX2 2.8.2): fresh boot (an
empty formatted card), the title, the main menu (cursor down and back), HARDCORE TOUR, the rider select (right
and back), the level select (right and back), the Bronx. In the level the input is a schedule by level frame
(tools/scripts/bmx-ref-run1-schedule.txt, applied inside the EE by bmxpad_hook.s, so on exactly those frames):
the overview and CHALLENGES dismissed, down the street and off the kerb, a hop onto the low rail and a grind
(line 1,309 banked), Homeless Jones's challenge (TRIANGLE by him, his lines: "Cans collected: 0 of 10"), cans
04, 09 and 10 ridden through, a hop with a trick (SQUARE+LEFT, score 2,409), a stop. The hook's seen log
(tools/scripts/bmx-ref-run1-seen.txt) holds every input change with the vsync and the level frame the game read
it on. The checkpoints are PCSX2 savestates; their values are in tools/scripts/bmx-ref-run1-values.txt (the RAM
images stay in $BMX_REF_DATA/ref, not committed).

The native script is the seen log, in vsyncs from the first pad read (the native ZX_PAD_SCRIPT origin):
  - front end: the presses at their vsyncs, with one SYNC anchor at the title (mode 0x29D040 becomes 1);
  - level: every input change at level frame f is anchored on the frame counter, `t:SYNC=0x2A2DE0>=f-1`, and
    takes effect half a vsync after t, so it lands on the pad read of frame f whether the native clock is early
    or late there (the schedule's changes are >= 2 frames apart, so each anchor's condition is false once after
    the previous one fired, as SYNC needs).
Front-end checkpoints are dumped at their script time (ZX_DUMP_WHEN script>=), level ones at their level
frame (0x2A2DE0>=f), at a VBlank like the savestates.

Compared at each checkpoint (career_ab.py's bmx fields: mode 0x29D040, level frame 0x2A2DE0, rider table
0x2F3A30; the rider's offsets are playbot bmx.py's): mode; the level being played (its folder, 0x283020); in
the front end the focused menu (the zgui focus 0x253958: its frames' names and selected index); in the level
the rider's position, velocity and heading within TOL (dm2logic's 0.01), physics state, trick, run score and
line score exactly; the goals (the objectives list at 0x2A0A14: every objective's flags and count) exactly.

  python3 -I tools/bmxlogic.py script            # the native ZX_PAD_SCRIPT
  python3 -I tools/bmxlogic.py refvalues         # (re)write the values file from $BMX_REF_DATA/ref/*.ram
  python3 -I tools/bmxlogic.py run NAME [SECS]   # run natively, dumps in $BMX_REF_DATA/dumps/logic/NAME-*.ram
  python3 -I tools/bmxlogic.py compare NAME [--off OFFNAME]
                                                 # reference values vs native dumps (--off: the same input with
                                                 # ZX_READABLE=off at the same clock, kept: a checkpoint that
                                                 # differs from the reference only as OFFNAME's does is a MATCH)
  python3 -I tools/bmxlogic.py show RAMFILE      # the compared values of any EE RAM image

Environment: RUNNER (default the collection build, $ZAXIS_MAIN/out/build-coll), ELF (default
$BMX_DATA/disc/SLUS_204.15), BMX_REF_DATA ($ZAXIS_DATA/bmx: ref/, dumps/, logs/), BMX_SCRATCH
($ZAXIS_SCRATCH/bmxlogic: HOME, app support, memory cards), ZX_GS_NULL (default 1) and other ZX_*
pass through. BMXLOGIC_FRAME_SKEW (default 1, dm2logic's): how many level frames later a native dump may sample;
BMXLOGIC_INPUT_DELAY (level frames, default 0) moves every level input, to see the check catch a difference;
BMXLOGIC_KEEP=1 keeps the native dumps of a run that matches (else `compare` deletes them).
"""
import json
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
import zpaths  # noqa: E402  (tools/paths.env)

SCRIPTS = os.path.join(ROOT, "tools", "scripts")
SEEN = os.path.join(SCRIPTS, "bmx-ref-run1-seen.txt")
COMMANDS = os.path.join(SCRIPTS, "bmx-ref-run1-commands.txt")
VALUES = os.path.join(SCRIPTS, "bmx-ref-run1-values.txt")
REF_DATA = os.environ.get("BMX_REF_DATA", os.path.join(zpaths.ZAXIS_DATA, "bmx"))
REF_RAM = os.path.join(REF_DATA, "ref")
NATIVE_RAM = os.path.join(REF_DATA, "dumps", "logic")
LOGS = os.path.join(REF_DATA, "logs")
RUNNER = os.environ.get("RUNNER", os.path.join(zpaths.ZAXIS_MAIN, "out", "build-coll", "runtime", "zx-runner"))
ELF = os.environ.get("ELF", os.path.join(zpaths.BMX_DATA, "disc", "SLUS_204.15"))
SCRATCH = os.environ.get("BMX_SCRATCH", os.path.join(zpaths.ZAXIS_SCRATCH, "bmxlogic"))

# BMX XXX anchors (career_ab.py's bmx fields; tools/bmxpcsx2.py)
MODE = 0x29D040          # 0 boot and movies, 1 front end, 2 loading and level, 3 paused
LEVEL_FRAME = 0x2A2DE0   # rider task frames since the level started; 0 in the front end
RIDER_TABLE = 0x2F3A30   # rider objects; [0] = player 1
LEVEL_PATH = 0x283020    # the level's folder ("assets\levels\CHL_MULY\")
FOCUS = 0x253958         # the zgui object with the focus: +0x08 parent, +0x14 name, +0x11C selected index
OBJECTIVES = 0x2A0A14    # the objectives list: +0 first node; node +0 next, +0x18 children, +0x20 count, +0x28 children
VSYNC = 0x0F7018         # the PCSX2 hook's VBlank counter (only in reference images)
RIDER = {"score": (0x1C0, "<f"), "line": (0x1C8, "<I"), "trick": (0x18E, "<B"), "physics": (0x2600, "<B"),
         "fwd": (0x2630, "<3f"), "pos": (0x2650, "<3f"), "vel": (0x26C0, "<3f")}
TOL = 0.01
FRAME_SKEW = int(os.environ.get("BMXLOGIC_FRAME_SKEW", "1"))
INPUT_DELAY = int(os.environ.get("BMXLOGIC_INPUT_DELAY", "0"))   # level frames every level input moves (a negative test)

BUTTONS = {
    "SELECT": 0x0001, "L3": 0x0002, "R3": 0x0004, "START": 0x0008,
    "UP": 0x0010, "RIGHT": 0x0020, "DOWN": 0x0040, "LEFT": 0x0080,
    "L2": 0x0100, "R2": 0x0200, "L1": 0x0400, "R1": 0x0800,
    "TRIANGLE": 0x1000, "CIRCLE": 0x2000, "CROSS": 0x4000, "SQUARE": 0x8000,
}


def u32(ram, a):
    return struct.unpack_from("<I", ram, a)[0]


def guest(ram, a):
    return 0x100000 <= a < len(ram) - 0x3000


def cstr(ram, a, n=48):
    t = ram[a:a + n].split(b"\0")[0]
    return t.decode("latin-1") if t and all(32 <= c < 127 for c in t) else ""


def focus(ram):
    """The focused menu: its names up to the frame ("FrameLevelSelect/OPTION/Menu") and its selected index."""
    o = u32(ram, FOCUS)
    if not guest(ram, o):
        return None
    sel, names = u32(ram, o + 0x11C), []
    while guest(ram, o) and len(names) < 6:
        p = u32(ram, o + 0x14)
        names.append(cstr(ram, p) if guest(ram, p) else "?")
        if names[-1].startswith("Frame"):
            break
        o = u32(ram, o + 0x08)
    return "/".join(reversed(names)) + ":%d" % sel


def common_name(names):
    """The objectives' shared name: their names' common start, cut back to a whole part ("CRACK_JONES")."""
    if not names:
        return "?"
    k = 0
    while all(len(x) > k for x in names) and len({x[k] for x in names}) == 1:
        k += 1
    if k == min(len(x) for x in names) and len(set(names)) == 1:
        return names[0]
    return names[0][:k].rsplit(".", 1)[0] if "." in names[0][:k] else names[0][:k]


def goals(ram):
    """The objectives: each node's count and its children's flags, as "NAME count flags,flags,..." strings."""
    head = u32(ram, OBJECTIVES)
    if not guest(ram, head):
        return None
    a, out = u32(ram, head), []
    while guest(ram, a) and len(out) < 400:
        n, count = u32(ram, a + 0x18), u32(ram, a + 0x20)
        kids = [u32(ram, a + 0x28 + 4 * i) for i in range(min(n, 16))]
        names = [cstr(ram, k + 4, 40) for k in kids if guest(ram, k)]
        name = common_name(names)
        flags = ",".join("%x" % u32(ram, k) for k in kids if guest(ram, k))
        out.append("%s %d %s" % (name, count, flags))
        a = u32(ram, a)
    return out


def state(ram):
    s = {"mode": u32(ram, MODE), "frame": u32(ram, LEVEL_FRAME), "level": cstr(ram, LEVEL_PATH)}
    if s["mode"] == 1:
        s["focus"] = focus(ram)
    obj = u32(ram, RIDER_TABLE)
    if s["frame"] and guest(ram, obj):
        for k, (off, fmt) in RIDER.items():
            v = struct.unpack_from(fmt, ram, obj + off)
            s[k] = list(v) if len(v) > 1 else v[0]
        s["posbits"] = "%08x %08x %08x" % struct.unpack_from("<3I", ram, obj + RIDER["pos"][0])
        g = goals(ram)
        if g is not None:
            s["goals"] = g
    return s


def fmt_state(s):
    out = "mode %d frame %d level %s" % (s["mode"], s["frame"], s["level"] or "-")
    if "focus" in s:
        out += " focus %s" % s["focus"]
    if "pos" in s:
        out += " pos %.4f %.4f %.4f vel %.4f %.4f %.4f physics %d trick %d score %.0f line %d" % (
            *s["pos"], *s["vel"], s["physics"], s["trick"], s["score"], s["line"])
    if s.get("goals"):
        live = [g for g in s["goals"] if g.split()[1] != "0" or "600" in g.split()[-1]]
        out += " goals %d (%s)" % (len(s["goals"]), "; ".join(live) or "none started")
    return out


# ---- reference ---------------------------------------------------------------

def read_seen():
    """(firstpoll, [(vsync, mask, frame)]) from the seen log."""
    first, ents = None, []
    for line in open(SEEN):
        if line.startswith("#"):
            first = int(line.split("firstpoll")[1].split()[0])
            continue
        f = line.split()
        ents.append((int(f[0]), int(f[1], 16), int(f[4])))
    return first, ents


def checkpoints():
    """[(name, vsync, frame)] from the reference run's command output."""
    out = []
    for line in open(COMMANDS):
        m = re.match(r"checkpoint (\S+): .*\(vsync (\d+), game frame (\d+)\)", line)
        if m:
            out.append((m.group(1), int(m.group(2)), int(m.group(3))))
    return out


def title_vsync():
    lines = open(COMMANDS).read().splitlines()
    for i, line in enumerate(lines):
        if line.startswith("[") and "waitval 0x29D040 u32 == 1" in line:
            return int(re.search(r"vsync (\d+)", lines[i + 1]).group(1))
    raise SystemExit("no title wait in %s" % COMMANDS)


def refvalues():
    first, _ = read_seen()
    lines = ["# BMX XXX reference run 1 (PCSX2 2.8.2, tools/scripts/bmx-ref-run1.txt): values at each checkpoint",
             "# name vsync script(=vsync-firstpoll %d) frame | state (JSON)" % first]
    for name, v, f in checkpoints():
        ram = open(os.path.join(REF_RAM, name + ".ram"), "rb").read()
        assert u32(ram, VSYNC) == v, (name, u32(ram, VSYNC), v)
        s = state(ram)
        lines.append("%s %d %d %d | %s" % (name, v, v - first, f, json.dumps(s, separators=(",", ":"))))
        print("%-24s %s" % (name, fmt_state(s)))
    open(VALUES, "w").write("\n".join(lines) + "\n")


def ref_values():
    out = []
    for line in open(VALUES):
        if line.startswith("#"):
            continue
        head, _, rest = line.partition("|")
        name, v, t, f = head.split()
        out.append((name, int(v), int(t), int(f), json.loads(rest)))
    return out


# ---- native ----------------------------------------------------------------

def native_script():
    first, ents = read_seen()
    items, held, prev = [], {}, 0
    title = title_vsync() - first
    items.append((title - 0.25, "%dv:SYNC=0x%X==1" % (title, MODE)))
    for v, mask, frame in ents + [(ents[-1][0] + 1, 0, ents[-1][2] + 2)]:
        t = v - first
        if frame > 0:                       # in the level: anchored on the frame counter, effective half a vsync on
            items.append((t - 0.25, "%dv:SYNC=0x%X>=%d" % (t, LEVEL_FRAME, frame - 1 + INPUT_DELAY)))
            at = t + 0.5
        else:
            at = t
        for name, bit in BUTTONS.items():
            if mask & bit and not prev & bit:
                held[name] = at
            elif prev & bit and not mask & bit:
                start = held.pop(name)
                items.append((start, "%gv:%s:%gv" % (start, name, at - start)))
        prev = mask
    items.sort(key=lambda i: i[0])
    return ",".join(i for _, i in items)


def dump_spec():
    return ",".join("%s:%s" % (name, "0x%X>=%d" % (LEVEL_FRAME, f) if f > 0 else "script>=%d" % t)
                    for name, _, t, f, _ in ref_values())


def run(name, secs):
    os.makedirs(NATIVE_RAM, exist_ok=True)
    os.makedirs(LOGS, exist_ok=True)
    for d in ("home", "mc0", "mc1", "run"):
        os.makedirs(os.path.join(SCRATCH, d), exist_ok=True)
    expected = [os.path.join(NATIVE_RAM, "%s-%s.ram" % (name, ck)) for ck, _, _, _, _ in ref_values()]
    for p in expected:
        if os.path.exists(p):
            os.remove(p)
    env = dict(os.environ)
    env["HOME"] = os.path.join(SCRATCH, "home")
    env["ZX_APP_SUPPORT_DIR"] = os.path.join(SCRATCH, "home", "support")
    env["ZX_APP_LOG_DIR"] = os.path.join(SCRATCH, "home", "logs")
    env.setdefault("ZX_MC_ROOT", os.path.join(SCRATCH, "mc0"))
    env.setdefault("ZX_MC_FD_BASE0", "1")       # (BMX's card as bmxroutes.py and the playbot run it)
    # The memory card path, as logiccheck: the reference runs went through
    # the card (BMX's native save files are the default and skip it).
    env.setdefault("ZX_SAVES", "card")
    env.setdefault("ZX_DEBUG_UI", "0")
    env.setdefault("ZX_HEADLESS", "1")
    env.setdefault("SDL_VIDEODRIVER", "dummy")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    env.setdefault("ZX_GS_BACKEND", "cpu")
    env.setdefault("ZX_GS_NULL", "1")
    env["ZX_HOST_INPUT"] = "0"
    env["ZX_PAD_STICK_CENTER"] = "0x7f"           # PCSX2's idle sticks (the seen log's 7f 7f)
    env["ZX_PAD_SCRIPT_CLOCK"] = "vsync"
    path = os.path.join(SCRATCH, "run", "script-%s.txt" % name)
    open(path, "w").write(native_script())
    env["ZX_PAD_SCRIPT"] = "@" + path
    env["ZX_DUMP_WHEN"] = dump_spec()
    env["ZX_DUMP_PREFIX"] = os.path.join(NATIVE_RAM, name)
    env.setdefault("ZX_STATE_LOG", ",".join("0x%X" % a for a in (MODE, LEVEL_FRAME, RIDER_TABLE)))
    log = os.path.join(LOGS, "logic-%s.log" % name)
    print("runner %s\nlog %s\nscript %s\ndumps %s" % (RUNNER, log, path, env["ZX_DUMP_WHEN"]))
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
    for l in syncs[:2] + late[:5]:
        print("  " + l)


def diffs_of(r, n):
    """(the differences of native state n from reference state r, the position note, whether pos is bit-exact)."""
    diffs = []
    for k in ("mode", "level", "focus", "physics", "trick", "line"):
        if r.get(k) != n.get(k):
            diffs.append("%s %s/%s" % (k, r.get(k), n.get(k)))
    if r.get("score") is not None and (n.get("score") is None or abs(r["score"] - n["score"]) > 0.5):
        diffs.append("score %s/%s" % (r.get("score"), n.get("score")))
    if r.get("goals") != n.get("goals"):
        ra, na = r.get("goals") or [], n.get("goals") or []
        d = [(x, y) for x, y in zip(ra, na) if x != y][:2]
        diffs.append("goals (%d/%d objectives) %s" % (len(ra), len(na), "; ".join("%s / %s" % x for x in d)))
    if n["frame"] - r["frame"] not in range(0, FRAME_SKEW + 1):
        diffs.append("frame %d/%d" % (r["frame"], n["frame"]))
    exact, bit_exact = "", True
    if "pos" in r:
        if "pos" not in n:
            diffs.append("no rider natively")
        else:
            exact_d = 0.0
            for k in ("pos", "vel", "fwd"):
                e = math.sqrt(sum((x - y) ** 2 for x, y in zip(r[k], n[k])))
                if k == "pos":
                    exact_d = e
                if e > TOL:
                    diffs.append("%s (%s)/(%s) d=%.4f" % (k, " ".join("%.3f" % x for x in r[k]),
                                                         " ".join("%.3f" % x for x in n[k]), e))
            bit_exact = r["posbits"] == n["posbits"]
            exact = " pos bit-exact" if bit_exact else " pos |d|=%.4f" % exact_d
    return diffs, exact, bit_exact


def compare(name, off=None):
    """The reference values against NAME's dumps. With OFF (the same input with ZX_READABLE=off at the same
    clock, its dumps kept: BMXLOGIC_KEEP=1), a checkpoint that differs from the reference only as OFF's does, in
    the same fields and values, and whose compared state equals OFF's exactly, is a MATCH and says so
    (logiccheck's rule)."""
    first_diff = first_inexact = None
    rows, matches = ref_values(), 0
    for ck, v, t, f, r in rows:
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
    if matches == len(rows) and os.environ.get("BMXLOGIC_KEEP") != "1":   # a pass deletes its dumps (32 MB each)
        for ck, _, _, _, _ in rows:
            for ext in (".ram", ".sp"):
                p = os.path.join(NATIVE_RAM, "%s-%s%s" % (name, ck, ext))
                if os.path.exists(p):
                    os.remove(p)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    if a[0] == "script":
        print(native_script())
    elif a[0] == "refvalues":
        refvalues()
    elif a[0] == "run":
        run(a[1], int(a[2]) if len(a) > 2 else 900)
    elif a[0] == "compare":
        compare(a[1], a[a.index("--off") + 1] if "--off" in a else None)
    elif a[0] == "show":
        print(fmt_state(state(open(a[1], "rb").read())))
    else:
        raise SystemExit("unknown command %r" % a[0])


if __name__ == "__main__":
    main()
