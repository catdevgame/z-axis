#!/usr/bin/env python3
"""Frame-time spikes from a frame timeline trace (PS2Recomp runtime/ps2_frame_trace.h).

    framespikes.py FILE [N]

Record with the control channel (PS2X_CONTROL, tools/nctl.py): "ftrace start", then "ftrace stop FILE"
(at most 4 M events; a 108 s ride is about 2 M). Prints:
  - per game frame (the EE's buffer swap to the next; for games that write DISPFB from the EE, those
    writes): wall ms, vsyncs, EE swap wait and drain, VU1 and GS worker busy, the window thread's longest
    frame; the frame-time distribution (median, p99, p99.9, max, frames over 20 / 34 / 50 ms);
  - game frames never presented (a buffer swap whose picture the window thread never latched: a frame
    the player does not see) and the window thread's frame gaps and swapchain acquires;
  - totals per thread and kind: GPU renderer work (pipeline and texture creation, by site, texture
    decodes, target loads and write-backs, replacement uploads, CPU draws, mip copies, buffer creation,
    passes, submits, fence waits), the EE's disc opens and reads, file writes, idle waits;
  - the N slowest frames and the N with the most GS work, with what each thread did in them.
tools/ftrace.py reads the same file (its per-frame table and timeline).
"""
import struct
import sys

BASE = ["", "taskB", "taskE", "drainB", "drainE", "swapWaitB", "swapWaitE", "swapPosted", "swapRun", "vsync",
        "hostB", "hostE", "claimB", "claimE", "submitB", "submitE", "passB", "passE", "vifPost", "latchB",
        "latchE", "u1", "u2", "u3", "u4", "u5", "u6", "u7", "u8",
        "pipe", "decode", "rtload", "rtdown", "repl", "cpudraw", "mipcopy", "bufcreate", "dispWrite", "flipSeen", "discOpen", "discRead", "fileWrite", "eeIdle"]
K = {n: i for i, n in enumerate(BASE) if n}
SPANS = {"u1": "download/acquire", "u2": "fence wait", "u3": "acquire cmd", "u4": "backend lock",
         "u5": "create texture", "u6": "upload map+copy", "u7": "materialize", "u8": "gather",
         "pipe": "pipeline create", "decode": "texture decode", "rtload": "target load", "rtdown": "target download",
         "repl": "replacement upload", "cpudraw": "CPU draw", "mipcopy": "mip copy", "bufcreate": "buffer create",
         "passB": "pass record", "submitB": "submit", "discOpen": "disc open", "discRead": "disc read",
         "fileWrite": "file write", "eeIdle": "EE idle wait"}


def end_of(name):
    if name == "passB":
        return K["passE"]
    if name == "submitB":
        return K["submitE"]
    return K[name] + 100


def load(path):
    data = open(path, "rb").read()
    ev = [struct.unpack_from("<qIHH", data, o) for o in range(0, len(data) - 15, 16)]
    ev.sort(key=lambda e: e[0])
    return ev


_spans = None


def _build(ev):
    # One pass: every (thread, begin kind) -> outermost spans, for every begin/end pair below.
    global _spans
    pairs = {K["taskB"]: K["taskE"], K["drainB"]: K["drainE"], K["swapWaitB"]: K["swapWaitE"], K["hostB"]: K["hostE"],
             K["claimB"]: K["claimE"], K["submitB"]: K["submitE"], K["passB"]: K["passE"], K["latchB"]: K["latchE"]}
    for n in ("u1", "u2", "u3", "u4", "u5", "u6", "u7", "u8", "pipe", "decode", "rtload", "rtdown", "repl", "cpudraw",
              "mipcopy", "bufcreate", "discOpen", "discRead", "fileWrite", "eeIdle"):
        pairs[K[n]] = K[n] + 100
    ends = {v: k for k, v in pairs.items()}
    _spans, state = {}, {}
    for ns, t, kind, a in ev:
        if kind in pairs:
            st = state.setdefault((t, kind), [0, 0, 0])
            if st[0] == 0:
                st[1], st[2] = ns, a
            st[0] += 1
        elif kind in ends:
            st = state.get((t, ends[kind]))
            if st and st[0] > 0:
                st[0] -= 1
                if st[0] == 0:
                    _spans.setdefault((t, ends[kind]), []).append((st[1], ns, st[2]))


def intervals(ev, th, b, e):
    if _spans is None:
        _build(ev)
    return _spans.get((th, b), [])


def clip(spans, lo, hi):
    return sum(max(0, min(e, hi) - max(s, lo)) for s, e, *_ in inside(spans, lo, hi))


_starts = {}


def inside(spans, lo, hi):
    # Spans are sorted and do not nest (outermost only): bisect on the starts.
    import bisect
    key = id(spans)
    st = _starts.get(key)
    if st is None or len(st) != len(spans):
        st = _starts[key] = [s for s, e, a in spans]
    j = max(0, bisect.bisect_left(st, lo) - 1)
    out = []
    while j < len(spans) and spans[j][0] < hi:
        s, e, a = spans[j]
        if e > lo:
            out.append(spans[j])
        j += 1
    return out


def main(argv):
    path = argv[1]
    n = int(argv[2]) if len(argv) > 2 and not argv[2].startswith("-") else 12
    ev = load(path)
    names = {}
    for ns, th, kind, arg in ev:
        if kind == K["taskB"]:
            names.setdefault(th, "VU1" if arg == 1 else "GS")
        elif kind in (K["swapPosted"], K["vsync"]):
            names.setdefault(th, "EE")
        elif kind == K["hostB"]:
            names.setdefault(th, "HOST")
    by = {v: k for k, v in names.items()}
    ee, vu, gs, host = by.get("EE"), by.get("VU1"), by.get("GS"), by.get("HOST")
    t0 = ev[0][0]
    swaps = [ns for ns, t, k, a in ev if k == K["swapPosted"]]
    # DM2 and other games that write DISPFB from the EE: those writes (a new FBP) are the frames.
    dw, last = [], None
    for ns, t, k, a in ev:
        if k == K["dispWrite"]:
            if a != last:
                dw.append(ns)
            last = a
    if not swaps and dw:
        swaps = dw
        print("frames: the EE's DISPFB writes")
    vs = [ns for ns, t, k, a in ev if k == K["vsync"]]
    vuT = intervals(ev, vu, K["taskB"], K["taskE"]) if vu else []
    gsT = intervals(ev, gs, K["taskB"], K["taskE"]) if gs else []
    eeW = intervals(ev, ee, K["swapWaitB"], K["swapWaitE"]) if ee else []
    eeD = intervals(ev, ee, K["drainB"], K["drainE"]) if ee else []
    hostF = intervals(ev, host, K["hostB"], K["hostE"]) if host else []
    spans = {u: {t: intervals(ev, t, K[u], end_of(u)) for t in names} for u in SPANS}
    print(f"threads {names}; {len(ev)} events over {(ev[-1][0] - t0) / 1e6:.0f} ms; {len(swaps)} swaps, {len(vs)} vsyncs")
    rows = []
    for i in range(len(swaps) - 1):
        lo, hi = swaps[i], swaps[i + 1]
        hosts = [e - s for s, e, _ in inside(hostF, lo, hi)]
        rows.append(dict(i=i, lo=lo, hi=hi, ms=(hi - lo) / 1e6, vs=sum(1 for v in vs if lo <= v < hi),
                         eew=clip(eeW, lo, hi) / 1e6, eed=clip(eeD, lo, hi) / 1e6, vu=clip(vuT, lo, hi) / 1e6,
                         gs=clip(gsT, lo, hi) / 1e6, hostmax=max(hosts) / 1e6 if hosts else 0.0))
    if not rows:
        return 1
    ms = sorted(r["ms"] for r in rows)
    pct = lambda p: ms[min(len(ms) - 1, int(p * len(ms)))]
    print(f"frame ms: median {pct(0.5):.2f} p99 {pct(0.99):.2f} p99.9 {pct(0.999):.2f} max {ms[-1]:.2f}; "
          f"frames > 20 ms {sum(1 for x in ms if x > 20)}, > 34 ms {sum(1 for x in ms if x > 34)}, > 50 ms {sum(1 for x in ms if x > 50)}")
    gsb = sorted(r["gs"] for r in rows)
    print(f"GS busy per frame: median {gsb[len(gsb) // 2]:.2f} max {gsb[-1]:.2f}; frames GS > 8 ms {sum(1 for x in gsb if x > 8)}")
    # Run totals per kind and thread.
    print("totals (thread kind: count, ms, max ms):")
    for u, label in SPANS.items():
        for t, nm in names.items():
            sp = spans[u][t]
            if sp:
                print(f"  {nm:4s} {label:20s} {len(sp):7d} {sum(e - s for s, e, _ in sp) / 1e6:9.1f} {max(e - s for s, e, _ in sp) / 1e6:8.2f}")
    # Presented frames: a GS-run buffer swap whose picture the window thread never latched (no latch
    # between it and the next swap run) is a game frame the player does not see.
    runs = [ns for ns, t, k, a in ev if k == K["swapRun"]] or dw
    seen = sum(1 for ns, t, k, a in ev if k == K["flipSeen"])
    print(f"display buffer changes seen by the window: {seen}")
    latchT = [s for s, e, _ in intervals(ev, host, K["latchB"], K["latchE"])] if host else []
    import bisect
    unseen = []
    for i in range(len(runs) - 1):
        j = bisect.bisect_left(latchT, runs[i])
        if j >= len(latchT) or latchT[j] >= runs[i + 1]:
            unseen.append(runs[i])
    hb = [s for s, e, _ in hostF]
    gaps = sorted(((b - a) / 1e6, a) for a, b in zip(hb, hb[1:]))
    acq = sorted((e - s) / 1e6 for s, e, _ in spans["u1"].get(host, [])) if host else []
    print(f"game frames never presented: {len(unseen)} of {len(runs)}"
          + (" at t(ms) " + ", ".join(f"{(u - t0) / 1e6:.0f}" for u in unseen[:30]) if unseen else ""))
    if gaps:
        print(f"window frames: {len(hb)}, gap median {gaps[len(gaps) // 2][0]:.2f} ms, max {gaps[-1][0]:.2f}; "
              f"gaps > 17 ms {sum(1 for g, _ in gaps if g > 17)}, > 25 ms {sum(1 for g, _ in gaps if g > 25)}; "
              f"swapchain acquire median {acq[len(acq) // 2] if acq else 0:.2f} max {acq[-1] if acq else 0:.2f}")
        print("  longest window gaps (ms at t): " + ", ".join(f"{g:.1f}@{(a - t0) / 1e6:.0f}" for g, a in gaps[-8:]))
    sites = ["other", "decode", "mip decode", "replacement", "mip copy", "palette", "clut", "target", "gather"]
    cr = spans["u5"].get(gs, []) if gs else []
    if cr:
        agg = {}
        for s_, e_, a in cr:
            c = agg.setdefault(a, [0, 0.0])
            c[0] += 1
            c[1] += (e_ - s_) / 1e6
        print("texture creation by site: " + ", ".join(f"{sites[a] if a < len(sites) else a} {c[0]}x {c[1]:.1f} ms" for a, c in sorted(agg.items())))
    worst = sorted(rows, key=lambda r: (-r["ms"], -r["gs"]))[:n]
    worstgs = sorted(rows, key=lambda r: -r["gs"])[:n]
    for title, lst in (("slowest frames", worst), ("frames with the most GS work", worstgs)):
        print(f"\n{title}:")
        print(" frame   t(ms)   ms   vs  EEwait EEdrain  VU1    GS  hostmax | GS work inside")
        for r in sorted(lst, key=lambda r: r["i"]):
            lo, hi = r["lo"], r["hi"]
            parts = []
            for u, label in SPANS.items():
                if u in ("passB", "submitB"):
                    continue
                sp = inside(spans[u].get(gs, []), lo, hi) if gs else []
                if sp:
                    tot = sum(min(e, hi) - max(s, lo) for s, e, _ in sp) / 1e6
                    if tot >= 0.2 or u in ("pipe", "cpudraw", "rtload", "bufcreate"):
                        extra = ""
                        if u == "repl":
                            extra = f" {sum(a for *_, a in sp) * 64} KB"
                        parts.append(f"{label} {len(sp)}x {tot:.1f}{extra}")
            # Host-side marks too (window thread).
            for u in ("discOpen", "discRead", "fileWrite", "eeIdle"):
                sp = inside(spans[u].get(ee, []), lo, hi) if ee else []
                if sp:
                    tot = sum(min(e, hi) - max(s, lo) for s, e, _ in sp) / 1e6
                    if tot >= 0.5:
                        parts.append(f"EE {SPANS[u]} {len(sp)}x {tot:.1f}")
            for u in ("u1", "pipe", "u5"):
                sp = inside(spans[u].get(host, []), lo, hi) if host else []
                if sp:
                    tot = sum(min(e, hi) - max(s, lo) for s, e, _ in sp) / 1e6
                    if tot >= 0.5:
                        parts.append(f"HOST {SPANS[u]} {len(sp)}x {tot:.1f}")
            print(f"{r['i']:6d} {(lo - t0) / 1e6:8.0f} {r['ms']:6.1f} {r['vs']:2d} {r['eew']:6.1f} {r['eed']:6.1f} "
                  f"{r['vu']:6.1f} {r['gs']:6.1f} {r['hostmax']:6.1f} | " + "; ".join(parts))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
