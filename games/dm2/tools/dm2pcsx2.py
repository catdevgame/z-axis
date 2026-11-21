#!/usr/bin/env python3
"""Drive Dave Mirra Freestyle BMX 2 (USA, SLUS-20159, CRC CEC42835) in a private PCSX2 over PINE.

Port of aggressive-inline/tools/pcsx2pad.py. No OS input, no Accessibility
permission, no window focus: the pnach hook in dm2pad_hook.s makes the
game's own scePadRead merge a mailbox in EE RAM, and this tool writes that
mailbox over PINE. DM2 has no VBlank callback, so the hook also installs an
INTC VBlank handler; all timing is in vsyncs of that counter (MAILBOX+0x18).
The hook also holds an input schedule keyed on the level frame counter
(`schedule FILE`), so a ride replays on exactly the level frames it says.

Examples
  dm2pcsx2.py launch                       # start our own PCSX2 (PINE slot 28021)
  dm2pcsx2.py status
  dm2pcsx2.py press START                  # tap (down, then up)
  dm2pcsx2.py hold CROSS 10                # hold 10 frames
  dm2pcsx2.py stick L 0 -1 30              # left stick full up for 30 frames
  dm2pcsx2.py release
  dm2pcsx2.py script "40:START,50:CROSS"   # ZX_PAD_SCRIPT syntax ("<t>v:" = vsyncs)
  dm2pcsx2.py run scripts/dm2-boot-to-level.txt
  dm2pcsx2.py peek 0x260494 32 ; poke 0x... 0 4
  dm2pcsx2.py dump 0 0x2000000 ram.bin
  dm2pcsx2.py savestate 1 ; loadstate 1 ; screenshot shot.png ; quit
  dm2pcsx2.py run FILE                     # with `schedule RIDE` and `waitframe N` lines

Schedule file: one entry per line, "KEY BUTTONS [LX LY]": KEY a level frame,
or "<n>v" a VBlank count (the front end); '-' = no button; LX LY stick
bytes in hex, 80 = centre (given, the entry sets all four sticks, RX RY at
80); in the order they are due; '#' comments.
"""

import argparse
import glob
import os
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import Pine, PineError, STATUS_NAMES  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(os.path.dirname(HERE), "workspace")   # (the game's workspace: dmbmx2.iso, recomp/, reference/)
DEFAULT_SLOT = int(os.environ.get("DM2PCSX2_SLOT", "28021"))
DATA_DIR = os.environ.get("DM2PCSX2_DATA", os.path.join(HERE, "pcsx2-data-dm2"))
PCSX2_BIN = "/Applications/PCSX2.app/Contents/MacOS/PCSX2"
ISO = os.path.join(REPO, "dmbmx2.iso")
PID_FILE = os.path.join(DATA_DIR, "dm2pcsx2.pid")
SERIAL = "SLUS-20159"

# ---- addresses (must match dm2pad_hook.s / dm2_build_hook.py) -------------
MAILBOX = 0x000F7000
MB_MAGIC, MB_BUTTONS, MB_FLAGS, MB_STICKS = 0x00, 0x04, 0x06, 0x08
MB_POLLS, MB_SEEN, MB_RAW, MB_FIRSTPOLL_VSYNC = 0x0C, 0x10, 0x12, 0x14
MB_VSYNC, MB_VSYNC_ON, MB_HANDLER_ID = 0x18, 0x1C, 0x20
MB_SEEN_COUNT = 0x24
MB_SCHED_N, MB_SCHED_NEXT, MB_SCHED_HELD, MB_SCHED_STICKS = 0x30, 0x34, 0x38, 0x3C
# Seen log ring: u32 vsync, u32 buttons(active-low)|LX<<16|LY<<24, u32 level frame, u32 polls
SEENLOG, SEENLOG_ENTRIES, SEENLOG_SIZE = 0x000E0000, 4096, 16
# Schedule: u32 level frame, u32 buttons(active-high)|flags<<16 (bit 16: sticks), u32 RX|RY<<8|LX<<16|LY<<24, u32 0
SCHED, SCHED_MAX, SCHED_SIZE = 0x000D0000, 4096, 16
MAGIC = 0x4D444150                      # "PADM"
CAVE = 0x000F8000
CAVE_FIRST_WORD = 0x27BDFFE0            # addiu sp, sp, -32
CALL_SITE = 0x00113108                  # jal scePadRead in func_00112D98
HOOKED_CALL = 0x0C000000 | (CAVE >> 2)
ORIGINAL_CALL = 0x0C0764D6              # jal scePadRead (0x001D9358)
VSYNC = MAILBOX + MB_VSYNC              # incremented by the hook's VBlank handler
PAD_BUFFER = 0x00260494                 # game's scePadRead destination (32 bytes)

FPS = 59.94                             # NTSC field rate, for second-based scripts

# ---- DM2 game state ------------------------------------------------------------
SOUND_MODE = 0x00259770     # u32 0 boot/movies, 1 front end, 2 level (incl. loading), 3 paused?
GAME_TYPE = 0x0025A1C0      # u32 FUN_001AD768 (0 in Freeride)
LEVEL_CURSOR = 0x0025A1A8   # u32 Choose-a-Level cursor (index into LEVEL_TABLE)
LEVEL_TABLE = 0x0025A1C8    # u8[9] level ids in Choose-a-Level order (02 01 00 03 06 05 04 07 08)
SESSION = 0x002566C8        # u8 [1] rider id, [2] level id, [3] player count (set when a level is picked)
LEVEL_ID = 0x002566CA       # u8 selected level id (2 = Woodward Camp)
LEVEL_FRAME = 0x0025BCC4    # u32 rider task frames since the level started (0 in the front end)
RIDER_TABLE = 0x002F4EA0    # u32[] rider object pointers (FUN_001A3C30); [0] = player 1
RIDER_ROT, RIDER_POS, RIDER_VEL = 0x1000, 0x1040, 0x10A0   # rider object: 3x4 rotation rows, pos xyz1, vel xyz (per frame)

BUTTONS = {
    "SELECT": 0x0001, "L3": 0x0002, "R3": 0x0004, "START": 0x0008,
    "UP": 0x0010, "RIGHT": 0x0020, "DOWN": 0x0040, "LEFT": 0x0080,
    "L2": 0x0100, "R2": 0x0200, "L1": 0x0400, "R1": 0x0800,
    "TRIANGLE": 0x1000, "CIRCLE": 0x2000, "CROSS": 0x4000, "SQUARE": 0x8000,
}
ALIASES = {"X": "CROSS", "O": "CIRCLE", "TRI": "TRIANGLE", "SQ": "SQUARE", "SEL": "SELECT"}

# Tap timing: a press is held, then released, each for at least this many
# vsync frames AND this many game pad polls, so the game sees both edges even
# when its loop runs at 30 fps or skips frames.
TAP_FRAMES = 6
TAP_POLLS = 2
# How long past the requested frames to keep waiting for the game to poll.
POLL_GRACE_FRAMES = 600


def parse_buttons(spec):
    mask = 0
    for name in spec.replace(",", "+").split("+"):
        name = name.strip().upper()
        if not name:
            continue
        name = ALIASES.get(name, name)
        if name not in BUTTONS:
            raise SystemExit("unknown button %r (known: %s)" % (name, " ".join(BUTTONS)))
        mask |= BUTTONS[name]
    return mask


def mask_names(mask):
    return "+".join(n for n, b in BUTTONS.items() if mask & b) or "-"


def axis_byte(v):
    v = max(-1.0, min(1.0, float(v)))
    return int(round(128 + v * (128 if v < 0 else 127)))


def parse_int(s):
    return int(s, 0)


class Pad:
    def __init__(self, slot=DEFAULT_SLOT, verbose=False):
        self.p = Pine(slot)
        self.verbose = verbose
        # DM2PCSX2_RECORD=FILE appends every input change with its vsync, so a
        # run can be turned into a ZX_PAD_SCRIPT afterwards (`padscript`).
        self.record = os.environ.get("DM2PCSX2_RECORD")
        self._last_rec = None

    def _rec(self, kind, value):
        if not self.record or (kind, value) == self._last_rec:
            return
        self._last_rec = (kind, value)
        with open(self.record, "a") as f:
            if os.path.getsize(self.record) == 0:
                f.write("# vsync kind value; firstpoll %d\n" % self.p.read32(MAILBOX + MB_FIRSTPOLL_VSYNC))
            f.write("%d %s %x\n" % (self.vsync(), kind, value))

    # ---- state --------------------------------------------------------
    def vsync(self):
        return self.p.read32(VSYNC)

    def polls(self):
        return self.p.read32(MAILBOX + MB_POLLS)

    def counters(self):
        v, n = self.p.read_words([VSYNC, MAILBOX + MB_POLLS])
        return v, n

    def hook_state(self):
        call, cave = self.p.read_words([CALL_SITE, CAVE])
        if call == HOOKED_CALL and cave == CAVE_FIRST_WORD:
            return "installed"
        if call == ORIGINAL_CALL:
            return "NOT installed (call site is original; is the pnach in the patches folder?)"
        if call == 0:
            return "not yet (game not loaded)"
        return "unknown (call site %08x, cave %08x)" % (call, cave)

    def require_hook(self):
        st = self.hook_state()
        if st != "installed":
            raise SystemExit("hook %s" % st)

    def arm(self):
        """Write the magic (and centred sticks if no override is active)."""
        magic, flags = self.p.read32(MAILBOX + MB_MAGIC), self.p.read8(MAILBOX + MB_FLAGS)
        if magic != MAGIC:
            self.p.write32(MAILBOX + MB_STICKS, 0x80808080)
            self.p.write16(MAILBOX + MB_BUTTONS, 0)
            self.p.write8(MAILBOX + MB_FLAGS, 0)
            self.p.write32(MAILBOX + MB_MAGIC, MAGIC)
        elif not (flags & 1):
            self.p.write32(MAILBOX + MB_STICKS, 0x80808080)

    def set_buttons(self, mask):
        self.p.write16(MAILBOX + MB_BUTTONS, mask)
        self._rec("B", mask)

    def get_buttons(self):
        return self.p.read16(MAILBOX + MB_BUTTONS)

    def set_stick(self, which, x, y):
        cur = bytearray(struct.pack("<I", self.p.read32(MAILBOX + MB_STICKS)))
        off = 0 if which.upper().startswith("R") else 2
        cur[off], cur[off + 1] = axis_byte(x), axis_byte(y)
        self.p.write32(MAILBOX + MB_STICKS, struct.unpack("<I", bytes(cur))[0])
        self.p.write8(MAILBOX + MB_FLAGS, self.p.read8(MAILBOX + MB_FLAGS) | 1)
        self._rec("S", struct.unpack("<I", bytes(cur))[0])

    def release(self):
        self.p.write16(MAILBOX + MB_BUTTONS, 0)
        self.p.write8(MAILBOX + MB_FLAGS, 0)
        self.p.write32(MAILBOX + MB_STICKS, 0x80808080)
        self._rec("B", 0)
        self._rec("S", 0x80808080)

    # ---- timing -------------------------------------------------------
    def wait_frames(self, frames, min_polls=0, timeout=None):
        """Wait until the vsync counter advanced `frames` and the poll counter
        advanced `min_polls`. Survives counter resets (backwards jumps)."""
        v0, n0 = self.counters()
        last_v, elapsed, last_change = v0, 0, time.time()
        limit = timeout if timeout is not None else max(5.0, frames / 30.0 + 5.0)
        while True:
            v, n = self.counters()
            if v != last_v:
                d = (v - last_v) & 0xFFFFFFFF
                elapsed += d if d < 0x10000 else 1   # backwards/huge jump: count 1
                last_v, last_change = v, time.time()
            if elapsed >= frames and ((n - n0) & 0xFFFFFFFF) >= min_polls:
                return
            if elapsed >= frames + POLL_GRACE_FRAMES:
                print("warning: game made only %d pad polls in %d frames (loading?); continuing"
                      % ((n - n0) & 0xFFFFFFFF, elapsed), file=sys.stderr)
                return
            if time.time() - last_change > limit:
                raise SystemExit("timeout: vsync counter stopped at %d (emulator paused or game not running?)" % v)
            time.sleep(0.002)

    def wait_until_vsync(self, target):
        """Wait until the vsync counter reaches `target` (absolute)."""
        last, last_change = None, time.time()
        while True:
            v = self.vsync()
            if v >= target:
                return v
            if v != last:
                last, last_change = v, time.time()
            elif time.time() - last_change > 10:
                raise SystemExit("timeout: vsync counter stopped at %d" % v)
            time.sleep(0.002)

    # ---- actions ------------------------------------------------------
    def press(self, mask, frames=TAP_FRAMES, polls=TAP_POLLS):
        self.arm()
        self.set_buttons(self.get_buttons() | mask)
        self.wait_frames(frames, polls)
        self.set_buttons(self.get_buttons() & ~mask)
        self.wait_frames(frames, polls)

    def hold(self, mask, frames):
        self.arm()
        self.set_buttons(self.get_buttons() | mask)
        self.wait_frames(frames, 1)
        self.set_buttons(self.get_buttons() & ~mask)
        self.wait_frames(TAP_FRAMES, TAP_POLLS)

    # ---- savestates / screenshots -------------------------------------
    def save_state(self, slot):
        # PCSX2 cancels a savestate while the memory card is busy (for a few
        # seconds after the game writes it: "The memory card is busy, so the
        # state save operation has been cancelled"), so re-request it until
        # the file appears.
        before = _state_mtime(slot)
        for attempt in range(8):
            if attempt:
                print("  savestate %d: no file yet (memory card busy?), retrying (vsync %d)" % (slot, self.vsync()))
            self.p.save_state(slot)
            deadline = time.time() + 4
            while time.time() < deadline:
                path, mtime = _state_file(slot)
                if path and mtime != before and _stable(path):
                    return path
                time.sleep(0.1)
        raise SystemExit("savestate %d: file did not appear" % slot)

    def load_state(self, slot):
        path, _ = _state_file(slot)
        if not path:
            raise SystemExit("no savestate in slot %d" % slot)
        self.p.load_state(slot)
        # Loading is asynchronous; the restored RAM includes the mailbox, so
        # wait for the load to land and then clear any restored input.
        time.sleep(1.0)
        self.wait_frames(2, timeout=10)
        self.release()
        return path


def _sstates_dir():
    return os.path.join(DATA_DIR, "PCSX2", "sstates")


def _state_file(slot):
    files = glob.glob(os.path.join(_sstates_dir(), "%s (*).%02d.p2s" % (SERIAL, slot)))
    if not files:
        return None, None
    f = max(files, key=os.path.getmtime)
    return f, os.path.getmtime(f)


def _state_mtime(slot):
    return _state_file(slot)[1]


def _stable(path):
    s1 = os.path.getsize(path)
    time.sleep(0.3)
    return os.path.getsize(path) == s1 and s1 > 0


# ---- process management -----------------------------------------------
def _cmdline(pid):
    try:
        return subprocess.run(["ps", "-o", "command=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    except OSError:
        return ""


def our_pid():
    """PID of the PCSX2 we launched (verified against its command line)."""
    try:
        pid = int(open(PID_FILE).read().strip())
    except (OSError, ValueError):
        return None
    cmd = _cmdline(pid)
    if PCSX2_BIN in cmd and DATA_DIR in cmd:
        return pid
    return None


def window_id(pid):
    winid = os.path.join(HERE, "winid")
    if not os.path.exists(winid):
        subprocess.run(["swiftc", "-O", "-o", winid, os.path.join(HERE, "winid.swift")], check=True)
    out = subprocess.run([winid, str(pid)], capture_output=True, text=True).stdout.strip()
    return int(out) if out else None


def screenshot(path, pid=None):
    pid = pid or our_pid()
    if not pid:
        raise SystemExit("no PCSX2 launched by this tool is running (pid file %s)" % PID_FILE)
    # The window can be briefly uncapturable (or replaced) during display
    # mode changes, so re-resolve the window id and retry a few times.
    for attempt in range(5):
        wid = window_id(pid)
        if wid and subprocess.run(["screencapture", "-x", "-o", "-l%d" % wid, path],
                                  capture_output=True).returncode == 0 and os.path.exists(path):
            return path
        time.sleep(0.3)
    raise SystemExit("screenshot failed (pid %d, window %s)" % (pid, wid))


CARD_SIZE = 8650752   # 8 MB PS2 card with ECC, as PCSX2 writes it
UNFORMATTED_CARD = "Mcd-unformatted.ps2"


def set_slot1_card(card):
    """Point memory card slot 1 of the test data dir at `card` (a file name in
    PCSX2/memcards). "unformatted" makes a fresh erased (all 0xFF) card, which
    makes the game show its "memory card ... is unformatted" dialog."""
    mc_dir = os.path.join(DATA_DIR, "PCSX2", "memcards")
    if card == "unformatted":
        card = UNFORMATTED_CARD
        with open(os.path.join(mc_dir, card), "wb") as f:
            f.write(b"\xff" * CARD_SIZE)
    if not os.path.exists(os.path.join(mc_dir, card)):
        raise SystemExit("no memory card %s in %s" % (card, mc_dir))
    ini = os.path.join(DATA_DIR, "PCSX2", "inis", "PCSX2.ini")
    lines = open(ini).read().split("\n")
    lines = ["Slot1_Filename = %s" % card if l.startswith("Slot1_Filename = ") else l for l in lines]
    open(ini, "w").write("\n".join(lines))
    return card


def launch(iso=ISO, wait=True, extra=(), card="Mcd001.ps2"):
    if our_pid():
        raise SystemExit("already running: pid %d" % our_pid())
    card = set_slot1_card(card)
    others = subprocess.run(["pgrep", "-fl", "PCSX2.app/Contents/MacOS/PCSX2"], capture_output=True, text=True).stdout.strip()
    if others:
        print("note: other PCSX2 instance(s) running, leaving them alone:\n  " + others.replace("\n", "\n  "))
    log = open(os.path.join(DATA_DIR, "dm2pcsx2-launch.log"), "w")
    args = [PCSX2_BIN, "-batch", "-fastboot", "-nofullscreen", "-datapath", DATA_DIR] + list(extra) + ["--", iso]
    proc = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    open(PID_FILE, "w").write("%d\n" % proc.pid)
    print("launched PCSX2 pid %d (datapath %s, slot-1 card %s)" % (proc.pid, DATA_DIR, card))
    if not wait:
        return proc.pid
    deadline = time.time() + 60
    while time.time() < deadline:
        if proc.poll() is not None:
            raise SystemExit("PCSX2 exited with %s" % proc.returncode)
        try:
            pad = Pad()
            if pad.p.game_id() == SERIAL and pad.hook_state() == "installed":
                print("game running, hook installed, vsync %d" % pad.vsync())
                return proc.pid
            pad.p.close()
        except (PineError, OSError):
            pass
        time.sleep(0.25)
    raise SystemExit("timed out waiting for the game / hook")


def quit_pcsx2():
    pid = our_pid()
    if not pid:
        print("nothing to stop (no verified pid in %s)" % PID_FILE)
        return
    os.kill(pid, signal.SIGTERM)
    for _ in range(50):
        if not _cmdline(pid):
            break
        time.sleep(0.1)
    else:
        os.kill(pid, signal.SIGKILL)
    os.remove(PID_FILE)
    print("stopped pid %d" % pid)


# ---- scripts ------------------------------------------------------------
AXES = {"RX": 0, "RY": 1, "LX": 2, "LY": 3}


def _script_time(s):
    """Seconds, or vsyncs with a "v" suffix; returned in seconds."""
    s = s.strip()
    return float(s[:-1]) / FPS if s.lower().endswith("v") else float(s)


def parse_native_script(text):
    """ZX_PAD_SCRIPT syntax (ps2/runtime/pad_script.h):
    "<t>:<BUTTON>[:<dur>]" (default dur 0.25 s) and "<t>:LX=<v>[:<dur>]"
    (LX LY RX RY, v in -1..1); times in seconds or vsyncs ("123v").
    SYNC=/WAIT= anchors only matter natively (PCSX2 is the reference) and
    are skipped here, like unknown entries.
    Returns [(start_s, end_s, mask, axis, axis_byte)]."""
    events = []
    for item in text.replace("\n", ",").split(","):
        item = item.strip()
        if ":" not in item or item.startswith("#"):
            continue
        parts = item.split(":")
        name = parts[1].strip().upper()
        try:
            start = _script_time(parts[0])
            dur = _script_time(parts[2]) if len(parts) > 2 else 0.25
        except ValueError:
            continue
        if "=" in name:
            key, _, val = name.partition("=")
            if key in AXES:
                events.append((start, start + dur, 0, AXES[key], axis_byte(float(val))))
            continue
        if name not in BUTTONS:
            continue
        events.append((start, start + dur, BUTTONS[name], None, None))
    return events


def run_native_script(pad, text, origin="firstpoll", fps=FPS, verbose=True):
    events = parse_native_script(text)
    if not events:
        raise SystemExit("empty script")
    pad.require_hook()
    pad.arm()
    if origin == "now":
        base = pad.vsync()
    elif origin == "boot":
        base = 0
    else:  # firstpoll: same origin as the native runtime (first pad read)
        while pad.polls() == 0:
            time.sleep(0.01)
        base = pad.p.read32(MAILBOX + MB_FIRSTPOLL_VSYNC)
    # Convert to absolute vsync numbers; ensure each press lasts >= 1 frame.
    sched = []
    for s, e, m, ax, val in events:
        fs = base + int(round(s * fps))
        fe = max(fs + 1, base + int(round(e * fps)))
        sched.append((fs, fe, m, ax, val))
    end = max(x[1] for x in sched)
    now = pad.vsync()
    if verbose:
        print("script: %d events, origin %s = vsync %d, now %d, ends at %d" % (len(sched), origin, base, now, end))
    last = None
    while True:
        v = pad.vsync()
        down, sticks = 0, [0x80] * 4
        override = False
        for fs, fe, m, ax, val in sched:
            if fs <= v < fe:
                down |= m
                if ax is not None:
                    sticks[ax], override = val, True
        state = (down, tuple(sticks), override)
        if state != last:
            pad.set_buttons(down)
            pad.p.write32(MAILBOX + MB_STICKS, struct.unpack("<I", bytes(sticks))[0])
            pad.p.write8(MAILBOX + MB_FLAGS, 1 if override else 0)
            if verbose:
                print("  vsync %d (t=%dv %.2fs): %s%s" % (v, v - base, (v - base) / fps, mask_names(down),
                                                         " sticks %s" % bytes(sticks).hex() if override else ""))
            last = state
        if v >= end:
            break
        time.sleep(0.002)
    pad.release()


def record_to_native(path, v_from=None, v_to=None, fps=FPS):
    """Turn a DM2PCSX2_RECORD log into a ZX_PAD_SCRIPT string: one
    "<sec>:<BUTTON>:<dur>" per press, seconds counted from the game's first
    pad poll (the native runtime's origin). Returns (script, stick_notes)."""
    first, events = None, []
    for line in open(path):
        if line.startswith("#"):
            first = int(line.rsplit(" ", 1)[1])
            continue
        v, kind, val = line.split()
        events.append((int(v), kind, int(val, 16)))
    if first is None:
        raise SystemExit("no header in %s" % path)
    items, sticks, held, prev_v = [], [], {}, None
    for v, kind, val in events + [(None, "END", 0)]:
        if v is not None and prev_v is not None and v < prev_v:
            raise SystemExit("vsync goes backwards at %d (loadstate in the log?); use FROM/TO" % v)
        prev_v = v if v is not None else prev_v
        if v is not None and ((v_from is not None and v < v_from) or (v_to is not None and v > v_to)):
            continue
        if kind == "S":
            if val != 0x80808080:
                sticks.append("%.2fs sticks RX RY LX LY = %s" % ((v - first) / fps, struct.pack("<I", val).hex(" ")))
            continue
        mask = val if kind == "B" else 0
        vv = v if v is not None else prev_v
        for name, bit in BUTTONS.items():
            if mask & bit and name not in held:
                held[name] = vv
            elif not (mask & bit) and name in held:
                start = held.pop(name)
                items.append((start, "%.2f:%s:%.2f" % ((start - first) / fps, name, max(1, vv - start) / fps)))
    items.sort()
    return ",".join(i for _, i in items), sticks


def read_seen_file(path):
    first, ents = None, []
    for line in open(path):
        if line.startswith("#"):
            if "firstpoll" in line:
                first = int(line.split("firstpoll")[1].split()[0])
            continue
        f = line.split()
        ents.append((int(f[0]), int(f[1], 16), int(f[2], 16), int(f[3], 16)))
    return first, ents


def seen_presses(path):
    """(firstpoll, [(start_vsync, button, duration_vsyncs)]) from a seen log."""
    first, ents = read_seen_file(path)
    presses, held = [], {}
    end = ents[-1][0] + 1 if ents else 0
    for v, mask, _, _ in ents + [(end, 0, 0x80, 0x80)]:
        for name, bit in BUTTONS.items():
            if mask & bit and name not in held:
                held[name] = v
            elif not (mask & bit) and name in held:
                start = held.pop(name)
                presses.append((start, name, max(1, v - start)))
    presses.sort()
    return first, presses


def seen_to_native(path, delay=0):
    first, presses = seen_presses(path)
    return ",".join("%dv:%s:%dv" % (s - first + delay, n, d) for s, n, d in presses)


def run_command_file(pad, lines, args):
    """One command per line (same verbs as the CLI); '#' starts a comment."""
    for lineno, raw in enumerate(lines, 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        argv = shlex.split(line)
        print("[%d] %s   (vsync %d)" % (lineno, line, pad.vsync()))
        dispatch(pad, argv, args)


def read_typed(pad, addr, typ):
    if typ == "f32":
        return struct.unpack("<f", pad.p.read_block(addr, 4))[0]
    if typ == "s32":
        return struct.unpack("<i", struct.pack("<I", pad.p.read32(addr)))[0]
    return {"u8": pad.p.read8, "u16": pad.p.read16, "u32": pad.p.read32}[typ](addr)


def compare(a, op, b):
    return {"==": a == b, "!=": a != b, "<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b}[op]


def shot(pad, path, slot=9):
    import zipfile
    state = pad.save_state(slot)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with zipfile.ZipFile(state) as z:
        open(path, "wb").write(z.read("Screenshot.png"))
    return path


def read_seenlog(pad):
    """[(vsync, buttons_active_high, lx, ly, level frame, polls)] from the hook's seen log."""
    n = pad.p.read32(MAILBOX + MB_SEEN_COUNT)
    if n > SEENLOG_ENTRIES:
        print("warning: seen log wrapped (%d entries, ring %d)" % (n, SEENLOG_ENTRIES), file=sys.stderr)
    first = max(0, n - SEENLOG_ENTRIES)
    raw = pad.p.read_block(SEENLOG, SEENLOG_ENTRIES * SEENLOG_SIZE)
    out = []
    for k in range(first, n):
        v, w, f, polls = struct.unpack_from("<IIII", raw, (k % SEENLOG_ENTRIES) * SEENLOG_SIZE)
        out.append((v, (~w) & 0xFFFF, (w >> 16) & 0xFF, (w >> 24) & 0xFF, f, polls))
    return out


SCHED_BY_VSYNC = 0x80000000            # a key with this bit: a VBlank count, else a level frame


def parse_schedule(path):
    """[(key, mask, sticks or None)] from "KEY BUTTON+BUTTON|- [LX LY]" lines ('#' comments): KEY a level frame or
    "<n>v" a VBlank count (SCHED_BY_VSYNC | n); sticks (RX, RY, LX, LY) bytes, RX RY centred."""
    out = []
    for line in open(path):
        f = line.split("#", 1)[0].split()
        if not f:
            continue
        key = SCHED_BY_VSYNC | int(f[0][:-1]) if f[0].endswith("v") else int(f[0])
        mask = 0 if f[1] == "-" else parse_buttons(f[1])
        sticks = (0x80, 0x80, int(f[2], 16), int(f[3], 16)) if len(f) >= 4 else None
        out.append((key, mask, sticks))
    for kind in (0, SCHED_BY_VSYNC):
        keys = [k for k, _, _ in out if k & SCHED_BY_VSYNC == kind]
        if keys != sorted(keys):
            raise SystemExit("%s: keys not in order" % path)
    return out


def load_schedule(pad, entries):
    if len(entries) > SCHED_MAX:
        raise SystemExit("schedule too long: %d entries (at most %d)" % (len(entries), SCHED_MAX))
    pad.p.write32(MAILBOX + MB_SCHED_N, 0)          # off while it is written
    blob = b"".join(struct.pack("<IIII", f, m | (0x10000 if st else 0), struct.unpack("<I", bytes(st))[0] if st else 0, 0)
                    for f, m, st in entries)
    for i in range(0, len(blob), 4096):
        pad.p.write_block(SCHED + i, blob[i:i + 4096])
    pad.p.write32(MAILBOX + MB_SCHED_NEXT, 0)
    pad.p.write32(MAILBOX + MB_SCHED_HELD, 0)
    pad.p.write32(MAILBOX + MB_SCHED_STICKS, 0)
    pad.p.write32(MAILBOX + MB_SCHED_N, len(entries))


def game_state(pad):
    """Key DM2 anchors, read live over PINE."""
    rd = pad.p.read32
    s = {"vsync": pad.vsync(), "mode": rd(SOUND_MODE), "gametype": rd(GAME_TYPE),
         "lvlcursor": rd(LEVEL_CURSOR), "session": rd(SESSION), "frame": rd(LEVEL_FRAME)}
    obj = rd(RIDER_TABLE)
    s["rider"] = obj
    if s["frame"] and 0x100000 <= obj < 0x2000000:
        s["pos"] = struct.unpack("<3f", pad.p.read_block(obj + RIDER_POS, 12))
        s["vel"] = struct.unpack("<3f", pad.p.read_block(obj + RIDER_VEL, 12))
    return s


def checkpoint_extract(state_path, name, ram_dir, png_dir):
    """Copy a savestate to pcsx2-data-dm2/reference/NAME.p2s and extract its
    EE RAM to RAM_DIR/NAME.ram and its frame thumbnail to PNG_DIR/NAME.png.
    The .p2s is a zip with zstd members (Python 3.14+ zipfile reads them)."""
    import zipfile
    keep = os.path.join(os.environ.get("DM2PCSX2_STATE_DIR", os.path.join(DATA_DIR, "reference")), name + ".p2s")
    for d in (os.path.dirname(keep), ram_dir, png_dir):
        os.makedirs(d, exist_ok=True)
    shutil.copyfile(state_path, keep)
    with zipfile.ZipFile(keep) as z:
        ram = z.read("eeMemory.bin")
        png = z.read("Screenshot.png")
    ram_path, png_path = os.path.join(ram_dir, name + ".ram"), os.path.join(png_dir, name + ".png")
    open(ram_path, "wb").write(ram)
    open(png_path, "wb").write(png)
    vs = struct.unpack_from("<I", ram, VSYNC)[0]
    gf = struct.unpack_from("<I", ram, LEVEL_FRAME)[0]
    return "checkpoint %s: %s, %s, %s (vsync %d, game frame %d)" % (name, keep, ram_path, png_path, vs, gf)


# ---- CLI ----------------------------------------------------------------
def hexdump(data, base):
    out = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hx = " ".join("%02x" % b for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        out.append("%08x  %-47s  %s" % (base + i, hx, asc))
    return "\n".join(out)


def cmd_status(pad):
    p = pad.p
    st = p.status()
    print("PCSX2      %s, %s, slot %d (%s)" % (p.version(), STATUS_NAMES.get(st, st), p.slot, p.path))
    print("game       %s / %s / crc-uuid %s" % (p.game_id(), p.title(), p.uuid()))
    print("hook       %s" % pad.hook_state())
    mb = p.read_block(MAILBOX, 0x18)
    magic, btn, flags, _, sticks, polls, seen, raw, fpv = struct.unpack("<IHBB4sIHHI", mb)
    print("mailbox    @%08x armed=%s held=%s sticks=%s (override %s)" % (
        MAILBOX, magic == MAGIC, mask_names(btn), sticks.hex(), bool(flags & 1)))
    print("polls      %d (first poll at vsync %d)" % (polls, fpv))
    print("game sees  %04x (%s held), real pad %04x" % (seen, mask_names(~seen & 0xFFFF), raw))
    print("vsync      %d (VBlank handler installed %s, id %d)" % (
        pad.vsync(), bool(p.read32(MAILBOX + MB_VSYNC_ON)), p.read32(MAILBOX + MB_HANDLER_ID)))
    n, nxt, held = p.read_words([MAILBOX + MB_SCHED_N, MAILBOX + MB_SCHED_NEXT, MAILBOX + MB_SCHED_HELD])
    print("schedule   %d entries, %d taken, holding %s (level frame %d)" % (n, nxt, mask_names(held & 0xFFFF),
                                                                            p.read32(LEVEL_FRAME)))
    pid = our_pid()
    print("our pid    %s" % (pid or "-"))


def dispatch(pad, argv, args):
    if not argv:
        return
    verb, rest = argv[0].lower(), argv[1:]

    def need_pad():
        if pad is None:
            raise SystemExit("not connected")
        return pad

    if verb == "press":
        mask = parse_buttons(rest[0])
        frames = int(rest[1]) if len(rest) > 1 else TAP_FRAMES
        need_pad().press(mask, frames)
    elif verb == "hold":
        need_pad().hold(parse_buttons(rest[0]), int(rest[1]) if len(rest) > 1 else 1)
    elif verb == "down":
        pad.arm()
        pad.set_buttons(pad.get_buttons() | parse_buttons(rest[0]))
    elif verb == "up":
        pad.set_buttons(pad.get_buttons() & ~parse_buttons(rest[0]))
    elif verb == "stick":
        which, x, y = rest[0], float(rest[1]), float(rest[2])
        need_pad().arm()
        pad.set_stick(which, x, y)
        if len(rest) > 3:
            pad.wait_frames(int(rest[3]), 1)
            pad.set_stick(which, 0, 0)
            if pad.p.read32(MAILBOX + MB_STICKS) == 0x80808080:
                pad.p.write8(MAILBOX + MB_FLAGS, 0)   # both centred: drop override
    elif verb == "release":
        need_pad().release()
    elif verb == "wait":
        need_pad().wait_frames(int(rest[0]))
    elif verb == "waitpolls":
        # waitpolls N: until the hook has seen N port-0 pad reads in total
        # (`waitpolls 1` = the game has started polling the pad)
        target = int(rest[0])
        t0 = time.time()
        while need_pad().polls() < target:
            if time.time() - t0 > 120:
                raise SystemExit("waitpolls timeout at %d" % pad.polls())
            time.sleep(0.01)
    elif verb == "waitsec":
        need_pad().wait_frames(int(round(float(rest[0]) * FPS)))
    elif verb == "waitmem":
        # waitmem ADDR VALUE [SIZE] [TIMEOUT_FRAMES]
        addr, value = parse_int(rest[0]), parse_int(rest[1])
        size = int(rest[2]) if len(rest) > 2 else 4
        limit = int(rest[3]) if len(rest) > 3 else 60 * 60
        rd = {1: pad.p.read8, 2: pad.p.read16, 4: pad.p.read32, 8: pad.p.read64}[size]
        v0 = pad.vsync()
        while rd(addr) != value:
            if pad.vsync() - v0 > limit:
                raise SystemExit("waitmem timeout: %08x = %x" % (addr, rd(addr)))
            time.sleep(0.005)
    elif verb == "waitval":
        # waitval ADDR TYPE OP VALUE [TIMEOUT_FRAMES]: until the typed value
        # compares true, e.g. `waitval 0x2A76F0 f32 > -5.0` (player x past -5).
        # TYPE u8|u16|u32|s32|f32, OP == != < <= > >=
        addr, typ, op = parse_int(rest[0]), rest[1], rest[2]
        value = float(rest[3]) if typ == "f32" else parse_int(rest[3])
        limit = int(rest[4]) if len(rest) > 4 else 60 * 60
        v0 = need_pad().vsync()
        while True:
            cur = read_typed(pad, addr, typ)
            if compare(cur, op, value):
                print("  %08x %s = %s (vsync %d)" % (addr, typ, cur, pad.vsync()))
                break
            if pad.vsync() - v0 > limit:
                raise SystemExit("waitval timeout: %08x %s = %s, wanted %s %s" % (addr, typ, cur, op, rest[3]))
            time.sleep(0.002)
    elif verb == "pressuntil":
        # pressuntil BTN ADDR TYPE OP VALUE [TIMEOUT_FRAMES] [GAP_FRAMES]: tap
        # BTN, then wait up to GAP frames (default 20) for the condition; repeat
        # until it holds. For dialogs that ignore input for a while after they open.
        mask, addr, typ, op = parse_buttons(rest[0]), parse_int(rest[1]), rest[2], rest[3]
        value = float(rest[4]) if typ == "f32" else parse_int(rest[4])
        limit = int(rest[5]) if len(rest) > 5 else 60 * 60
        gap = int(rest[6]) if len(rest) > 6 else 20
        v0, taps = need_pad().vsync(), 0
        while not compare(read_typed(pad, addr, typ), op, value):
            if pad.vsync() - v0 > limit:
                raise SystemExit("pressuntil timeout after %d taps: %08x %s = %s" % (
                    taps, addr, typ, read_typed(pad, addr, typ)))
            pad.press(mask)
            taps += 1
            t0 = pad.vsync()
            while pad.vsync() - t0 < gap and not compare(read_typed(pad, addr, typ), op, value):
                time.sleep(0.002)
        print("  %08x %s = %s after %d taps (vsync %d)" % (addr, typ, read_typed(pad, addr, typ), taps, pad.vsync()))
    elif verb == "waitnear":
        # waitnear ADDR X Z RADIUS [TIMEOUT_FRAMES]: until the float vec3 at
        # ADDR is within RADIUS of (X, Z) in the ground plane.
        addr, x, z, r = parse_int(rest[0]), float(rest[1]), float(rest[2]), float(rest[3])
        limit = int(rest[4]) if len(rest) > 4 else 60 * 60
        v0 = need_pad().vsync()
        while True:
            px, py, pz = struct.unpack("<3f", pad.p.read_block(addr, 12))
            if (px - x) ** 2 + (pz - z) ** 2 <= r * r:
                print("  at %.2f %.2f %.2f (vsync %d)" % (px, py, pz, pad.vsync()))
                break
            if pad.vsync() - v0 > limit:
                raise SystemExit("waitnear timeout: at %.2f %.2f %.2f" % (px, py, pz))
            time.sleep(0.002)
    elif verb == "peekf":
        # peekf ADDR [N]: N floats
        addr = parse_int(rest[0])
        n = int(rest[1]) if len(rest) > 1 else 4
        vals = struct.unpack("<%df" % n, need_pad().p.read_block(addr, 4 * n))
        print("%08x: %s (vsync %d)" % (addr, " ".join("%.4f" % v for v in vals), pad.vsync()))
    elif verb == "checkpoint":
        # checkpoint SLOT NAME [RAM_DIR] [PNG_DIR]: savestate SLOT, keep a copy
        # as pcsx2-data-dm2/reference/NAME.p2s, and extract from it the EE RAM
        # (atomic, unlike `dump`) and PCSX2's 640x480 frame thumbnail.
        slot, name = int(rest[0]), rest[1]
        # Defaults can be redirected with DM2PCSX2_RAM_DIR / DM2PCSX2_PNG_DIR /
        # DM2PCSX2_STATE_DIR (e.g. for a second run that should not replace the first).
        ram_dir = rest[2] if len(rest) > 2 else os.environ.get(
            "DM2PCSX2_RAM_DIR", os.path.join(REPO, "recomp", "dumps", "ref"))
        png_dir = rest[3] if len(rest) > 3 else os.environ.get(
            "DM2PCSX2_PNG_DIR", os.path.join(REPO, "reference", "pcsx2-run1"))
        path = need_pad().save_state(slot)
        print(checkpoint_extract(path, name, ram_dir, png_dir))
    elif verb == "script":
        text = open(rest[0][1:]).read() if rest[0].startswith("@") else rest[0]
        run_native_script(need_pad(), text, origin=getattr(args, "origin", "firstpoll") or "firstpoll")
    elif verb == "run":
        run_command_file(need_pad(), open(rest[0]).read().splitlines(), args)
    elif verb == "peek":
        addr = parse_int(rest[0])
        n = parse_int(rest[1]) if len(rest) > 1 else 16
        print(hexdump(pad.p.read_block(addr, n), addr))
    elif verb == "poke":
        addr, value = parse_int(rest[0]), parse_int(rest[1])
        size = int(rest[2]) if len(rest) > 2 else 4
        {1: pad.p.write8, 2: pad.p.write16, 4: pad.p.write32, 8: pad.p.write64}[size](addr, value)
    elif verb == "dump":
        addr, n, path = parse_int(rest[0]), parse_int(rest[1]), rest[2]
        data = pad.p.read_block(addr, n)
        open(path, "wb").write(data)
        print("dumped %d bytes from %08x to %s (vsync %d)" % (len(data), addr, path, pad.vsync()))
    elif verb == "savestate":
        path = need_pad().save_state(int(rest[0]))
        print("saved %s" % path)
        if len(rest) > 1:   # savestate N COPY_TO: keep a named copy
            os.makedirs(os.path.dirname(os.path.abspath(rest[1])), exist_ok=True)
            shutil.copyfile(path, rest[1])
            print("copied to %s" % rest[1])
    elif verb == "loadstate":
        print("loaded %s" % need_pad().load_state(int(rest[0])))
    elif verb == "screenshot":
        os.makedirs(os.path.dirname(os.path.abspath(rest[0])), exist_ok=True)
        print("screenshot %s" % screenshot(rest[0]))
    elif verb == "shot":
        # shot FILE [SLOT]: the emulated frame as PNG (640x480), from a
        # savestate's thumbnail (scratch slot 9 by default). Unlike
        # `screenshot` this works with the window hidden, minimised or on
        # another Space (PCSX2 then stops presenting, so window captures go stale).
        print("shot %s (vsync %d)" % (shot(need_pad(), rest[0], int(rest[1]) if len(rest) > 1 else 9), pad.vsync()))
    elif verb == "seenlog":
        # seenlog FILE: write the hook's log of what the game saw (one line
        # per change: vsync, buttons active-high hex, LX, LY, level frame,
        # polls) with a header
        # holding the first-poll vsync. `padscript` turns it into a native script.
        ents = read_seenlog(need_pad())
        with open(rest[0], "w") as f:
            f.write("# dm2 seen log; firstpoll %d\n" % pad.p.read32(MAILBOX + MB_FIRSTPOLL_VSYNC))
            for v, b, lx, ly, fr, polls in ents:
                f.write("%d %04x %02x %02x %d %d %s\n" % (v, b, lx, ly, fr, polls, mask_names(b)))
        print("seen log: %d entries -> %s" % (len(ents), rest[0]))
    elif verb == "state":
        s = game_state(need_pad())
        print("vsync %(vsync)d mode %(mode)d gametype %(gametype)d lvlcursor %(lvlcursor)d session %(session)08x frame %(frame)d rider %(rider)08x" % s
              + ("  pos %.3f %.3f %.3f vel %.4f %.4f %.4f" % (s["pos"] + s["vel"]) if "pos" in s else ""))
    elif verb == "schedule":
        # schedule FILE: a ride by level frame into the hook's schedule (see the file format above)
        entries = parse_schedule(rest[0])
        load_schedule(need_pad(), entries)
        frames = [k for k, _, _ in entries if not k & SCHED_BY_VSYNC]
        print("schedule: %d entries (%d by vsync), level frames %s..%s" % (
            len(entries), len(entries) - len(frames), frames[0] if frames else "-", frames[-1] if frames else "-"))
    elif verb == "waitframe":
        # waitframe N [TIMEOUT_VSYNCS]: until the level frame counter reaches N
        target, limit = int(rest[0]), int(rest[1]) if len(rest) > 1 else 60 * 600
        v0 = need_pad().vsync()
        while pad.p.read32(LEVEL_FRAME) < target:
            if pad.vsync() - v0 > limit:
                raise SystemExit("waitframe timeout: frame %d" % pad.p.read32(LEVEL_FRAME))
            time.sleep(0.002)
        print("  frame %d (vsync %d)" % (pad.p.read32(LEVEL_FRAME), pad.vsync()))
    elif verb == "waitvsync":
        # waitvsync N: until the VBlank counter reaches N (absolute)
        need_pad().wait_until_vsync(int(rest[0]))
    elif verb == "frame":
        v, n = pad.counters()
        print("vsync %d polls %d" % (v, n))
    elif verb == "status":
        cmd_status(pad)
    elif verb == "echo":
        print(" ".join(rest))
    else:
        raise SystemExit("unknown command %r" % verb)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--slot", type=int, default=DEFAULT_SLOT, help="PINE slot (default %(default)s)")
    ap.add_argument("--origin", choices=["firstpoll", "now", "boot"], default="firstpoll",
                    help="time zero for `script`: the game's first pad read (like the native "
                         "runtime), now, or vsync 0")
    ap.add_argument("command", nargs=argparse.REMAINDER,
                    help="launch | quit | status | press BTN[+BTN] [frames] | hold BTN N | down BTN | up BTN | "
                         "stick L|R x y [frames] | release | wait N | waitsec S | waitmem ADDR VAL [size] [timeout] | "
                         "script '<ZX_PAD_SCRIPT>'|@file | run FILE | peek ADDR [N] | poke ADDR VAL [size] | "
                         "dump ADDR LEN FILE | savestate N | loadstate N | screenshot FILE | frame | "
                         "waitval ADDR TYPE OP VAL | pressuntil BTN ADDR TYPE OP VAL | "
                         "waitnear ADDR X Z R | peekf ADDR [N] | checkpoint SLOT NAME | "
                         "padscript LOG [FROM [TO]] (see REFERENCE-RUN.md, REFERENCE-RUN-2.md)")
    args = ap.parse_args()
    if not args.command:
        ap.print_help()
        return
    verb = args.command[0].lower()
    if verb == "launch":
        # launch [--card unformatted|FILE] [ISO]
        rest, card = args.command[1:], "Mcd001.ps2"
        if rest[:1] == ["--card"]:
            card, rest = rest[1], rest[2:]
        launch(rest[0] if rest else ISO, card=card)
        return
    if verb == "quit":
        quit_pcsx2()
        return
    if verb == "screenshot":
        print("screenshot %s" % screenshot(args.command[1]))
        return
    if verb == "padscript":
        # padscript SEENLOG [DELAY_VSYNCS]: the seen log (`seenlog`) as a
        # ZX_PAD_SCRIPT in vsync units from the first pad read.
        rest = args.command[1:]
        print(seen_to_native(rest[0], int(rest[1]) if len(rest) > 1 else 0))
        return
    try:
        pad = Pad(args.slot)
    except PineError as e:
        raise SystemExit(str(e))
    try:
        dispatch(pad, args.command, args)
    except KeyboardInterrupt:
        pad.release()
        raise


if __name__ == "__main__":
    main()
