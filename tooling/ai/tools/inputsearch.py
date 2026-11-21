#!/usr/bin/env python3
"""Find recorded input edges the reference run's game saw on another vsync.

The PCSX2PAD_RECORD log says when pcsx2pad.py wrote an input over PINE; the
game normally reads it at the next pad read, but now and then a write lands
before the read of the same frame (or after the next). Natively the replay
is bit-identical to PCSX2 replaying the same input, so a checkpoint that
differs from the reference while PCSX2's own replay differs the same way
points at such an edge. This moves each edge of a window by the given
shifts, one candidate at a time, replays natively in parallel (logiccheck.py
run, dumping only the checkpoint) and scores the player position and yaw
against the reference RAM image. A shift that scores 0.0 is the candidate;
confirm it with tools/pcsx2replay.py (--input) before adopting it.

  RUNNER=<zx-runner> [SEARCH_BASE=<input log>] [SEARCH_SHIFTS=-1,1] \
    python3 tools/inputsearch.py FROM_VSYNC TO_VSYNC CHECKPOINT PARALLEL SECS

Candidate logs go to recomp/logs/inputsearch/; their dumps and run logs are
deleted after scoring.
"""
import os, subprocess, sys, struct, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import logiccheck as L
SRC = os.environ.get("SEARCH_BASE", L.INPUT_LOG)
SHIFTS = [int(x) for x in os.environ.get("SEARCH_SHIFTS", "-1,1").split(",")]
OUT = os.path.join(ROOT, "recomp/logs/inputsearch")

def load():
    head, rows = [], []
    for line in open(SRC):
        if line.startswith("#"):
            head.append(line); continue
        v, k, val = line.split()
        rows.append([int(v), k, val])
    return head, rows

def candidates(lo, hi):
    head, rows = load()
    idx = [i for i, r in enumerate(rows) if lo <= r[0] <= hi and r[1] == "B"]
    out = []
    for i in idx:
        for d in SHIFTS:
            new = [list(r) for r in rows]
            new[i][0] += d
            # keep order; an edge moved onto its neighbour's vsync replaces it
            prev_v = new[i - 1][0] if i > 0 else -1
            next_v = new[i + 1][0] if i + 1 < len(new) else 1 << 30
            if new[i][0] < prev_v or new[i][0] > next_v:
                continue
            if new[i][0] == prev_v:
                del new[i - 1]
            elif new[i][0] == next_v:
                del new[i]   # the later edge wins at that vsync
            out.append(("e%d%+d" % (rows[i][0], d), head, new))
    return out

def write(name, head, rows):
    p = os.path.join(OUT, name + ".txt")
    with open(p, "w") as f:
        f.writelines(head)
        for v, k, val in rows:
            f.write("%d %s %s\n" % (v, k, val))
    return p

def score(name, ck):
    rp = os.path.join(L.REF_RAM, ck + ".ram")
    np_ = os.path.join(L.NATIVE_RAM, "%s-%s.ram" % (name, ck))
    if not os.path.exists(np_):
        return None
    r = L.state(open(rp, "rb").read(), False); n = L.state(open(np_, "rb").read(), True)
    return L.dist(r["pos"], n["pos"]), r["yaw"] - n["yaw"], r["mode"] == n["mode"] and r["panel"] == n["panel"]

if __name__ == "__main__":
    if len(sys.argv) != 6 or int(sys.argv[4]) < 1:
        raise SystemExit(__doc__)
    os.makedirs(OUT, exist_ok=True)
    lo, hi, ck, par, secs = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
    cands = candidates(lo, hi)
    print("%d candidates" % len(cands), flush=True)
    env = dict(os.environ)
    env.update(ZX_GS_NULL="1", ZX_GS_BACKEND="cpu", ZX_VU1_SKIP="1", LOGICCHECK_DUMPS=ck, ZX_STATE_LOG="0x231CAC")
    env.pop("ZX_PAD_LOG", None)
    queue = list(cands)
    running = []
    results = []
    while queue or running:
        while queue and len(running) < par:
            tag, head, rows = queue.pop(0)
            name = "search-" + tag
            e = dict(env, LOGICCHECK_INPUT=write(name, head, rows))
            p = subprocess.Popen(["python3", os.path.join(ROOT, "tools/logiccheck.py"), "run", name, str(secs)],
                                 env=e, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            running.append((name, p))
        time.sleep(2)
        for name, p in list(running):
            if p.poll() is not None:
                running.remove((name, p))
                s = score(name, ck)
                results.append((name, s))
                for f in os.listdir(L.NATIVE_RAM):
                    if f.startswith(name + "-"):
                        os.remove(os.path.join(L.NATIVE_RAM, f))
                log = os.path.join(L.LOGS, name + ".log")
                if os.path.exists(log):
                    os.remove(log)
                print(name, s, flush=True)
    results = [r for r in results if r[1]]
    results.sort(key=lambda r: r[1][0])
    print("best:", results[:5])
