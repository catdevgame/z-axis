#!/usr/bin/env python3
"""Replay the Career part of PCSX2 reference run 2 natively and compare state.

The sibling of logiccheck.py for REFERENCE-RUN-2.md's Career run (fresh
boot, MOVIE LOT, pause menu, CHALLENGES, a memory card save, a grind, a
manual, an air grab). The native script is the recorded input
(scripts/native/ref-run2-career-input.txt, vsync units, delayed by
logiccheck.INPUT_DELAY) plus SYNC anchors on the game state the reference
script waited on (tools/scripts/career-*.txt). There is no command log for
this run, so the anchor times are derived from the script: every
"waitval COND; wait N; press" puts the event N vsyncs before the press.

  - front end: the same waits as run 1 (identical inputs up to car01);
  - level: the level clock (0x276FF8, f32 seconds since loading ended, also
    counting paused time; +1/60 per vsync) reaching its car05 value at
    vsync 3508. Juice drains and trick points accrue with this clock, and
    the grind balance meter starts drifting to a side chosen by the parity
    of a time value, so the clock must match PCSX2, not just the frame
    count (PCSX2 runs some fly-thru frames in two vsyncs and the game clamps
    long frames, so frame counter and clock drift apart differently).
    Loading takes ~10 vsyncs natively, ~1250 in PCSX2;
  - pause menu, CHALLENGES, back to PAUSE, the pause menu closing for SAVE:
    the zgui panel the reference waited on;
  - inside the save dialog (slot select, the "Are you sure" YES/NO and
    "Saving..." / "Saving complete.") nothing is anchored: the dialog timing
    is compared. Native card writes are instant and PCSX2's take ~5 s, so
    native returns to PAUSE early and idles there; the level clock again
    anchors the inputs that resume play (vsync 4650, before UP UP UP X).
    No anchor on play resuming: the reference's "mode == 2" was seen 10
    vsyncs late (pcsx2pad polls after its 6-vsync tap), natively it comes
    one frame after the X.

Dumps: car01/car02 and car05..car16 at their script time; car03 (loading)
and car04 (fly-thru, level frame 200) on the level frame counter, because
native loading is ~1240 vsyncs shorter.

`run` uses the memory card path (PS2X_SAVES=card), as the reference does.
`nativerun` is the same replay with native save files (the default
otherwise): there is no save dialog, so the three presses inside it (slot 1,
UP, YES) are dropped and the save is written at once; the clock anchor at
4650 lines up the rest. Its compare skips car08/car09 (inside the card
dialog) and `savecheck` checks the save file it wrote (size, checksum, and
the career against a card run's save, if one is given; autosave is off).

  python3 tools/careercheck.py script [native]    # the native pad script
  python3 tools/careercheck.py run NAME [SECS]    # replay, dumps in recomp/dumps/career/NAME-*.ram
  python3 tools/careercheck.py nativerun NAME [SECS]  # the same with native save files
  python3 tools/careercheck.py savecheck NAME [CARDRUN]  # nativerun's save (vs run CARDRUN's card save)
  python3 tools/careercheck.py compare NAME       # reference vs native table
  python3 tools/careercheck.py anchors NAME      # anchor shifts from the run log
  python3 tools/careercheck.py pxwatch           # --watch for pcsx2replay.py
  python3 tools/careercheck.py pxcompare PXLOG NAME   # per frame against a PCSX2 replay

Per frame: the reference run took savestates mid-play, which perturbs
PCSX2's own timing, so the open-loop inputs do not reproduce its
checkpoints exactly even in PCSX2. The ground truth for the native run is
PCSX2 replaying the same inputs (tools/pcsx2replay.py, own data dir):
  pcsx2replay.py launch --card <copy of reference/Mcd001-before-run2.ps2>
  pcsx2replay.py run boot 5430 OUT --input scripts/native/ref-run2-career-input.txt \
      --watch $(careercheck.py pxwatch)
  careercheck.py pxcompare OUT.log NAME

Environment: RUNNER, PS2X_MC_ROOT (default: a new empty recomp/run/mc-NAME;
nativerun: also a new empty recomp/run/saves-NAME as PS2X_SAVES_DIR), extra
PS2X_* vars pass through. CAREERCHECK_TO=<checkpoint> stops the script there.
"""
import math
import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import logiccheck as lc  # noqa: E402

ROOT = lc.ROOT
INPUT_LOG = os.path.join(lc.NATIVE_DIR, "ref-run2-career-input.txt")
REF_RAM = os.path.join(ROOT, "recomp", "dumps", "ref2")
NATIVE_RAM = os.path.join(ROOT, "recomp", "dumps", "career")
LOGS = lc.LOGS

MODE, PANEL, FRAME, POS, VSYNC = lc.MODE, lc.PANEL, lc.FRAME, lc.POS, lc.VSYNC
SECTION = lc.SECTION
LEVEL = 0x24C9B4
PLAYER = 0x24C860
# zgui heap objects: PCSX2 address -> native = PCSX2 + delta (logiccheck:
# zgui panels -0x1290 in the front end and the Tutorial).
HEAP_DELTA = lc.PANEL_HEAP_DELTA
SCORE = 0x836250               # zgui variable Score (u32)
CLOCK = 0x276FF8               # level clock, f32 seconds (counts paused time)
SKIN_NODE = (0x659A50, -0xF80)  # MOVIE LOT player skin node (PCSX2, native offset)
BANKED = 0x764                 # player object: banked score (MOVIE LOT)
COMBO = 0x8D8                  # player object: current combo points (f32)
JUICE = 0x838F70               # zgui variable CartonSize (f32)
CURSOR = 0x114                 # zgui panel: list cursor

P_MAIN = 0x8323E0
P_PLAYER = 0x840F90
P_NAME = 0x847700
P_LEVELS = 0x857D40
P_PAUSE = 0x839D00
P_CHALLENGES = 0x863E70

# (reference vsync of the event, condition in PCSX2 terms, why, kind).
# Panels are translated with native_panel(). Derived from career-*.txt and
# ref-run2-career-input.txt (see the docstring).
ANCHORS = [
    (856, "0x%X==1" % MODE, "front end up (START taps)", "SYNC"),
    (1093, "0x%X==P%X" % (PANEL, P_MAIN), "main menu", "SYNC"),
    (1300, "0x%X==P%X" % (PANEL, P_PLAYER), "player select", "SYNC"),
    (1373, "0x%X==P%X" % (PANEL, P_NAME), "name entry", "SYNC"),
    (1451, "0x%X==P%X" % (PANEL, P_LEVELS), "level select", "SYNC"),
    (3508, "CLOCK@3508", "level clock at car05", "SYNC"),
    (3558, "0x%X==P%X" % (PANEL, P_PAUSE), "pause menu", "SYNC"),
    (3644, "0x%X==P%X" % (PANEL, P_CHALLENGES), "challenges", "SYNC"),
    (3743, "0x%X==P%X" % (PANEL, P_PAUSE), "back to pause", "SYNC"),
    (3849, "0x%X==0" % PANEL, "pause menu closes for SAVE", "SYNC"),
    (4650, "CLOCK@4650", "level clock before RESUME", "SYNC"),
]

CHECKPOINTS = [
    ("car01-main-menu", 1122), ("car02-level-select-movielot", 1527),
    ("car03-loading", 2076), ("car04-flythru", 3207), ("car05-level-start", 3508),
    ("car06-pause", 3578), ("car07-challenges", 3674), ("car08-save-slot", 3939),
    ("car09-save-confirm", 4045), ("car11-save-done", 4622), ("car12-grind", 4829),
    ("car13-manual", 5043), ("car14-air-grab", 5260), ("car15-landed", 5354),
    ("car16-pause-end", 5424),
]
# Checkpoints taken on game state instead of the script clock.
# Inside the card save dialog: slot 1, UP to YES, YES. No dialog with native
# save files, so nativerun leaves these presses out (they would select pause
# menu entries) and compare skips the checkpoints taken in the dialog.
SAVE_DIALOG_PRESSES = (3973, 4079, 4101)
SAVE_DIALOG_CHECKPOINTS = ("car08-save-slot", "car09-save-confirm")
CAREER_SAVE = "BASLUS-20327CEAIGAME"
CAREER_SIZE = 0x70400

STATE_DUMPS = {
    "car03-loading": "0x%X!=0&0x%X==2&0x%X<2" % (PLAYER, MODE, FRAME),
    "car04-flythru": "0x%X!=0&0x%X==2&0x%X>=200&0x%X<400" % (PLAYER, MODE, FRAME, FRAME),
}


def ref_clock(vsync):
    """The reference level clock (f32 seconds at 0x276FF8) at a vsync: read
    from the nearest earlier in-level checkpoint, + 1/60 s per vsync (it
    advances by exactly that between car05 and car16)."""
    best = None
    for name, v in CHECKPOINTS:
        if v <= vsync and name >= "car05":
            best = (name, v)
    ram = open(os.path.join(REF_RAM, best[0] + ".ram"), "rb").read()
    return struct.unpack_from("<f", ram, CLOCK)[0] + (vsync - best[1]) / 60.0


def clock_condition(vsync):
    # Positive floats order like their bit patterns, so a u32 >= works. Half
    # a vsync of slack for rounding.
    # -1.5 vsyncs: the condition is seen at the VBlank after the frame that
    # set the clock, so this lines the native clock up exactly with the
    # reference one (car12: 30.583 both).
    slack = float(os.environ.get("CAREERCHECK_CLOCK_SLACK", "-1.5"))   # vsyncs
    bits = struct.unpack("<I", struct.pack("<f", ref_clock(vsync) + slack / 60.0))[0]
    return "0x%X>=0x%X" % (CLOCK, bits)


# pxcompare: the words logged per pad read, PCSX2 address (pcsx2replay.py
# --watch) and native address. MOVIE LOT player object: 0x972FF0 in PCSX2,
# 0x974000 natively.
PX_FIELDS = ["frame", "mode", "panel", "clock", "x", "y", "z", "juice", "grind_bal", "manual_bal", "combo", "banked"]
PX_WATCH = [(FRAME, 1), (MODE, 1), (PANEL, 1), (CLOCK, 1), (POS, 3), (JUICE, 1), (0x972FF0 + 0x280, 2),
            (0x972FF0 + COMBO, 1), (0x972FF0 + BANKED, 1)]
PX_WATCH_NATIVE = [(FRAME, 1), (MODE, 1), (PANEL, 1), (CLOCK, 1), (POS, 3), (JUICE + HEAP_DELTA, 1),
                   (0x974000 + 0x280, 2), (0x974000 + COMBO, 1), (0x974000 + BANKED, 1)]


def native_panel(v):
    return v + HEAP_DELTA if 0x800000 <= v < 0x900000 else v


def subst(cond):
    cond = re.sub(r"CLOCK@(\d+)", lambda m: clock_condition(int(m.group(1))), cond)
    return re.sub(r"P([0-9A-F]+)", lambda m: "0x%X" % native_panel(int(m.group(1), 16)), cond)


def read_inputs():
    lc.INPUT_LOG = INPUT_LOG
    return lc.read_inputs()


def last_vsync():
    to = os.environ.get("CAREERCHECK_TO")
    if not to:
        return None
    return dict(CHECKPOINTS)[to]


def native_script(native_saves=False):
    first, presses = read_inputs()
    stop = last_vsync()
    d = lc.INPUT_DELAY
    items = [(s - first + d, "%dv:%s:%dv" % (s - first + d, n, dur)) for s, n, dur in presses
             if (stop is None or s <= stop) and not (native_saves and s in SAVE_DIALOG_PRESSES)]
    items += [(v - first - 0.5, "%dv:%s=%s" % (v - first, kind, subst(c))) for v, c, _, kind in ANCHORS
              if stop is None or v <= stop]
    items.sort(key=lambda i: i[0])
    return ",".join(i for _, i in items)


def dump_spec():
    first, _ = read_inputs()
    out = []
    for name, v in CHECKPOINTS:
        out.append("%s:%s" % (name, STATE_DUMPS.get(name, "script>=%d" % (v - first + lc.DUMP_OFFSET))))
    return ",".join(out)


def fresh_dir(path):
    if os.path.isdir(path):
        shutil.rmtree(path)
    os.makedirs(path)
    return path


def native_marker(name):
    return os.path.join(NATIVE_RAM, name + ".native-saves")


def saves_dir(name):
    return os.path.join(ROOT, "recomp", "run", "saves-" + name)


def run(name, secs, native_saves=False):
    os.makedirs(NATIVE_RAM, exist_ok=True)
    env = dict(os.environ)
    env.setdefault("PS2X_GS_NULL", "1")
    env.setdefault("PS2X_GS_BACKEND", "cpu")  # pictures from the reference renderer
    if "PS2X_MC_ROOT" not in env:
        # The reference starts with no Aggressive Inline save on the card (a
        # save would be autoloaded and skip the name entry): a fresh, empty
        # card folder per run, which keeps the save this run writes.
        env["PS2X_MC_ROOT"] = fresh_dir(os.path.join(ROOT, "recomp", "run", "mc-" + name))
    if native_saves:
        # Native save files, starting from none (nothing to import either).
        env["PS2X_SAVES"] = "native"
        env.setdefault("PS2X_SAVES_DIR", fresh_dir(saves_dir(name)))
        # The file savecheck reads is the pause menu's save, not an autosave
        # on the way out.
        env.setdefault("PS2X_AUTOSAVE", "0")
        open(native_marker(name), "w").write(env["PS2X_SAVES_DIR"] + "\n")
    else:
        env["PS2X_SAVES"] = "card"   # the reference's card dialogs and timing
        if os.path.exists(native_marker(name)):
            os.remove(native_marker(name))
    env["PS2X_PAD_SCRIPT_CLOCK"] = "vsync"
    env["PS2X_PAD_SCRIPT"] = native_script(native_saves)
    env["PS2X_DUMP_WHEN"] = dump_spec()
    env["PS2X_DUMP_PREFIX"] = os.path.join(NATIVE_RAM, name)
    env.setdefault("PS2X_STATE_LOG", ",".join("0x%X" % a for a in (
        MODE, PANEL, SECTION, LEVEL, PLAYER, FRAME, POS, POS + 4, POS + 8)))
    # At every pad read, for pxcompare (needs a runtime with PS2X_PAD_LOG).
    env.setdefault("PS2X_PAD_LOG", ",".join("0x%X:%d" % (a, n) for a, n in PX_WATCH_NATIVE))
    subprocess.call([os.path.join(ROOT, "recomp", "run.sh"), str(secs), name], env=env)


def career_layout(body):
    """(start, end, part) of a career save body as the game's serializer
    (0x1D8948) lays it out: globals, then skaters 1..19, each its record and
    per level six words (five blob sizes, one more) and the blobs. The rest
    of the 0x70400 buffer is whatever was in memory."""
    parts = [(0, 0xF74, "globals")]
    at = 0xF74
    for n in range(1, 20):
        parts.append((at, at + 0x2A8, "skater %d record" % n))
        at += 0x2A8
        for level in range(10):
            sizes = struct.unpack_from("<6I", body, at)
            parts.append((at, at + 24, "skater %d level %d sizes" % (n, level)))
            at += 24
            for k in range(5):
                if sizes[k]:
                    parts.append((at, at + sizes[k], "skater %d level %d blob %d" % (n, level, k)))
                    at += sizes[k]
    return parts


def check_save_file(data):
    """None if data is a valid career save file (u32 byte sum, 0x70400 body)."""
    if len(data) != 4 + CAREER_SIZE:
        return "size %d, expected %d" % (len(data), 4 + CAREER_SIZE)
    if struct.unpack_from("<I", data)[0] != sum(data[4:]) & 0xFFFFFFFF:
        return "checksum mismatch"
    return None


# globals+0x20 (0x24CBD8) counts on during play: the runs reach the save a
# few vsyncs apart, as PCSX2's own save does.
TIMING_WORDS = {0x20, 0x21, 0x22, 0x23}


def savecheck(name, card_run=None):
    """The save nativerun NAME wrote (the pause menu's SAVE): a valid career
    file; with CARDRUN, the same career as the card save `run CARDRUN` wrote
    at that point: same layout and sizes, same skater records and globals
    (but the play counter at +0x20). The level-state blobs of the level being
    played are snapshots of moving objects and are only reported. Exit
    status 1 on a failure."""
    folder = open(native_marker(name)).read().strip() if os.path.exists(native_marker(name)) else saves_dir(name)
    path = os.path.join(folder, "Career.sav")
    if not os.path.exists(path):
        print("FAIL: no save file %s" % path)
        return 1
    data = open(path, "rb").read()
    problem = check_save_file(data)
    if problem:
        print("FAIL: %s: %s" % (path, problem))
        return 1
    body = data[4:]
    parts = career_layout(body)
    print("save file %s: valid (%d bytes, checksum ok, %d bytes of career data)" % (path, len(data), parts[-1][1]))
    if not card_run:
        return 0
    card = os.path.join(ROOT, "recomp", "run", "mc-" + card_run, CAREER_SAVE, CAREER_SAVE)
    if not os.path.exists(card):
        print("FAIL: no card save %s" % card)
        return 1
    ref = open(card, "rb").read()[4:]
    ref_parts = career_layout(ref)
    if [p[:2] for p in parts] != [p[:2] for p in ref_parts]:
        print("FAIL: the layout (blob sizes) differs from %s" % card)
        return 1
    failed = False
    for start, end, part in parts:
        diffs = [i for i in range(start, end) if body[i] != ref[i]]
        if part == "globals":
            diffs = [i for i in diffs if i not in TIMING_WORDS]
        if not diffs:
            continue
        if "blob" in part:
            print("note: %s differs in %d of %d bytes (level state snapshot)" % (part, len(diffs), end - start))
        else:
            print("FAIL: %s differs in %d bytes (first body offset 0x%X)" % (part, len(diffs), diffs[0]))
            failed = True
    if failed:
        return 1
    print("same career as the card save of run %s (layout, sizes, skater records, globals)" % card_run)
    return 0


# ---- comparison ------------------------------------------------------------

def u32(ram, a):
    return struct.unpack_from("<I", ram, a)[0]


def f32(ram, a, n=1):
    return struct.unpack_from("<%df" % n, ram, a)



def state(ram, native):
    delta = HEAP_DELTA if native else 0
    panel = u32(ram, PANEL)
    p = {
        "mode": u32(ram, MODE),
        "panel": panel - delta if native and 0x800000 <= panel < 0x900000 else panel,
        "cursor": u32(ram, panel + CURSOR) if panel else None,
        "level": u32(ram, LEVEL),
        "frame": u32(ram, FRAME),
        "cam": u32(ram, SECTION),
        "player": u32(ram, PLAYER),
        "pos": f32(ram, POS, 3),
        "score": u32(ram, SCORE + delta),
        "juice": f32(ram, JUICE + delta)[0],
    }
    p["clock"] = f32(ram, CLOCK)[0]
    obj = p["player"]
    p["banked"] = u32(ram, obj + BANKED) if obj else 0
    p["combo"] = f32(ram, obj + COMBO)[0] if obj else 0.0
    if p["pos"][1] < 1000 and p["level"] == 5:
        m = f32(ram, SKIN_NODE[0] + (SKIN_NODE[1] if native else 0) + 0x10, 16)
        p["yaw"] = lc.yaw(m) if m[12:15] == p["pos"] else None
    else:
        p["yaw"] = None
    return p


def compare(name, tol=0.01):
    """One row per checkpoint, reference values; MATCH when native agrees
    (positions within tol, yaw within 0.1 degree, juice and the level clock
    within 0.005), else the differing fields as reference/native."""
    first_diff = None
    print("%-28s %-5s %-11s %-4s %-6s %-6s %-5s %-6s %-6s %-7s %-6s %-7s %s" % (
        "checkpoint", "", "frame", "mode", "panel", "cursor", "level", "clock", "score", "juice", "yaw",
        "combo", "pos / differences (reference/native)"))
    native_saves = os.path.exists(native_marker(name))
    for ck, _ in CHECKPOINTS:
        if native_saves and ck in SAVE_DIALOG_CHECKPOINTS:
            print("%-28s n/a   (native save files: no card dialog)" % ck)
            continue
        rp = os.path.join(REF_RAM, ck + ".ram")
        np_ = os.path.join(NATIVE_RAM, "%s-%s.ram" % (name, ck))
        if not os.path.exists(np_):
            print("%-28s native dump missing" % ck)
            first_diff = first_diff or ck
            continue
        r, n = state(open(rp, "rb").read(), False), state(open(np_, "rb").read(), True)
        level = r["mode"] in (2, 3) and r["level"] != 14
        placed = level and r["pos"][1] < 1000 and r["pos"] != (0.0, 0.0, 0.0)
        diffs = []
        keys = ["mode", "panel", "level"]
        if r["panel"]:
            keys.append("cursor")
        if level and ck != "car03-loading":
            keys += ["score", "banked"]
        for k in keys:
            if r[k] != n[k]:
                diffs.append("%s %s/%s" % (k, fmt(r[k]), fmt(n[k])))
        if level:
            for k in ("clock", "juice", "combo"):
                if abs(r[k] - n[k]) > 0.005:
                    diffs.append("%s %.3f/%.3f" % (k, r[k], n[k]))
        if placed:
            e = lc.dist(r["pos"], n["pos"])
            if not e <= tol:                 # NaN counts as a difference
                diffs.append("pos %s/%s d=%.3f" % (lc.vec(r["pos"]), lc.vec(n["pos"]), e))
            if r["yaw"] is not None and (n["yaw"] is None or abs(r["yaw"] - n["yaw"]) > 0.1):
                diffs.append("yaw %s/%s" % (fmt(r["yaw"]), fmt(n["yaw"])))
        status = "MATCH" if not diffs else "DIFF"
        if diffs and first_diff is None:
            first_diff = ck
        print("%-28s %-5s %5d/%-5d %-4d %-6x %-6s %-5d %-6s %-6s %-7s %-6s %-7s %s" % (
            ck, status, r["frame"], n["frame"], r["mode"], r["panel"], fmt(r["cursor"]), r["level"],
            "%.2f" % r["clock"] if level else "-", r["score"] if level else "-",
            "%.2f" % r["juice"] if level else "-", fmt(r["yaw"]), "%.1f" % r["combo"] if level else "-",
            "; ".join(diffs) if diffs else (lc.vec(r["pos"]) if placed else "")))
    if first_diff:
        print("first divergence: %s" % first_diff)


def fmt(v):
    if v is None:
        return "-"
    if isinstance(v, float):
        return "%.1f" % v
    if isinstance(v, int) and v >= 0x10000:
        return "%x" % v
    return str(v)


def pxcompare(pxlog, name):
    """Native PS2X_PAD_LOG records against a PCSX2 replay of the same inputs
    (tools/pcsx2replay.py, --watch from `careercheck.py pxwatch`), matched on
    the level clock: every in-level clock value both have is compared
    word for word. Frame pacing differs (the game's adaptive frame limiter
    runs some frames in 2 or 3 vsyncs in PCSX2), so not every clock value
    exists on both sides."""
    px = []
    for line in open(pxlog):
        if not line.startswith("#"):
            w = [int(x, 16) for x in line.split()]
            px.append((w[0], w[2:]))                 # vsync, words
    keys = ["%x" % (a + 4 * i) for a, n in PX_WATCH_NATIVE for i in range(n)]
    nat = []
    for line in open(os.path.join(LOGS, name + ".log"), errors="replace"):
        if line.startswith("[pad-state] tick="):
            d = dict(re.findall(r"\b([0-9a-f]+)=([0-9a-f]+)\b", line))
            if all(k in d for k in keys):
                nat.append((int(line.split()[1].split("=")[1]), [int(d[k], 16) for k in keys]))
    if not nat:
        raise SystemExit("no [pad-state] records in %s.log (runtime without PS2X_PAD_LOG?)" % name)

    def level(rows):
        # From the last time the clock restarted (loading MOVIE LOT) on.
        start = max(i for i in range(1, len(rows)) if rows[i][1][3] < rows[i - 1][1][3] and rows[i][1][3] < 0x3F800000)
        return rows[start:]
    nmap = {}
    for tick, w in level(nat):
        nmap.setdefault(w[3], (tick, w))
    compared, diffs = 0, {}
    first = None
    seen = set()
    for vsync, w in level(px):
        if w[3] in seen or w[3] not in nmap:
            continue
        seen.add(w[3])
        compared += 1
        tick, n = nmap[w[3]]
        for i, field in enumerate(PX_FIELDS):
            if field in ("frame", "panel"):
                continue
            if w[i] != n[i]:
                diffs[field] = diffs.get(field, 0) + 1
                if first is None or (field != "juice" and first[0] == "juice"):
                    first = (field, vsync, tick, w[i], n[i], struct.unpack("<f", struct.pack("<I", w[3]))[0])
    print("%d level clock values compared" % compared)
    if not diffs:
        print("all identical")
    for field, count in sorted(diffs.items()):
        print("%-11s differs in %d" % (field, count))
    if first:
        fv = lambda u: struct.unpack("<f", struct.pack("<I", u))[0]
        print("first difference (non-juice first): %s at clock %.4f (PCSX2 vsync %d, native tick %d): %08x (%g) / %08x (%g)" % (
            first[0], first[5], first[1], first[2], first[3], fv(first[3]), first[4], fv(first[4])))


def show_anchors(name):
    shifts = {}
    for line in open(os.path.join(LOGS, name + ".log"), errors="replace"):
        m = re.match(r"\[pad\] sync (\d+) at script (\d+)v \(tick (\d+)\): clock ([+-]\d+) vsyncs", line)
        if m:
            shifts[int(m.group(1))] = (int(m.group(3)), int(m.group(4)))
    for i, (v, cond, why, kind) in enumerate(ANCHORS):
        s = shifts.get(i)
        print("%3d ref %5d  %-4s %-30s %-28s %s" % (i, v, kind, subst(cond)[:30], why,
                                                   "tick %d shift %+d" % s if s else "not reached"))


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return
    verb = argv[1]
    if verb == "script":
        print(native_script(len(argv) > 2 and argv[2] == "native"))
    elif verb == "dumpspec":
        print(dump_spec())
    elif verb == "run":
        run(argv[2], int(argv[3]) if len(argv) > 3 else 200)
    elif verb == "nativerun":
        run(argv[2], int(argv[3]) if len(argv) > 3 else 200, native_saves=True)
    elif verb == "savecheck":
        sys.exit(savecheck(argv[2], argv[3] if len(argv) > 3 else None))
    elif verb == "compare":
        compare(argv[2])
    elif verb == "pxwatch":
        print(",".join("0x%X:%d" % (a, n) for a, n in PX_WATCH))
    elif verb == "pxcompare":
        pxcompare(argv[2], argv[3])
    elif verb == "anchors":
        show_anchors(argv[2])
    else:
        raise SystemExit("unknown verb %s" % verb)


if __name__ == "__main__":
    main(sys.argv)
