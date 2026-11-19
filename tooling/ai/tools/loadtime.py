#!/usr/bin/env python3
"""Load times of the native build: boot and level loads, wall clock and vsyncs.

    RUNNER=<ps2EntryRunner> loadtime.py NAME [KEY=VAL ...]

Starts the runner (nctl, CPU renderer) on a fresh copy of the unlocked card
with a saved park (recomp/run/mc-sweep-park, copied to recomp/run/mc-NAME)
and measures:

    boot       launch -> main menu (the sweep's boot: START through the intros;
               LOADTIME_BOOT=mash: START every 20 vsyncs instead)
    tutorial   career, level select entry 0: CROSS -> first level frame
    movielot   pause > LEVEL SELECT, entry 1: CROSS -> first level frame
    civic      the same from Movie Lot, entry 2 (a level change)
    split      main menu > MULTIPLAYER (Most Points, Movie Lot): last CROSS -> first frame

"First level frame": game mode 0x276FE4 is 2 and the level frame counter
0x231CAC (0 during the loading screen) is past 2, or a message box is up
(mode 3). Level fly-throughs after it are not counted.
Polled every 3 vsyncs (LOADTIME_SHOTS=<s>: a screenshot every s seconds of a load). Results: recomp/logs/loadtime-NAME.json and a table.
KEY=VAL pairs go to the runner's environment (e.g. PS2X_IOP_WAKE=0).
"""
import json
import os
import shutil
import sys
import time

import nctl
import sweep

VSYNC = 0x231CA4


class LoadDriver(sweep.Driver):
    def vsync(self):
        return self.u32(VSYNC)

    def loaded(self):
        mode = self.u32(sweep.MODE)
        if os.environ.get("LOADTIME_TRACE"):
            self.log("trace mode", mode, "frame", self.u32(sweep.FRAME), "y", hex(self.u32(sweep.POS + 4)),
                     "vsync", self.vsync())
        if mode == 3:
            return True
        return mode == 2 and self.u32(sweep.FRAME) > 2

    def timed_cross(self, tag, timeout=60.0):
        """CROSS, then wait for the first level frame; returns (seconds, vsyncs)."""
        v0 = self.vsync()
        t0 = time.time()
        self.do("press CROSS 3 0")
        shots = float(os.environ.get("LOADTIME_SHOTS", "0"))
        next_shot = t0 + shots
        while time.time() - t0 < timeout:
            if self.loaded():
                break
            if shots and time.time() >= next_shot:
                self.shot(f"loadtime-{self.name}-{tag}-{time.time() - t0:04.1f}")
                next_shot += shots
            self.do("wait 3")
        secs, vs = time.time() - t0, self.vsync() - v0
        self.log(f"LOAD {tag}: {secs:.2f} s, {vs} vsyncs")
        self.shot(f"loadtime-{self.name}-{tag}")
        return round(secs, 2), vs

    def to_level_select(self, tag):
        self.do("wait 60")
        return self.pause_exit(tag, "levelselect")


def main(argv):
    name = argv[1]
    extra = argv[2:]
    run = os.path.join(nctl.RECOMP, "run")
    card = os.path.join(run, "mc-" + name)
    for d in (card, card + "_slot1"):
        shutil.rmtree(d, ignore_errors=True)
    shutil.copytree(os.path.join(run, "mc-sweep-park"), card)
    scratch = os.path.join(run, "scratch-" + name)
    os.makedirs(scratch, exist_ok=True)
    # PS2X_PAD_PORT1=mirror: `split` reaches MULTIPLAYER with one input
    # (without a second gamepad port 1 is empty and the game skips the icon).
    env = [f"PS2X_MC_ROOT={card}", f"PS2X_APP_SUPPORT_DIR={scratch}",
           f"PS2X_APP_LOG_DIR={scratch}", f"PS2X_CONTROLS_INI={scratch}/controls.ini",
           "PS2X_PAD_PORT1=mirror"] + extra
    d = LoadDriver(name)
    results = {}
    t0 = time.time()
    nctl.start(name, env)
    try:
        time.sleep(1.0)
        if os.environ.get("LOADTIME_BOOT") == "mash":
            # START every 20 vsyncs (skips each intro movie as soon as it
            # takes input) until the main menu panel is up.
            shots = float(os.environ.get("LOADTIME_SHOTS", "0"))
            next_shot = time.time() + shots
            while time.time() - t0 < 120:
                if d.u32(sweep.MODE) == 1 and d.u32(sweep.PANEL) != 0:
                    break
                if shots and time.time() >= next_shot:
                    d.shot(f"loadtime-{name}-boot-{time.time() - t0:04.1f}")
                    next_shot += shots
                d.do("press START 4 16")
        else:
            d.do("wait 400", "press START 10 30", "press START 10 30", "press START 10 30",
                 "until 0x276FE4==1 1200", "until 0x240A20!=0 1800")
        results["boot"] = (round(time.time() - t0, 2), d.vsync())
        d.log("LOAD boot:", results["boot"])
        if os.environ.get("LOADTIME_ONLY") == "boot":
            return 0
        d.do("wait 90")

        p0 = d.u32(sweep.PANEL)
        d.press("CROSS", gap=30)
        p1 = d.new_panel(p0)              # player select
        d.press("CROSS", gap=30)
        p2 = d.new_panel(p1)              # name entry
        d.press("CROSS", gap=30)
        d.new_panel(p2)                   # level select
        d.set_cursor(0)
        results["tutorial"] = d.timed_cross("tutorial")
        d.wait_placed()
        d.to_level_select("loadtime-tutorial")
        d.set_cursor(1)
        results["movielot"] = d.timed_cross("movielot")
        d.wait_placed()
        d.to_level_select("loadtime-movielot")
        d.set_cursor(2)
        results["civic"] = d.timed_cross("civic")
        d.wait_placed()
        d.do("wait 60")
        d.pause_exit("loadtime-civic", "exit")
        d.do("wait 60")

        for _ in range(3):
            d.press("RIGHT", gap=40)
        for i in range(7):
            if i == 4:
                d.press("RIGHT", gap=40)
            v0, t1 = d.vsync(), time.time()
            d.do("press CROSS 3 0", "wait 30")
            if d.u32(sweep.MODE) == 2:
                while time.time() - t1 < 60 and not d.loaded():
                    d.do("wait 3")
                results["split"] = (round(time.time() - t1, 2), d.vsync() - v0)
                d.log("LOAD split:", results["split"])
                d.shot(f"loadtime-{name}-split")
                break
            d.do("wait 60")
        d.wait_placed()
    finally:
        nctl.stop(name)
    out = os.path.join(nctl.LOGS, f"loadtime-{name}.json")
    with open(out, "w") as f:
        json.dump(results, f, indent=1)
    print("| load | seconds | vsyncs |")
    print("|---|---|---|")
    for k, (s, v) in results.items():
        print(f"| {k} | {s} | {v} |")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
