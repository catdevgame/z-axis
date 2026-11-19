#!/usr/bin/env python3
"""List functions from the split asm with size/leaf info, and tag them with any
source-file path strings they reference.  usage: funcs.py [asm/cod/000000.s] > funcs.tsv"""
import re, sys, struct
from elftools.elf.elffile import ELFFile
asm = sys.argv[1] if len(sys.argv) > 1 else "asm/cod/000000.s"
elf = ELFFile(open("SLUS_203.27", "rb")); raw = open("SLUS_203.27", "rb").read()
def rd(va):
    for s in elf.iter_sections():
        if s["sh_addr"] and s["sh_type"] == "SHT_PROGBITS" and s["sh_addr"] <= va < s["sh_addr"] + s["sh_size"]:
            o = s["sh_offset"] + va - s["sh_addr"]; e = raw.index(b"\0", o); return raw[o:e]
funcs = []; cur = None
for line in open(asm):
    m = re.match(r"nonmatching (\S+), 0x([0-9A-Fa-f]+)", line)
    if m:
        cur = {"name": m.group(1), "size": int(m.group(2), 16), "jal": 0, "refs": set(), "addr": None}
        funcs.append(cur); continue
    if cur is None: continue
    m = re.match(r"\s*/\* \w+ ([0-9A-F]{8}) [0-9A-F]{8} \*/\s+(\S+)\s*(.*)", line)
    if m:
        if cur["addr"] is None: cur["addr"] = int(m.group(1), 16)
        op, args = m.group(2), m.group(3)
        if op in ("jal", "jalr"): cur["jal"] += 1
        for sym in re.findall(r"%lo\((D_[0-9A-F]+)\)", args):
            s = rd(int(sym[2:], 16))
            if s and (s.endswith(b".c") or s.endswith(b".cpp")) and len(s) < 120: cur["refs"].add(s.decode(errors="replace"))
for f in funcs:
    print(f"{f['addr']:08X}\t{f['size']:#x}\t{f['jal']}\t{f['name']}\t{';'.join(sorted(f['refs']))}")
