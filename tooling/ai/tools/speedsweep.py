#!/usr/bin/env python3
"""Speed sweep: guest vsyncs per second while skating, per level/mode (native build).

Like sweep.py (same navigation), but the skate routine is measured from the
runner's PS2X_PERF_LOG lines (start the runner with PS2X_PERF_LOG=2):

    speedsweep.py NAME career K TAG              main menu (cursor on CAREER) -> career level K, measure, back to level select
    speedsweep.py NAME level K TAG [exit|levelselect]   from the level select screen
    speedsweep.py NAME measure TAG [skate|SECONDS]       in a level: measure the skate routine, or SECONDS idle
    speedsweep.py NAME all PREFIX                the whole sweep from boot (card: mc-sweep-park)
    speedsweep.py NAME boot                      boot to the main menu
    speedsweep.py NAME mp TAG [skate]            main menu -> multiplayer split screen (Most Points), [measure]
                                                 (the runner started with PS2X_PAD_PORT1=mirror: without a
                                                 second gamepad port 1 is empty and the game skips the icon)
    speedsweep.py NAME report                    print the table of everything measured in this session

Results are appended to recomp/logs/fullspeed-speed-NAME.jsonl: vsyncs/s (mean, min),
game frames/s, CPU ms per vsync per thread (EE game thread, VIF1/VU1 worker, GS
worker, host, process), EE waits on the workers and the load average.
"""
import json
import os
import re
import statistics
import sys
import time

import nctl
import sweep

PERF_T = re.compile(r"\[perf\] t=\S+ host_fps=(\S+) vsync_hz=(\S+) game_fps=(\S+)")
PERF_CPU = re.compile(r"\[perf\] cpu ms/vsync: (.*)")
PERF_WAITS = re.compile(r"\[perf\] ee waits: (.*)")
VU1 = re.compile(r"\[vu1\] micro memory (\S+): (.*)")


def loadavg():
    # Not on Windows.
    return os.getloadavg() if hasattr(os, "getloadavg") else (0.0, 0.0, 0.0)


def kv(text):
    return {k: float(v) for k, v in (p.split("=") for p in text.split())}


def log_path(name):
    return os.path.join(nctl.LOGS, name + ".log")


def results_path(name):
    return os.path.join(nctl.LOGS, f"fullspeed-speed-{name}.jsonl")


def summarize(text):
    hz, fps, cpu, waits = [], [], [], []
    for line in text.splitlines():
        if m := PERF_T.search(line):
            hz.append(float(m.group(2)))
            fps.append(float(m.group(3)))
        elif m := PERF_CPU.search(line):
            cpu.append(kv(m.group(1)))
        elif m := PERF_WAITS.search(line):
            waits.append(kv(m.group(1)))
    # The first interval started before the measurement.
    hz, fps, cpu, waits = hz[1:], fps[1:], cpu[1:], waits[1:]
    out = {"samples": len(hz)}
    if hz:
        out.update(vsync_mean=round(statistics.mean(hz), 1), vsync_min=min(hz), vsync_max=max(hz),
                   game_fps=round(statistics.mean(fps), 1))
    if cpu:
        out["cpu"] = {k: round(statistics.mean(c[k] for c in cpu), 2) for k in cpu[0]}
        out["cpu_max"] = {k: round(max(c[k] for c in cpu), 2) for k in cpu[0]}
    if waits:
        out["waits_per_s"] = {k: round(sum(w[k] for w in waits) / (2.0 * len(waits)), 1) for k in waits[0]}
    return out


class SpeedDriver(sweep.Driver):
    def measure(self, tag, body):
        log = log_path(self.name)
        off = os.path.getsize(log)
        load0 = loadavg()
        t0 = time.time()
        f0 = self.u32(sweep.FRAME)
        body()
        f1 = self.u32(sweep.FRAME)
        secs = time.time() - t0
        with open(log, errors="replace") as f:
            f.seek(off)
            text = f.read()
        r = {"tag": tag, "level": self.u32(sweep.LEVEL), "wall_s": round(secs, 1),
             "level_frames": f1 - f0, "load": [round(x, 1) for x in load0 + loadavg()]}
        r.update(summarize(text))
        r["vu1"] = VU1.findall(text)
        with open(results_path(self.name), "a") as f:
            f.write(json.dumps(r) + "\n")
        self.log("MEASURED", json.dumps(r))
        return r

    def boot(self):
        """Boot to the main menu (cursor on CAREER)."""
        self.do("wait 400", "press START 10 30", "press START 10 30", "press START 10 30",
                "until 0x276FE4==1 1200", "until 0x240A20!=0 1800", "wait 90")

    def multiplayer(self, tag):
        """Main menu (cursor on CAREER) -> MULTIPLAYER with the default game (Most Points,
        Movie Lot), P2 on the next skater; returns once the players are placed."""
        for _ in range(3):
            self.press("RIGHT", gap=40)
        p = self.u32(sweep.PANEL)
        for i in range(7):
            if i == 4:
                self.press("RIGHT", gap=40)
            self.press("CROSS", gap=60)
            self.do("wait 30")
            self.shot(f"{tag}-setup{i}")
            if self.u32(sweep.MODE) == 2:
                break
        ok = self.wait_placed()
        self.do("wait 60")
        self.shot(f"{tag}-start")
        return ok

    def enter(self, presses):
        """CROSS through `presses` new zgui panels (e.g. player select, level select)."""
        p = self.u32(sweep.PANEL)
        for _ in range(presses):
            self.press("CROSS", gap=30)
            p = self.new_panel(p)

    def menu(self, index):
        """Main menu (cursor on CAREER after boot and after leaving a mode): entry index."""
        for _ in range(index):
            self.press("RIGHT", gap=40)

    def play(self, tag, to="exit"):
        """Loading -> placed -> measured skate -> pause, leave."""
        if not self.wait_placed():
            self.shot(f"{tag}-stuck")
            return False
        self.do("wait 60")
        self.shot(f"{tag}-start")
        self.skate(tag)
        return self.pause_exit(tag, to)

    def load_park(self):
        """Load park dialog: slot 1, the first park, YES."""
        self.do("press CROSS 6 60", "wait 120", "press CROSS 6 60", "wait 60", "press UP 6 20",
                "press CROSS 6 60")

    def sweep_all(self, prefix):
        """Every career level, Freeskate (Civic Center, Editor Level with the card's
        park), Timed Run (Boardwalk), split-screen Most Points and Egg Hunt (Movie
        Lot), Park Editor (load the park, edit, Test Run). Needs the unlocked
        card with a saved park (recomp/run/mc-sweep-park)."""
        levels = ["tutorial", "movielot", "civic", "industrial", "boardwalk", "cannery", "airfield", "museum"]
        self.boot()
        self.career(0, f"{prefix}-{levels[0]}", "levelselect")
        for k in range(1, 8):
            self.level(k, f"{prefix}-{levels[k]}", "exit" if k == 7 else "levelselect")
        self.do("wait 60")
        self.menu(1)                      # FREESKATE
        self.enter(2)
        self.level(1, f"{prefix}-freeskate-civic", "levelselect")
        self.set_cursor(7)                # Editor Level
        self.press("CROSS", gap=60)
        self.do("wait 300")
        self.load_park()
        self.play(f"{prefix}-freeskate-editorlevel")
        self.do("wait 60")
        self.menu(2)                      # TIMED RUN
        self.enter(2)
        self.level(3, f"{prefix}-timedrun-boardwalk", "exit")
        self.do("wait 60")
        self.multiplayer(f"{prefix}-mp")
        self.skate(f"{prefix}-mp-mostpoints")
        self.do("until 0x276FE4!=2 9000", "wait 60", "press CROSS 6 90", "wait 30",
                "press DOWN 6 30", "press DOWN 6 30", "press CROSS 6 60", "wait 300",
                "press CROSS 6 60", "wait 120")
        self.shot(f"{prefix}-mp-egghunt-start")
        self.skate(f"{prefix}-mp-egghunt")
        self.pause_exit(f"{prefix}-mp-egghunt", "exit")
        self.do("wait 60")
        self.menu(4)                      # PARK EDITOR
        self.do("press CROSS 6 30", "wait 300", "press CROSS 6 30", "press CROSS 6 30", "wait 300",
                "press DOWN 6 30", "press CROSS 6 60", "wait 120")
        self.load_park()
        self.do("wait 720", "press CROSS 6 60", "wait 60", "press CIRCLE 6 30")
        self.shot(f"{prefix}-parkeditor")

        def edit():
            for _ in range(3):
                self.do("hold RIGHT 60", "hold DOWN 60", "hold LEFT 60", "hold UP 60",
                        "stick LX 1 120", "stick RY -1 120", "wait 120")
        self.measure(f"{prefix}-parkeditor", edit)
        self.do("press START 6 60", "wait 30", "press DOWN 6 20", "press CROSS 6 60", "wait 180")
        self.shot(f"{prefix}-parkeditor-testrun")
        self.skate(f"{prefix}-parkeditor-testrun")
        return True

    def skate(self, tag):
        ok = []
        self.measure(tag, lambda: ok.append(sweep.Driver.skate(self, tag)))
        return ok[0]


def report(name):
    print("| tag | level | vsyncs/s mean (min) | game fps | EE | VIF1/VU1 | GS | host | process | load |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    with open(results_path(name)) as f:
        for line in f:
            r = json.loads(line)
            c = r.get("cpu", {})
            print(f"| {r['tag']} | {r['level']} | {r.get('vsync_mean')} ({r.get('vsync_min')}) | {r.get('game_fps')} | "
                  f"{c.get('game')} | {c.get('vif1')} | {c.get('gs')} | {c.get('host')} | {c.get('process')} | "
                  f"{r['load'][0]}-{r['load'][3]} |")


def main(argv):
    if argv[2] == "report":
        report(argv[1])
        return 0
    d = SpeedDriver(argv[1])
    if argv[2] == "career":
        return 0 if d.career(int(argv[3]), argv[4], "levelselect") else 1
    if argv[2] == "level":
        return 0 if d.level(int(argv[3]), argv[4], argv[5] if len(argv) > 5 else "levelselect") else 1
    if argv[2] == "all":
        return 0 if d.sweep_all(argv[3]) else 1
    if argv[2] == "boot":
        d.boot()
        return 0
    if argv[2] == "mp":
        if not d.multiplayer(argv[3]):
            return 1
        d.log("tick", d.do("until 0x276FE4==2 1")[0])
        if len(argv) > 4 and argv[4] == "skate":
            d.skate(argv[3])
        return 0
    if argv[2] == "measure":
        what = argv[4] if len(argv) > 4 else "skate"
        if what == "skate":
            d.skate(argv[3])
        else:
            d.measure(argv[3], lambda: time.sleep(float(what)))
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except nctl.RunnerGone:
        print("runner gone")
        sys.exit(2)
