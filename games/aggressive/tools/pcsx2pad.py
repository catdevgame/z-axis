#!/usr/bin/env python3
"""Drive controller input into PCSX2 (Aggressive Inline USA, SLUS-20327) over PINE.

No OS input, no Accessibility permission, no window focus: the pnach hook in
pcsx2pad_hook.s makes the game's own scePadRead merge a mailbox in EE RAM, and
this tool writes that mailbox over PINE. Timing is in frames of the game's
VBlank counter (0x00231CA4), not wall time. See PCSX2PAD.md.

Examples
  pcsx2pad.py launch                       # start the test PCSX2 (slot 28012)
  pcsx2pad.py status
  pcsx2pad.py press START                  # tap (down, then up)
  pcsx2pad.py hold CROSS 10                # hold 10 frames
  pcsx2pad.py press UP+CROSS
  pcsx2pad.py stick L 0 -1 30              # left stick full up for 30 frames
  pcsx2pad.py release
  pcsx2pad.py script "7:START,11:START,15:START"    # ZX_PAD_SCRIPT syntax
  pcsx2pad.py run tools/scripts/boot-to-main-menu.txt
  pcsx2pad.py peek 0x282494 32 ; poke 0x231ca4 0 --size 4
  pcsx2pad.py dump 0 0x2000000 ram.bin
  pcsx2pad.py savestate 1 ; loadstate 1 ; screenshot shot.png ; quit
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
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools"))   # (tools/: zpaths.py)
import zpaths  # noqa: E402  (tools/paths.env: the discs)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_SLOT = int(os.environ.get("PCSX2PAD_SLOT", "28012"))
DATA_DIR = os.environ.get("PCSX2PAD_DATA", os.path.join(HERE, "pcsx2-test-data"))
PCSX2_BIN = "/Applications/PCSX2.app/Contents/MacOS/PCSX2"
ISO = os.path.join(zpaths.GAME_DISCS, "aggressive inline", "Aggressive Inline-ps2.iso")
PID_FILE = os.path.join(DATA_DIR, "pcsx2pad.pid")
SERIAL = "SLUS-20327"

# ---- addresses (must match pcsx2pad_hook.s / build_hook.py) ---------------
MAILBOX = 0x000F7000
MB_MAGIC, MB_BUTTONS, MB_FLAGS, MB_STICKS = 0x00, 0x04, 0x06, 0x08
MB_POLLS, MB_SEEN, MB_RAW, MB_FIRSTPOLL_VSYNC = 0x0C, 0x10, 0x12, 0x14
MAGIC = 0x4D444150                      # "PADM"
CAVE = 0x000F8000
CAVE_FIRST_WORD = 0x27BDFFE0            # addiu sp, sp, -32
CALL_SITE = 0x001188D0
HOOKED_CALL = 0x0C000000 | (CAVE >> 2)
ORIGINAL_CALL = 0x0C08468C              # jal scePadRead (0x00211A30)
VSYNC = 0x00231CA4                      # incremented by the game's VBlank handler
PAD_BUFFER = 0x00282494                 # game's scePadRead destination (32 bytes)

FPS = 59.94                             # NTSC field rate, for second-based scripts

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
        # PCSX2PAD_RECORD=FILE appends every input change with its vsync, so a
        # run can be turned into a ZX_PAD_SCRIPT afterwards (`padscript`).
        self.record = os.environ.get("PCSX2PAD_RECORD")
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
        subprocess.run(["swiftc", "-O", "-o", winid, os.path.join(REPO, "recomp", "tools", "winid.swift")], check=True)
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
    log = open(os.path.join(DATA_DIR, "pcsx2pad-launch.log"), "w")
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
def parse_native_script(text):
    """ZX_PAD_SCRIPT syntax: "<sec>:<BUTTON>[:<dur>],..." (default dur 0.25 s).
    Same parsing rules as ps2/runtime/pad.cpp: unknown buttons and
    items without ':' are skipped."""
    events = []
    for item in text.replace("\n", ",").split(","):
        item = item.strip()
        if ":" not in item:
            continue
        parts = item.split(":")
        try:
            start = float(parts[0])
            dur = float(parts[2]) if len(parts) > 2 else 0.25
        except ValueError:
            continue
        name = parts[1].strip().upper()
        if name not in BUTTONS:
            continue
        events.append((start, start + dur, BUTTONS[name]))
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
    for s, e, m in events:
        fs = base + int(round(s * fps))
        fe = max(fs + 1, base + int(round(e * fps)))
        sched.append((fs, fe, m))
    end = max(fe for _, fe, _ in sched)
    now = pad.vsync()
    if verbose:
        print("script: %d events, origin %s = vsync %d, now %d, ends at %d" % (len(sched), origin, base, now, end))
    last = None
    while True:
        v = pad.vsync()
        down = 0
        for fs, fe, m in sched:
            if fs <= v < fe:
                down |= m
        if down != last:
            pad.set_buttons(down)
            if verbose:
                print("  vsync %d (t=%.2fs): %s" % (v, (v - base) / fps, mask_names(down)))
            last = down
        if v >= end:
            break
        time.sleep(0.002)
    pad.set_buttons(0)


def record_to_native(path, v_from=None, v_to=None, fps=FPS):
    """Turn a PCSX2PAD_RECORD log into a ZX_PAD_SCRIPT string: one
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


PLAYER_POS = 0x002A76F0                 # player x, y, z floats (see REFERENCE-RUN.md)
BOX_MODE = 0x00276FE4                   # 3 while a message box (tip, HINT) pauses play, 2 in play
ZGUI_PANEL = 0x00240A20                 # zgui active panel pointer (D_00240A10+0x10), 0 = none
TIP_GAP = 20                            # frames to wait for a tap to register
STEER_DEADBAND = 0.08                   # radians of heading error before steering


def wait_value(pad, addr, typ, op, value, limit):
    """Wait up to `limit` frames for the condition; return True if it held."""
    v0 = pad.vsync()
    while not compare(read_typed(pad, addr, typ), op, value):
        if pad.vsync() - v0 > limit:
            return False
        time.sleep(0.002)
    return True


def wait_box(pad, limit):
    """Wait up to `limit` frames for a message box; True if one is up."""
    v0 = pad.vsync()
    while not (pad.p.read32(BOX_MODE) == 3 and pad.p.read32(ZGUI_PANEL) != 0):
        if pad.vsync() - v0 > limit:
            return False
        time.sleep(0.002)
    return True


def dismiss_box(pad, limit):
    """Wait for a message box, then tap CROSS until it closes: the zgui panel
    pointer drops to 0 (for one frame between chained boxes) or play resumes.
    Boxes ignore input for about 40 frames after they open; taps then do nothing."""
    v0 = pad.vsync()
    while not (pad.p.read32(BOX_MODE) == 3 and pad.p.read32(ZGUI_PANEL) != 0):
        if pad.vsync() - v0 > limit:
            raise SystemExit("no message box within %d frames" % limit)
        time.sleep(0.002)
    closed = lambda: pad.p.read32(ZGUI_PANEL) == 0 or pad.p.read32(BOX_MODE) != 3
    pad.arm()
    while True:
        if pad.vsync() - v0 > limit:
            raise SystemExit("message box did not close")
        pad.set_buttons(BUTTONS["CROSS"])
        t0 = pad.vsync()
        while pad.vsync() - t0 < TAP_FRAMES:
            if closed():
                pad.set_buttons(0)
                return
            time.sleep(0.001)
        pad.set_buttons(0)
        t0 = pad.vsync()
        while pad.vsync() - t0 < TIP_GAP:
            if closed():
                return
            time.sleep(0.001)


def skate_to(pad, x, z, r, limit, extra=0, sharp=False):
    """Hold UP (+extra) and steer with LEFT/RIGHT towards (x, z) until the
    player is within r of it (r a number) or a condition like "x>-16" holds.
    In this game, RIGHT turns the heading from +x towards -z."""
    import math
    if isinstance(r, str):
        axis, op, val = r[0].lower(), r[1], float(r[2:])
        reached = lambda px, py, pz: compare({"x": px, "y": py, "z": pz}[axis], op, val)
    else:
        reached = lambda px, py, pz: (x - px) ** 2 + (z - pz) ** 2 <= r * r
    pad.arm()
    v0 = last_v = pad.vsync()
    px, _, pz = struct.unpack("<3f", pad.p.read_block(PLAYER_POS, 12))
    heading, steer = None, 0
    try:
        while True:
            v = pad.vsync()
            if v == last_v:
                time.sleep(0.002)
                continue
            last_v = v
            nx, ny, nz = struct.unpack("<3f", pad.p.read_block(PLAYER_POS, 12))
            dx, dz = nx - px, nz - pz
            px, pz = nx, nz
            if reached(px, ny, pz):
                print("  reached %.2f %.2f %.2f (vsync %d, %d frames)" % (px, ny, pz, v, v - v0))
                return
            if v - v0 > limit:
                raise SystemExit("goto timeout at %.2f %.2f %.2f" % (px, ny, pz))
            if dx * dx + dz * dz > 1e-4:
                heading = math.atan2(dz, dx)
            steer = 0
            if heading is not None:
                err = math.atan2(z - pz, x - px) - heading
                err = (err + math.pi) % (2 * math.pi) - math.pi
                if err > STEER_DEADBAND:
                    steer = BUTTONS["LEFT"] | (BUTTONS["L1"] if sharp else 0)
                elif err < -STEER_DEADBAND:
                    steer = BUTTONS["RIGHT"] | (BUTTONS["R1"] if sharp else 0)
            pad.set_buttons(BUTTONS["UP"] | extra | steer)
    finally:
        pad.set_buttons(0)


def chase(pad, addr, back, r, limit, grab, until_y):
    """Skate behind a moving object whose position (x, y, z floats) is at
    `addr`: first to a point `back` units behind it (opposite its measured
    motion), then at its back, holding `grab` once within `r` of it. Returns
    when the player's y drops below `until_y` (lifted onto it, e.g. skitching),
    leaving `grab` held."""
    import math
    rd = lambda a: struct.unpack("<3f", pad.p.read_block(a, 12))
    pad.arm()
    v0 = last_v = pad.vsync()
    px, _, pz = rd(PLAYER_POS)
    ox, _, oz = rd(addr)
    heading, vdir, phase = None, None, 1
    while True:
        v = pad.vsync()
        if v == last_v:
            time.sleep(0.002)
            continue
        last_v = v
        nx, ny, nz = rd(PLAYER_POS)
        cx, _, cz = rd(addr)
        dx, dz, px, pz = nx - px, nz - pz, nx, nz
        ex, ez, ox, oz = cx - ox, cz - oz, cx, cz
        if ny < until_y:
            pad.set_buttons(grab)
            print("  attached at %.2f %.2f %.2f, object at %.2f %.2f (vsync %d, %d frames)" % (px, ny, pz, cx, cz, v, v - v0))
            return
        if v - v0 > limit:
            pad.set_buttons(0)
            raise SystemExit("chase timeout at %.2f %.2f %.2f, object at %.2f %.2f" % (px, ny, pz, cx, cz))
        if 1e-4 < ex * ex + ez * ez < 1:
            n = math.hypot(ex, ez)
            vdir = (ex / n, ez / n)
        if dx * dx + dz * dz > 1e-4:
            heading = math.atan2(dz, dx)
        if vdir is None:
            continue
        k = back if phase == 1 else 0.3
        tx, tz = cx - k * vdir[0], cz - k * vdir[1]
        if phase == 1 and math.hypot(tx - px, tz - pz) < 1.5:
            phase = 2
        mask = BUTTONS["UP"]
        if heading is not None:
            err = (math.atan2(tz - pz, tx - px) - heading + math.pi) % (2 * math.pi) - math.pi
            if err > STEER_DEADBAND:
                mask |= BUTTONS["LEFT"]
            elif err < -STEER_DEADBAND:
                mask |= BUTTONS["RIGHT"]
        if phase == 2 and math.hypot(cx - px, cz - pz) < r:
            mask |= grab
        pad.set_buttons(mask)


PLAYER_OBJ_PTR = 0x0024C860             # -> player object in a level (PCSX2: 0x971990 Tutorial, 0x972FF0 Movie Lot), 0 in the front end
BALANCE = {                             # (offset in the player object, button raising it, button lowering it)
    "grind": (0x280, "RIGHT", "LEFT"),  # horizontal meter on rails, falls off at +-1
    "manual": (0x284, "DOWN", "UP"),    # vertical meter in manuals, falls at +-1
}
TRICK_POINTS = 0x8D8                    # f32 in the player object: points of the current combo (before multiplier)


def balance(pad, kind, frames, kd=24.0, deadband=0.02, extra=0):
    """Hold a grind or manual for `frames` with a PD rule on the balance meter:
    press the raising button when bal + kd * (change per frame) < -deadband,
    the lowering one when it is > deadband. Stops early if the meter and the
    combo points both read 0 for 10 frames in a row (combo over: banked or
    bailed). Returns the combo points."""
    off, up_name, down_name = BALANCE[kind]
    obj = pad.p.read32(PLAYER_OBJ_PTR)
    rdf = lambda o: struct.unpack("<f", pad.p.read_block(obj + o, 4))[0]
    pad.arm()
    v0 = last_v = pad.vsync()
    prev, zero = rdf(off), 0
    try:
        while pad.vsync() - v0 < frames:
            v = pad.vsync()
            if v == last_v:
                time.sleep(0.001)
                continue
            last_v = v
            bal = rdf(off)
            pred = bal + kd * (bal - prev)
            prev = bal
            zero = zero + 1 if bal == 0 and rdf(TRICK_POINTS) == 0 else 0
            if zero >= 10:
                print("  %s over after %d frames (vsync %d)" % (kind, v - v0, v))
                break
            mask = BUTTONS[up_name] if pred < -deadband else (BUTTONS[down_name] if pred > deadband else 0)
            pad.set_buttons(mask | extra)
    finally:
        pad.set_buttons(extra)
    pts = rdf(TRICK_POINTS)
    print("  %s: points %.0f (vsync %d)" % (kind, pts, pad.vsync()))
    return pts


def checkpoint_extract(state_path, name, ram_dir, png_dir):
    """Copy a savestate to pcsx2-test-data/reference/NAME.p2s and extract its
    EE RAM to RAM_DIR/NAME.ram and its frame thumbnail to PNG_DIR/NAME.png.
    The .p2s is a zip with zstd members (Python 3.14+ zipfile reads them)."""
    import zipfile
    keep = os.path.join(os.environ.get("PCSX2PAD_STATE_DIR", os.path.join(DATA_DIR, "reference")), name + ".p2s")
    for d in (os.path.dirname(keep), ram_dir, png_dir):
        os.makedirs(d, exist_ok=True)
    shutil.copyfile(state_path, keep)
    with zipfile.ZipFile(keep) as z:
        ram = z.read("eeMemory.bin")
        png = z.read("Screenshot.png")
    ram_path, png_path = os.path.join(ram_dir, name + ".ram"), os.path.join(png_dir, name + ".png")
    open(ram_path, "wb").write(ram)
    open(png_path, "wb").write(png)
    vs, fl, gf = struct.unpack_from("<3I", ram, VSYNC)
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
    print("vsync      %d" % pad.vsync())
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
    elif verb in ("nexttip", "tips"):
        # nexttip [TIMEOUT_FRAMES]: wait for a message box (tip or HINT), tap
        # CROSS until it closes.
        # tips [SETTLE_FRAMES] [TIMEOUT_FRAMES]: wait for a box, then keep
        # dismissing boxes until none opens for SETTLE frames (default 120).
        need_pad()
        if verb == "nexttip":
            dismiss_box(pad, int(rest[0]) if rest else 60 * 30)
        else:
            settle = int(rest[0]) if rest else 120
            limit = int(rest[1]) if len(rest) > 1 else 60 * 30
            n = 0
            dismiss_box(pad, limit)
            n += 1
            while wait_box(pad, settle):
                dismiss_box(pad, limit)
                n += 1
            print("  %d boxes dismissed" % n)
        print("  box mode %d (vsync %d)" % (pad.p.read32(BOX_MODE), pad.vsync()))
    elif verb == "goto":
        # goto X Z RADIUS [TIMEOUT_FRAMES] [EXTRA]: skate to (X, Z) on the
        # ground plane. Holds UP (forward) plus EXTRA buttons and steers with
        # LEFT/RIGHT from the heading measured as frame-to-frame motion of the
        # player position. EXTRA may include SHARP: hold R1 while steering right
        # and L1 while steering left (the game's sharp turn). Works fakie too
        # (UP and LEFT/RIGHT stay camera-relative). Releases all
        # buttons when within RADIUS. Closed loop, so it does not depend on
        # wall-clock latency; not expressible as a ZX_PAD_SCRIPT.
        # RADIUS may instead be a stop condition on the position, e.g. x>-16.
        x, z = float(rest[0]), float(rest[1])
        r = rest[2] if any(c in rest[2] for c in "<>") else float(rest[2])
        limit = int(rest[3]) if len(rest) > 3 else 60 * 30
        spec = rest[4].upper().split("+") if len(rest) > 4 else []
        sharp = "SHARP" in spec
        extra = parse_buttons("+".join(b for b in spec if b != "SHARP"))
        skate_to(need_pad(), x, z, r, limit, extra, sharp)
    elif verb == "chase":
        # chase ADDR BACK R [TIMEOUT] [GRAB] [UNTIL_Y]: skate behind a moving
        # object (float vec3 at ADDR, e.g. the golf cart node translation),
        # first BACK units behind it, then at its back holding GRAB (default
        # CIRCLE) within R. Ends with GRAB held once the player's y < UNTIL_Y
        # (default -0.5: lifted onto it). Closed loop, like goto.
        addr, back, r = parse_int(rest[0]), float(rest[1]), float(rest[2])
        limit = int(rest[3]) if len(rest) > 3 else 60 * 30
        grab = parse_buttons(rest[4]) if len(rest) > 4 else BUTTONS["CIRCLE"]
        until_y = float(rest[5]) if len(rest) > 5 else -0.5
        chase(need_pad(), addr, back, r, limit, grab, until_y)
    elif verb == "balance":
        # balance grind|manual FRAMES [KD] [EXTRA]: keep the skater balanced on a
        # rail (LEFT/RIGHT, player obj+0x280) or in a manual (UP/DOWN, obj+0x284)
        # for FRAMES, PD rule on the meter; EXTRA buttons stay held.
        kind, frames = rest[0].lower(), int(rest[1])
        kd = float(rest[2]) if len(rest) > 2 else 24.0
        extra = parse_buttons(rest[3]) if len(rest) > 3 else 0
        balance(need_pad(), kind, frames, kd, extra=extra)
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
        # as pcsx2-test-data/reference/NAME.p2s, and extract from it the EE RAM
        # (atomic, unlike `dump`) and PCSX2's 640x480 frame thumbnail.
        slot, name = int(rest[0]), rest[1]
        # Defaults can be redirected with PCSX2PAD_RAM_DIR / PCSX2PAD_PNG_DIR /
        # PCSX2PAD_STATE_DIR (e.g. for a second run that should not replace the first).
        ram_dir = rest[2] if len(rest) > 2 else os.environ.get(
            "PCSX2PAD_RAM_DIR", os.path.join(REPO, "recomp", "dumps", "ref"))
        png_dir = rest[3] if len(rest) > 3 else os.environ.get(
            "PCSX2PAD_PNG_DIR", os.path.join(REPO, "reference", "pcsx2-run1"))
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
                         "waitval ADDR TYPE OP VAL | pressuntil BTN ADDR TYPE OP VAL | nexttip | tips | "
                         "goto X Z R | chase ADDR BACK R | balance grind|manual N | waitnear ADDR X Z R | peekf ADDR [N] | checkpoint SLOT NAME | "
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
        # padscript LOG [FROM_VSYNC [TO_VSYNC]]: ZX_PAD_SCRIPT for a recorded run
        rest = args.command[1:]
        script, sticks = record_to_native(rest[0], int(rest[1]) if len(rest) > 1 else None,
                                          int(rest[2]) if len(rest) > 2 else None)
        print(script)
        for s in sticks:
            print("# not expressible natively: %s" % s, file=sys.stderr)
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
