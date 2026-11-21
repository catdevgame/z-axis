#!/usr/bin/env python3
"""Replay a recorded input sequence inside PCSX2 and record per-frame state.

pcsx2pad.py drives the game closed loop over PINE, so its inputs land on
whatever vsync PINE gets them in. For differential debugging against the
native build we want the opposite: the exact recorded input
(scripts/native/ref-run1-input.txt) replayed on exactly the vsyncs it says,
and the game state of every frame. pcsx2replay_hook.s does that inside the
emulated EE: the input schedule and a ring buffer of per-poll records live
in EE RAM, the hook (on the game's only scePadRead call) applies the
schedule and logs, and it can freeze the game at a vsync so a RAM image of
exactly that frame can be read.

Only for a private PCSX2 data dir of your own (PCSX2REPLAY_DATA, a copy of
tools/pcsx2-test-data with its own PINESlot; never the baseline or the
shared test dir), with PCSX2REPLAY_SLOT set to that PINE slot.

  pcsx2replay.py install                       # assemble, write the pnach into $PCSX2REPLAY_DATA
  pcsx2replay.py launch [--card FILE]          # start PCSX2 on that data dir (pcsx2pad.py launch)
  pcsx2replay.py run STATE TO OUT [options]    # load savestate slot STATE, replay to vsync TO
                                               # (STATE "boot": from a fresh launch, vsync 0)
      --watch ADDR:N[,ADDR:N...]               # words logged every poll (default: frame, pos, ctrl)
      --dump V[,V...]                          # freeze at each vsync V and write OUT-V.ram
      --delay D                                # schedule vsync = recorded vsync + D (default 1)
      --input FILE                             # PCSX2PAD_RECORD log (default ref-run1-input.txt)
  pcsx2replay.py quit

Loading a savestate perturbs the game: the first frame after the load takes
two vsyncs, the game's adaptive frame limiter (func_00185DA0) then holds 30
fps for 0.5 s, and the level frame counter ends up 16 frames behind the
original run. So per-frame comparisons replay from a fresh boot ("boot",
with the memory card the reference started with, see launch --card).

A freeze perturbs what follows: VBlanks keep coming while the EE spins.
The host writes the vsync counter back before releasing, but that is not
enough (in a test the next frames took two vsyncs and a message box closed
early), so only the first --dump of a run is exact; use one run per image.

The log OUT.log has one line per poll: "vsync polls w0 w1 ..." (hex words).
A record is taken at the game's pad poll, after the poll; natively the same
point is the scePadRead HLE (ZX_PAD_LOG / ZX_PAD_DUMP in the runtime), and
`logiccheck.py frames` compares the two logs per level frame.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.environ.get("PCSX2REPLAY_DATA")
SLOT = int(os.environ.get("PCSX2REPLAY_SLOT", "0"))
if not DATA_DIR or not SLOT:
    sys.exit("set PCSX2REPLAY_DATA (your own copy of pcsx2-test-data) and PCSX2REPLAY_SLOT (its PINESlot)")
DATA_DIR = os.path.abspath(DATA_DIR)
if os.path.basename(DATA_DIR) == "pcsx2-test-data" or "baseline" in DATA_DIR:
    sys.exit("refusing the shared test data dir / baseline: use a copy of your own")
os.environ["PCSX2PAD_DATA"] = DATA_DIR
os.environ["PCSX2PAD_SLOT"] = str(SLOT)
sys.path.insert(0, HERE)
import build_hook  # noqa: E402
import pcsx2pad  # noqa: E402
from pine import Pine  # noqa: E402

HOOK_SRC = os.path.join(HERE, "pcsx2replay_hook.s")
INPUT_LOG = os.path.join(HERE, "scripts", "native", "ref-run1-input.txt")
CAVE = 0x000F8000
CTRL = 0x000F6800
SCHED = 0x00096000
SCHED_END = 0x000A0000
LOG = 0x000A0000
LOG_END = 0x000F6000
MAGIC = 0x594C5052
VSYNC = 0x00231CA4
WATCH_MAX = 48
DEFAULT_WATCH = "0x231CAC:1,0x2A76F0:3,0x27E0C4:1"


def assemble():
    with tempfile.TemporaryDirectory() as tmp:
        obj, elf, binf = (os.path.join(tmp, n) for n in ("h.o", "h.elf", "h.bin"))
        B = build_hook.BINUTILS
        subprocess.run([B + "as", "-EL", "-march=r5900", "-mabi=eabi", "-o", obj, HOOK_SRC], check=True)
        subprocess.run([B + "ld", "-EL", "-Ttext", "0x%08X" % CAVE, "-e", "replay_hook", "-o", elf, obj], check=True)
        subprocess.run([B + "objcopy", "-O", "binary", "-j", ".text", elf, binf], check=True)
        dis = subprocess.run([B + "objdump", "-d", "-mmips:5900", elf], check=True,
                             capture_output=True, text=True).stdout
        code = open(binf, "rb").read()
    return code + b"\0" * (-len(code) % 4), dis


def install(show=False):
    code, dis = assemble()
    if show:
        print(dis)
    text = build_hook.make_pnach(code).replace("pcsx2pad input-injection hook", "pcsx2replay replay/record hook")
    dest = os.path.join(DATA_DIR, "PCSX2", "patches", build_hook.PNACH_NAME)
    open(dest, "w").write(text)
    print("installed %s (%d hook bytes)" % (dest, len(code)))


def read_schedule(path, delay):
    """[(vsync, buttons)] from a PCSX2PAD_RECORD log, shifted by delay."""
    out = []
    for line in open(path):
        if line.startswith("#"):
            continue
        v, kind, val = line.split()
        if kind == "B":
            out.append((int(v) + delay, int(val, 16)))
    return out


def parse_watch(spec):
    out = []
    for item in spec.split(","):
        a, _, n = item.partition(":")
        out.append((int(a, 0), int(n or "1", 0)))
    return out


def write_words(p, addr, words):
    body = b"".join(struct.pack("<BII", 6, addr + 4 * i, w) for i, w in enumerate(words))
    for i in range(0, len(body), 9 * 6000):
        p.transact(body[i:i + 9 * 6000])


def state_vsync(slot):
    if slot == "boot":
        return 0
    """vsync stored in a savestate slot's RAM image."""
    import zipfile
    path = os.path.join(DATA_DIR, "PCSX2", "sstates", "SLUS-20327 (67DB3ED8).%02d.p2s" % int(slot))
    with zipfile.ZipFile(path) as z:
        with z.open("eeMemory.bin") as f:
            ram = f.read(VSYNC + 4)
    return struct.unpack_from("<I", ram, VSYNC)[0]


def run(a):
    watch = parse_watch(a.watch)
    words = sum(n for _, n in watch)
    if len(watch) > WATCH_MAX:
        sys.exit("too many watch entries")
    recsize = 8 + 4 * words
    cap = (LOG_END - LOG) // recsize
    start = state_vsync(a.state)
    sched = [(v, m) for v, m in read_schedule(a.input, a.delay)]
    held = 0
    for v, m in sched:
        if v <= start:
            held = m
    sched = [(start, held)] + [(v, m) for v, m in sched if start < v <= a.to + 1]
    if 8 * len(sched) > SCHED_END - SCHED:
        sys.exit("schedule too long")
    dumps = sorted(int(x, 0) for x in a.dump.split(",")) if a.dump else []
    p = Pine(SLOT)
    print("state %s at vsync %d, %d schedule entries, record %d bytes, ring %d records" % (
        a.state, start, len(sched), recsize, cap))
    if a.state == "boot":
        if p.read32(VSYNC) >= sched[1][0] - 2:
            sys.exit("not a fresh boot (vsync %d)" % p.read32(VSYNC))
    else:
        p.load_state(int(a.state))
    # Arm as soon as the loaded state runs (the first input is normally tens of
    # vsyncs after a checkpoint; refuse if it was too close).
    t0 = time.time()
    while True:
        v = p.read32(VSYNC)
        if start <= v < start + 120:
            break
        if time.time() - t0 > 20:
            sys.exit("state did not load (vsync %d)" % v)
        time.sleep(0.005)
    write_words(p, CTRL, [0] * 0x10)
    write_words(p, SCHED, [x for e in sched for x in e])
    write_words(p, CTRL + 0x40, [x for e in watch for x in e])
    write_words(p, CTRL + 0x04, [len(sched), 0, held, 0, cap, recsize, dumps[0] if dumps else 0, 0, len(watch), 0])
    write_words(p, CTRL, [MAGIC])
    armed = p.read32(VSYNC)
    print("armed at vsync %d" % armed)
    if len(sched) > 1 and sched[1][0] <= armed + 1:
        print("WARNING: first scheduled input at %d is not after arming" % sched[1][0])
    log = open(a.out + ".log", "w")
    log.write("# state %s start %d armed %d delay %d watch %s\n" % (a.state, start, armed, a.delay, a.watch))
    done = 0
    last_v = 0
    while True:
        total = p.read32(CTRL + 0x10)
        if total - done > cap:
            print("WARNING: ring overrun, lost %d records" % (total - done - cap))
            done = total - cap
        while done < total:
            n = min(total - done, cap - done % cap, 2000)
            raw = p.read_block(LOG + (done % cap) * recsize, n * recsize)
            for i in range(n):
                rec = struct.unpack_from("<%dI" % (2 + words), raw, i * recsize)
                log.write(" ".join("%x" % x for x in rec) + "\n")
                last_v = rec[0]
            done += n
        frozen = p.read32(CTRL + 0x20)
        if dumps and frozen and frozen >= dumps[0]:
            path = "%s-%d.ram" % (a.out, dumps[0])
            t = time.time()
            ram = p.read_block(0, 32 * 1024 * 1024)
            open(path, "wb").write(ram)
            print("froze at vsync %d (wanted %d): %s (%.1f s)" % (frozen, dumps[0], path, time.time() - t))
            dumps.pop(0)
            p.write32(CTRL + 0x20, 0)
            p.write32(VSYNC, frozen)
            p.write32(CTRL + 0x1C, dumps[0] if dumps else 0)
            continue
        if last_v >= a.to and not dumps:
            break
        time.sleep(0.1)
    log.close()
    print("logged %d records up to vsync %d: %s.log" % (done, last_v, a.out))
    if a.stop:
        # Leave the game frozen at the next poll so later state does not move.
        p.write32(CTRL + 0x1C, 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="verb", required=True)
    s = sub.add_parser("install")
    s.add_argument("--disasm", action="store_true")
    s = sub.add_parser("launch")
    s.add_argument("--card", default="Mcd001.ps2", help="slot-1 card in PCSX2/memcards")
    sub.add_parser("quit")
    s = sub.add_parser("run")
    s.add_argument("state", help="savestate slot, or boot")
    s.add_argument("to", type=int)
    s.add_argument("out")
    s.add_argument("--watch", default=DEFAULT_WATCH)
    s.add_argument("--dump", default="")
    s.add_argument("--delay", type=int, default=1)
    s.add_argument("--input", default=INPUT_LOG)
    s.add_argument("--stop", action="store_true", help="freeze the game when done")
    a = ap.parse_args()
    if a.verb == "install":
        install(a.disasm)
    elif a.verb == "launch":
        pcsx2pad.launch(card=a.card)
    elif a.verb == "quit":
        pcsx2pad.quit_pcsx2()
    elif a.verb == "run":
        run(a)


if __name__ == "__main__":
    main()
