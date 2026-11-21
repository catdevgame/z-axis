#!/usr/bin/env python3
"""Coverage of the readable game (engine/, ps2/, games/: tools/checks/zlayout.py): how much of the
recompiled game is named, replaced by readable C++, and where the time goes.

    readable_coverage.py [--workspace DIR] [--profile FILE.csv] [--top N] [--json]

--workspace   the AI workspace (default: AI_WORKSPACE, tools/paths.env), for
              recomp/ghidra-functions.csv and recomp/generated; the docs are
              this checkout's docs/*.md
--profile     a ZX_FNPROF profile (address,calls,inclusive_ns,self_ns)
--top         how many hot functions to list (default 30)

Counts:
  functions      the game's functions in the Ghidra map (recomp/ghidra-functions.csv
                 without its entry_* rows, which are switch labels and blocks the
                 recompiler splits off); recompiled = those with a generated source
  named          a function with a name: an SDK/library name from the Ghidra map
                 (not FUN_*), a readable function, or an address the bring-up
                 documents (FUN_/func_ names and 0x addresses in docs/*.md and in
                 the game overrides' constants)
  hooked         addresses the game overrides wrap or replace (lookupFunction /
                 replaceFunction / bindAddressHandler targets)
  readable       registered in the readable code (READABLE_FUNCTION / READABLE_PLANNED), by
                 status: planned, draft, tested, verified (verified = on by default)
"""
import argparse
import csv
import json
import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import zaxis_paths  # noqa: E402  (tools/paths.env: the workspaces, the data, the work database)
import readable_symbols
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ZAXIS_TREE = os.path.dirname(os.path.dirname(HERE))
GAME_SRC = ZAXIS_TREE     # (the readable code is the tree's: zaxis_paths.walk, key_of)
OVERRIDES = [d for _, d in zaxis_paths.zlayout.overrides_dirs(ZAXIS_TREE)]   # (the engine's and each game's)

# Address ranges of the retail ELF (decomp/README.md).
AREAS = [
    (0x00100000, 0x00100100, "crt0"),
    (0x00100100, 0x00171000, "ZGE3 engine"),
    (0x00171000, 0x00202800, "game (Aggressive Inline)"),
    (0x00202800, 0x0022DB40, "libraries (libm, Sony SDK, libc)"),
]


def area_of(address):
    for begin, end, name in AREAS:
        if begin <= address < end:
            return name
    return "other"


def read_functions(workspace, path=None):
    """[(address, end, name)] from the Ghidra map, without entry_* rows."""
    path = path or os.path.join(workspace, "recomp", "ghidra-functions.csv")
    out = []
    with open(path) as f:
        for row in csv.DictReader(f):
            if row["Name"].startswith("entry_") or row["Name"] == "entry":
                continue
            out.append((int(row["Start"], 16), int(row["End"], 16), row["Name"]))
    return sorted(out)


def generated_addresses(workspace):
    folder = os.path.join(workspace, "recomp", "generated")
    found = set()
    if not os.path.isdir(folder):
        return found
    for name in os.listdir(folder):
        m = re.search(r"_0x([0-9a-f]+)\.cpp$", name)
        if m:
            found.add(int(m.group(1), 16))
    return found


def readable_functions():
    """{address: (name, status, file)} from READABLE_FUNCTION / READABLE_PLANNED."""
    out = {}
    pattern = re.compile(r"READABLE_(FUNCTION_WAITS|FUNCTION_VU0|FUNCTION|PLANNED)\(\s*(0x[0-9A-Fa-f]+)\s*,\s*([^,\)]+)\s*(?:,\s*(\w+))?")
    for root, _, files in zaxis_paths.walk(GAME_SRC):
        for name in files:
            if not name.endswith(".cpp"):
                continue
            path = os.path.join(root, name)
            text = readable_symbols.literal_view(open(path, errors="replace").read())
            for m in pattern.finditer(text):
                if m.group(0).startswith("READABLE_FUNCTION(ADDRESS"):
                    continue
                kind, address, fn, status = m.groups()
                status = "planned" if kind == "PLANNED" else (status or "draft").lower()
                out[int(address, 16)] = (fn.strip().strip('"').rstrip('_'), status, zaxis_paths.key_of(path, GAME_SRC),
                                         kind == "FUNCTION_WAITS")
    return out


def documented_addresses(workspace, functions):
    """Function starts mentioned in the bring-up docs and the overrides."""
    starts = {a for a, _, _ in functions}
    texts = []
    for name in ("BRINGUP.md", "NATIVE.md", "DEVTOOLS.md", "SPU2.md"):
        path = os.path.join(ZAXIS_TREE, "docs", name)
        if os.path.exists(path):
            texts.append(readable_symbols.literal_view(open(path, errors="replace").read()))
    for root, _, files in (w for d in OVERRIDES for w in os.walk(d)):
        for name in files:
            if name.endswith((".cpp", ".h")):
                texts.append(readable_symbols.literal_view(open(os.path.join(root, name), errors="replace").read()))
    found = set()
    for text in texts:
        for m in re.finditer(r"\b(?:FUN_|func_|0x)([0-9A-Fa-f]{6,8})\b", text):
            a = int(m.group(1), 16)
            if a in starts:
                found.add(a)
    return found


def hooked_addresses(functions):
    """Addresses the overrides hook: constants passed to lookupFunction / replaceFunction."""
    starts = {a for a, _, _ in functions}
    found = set()
    for root, _, files in (w for d in OVERRIDES for w in os.walk(d)):
        for name in files:
            if not name.endswith(".cpp"):
                continue
            text = readable_symbols.literal_view(open(os.path.join(root, name), errors="replace").read())
            constants = {m.group(1): int(m.group(2), 16)
                         for m in re.finditer(r"constexpr\s+uint32_t\s+(\w+)\s*=\s*0x([0-9A-Fa-f]+)u?", text)}
            for m in re.finditer(r"(?:replaceFunction|lookupFunction|bindAddressHandler)\(\s*([\w:]+|0x[0-9A-Fa-f]+u?)", text):
                token = m.group(1)
                if token.startswith("0x"):
                    a = int(token.rstrip("u"), 16)
                else:
                    a = constants.get(token.split("::")[-1])
                if a is not None and a in starts:
                    found.add(a)
            for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]+)u\s*,\s*\"", text):  # binding tables
                a = int(m.group(1), 16)
                if a in starts:
                    found.add(a)
    return found


def read_profile(path):
    rows = []
    with open(path) as f:
        for row in csv.DictReader(f):
            rows.append((int(row["address"], 16), int(row["calls"]), int(row["inclusive_ns"]), int(row["self_ns"])))
    return rows


def owner_of(functions, address):
    """The function containing an address (split-off blocks count for their owner)."""
    lo, hi = 0, len(functions)
    while lo < hi:
        mid = (lo + hi) // 2
        if functions[mid][0] <= address:
            lo = mid + 1
        else:
            hi = mid
    if lo and functions[lo - 1][0] <= address < max(functions[lo - 1][1], functions[lo - 1][0] + 4):
        return functions[lo - 1]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--workspace", default=zaxis_paths.get("AI_WORKSPACE"))
    ap.add_argument("--profile")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    functions = read_functions(args.workspace)
    names = {a: n for a, _, n in functions}
    generated = generated_addresses(args.workspace)
    readable = readable_functions()
    documented = documented_addresses(args.workspace, functions)
    hooked = hooked_addresses(functions)

    sdk_named = {a for a, n in names.items() if not n.startswith("FUN_")}
    named = sdk_named | documented | set(readable)
    status = Counter(s for _, s, _, _ in readable.values())
    waits = sum(1 for _, s, _, w in readable.values() if w and s != "planned")
    replaced = {a for a, (_, s, _, _) in readable.items() if s != "planned"}

    by_area = defaultdict(lambda: Counter())
    for a, _, _ in functions:
        area = area_of(a)
        by_area[area]["functions"] += 1
        by_area[area]["named"] += a in named
        by_area[area]["readable"] += a in replaced

    report = {
        "functions": len(functions),
        "recompiled": len([a for a, _, _ in functions if a in generated]) if generated else None,
        "named": len(named),
        "named_sdk": len(sdk_named),
        "named_documented": len(documented - sdk_named),
        "hooked": len(hooked),
        "readable": {k: status.get(k, 0) for k in ("planned", "draft", "tested", "verified")},
        "on_fibers": waits,
        "areas": {k: dict(v) for k, v in by_area.items()},
    }

    hot = []
    if args.profile:
        rows = read_profile(args.profile)
        total_self = sum(r[3] for r in rows) or 1
        merged = defaultdict(lambda: [0, 0, 0])
        for address, calls, inclusive, self_ns in rows:
            owner = owner_of(functions, address)
            key = owner[0] if owner else address
            m = merged[key]
            m[0] += calls
            m[1] = max(m[1], inclusive)  # a split-off block's time is inside its owner's
            m[2] += self_ns
        for address, (calls, inclusive, self_ns) in merged.items():
            name, st = names.get(address, "?"), "-"
            if address in readable:
                name, st = readable[address][0], readable[address][1]
            hot.append({"address": "0x%08x" % address, "name": name, "status": st,
                        "hooked": address in hooked, "named": address in named, "area": area_of(address),
                        "calls": calls, "self_ms": self_ns / 1e6, "inclusive_ms": inclusive / 1e6,
                        "self_share": self_ns / total_self})
        hot.sort(key=lambda h: -h["self_share"])
        report["profile"] = {"file": args.profile, "functions_seen": len(merged),
                             "self_ms_total": total_self / 1e6,
                             "readable_self_share": sum(h["self_share"] for h in hot if h["status"] not in ("-", "planned")),
                             "named_self_share": sum(h["self_share"] for h in hot if h["named"])}
        report["hot"] = hot[:args.top]
        # Self-time share covered by the N hottest functions.
        acc, marks = 0.0, {}
        for i, h in enumerate(hot, 1):
            acc += h["self_share"]
            for n in (10, 50, 100, 250, 500):
                if i == n:
                    marks[n] = acc
        report["profile"]["self_share_of_top"] = marks

    if args.json:
        json.dump(report, sys.stdout, indent=2)
        print()
        return

    print("Readable game coverage")
    print("  functions in the Ghidra map:   %d" % report["functions"])
    if report["recompiled"] is not None:
        print("  recompiled (generated source): %d" % report["recompiled"])
    print("  named:                         %d  (%d SDK/library names, %d more documented in the bring-up)"
          % (report["named"], report["named_sdk"], report["named_documented"]))
    print("  hooked by game overrides:      %d" % report["hooked"])
    r = report["readable"]
    print("  readable (src/game):           %d  (verified %d, tested %d, draft %d; planned %d; %d run on fibers, their callees can wait)"
          % (r["verified"] + r["tested"] + r["draft"], r["verified"], r["tested"], r["draft"], r["planned"],
             report["on_fibers"]))
    print()
    print("  %-34s %9s %7s %8s" % ("area", "functions", "named", "readable"))
    for _, _, area in AREAS:
        a = report["areas"].get(area, {})
        print("  %-34s %9d %7d %8d" % (area, a.get("functions", 0), a.get("named", 0), a.get("readable", 0)))
    if hot:
        p = report["profile"]
        print()
        print("Profile %s: %d functions ran, %.0f ms of EE self time" % (os.path.basename(p["file"]), p["functions_seen"], p["self_ms_total"]))
        print("  self time in readable functions: %.1f %%, in named ones: %.1f %%"
              % (100 * p["readable_self_share"], 100 * p["named_self_share"]))
        print("  self time of the hottest 10 / 50 / 100 / 250 / 500: %s"
              % " / ".join("%.0f %%" % (100 * p["self_share_of_top"].get(n, 0)) for n in (10, 50, 100, 250, 500)))
        print()
        print("  %-11s %-28s %-8s %-6s %10s %8s %10s  %s" % ("address", "name", "status", "hooked", "calls", "self %", "incl ms", "area"))
        for h in report["hot"]:
            print("  %-11s %-28s %-8s %-6s %10d %7.2f%% %10.1f  %s" % (
                h["address"], h["name"][:28], h["status"], "yes" if h["hooked"] else "", h["calls"],
                100 * h["self_share"], h["inclusive_ms"], h["area"]))


if __name__ == "__main__":
    main()
