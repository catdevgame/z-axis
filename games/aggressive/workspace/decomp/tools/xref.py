#!/usr/bin/env python3
"""Find strings in the ELF and the code that references them (lui/addiu|ori pairs).
Usage: xref.py ELF "substring" [...]  -> prints string vaddr and disassembly around each xref."""
import sys, struct
import rabbitizer
from elftools.elf.elffile import ELFFile

rabbitizer.config.toolchainTweaks_treatJAsUnconditionalBranch = False
elf = sys.argv[1]
data = open(elf, 'rb').read()
f = ELFFile(open(elf, 'rb'))
secs = [(s['sh_addr'], s['sh_offset'], s['sh_size'], s.name) for s in f.iter_sections()
        if s['sh_addr'] and s['sh_type'] == 'SHT_PROGBITS']
def off2va(o):
    for a, off, sz, n in secs:
        if off <= o < off + sz: return a + o - off
def va2off(v):
    for a, off, sz, n in secs:
        if a <= v < a + sz: return off + v - a
text = f.get_section_by_name('.text')
ta, to, ts = text['sh_addr'], text['sh_offset'], text['sh_size']
words = struct.unpack_from('<%dI' % (ts // 4), data, to)

def dis(va):
    w = words[(va - ta) // 4]
    return rabbitizer.Instruction(w, va, rabbitizer.InstrCategory.R5900)

def find_refs(target):
    hi = (target + 0x8000) >> 16
    lo = target & 0xffff
    lo_s = lo - 0x10000 if lo >= 0x8000 else lo
    out = []
    for i, w in enumerate(words):
        if (w >> 26) == 0x0f and (w & 0xffff) == hi:   # lui
            rt = (w >> 16) & 31
            for j in range(i + 1, min(i + 40, len(words))):
                w2 = words[j]
                op = w2 >> 26
                if op in (0x09, 0x0d) and ((w2 >> 21) & 31) == rt and (w2 & 0xffff) == lo:
                    out.append(ta + i * 4); break
    return out

for s in sys.argv[2:]:
    pos = data.find(s.encode())
    while pos != -1:
        # back up to start of string
        st = pos
        while st > 0 and data[st - 1] != 0: st -= 1
        va = off2va(st)
        print(f'== {data[st:data.index(b"\0", st)]!r} @ {va:#x}')
        for r in find_refs(va):
            print(f'  xref {r:#x}')
            for k in range(-24, 24):
                a = r + k * 4
                print(f'    {a:08x}: {dis(a).disassemble()}')
        pos = data.find(s.encode(), pos + 1)
