#!/usr/bin/env python3
"""Level/mode sweep driver for the native build (uses nctl.py / PS2X_CONTROL).

    sweep.py NAME career K TAG     from the main menu (cursor on CAREER): career, first skater,
                                   level-select entry K, load, skate, pause, EXIT
    sweep.py NAME level K TAG [exit|levelselect]   from the level select screen
    sweep.py NAME skate TAG        the skate routine only (in a level)
Shots go to recomp/logs/sweep-TAG-*.png.
"""
import struct
import sys
import time

import nctl

MODE, PANEL, LEVEL, FRAME, POS = 0x276FE4, 0x240A20, 0x24C9B4, 0x231CAC, 0x2A76F0
Y_UNPLACED = 0x469C4000  # 20000.0


class Driver:
    def __init__(self, name):
        self.name = name

    def do(self, *commands, quiet=True):
        out = []
        nctl.send(self.name, list(commands), quiet=quiet, replies=out)
        return out

    def u32(self, addr):
        return int(self.do(f"peek {addr:#x}")[0].split()[1], 16)

    def f32(self, addr, n=3):
        return [float(x) for x in self.do(f"peekf {addr:#x} {n}")[0].split()[1:]]

    def until(self, cond, timeout=3600):
        r = self.do(f"until {cond} {timeout}")[0]
        return r.startswith("ok")

    def log(self, *a):
        print(time.strftime("%H:%M:%S"), *a, flush=True)

    def panel_cursor(self):
        p = self.u32(PANEL)
        return p, (self.u32(p + 0x114) if p else None)

    def new_panel(self, old, timeout=1200):
        """Wait for a new zgui panel that stays active for 40 vsyncs."""
        last = None
        for _ in range(timeout // 20):
            p = self.u32(PANEL)
            if p and p != old and p == last:
                return p
            last = p
            self.do("wait 20")
        return self.u32(PANEL)

    def press(self, button, n=1, gap=15):
        for _ in range(n):
            self.do(f"press {button} 6 {gap}")

    def shot(self, tag):
        self.do(f"shot sweep-{tag}.png")

    def set_cursor(self, k, inc="RIGHT", dec="LEFT", limit=20):
        for _ in range(limit):
            p, c = self.panel_cursor()
            if c == k:
                return True
            self.press(inc if c < k else dec, gap=25)
        return False

    def wait_placed(self, timeout=9000):
        """Wait for level play with the player placed; X through message boxes."""
        self.until(f"{MODE:#x}==2", 3600)
        self.log("level", self.u32(LEVEL), "loading")
        t = 0
        while t < timeout:
            mode = self.u32(MODE)
            y = self.u32(POS + 4)
            if mode == 2 and y != Y_UNPLACED and self.u32(FRAME) > 2:
                self.log("placed frame", self.u32(FRAME), "pos", self.f32(POS))
                return True
            if mode == 3:
                self.press("CROSS", gap=40)
                t += 46
            self.do("wait 60")
            t += 60
        self.log("NOT placed: mode", self.u32(MODE), "frame", self.u32(FRAME), "pos", self.f32(POS))
        return False

    def skate(self, tag):
        start = self.f32(POS)
        f0 = self.u32(FRAME)
        seq = ["down UP", "wait 120", "press CROSS 4 30", "press SQUARE 20 40", "wait 60",
               "hold LEFT 25", "wait 120", "press CROSS 4 20", "press TRIANGLE 40 30",
               f"shot sweep-{tag}-skate1.png", "hold RIGHT 40", "wait 150", "press CROSS 4 20",
               "press SQUARE 20 40", "hold L1 20", "wait 120", "up UP", "press CROSS 4 30",
               "down DOWN", "wait 40", "up DOWN", "down UP", "hold RIGHT 60", "wait 200",
               "press CROSS 4 10", "press CIRCLE 30 30", f"shot sweep-{tag}-skate2.png",
               "hold LEFT 50", "wait 200", "press CROSS 4 20", "press TRIANGLE 50 30",
               "hold R1 20", "wait 200", "press CROSS 20 20", "hold LEFT 30", "wait 200", "up UP",
               f"shot sweep-{tag}-skate3.png"]
        self.do(*seq)
        end = self.f32(POS)
        f1 = self.u32(FRAME)
        dist = sum((a - b) ** 2 for a, b in zip(start, end)) ** 0.5
        self.log(f"skated: frames {f0}->{f1}, pos {start} -> {end} (moved {dist:.1f}), mode {self.u32(MODE)}")
        return f1 > f0

    def pause_exit(self, tag, to="exit"):
        """Pause, then the last entry (EXIT) or the one before it (LEVEL SELECT)."""
        for _ in range(10):
            if self.u32(MODE) != 3:
                break
            self.press("CROSS", gap=40)
        self.press("START", gap=40)
        self.shot(f"{tag}-pause")
        last = -1
        for _ in range(40):
            self.press("DOWN", gap=10)
            c = self.panel_cursor()[1]
            if c == last:
                break
            last = c
        if to != "exit":
            self.press("UP", gap=20)
        self.log("pause panel", hex(self.u32(PANEL)), "cursor", self.panel_cursor()[1], "->", to)
        self.press("CROSS", gap=40)
        ok = self.until(f"{MODE:#x}==1", 3600)
        p = self.new_panel(0)
        self.shot(f"{tag}-exit")
        self.log("back in front end" if ok else "NOT back in front end", "panel", hex(p))
        return ok

    def level(self, k, tag, to="levelselect"):
        """From the level select screen: entry k, load, skate, pause, back."""
        self.set_cursor(k)
        self.shot(f"{tag}-select")
        self.log(tag, "level select cursor", self.panel_cursor()[1])
        self.press("CROSS", gap=30)
        self.do("wait 200")
        self.shot(f"{tag}-loading")
        if not self.wait_placed():
            self.shot(f"{tag}-stuck")
            return False
        self.do("wait 60")
        self.shot(f"{tag}-start")
        self.skate(tag)
        return self.pause_exit(tag, to)

    def career(self, k, tag, to="exit"):
        p0 = self.u32(PANEL)
        self.press("CROSS", gap=30)
        p1 = self.new_panel(p0)           # player select
        self.press("CROSS", gap=30)
        p2 = self.new_panel(p1)           # name entry
        self.press("CROSS", gap=30)
        p3 = self.new_panel(p2)           # level select
        self.set_cursor(k)
        self.shot(f"{tag}-select")
        self.log("level select", hex(p3), "cursor", self.panel_cursor()[1])
        self.press("CROSS", gap=30)
        self.do("wait 200")
        self.shot(f"{tag}-loading")
        if not self.wait_placed():
            self.shot(f"{tag}-stuck")
            return False
        self.do("wait 60")
        self.shot(f"{tag}-start")
        self.skate(tag)
        return self.pause_exit(tag, to)


def main(argv):
    d = Driver(argv[1])
    if argv[2] == "career":
        return 0 if d.career(int(argv[3]), argv[4]) else 1
    if argv[2] == "level":
        return 0 if d.level(int(argv[3]), argv[4], argv[5] if len(argv) > 5 else "levelselect") else 1
    if argv[2] == "skate":
        return 0 if d.skate(argv[3]) else 1
    print(__doc__)
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except nctl.RunnerGone:
        print("runner gone")
        sys.exit(2)
