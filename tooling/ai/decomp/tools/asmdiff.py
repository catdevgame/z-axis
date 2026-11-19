#!/usr/bin/env python3
"""Side-by-side retail vs compiled disassembly for one function.
usage: asmdiff.py build/matchtest/<func>.<cc>.<flags>.o"""
import os, struct, subprocess, sys, re
sys.path.insert(0, os.path.dirname(__file__))
import rabbitizer
from matchtest import func_from_obj, retail_func, retail_size
o = sys.argv[1]
name = os.path.basename(o).split(".")[0]
words, mask = func_from_obj(o, name)
n = retail_size(name) // 4
ref = retail_func(name, n)
addr = int(name.split("_")[1], 16)
def d(w, a):
    return rabbitizer.Instruction(w, a, rabbitizer.InstrCategory.R5900).disassemble() if w is not None else ""
for i in range(max(n, len(words))):
    a = ref[i] if i < n else None
    b = words[i] if i < len(words) else None
    m = mask[i] if i < len(mask) else 0xFFFFFFFF
    flag = "  " if a is not None and b is not None and (a & m) == (b & m) else "!!"
    print(f"{flag} {d(a, addr+4*i):40.40} | {d(b, addr+4*i)}")
