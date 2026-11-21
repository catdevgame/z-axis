"""List calls to an EE RPC wrapper with constant a0 (fn) and a2 (size). Data only.
Usage: rpccalls.py <elf> <rpc_func_hex>"""
import sys, collections
sys.path.insert(0, __import__('os').path.dirname(__file__))
from elfinfo import Elf
e = Elf(sys.argv[1]); target = int(sys.argv[2], 16)
t = e.section('.text'); ws = e.words(t['addr'], t['addr'] + t['size'])
out = collections.defaultdict(set)
for i, w in enumerate(ws):
    if w >> 26 == 3 and ((w & 0x3FFFFFF) << 2) == target:
        regs = {}
        for j in range(max(0, i - 12), i + 2):  # include delay slot
            x = ws[j]; op = x >> 26; rs = (x >> 21) & 31; rt = (x >> 16) & 31; imm = x & 0xFFFF
            if op in (0x09, 0x0D, 0x19) and rs == 0:
                regs[rt] = imm
            elif op == 0 and (x & 0x3F) in (0x2D, 0x21, 0x25) and rs == 0 and rt == 0:
                regs[(x >> 11) & 31] = 0
            elif op == 0 or op in (0x09, 0x0D, 0x23, 0x0F, 0x19):
                d = (x >> 11) & 31 if op == 0 else rt
                if not (op in (0x09, 0x0D, 0x19) and rs == 0):
                    regs.pop(d, None)
            if x >> 26 in (2, 3) and j < i:
                regs = {}
        fn, size, rep = regs.get(4), regs.get(6), regs.get(7)
        out[fn].add((size, rep, t['addr'] + 4 * i))
for fn in sorted(out, key=lambda x: (-1 if x is None else x)):
    print('fn', fn, 'calls', len(out[fn]), 'sizes', sorted({s for s, r, a in out[fn]}, key=str), 'reply', sorted({r for s, r, a in out[fn]}, key=str))
