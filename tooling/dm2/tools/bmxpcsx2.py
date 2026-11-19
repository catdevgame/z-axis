#!/usr/bin/env python3
"""Drive BMX XXX (USA, SLUS-20415, CRC 2999BCF9) in a private PCSX2 over PINE: dm2pcsx2.py at BMX's addresses.

The hook (bmxpad_hook.s, installed by bmx_build_hook.py) is DM2's mailbox and VBlank counter plus an
input schedule in EE RAM keyed on the level frame counter, so a ride replays on exactly the level
frames it says. Every dm2pcsx2.py verb works (press, hold, wait, waitval, pressuntil, checkpoint,
seenlog, shot, ...); added here:

  bmxpcsx2.py setup                     make the data dir ($BMXPCSX2_DATA): ini from DM2's with PINE slot
                                        28031, no keyboard or mouse input, no hotkeys, BIOS linked, an empty
                                        formatted card in slot 1, the pnach
  bmxpcsx2.py launch [ISO]              start our own PCSX2 on it (the PID in bmxpcsx2.pid; `quit` stops that PID)
  schedule FILE                         load a ride ("FRAME BUTTON+BUTTON|-" per line) into the hook's schedule
  waitframe N [TIMEOUT_VSYNCS]          until the level frame counter reaches N
  checkpoint SLOT NAME                  dm2pcsx2's, into $BMX_REF_DATA/ref (RAM), pcsx2-run1 (PNG), states (p2s)
  state                                 mode, level frame, the rider's position, speed, score

Environment: BMXPCSX2_DATA (default $ZAXIS_DATA/bmx/pcsx2-data-bmx), BMXPCSX2_SLOT (28031),
BMX_ISO (a copy of the PS2 disc; the user's backups are read only), BMX_REF_DATA ($ZAXIS_DATA/bmx).
"""

import os
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dm2pcsx2 as base  # noqa: E402
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))
import zpaths  # noqa: E402  (tooling/paths.env: the data folder)

BMX_HOME = os.path.join(zpaths.ZAXIS_DATA, "bmx")    # the PCSX2 data dir, the disc copy, the reference
REF_DATA = os.environ.get("BMX_REF_DATA", BMX_HOME)               # checkpoints: ref/ (RAM), pcsx2-run1/ (PNG), states/
base.DEFAULT_SLOT = int(os.environ.get("BMXPCSX2_SLOT", "28031"))
base.DATA_DIR = os.environ.get("BMXPCSX2_DATA", os.path.join(BMX_HOME, "pcsx2-data-bmx"))
base.ISO = os.environ.get("BMX_ISO", os.path.join(BMX_HOME, "disc", "bmxxxx-ps2.iso"))
base.PID_FILE = os.path.join(base.DATA_DIR, "bmxpcsx2.pid")
base.SERIAL = "SLUS-20415"
DM2_DATA = os.environ.get("DM2PCSX2_DATA", os.path.join(zpaths.ZAXIS_DATA, "dm2", "tools", "pcsx2-data-dm2"))
# (DM2's data dir: the ini, BIOS and empty card this one starts from; read, never written)

# ---- the hook (bmxpad_hook.s / bmx_build_hook.py) -----------------------------
MAILBOX = base.MAILBOX
base.CALL_SITE = 0x00117D60                 # jal scePadRead in the pad function 0x001179D0
base.HOOKED_CALL = 0x0C000000 | (base.CAVE >> 2)
base.ORIGINAL_CALL = 0x0C08ACA0             # jal scePadRead (0x0022B280)
base.PAD_BUFFER = 0x002ABC94
SEENLOG, SEENLOG_ENTRIES, SEENLOG_SIZE = 0x000E0000, 4096, 16   # u32 vsync, u32 packed, u32 level frame, u32 polls
SCHED, SCHED_MAX = 0x000D0000, 8192                             # u32 level frame, u32 buttons (active-high)
MB_SCHED_N, MB_SCHED_NEXT, MB_SCHED_HELD = 0x30, 0x34, 0x38

# ---- BMX game state (career_ab.py's bmx fields, playbot bmx.py's rider offsets) ----
MODE = 0x0029D040          # 0 boot and movies, 1 front end, 2 loading and level, 3 paused
LEVEL_FRAME = 0x002A2DE0   # rider task frames since the level started; 0 in the front end
RIDER_TABLE = 0x002F3A30   # rider objects; [0] = player 1
RIDER_SCORE, RIDER_POS, RIDER_SPEED = 0x1C0, 0x2650, 0x26F8
base.SOUND_MODE, base.LEVEL_FRAME, base.RIDER_TABLE = MODE, LEVEL_FRAME, RIDER_TABLE
INI_CHANGES = {"PINESlot": str(base.DEFAULT_SLOT), "Keyboard": "false", "Mouse": "false", "Slot1_Filename": "Mcd-formatted.ps2",
               "OutputMuted": "true", "StartFullscreen": "false", "EnableCheats": "false", "EnablePatches": "true",
               "SaveStateOnShutdown": "false", "PauseOnFocusLoss": "false", "StartPaused": "false"}


def setup():
    """The data dir: DM2's ini with BMX's slot, PCSX2's own folders, the BIOS linked (read only), a card."""
    d = os.path.join(base.DATA_DIR, "PCSX2")
    for sub in ("bios", "cache", "cheats", "covers", "gamesettings", "inis", "inputprofiles", "logs", "memcards",
                "patches", "resources", "snaps", "sstates", "textures", "videos"):
        os.makedirs(os.path.join(d, sub), exist_ok=True)
    src = os.path.join(DM2_DATA, "PCSX2")
    out, section = [], None
    for line in open(os.path.join(src, "inis", "PCSX2.ini")).read().split("\n"):
        if line.startswith("["):
            section = line
        key = line.split(" = ", 1)[0]
        if section == "[Hotkeys]" and " = " in line:
            continue                                  # no hotkeys: a key on the window changes nothing
        if section == "[Pad1]" and " = " in line and key not in ("Type", "InvertL", "InvertR", "Deadzone", "AxisScale",
                                                                  "ButtonDeadzone", "PressureModifier"):
            continue                                  # a DualShock 2 with no bindings: only the hook gives input
        if key in INI_CHANGES and " = " in line:
            line = "%s = %s" % (key, INI_CHANGES[key])
        if key in ("MainWindowGeometry", "MainWindowState"):
            continue
        out.append(line)
    open(os.path.join(d, "inis", "PCSX2.ini"), "w").write("\n".join(out))
    bios = os.path.join(d, "bios", "scph39001.bin")
    if not os.path.lexists(bios):
        os.symlink(os.path.realpath(os.path.join(src, "bios", "scph39001.bin")), bios)
    for f in ("scph39001.nvm", "scph39001.mec"):
        shutil.copyfile(os.path.join(src, "bios", f), os.path.join(d, "bios", f))
    shutil.copyfile(os.path.join(src, "memcards", "Mcd-formatted.ps2"), os.path.join(d, "memcards", "Mcd-formatted.ps2"))
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "bmx_build_hook.py")], check=True,
                   env=dict(os.environ, BMXPCSX2_DATA=base.DATA_DIR))
    print("data dir %s ready (slot %d)" % (base.DATA_DIR, base.DEFAULT_SLOT))


def launch(iso):
    if base.our_pid():
        raise SystemExit("already running: pid %d" % base.our_pid())
    if not os.path.exists(iso):
        raise SystemExit("no disc copy at %s (copy the PS2 disc there; the backups are read only)" % iso)
    others = subprocess.run(["pgrep", "-fl", "PCSX2.app/Contents/MacOS/PCSX2"], capture_output=True, text=True).stdout.strip()
    if others:
        print("note: other PCSX2 instance(s) running, leaving them alone:\n  " + others.replace("\n", "\n  "))
    log = open(os.path.join(base.DATA_DIR, "bmxpcsx2-launch.log"), "w")
    args = [base.PCSX2_BIN, "-batch", "-fastboot", "-nofullscreen", "-datapath", base.DATA_DIR, "--", iso]
    proc = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    open(base.PID_FILE, "w").write("%d\n" % proc.pid)
    print("launched PCSX2 pid %d (datapath %s)" % (proc.pid, base.DATA_DIR))
    deadline = time.time() + 90
    while time.time() < deadline:
        if proc.poll() is not None:
            raise SystemExit("PCSX2 exited with %s" % proc.returncode)
        try:
            pad = base.Pad(base.DEFAULT_SLOT)
            if pad.p.game_id() == base.SERIAL and pad.hook_state() == "installed":
                print("game running, hook installed, vsync %d" % pad.vsync())
                return proc.pid
            pad.p.close()
        except (base.PineError, OSError):
            pass
        time.sleep(0.25)
    raise SystemExit("timed out waiting for the game / hook")


def read_seenlog(pad):
    """[(vsync, buttons_active_high, lx, ly, frame, polls)] from the hook's seen log."""
    n = pad.p.read32(MAILBOX + base.MB_SEEN_COUNT)
    if n > SEENLOG_ENTRIES:
        print("warning: seen log wrapped (%d entries, ring %d)" % (n, SEENLOG_ENTRIES), file=sys.stderr)
    raw = pad.p.read_block(SEENLOG, SEENLOG_ENTRIES * SEENLOG_SIZE)
    out = []
    for k in range(max(0, n - SEENLOG_ENTRIES), n):
        v, w, f, polls = struct.unpack_from("<IIII", raw, (k % SEENLOG_ENTRIES) * SEENLOG_SIZE)
        out.append((v, (~w) & 0xFFFF, (w >> 16) & 0xFF, (w >> 24) & 0xFF, f, polls))
    return out


def parse_schedule(path):
    """[(frame, mask)] from "FRAME BUTTON+BUTTON|-" lines ('#' comments)."""
    out = []
    for line in open(path):
        line = line.split("#", 1)[0].split()
        if line:
            out.append((int(line[0]), 0 if line[1] == "-" else base.parse_buttons(line[1])))
    if [f for f, _ in out] != sorted(f for f, _ in out):
        raise SystemExit("%s: frames not in order" % path)
    return out


def load_schedule(pad, entries):
    if len(entries) > SCHED_MAX:
        raise SystemExit("schedule too long: %d entries (at most %d)" % (len(entries), SCHED_MAX))
    pad.p.write32(MAILBOX + MB_SCHED_N, 0)          # off while it is written
    pad.p.write_block(SCHED, b"".join(struct.pack("<II", f, m) for f, m in entries))
    pad.p.write32(MAILBOX + MB_SCHED_NEXT, 0)
    pad.p.write32(MAILBOX + MB_SCHED_HELD, 0)
    pad.p.write32(MAILBOX + MB_SCHED_N, len(entries))


def game_state(pad):
    rd = pad.p.read32
    s = {"vsync": pad.vsync(), "mode": rd(MODE), "frame": rd(LEVEL_FRAME), "rider": rd(RIDER_TABLE)}
    if s["frame"] and 0x100000 <= s["rider"] < 0x2000000:
        s["pos"] = struct.unpack("<3f", pad.p.read_block(s["rider"] + RIDER_POS, 12))
        s["speed"] = struct.unpack("<f", pad.p.read_block(s["rider"] + RIDER_SPEED, 4))[0]
        s["score"] = struct.unpack("<f", pad.p.read_block(s["rider"] + RIDER_SCORE, 4))[0]
    return s


_base_dispatch = base.dispatch


def dispatch(pad, argv, args):
    if not argv:
        return
    verb, rest = argv[0].lower(), argv[1:]
    if verb == "schedule":
        entries = parse_schedule(rest[0])
        load_schedule(pad, entries)
        print("schedule: %d entries, frames %d..%d" % (len(entries), entries[0][0], entries[-1][0]))
    elif verb == "waitframe":
        target, limit = int(rest[0]), int(rest[1]) if len(rest) > 1 else 60 * 600
        v0 = pad.vsync()
        while pad.p.read32(LEVEL_FRAME) < target:
            if pad.vsync() - v0 > limit:
                raise SystemExit("waitframe timeout: frame %d" % pad.p.read32(LEVEL_FRAME))
            time.sleep(0.002)
        print("  frame %d (vsync %d)" % (pad.p.read32(LEVEL_FRAME), pad.vsync()))
    elif verb == "checkpoint":
        slot, name = int(rest[0]), rest[1]
        os.environ.setdefault("DM2PCSX2_STATE_DIR", os.path.join(REF_DATA, "states"))
        path = pad.save_state(slot)
        print(base.checkpoint_extract(path, name, os.path.join(REF_DATA, "ref"), os.path.join(REF_DATA, "pcsx2-run1")))
    elif verb == "seenlog":
        ents = read_seenlog(pad)
        with open(rest[0], "w") as f:
            f.write("# bmx seen log; firstpoll %d\n" % pad.p.read32(MAILBOX + base.MB_FIRSTPOLL_VSYNC))
            for v, b, lx, ly, fr, polls in ents:
                f.write("%d %04x %02x %02x %d %d %s\n" % (v, b, lx, ly, fr, polls, base.mask_names(b)))
        print("seen log: %d entries -> %s" % (len(ents), rest[0]))
    elif verb == "state":
        s = game_state(pad)
        print("vsync %(vsync)d mode %(mode)d frame %(frame)d rider %(rider)08x" % s
              + ("  pos %.3f %.3f %.3f speed %.4f score %.0f" % (s["pos"] + (s["speed"], s["score"])) if "pos" in s else ""))
    else:
        _base_dispatch(pad, argv, args)


base.dispatch = dispatch


def main():
    a = sys.argv[1:]
    if a[:1] == ["setup"]:
        setup()
    elif a[:1] == ["launch"]:
        launch(a[1] if len(a) > 1 else base.ISO)
    elif a[:1] == ["state"] or a[:1] == ["seenlog"]:
        dispatch(base.Pad(base.DEFAULT_SLOT), a, None)
    else:
        base.main()


if __name__ == "__main__":
    main()
