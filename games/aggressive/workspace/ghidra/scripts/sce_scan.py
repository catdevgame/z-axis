#!/usr/bin/env python3
"""SCE SDK function recogniser for a stripped PS2 ELF.

Python port of recompiler/ps2xAnalyzer/src/sce_symbol_scanner.cpp (read-only use of
its embedded sce-symbol-scanner database). Same algorithm:
  - walk the match tree at every 4-byte offset of executable sections,
  - mask relocated fields, SHA-1 the candidate body and compare with the DB hash,
  - keep candidates with >= 256 static bits and a single (library, name) identity per address.

Usage:
  sce_scan.py extract <sce_symbol_database_data.h> <outdir>      # writes symbols.json + tree.json
  sce_scan.py scan <dbdir> <elf> <out.csv>
"""
import csv
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

MASK = {"NONE": 0xFFFFFFFF, "MIPS_26": 0xFC000000, "MIPS_LO16": 0xFFFF0000, "MIPS_HI16": 0xFFFF0000,
        "MIPS_GPREL16": 0xFFFF0000, "MIPS_LITERAL": 0xFFFF0000, "MIPS_32": 0}
STATIC_RELOC_BITS = {"NONE": 32, "MIPS_26": 6, "MIPS_LO16": 16, "MIPS_HI16": 16, "MIPS_GPREL16": 16,
                     "MIPS_LITERAL": 16, "MIPS_32": 0}


def norm_reloc(t):
    u = (t or "none").upper()
    aliases = {"MIPS26": "MIPS_26", "LO16": "MIPS_LO16", "MIPSLO16": "MIPS_LO16", "HI16": "MIPS_HI16",
               "MIPSHI16": "MIPS_HI16", "MIPS32": "MIPS_32", "MIPSGPREL16": "MIPS_GPREL16",
               "MIPSLITERAL": "MIPS_LITERAL"}
    u = aliases.get(u, u)
    return u if u in MASK else "NONE"


def extract(header, outdir):
    text = Path(header).read_text(encoding="utf-8")
    split = text.index("kTreeJsonChunks")
    pat = re.compile(r'R"PS2SDB\((.*?)\)PS2SDB", (\d+)\}', re.S)
    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)
    for name, region in (("symbols.json", text[:split]), ("tree.json", text[split:])):
        parts = []
        for m in pat.finditer(region):
            chunk = m.group(1)
            if len(chunk.encode("utf-8")) != int(m.group(2)):
                raise SystemExit(f"chunk size mismatch in {name}")
            parts.append(chunk)
        (out / name).write_text("".join(parts), encoding="utf-8")
        json.loads((out / name).read_text(encoding="utf-8"))  # validate
        print(f"wrote {out / name} from {len(parts)} chunks")


def load_sections(elf):
    d = Path(elf).read_bytes()
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    shnum, shstrndx = struct.unpack_from("<HH", d, 0x30)
    secs = [struct.unpack_from("<10I", d, shoff + i * 40) for i in range(shnum)]
    so = secs[shstrndx][4]
    res = []
    for s in secs:
        name = d[so + s[0]:d.index(b"\0", so + s[0])].decode()
        # EE code only: alloc+exec PROGBITS, excluding VU microcode in .vutext
        if s[1] == 1 and (s[2] & 0x6) == 0x6 and name != ".vutext":
            res.append((name, s[3], d[s[4]:s[4] + s[5]]))
    return res


def scan(dbdir, elf, outcsv):
    db = Path(dbdir)
    symbols_json = json.loads((db / "symbols.json").read_text(encoding="utf-8"))
    tree = json.loads((db / "tree.json").read_text(encoding="utf-8"))
    symbols = {}
    for lib, names in symbols_json.items():
        for name, hashes in names.items():
            for h, variants in hashes.items():
                for var, js in variants.items():
                    relocs = [(int(k, 0), norm_reloc(v.get("type"))) for k, v in js.get("relocations", {}).items()]
                    size = js.get("size", 0)
                    static = size * 8 - sum(STATIC_RELOC_BITS[t] for _, t in relocs)
                    symbols[(lib, name, h, int(var, 16))] = {
                        "lib": lib, "name": name, "hash": h, "variant": int(var, 16), "size": size,
                        "func": str(js.get("type", "")).upper() in ("FUNCTION", "FUNC"),
                        "relocs": relocs, "static": max(static, 0)}

    def find_candidates(data, off):
        out = []
        stack = [tree]
        n = len(data)
        while stack:
            node = stack.pop()
            no = node.get("offset", 0)
            if off + no + 4 > n:
                continue
            val = struct.unpack_from("<I", data, off + no)[0]
            for edge in node.get("next", []):
                m = edge["match"]
                mask = MASK[norm_reloc(m.get("relocation", {}).get("type"))] if "relocation" in m else 0xFFFFFFFF
                if (val & mask) != (m.get("value", 0) & mask):
                    continue
                child = edge["child"]
                for k in child.get("symbols", []):
                    s = symbols.get((k.get("library", ""), k.get("name", ""), k.get("hash", ""), k.get("variant", 0)))
                    if s:
                        out.append(s)
                if child.get("next"):
                    stack.append(child)
        return out

    results = []
    for secname, base, data in load_sections(elf):
        n = len(data)
        for off in range(0, n - 3, 4):
            cands = {}
            for s in find_candidates(data, off):
                if not s["func"] or s["size"] == 0 or s["size"] > n - off:
                    continue
                body = bytearray(data[off:off + s["size"]])
                for ro, rt in s["relocs"]:
                    if ro + 4 <= len(body):
                        v = struct.unpack_from("<I", body, ro)[0]
                        struct.pack_into("<I", body, ro, v & MASK[rt])
                if hashlib.sha1(body).hexdigest() != s["hash"]:
                    continue
                actual = s["size"]
                while actual <= n - off - 4 and struct.unpack_from("<I", data, off + actual)[0] == 0:
                    actual += 4
                cands[(s["lib"], s["name"], s["hash"], s["variant"])] = (s, actual)
            viable = [(s, a) for s, a in cands.values() if s["static"] >= 256]
            if not viable or len({(s["lib"], s["name"]) for s, _ in viable}) != 1:
                continue
            best = max(viable, key=lambda sa: (sa[1], sa[0]["static"]))
            results.append((base + off, best[1], best[0]["name"], best[0]["lib"], best[0]["hash"]))
    results.sort()
    with open(outcsv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Address", "Size", "Name", "Library", "Hash"])
        for a, sz, name, lib, h in results:
            w.writerow([f"0x{a:08X}", sz, name, lib, h])
    print(f"{len(results)} SCE matches written to {outcsv}")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "extract":
        extract(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 5 and sys.argv[1] == "scan":
        scan(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        print(__doc__)
        sys.exit(2)
