#!/usr/bin/env python3
"""A Z-Axis game's PS2 functions paired with its GameCube build's (main.dol: PowerPC Gekko,
Metrowerks CodeWarrior, no symbols, no __FILE__ paths). Data only: reads the PS2 ELF with its
Ghidra function CSV and SCE scan, and the DOL with tools/ghidra/gc_pipeline.sh's export; writes
a JSON in scratch in xmatch.py's format (the games' addresses: never commit it).

    python3 -I tools/gcmatch.py <ps2.elf> <ps2.csv> <ps2 sce-matches.csv> <main.dol> \
        <gc.jsonl> <out.json> [--layers ENGINE:GAME:SDK] [--holdout]

The pairing is xmatch.py's (the same tokens, passes and confidence rules; MATCH3.md section 4),
with a GameCube side in place of the Xbox's:
  - functions, calls and data references from Ghidra (the loader's analyzer resolves the
    r2/r13 small-data bases, so `lfs f1,-0x7ff0(r2)` is a reference to the constant);
  - strings: references to printable text; floats: lfs/lfd/psq_l references, read from the DOL
    (doubles narrowed to float);
  - integers, struct offsets and 32-bit constants decoded from the DOL's words as the MIPS side
    does it: li (addi rD,0,n), cmpwi/cmplwi, andi., mulli, subfic immediates >= 2; the
    displacement of every load and store (D-form, psq 12-bit) off a register that is not r1
    (stack), r2 or r13 (small data), 0, or a register a lis just set (an address);
    lis + ori/addi pairs as 32-bit constants (name hashes, kept for tools/workdb.py).
The GameCube's libraries (the Dolphin OS, MSL, the Metrowerks runtime) have no names, so the
whole .text is in the pool; the PS2's SDK stays out of it as before.

--layers: as in xmatch.py (DM2 100100:150000:1C2728, BMX XXX 100100:170000:21DAA8).
--holdout: xmatch.py's precision check (every second unique-string pair hidden).
"""
import collections, json, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xmatch
from xmatch import (F32, Matcher, f32, nice_float, printable, ps2_side, names_from_xbox,
                    summary, unique_string_pairs)

# CodeWarrior PowerPC against GCC MIPS instruction counts of a pair: log mean and spread,
# fitted on the unique-string pairs of the three games (AI 0.25 / 0.39 over 255, DM2 0.14 /
# 0.34 over 164, BMX XXX 0.14 / 0.24 over 268; main() prints the fit)
xmatch.SIZE_LOG_MEAN, xmatch.SIZE_LOG_SD = 0.18, 0.35


class Dol:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        offs = struct.unpack_from('>18I', d, 0)
        addrs = struct.unpack_from('>18I', d, 0x48)
        sizes = struct.unpack_from('>18I', d, 0x90)
        self.secs = [('T%d' % i if i < 7 else 'D%d' % (i - 7), addrs[i], sizes[i], offs[i])
                     for i in range(18) if sizes[i]]
        self.bss, self.bss_size, self.entry = struct.unpack_from('>3I', d, 0xD8)

    def read(self, va, n):
        for nm, a, sz, o in self.secs:
            if a <= va < a + sz:
                return self.d[o + va - a:o + min(sz, va - a + n)]
        return None

    def words(self, a, e):
        b = self.read(a, e - a) or b''
        return struct.unpack('>%dI' % (len(b) // 4), b[:len(b) // 4 * 4])

    def is_text(self, va):
        return any(nm[0] == 'T' and a <= va < a + sz for nm, a, sz, o in self.secs)


# D-form loads and stores (lwz .. stfdu); psq_l, psq_lu, psq_st, psq_stu have 12-bit offsets
LS_D = set(range(32, 56))
LS_PSQ = {56, 57, 60, 61}


def decode_consts(ws):
    """ints, displacements and 32-bit constants of a function's PowerPC words."""
    ints, disps, c32 = set(), set(), set()
    lis = {}
    for w in ws:
        op = w >> 26
        rd = (w >> 21) & 31
        ra = (w >> 16) & 31
        imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 15 and ra == 0:  # lis
            lis[rd] = imm << 16
            continue
        if op in (14, 24) and ra in lis and ra != 0:  # addi / ori after lis: an address or a constant
            v = (lis[ra] + simm if op == 14 else lis[ra] | imm) & 0xFFFFFFFF
            c32.add(v)
            lis.pop(rd, None)  # rD holds the address or constant now, no longer a lis half
            continue
        if op == 14 and ra == 0:  # li
            v = simm & 0xFFFFFFFF
            if v >= 2:
                ints.add(v)
        elif op in (11, 7, 8):  # cmpwi, mulli, subfic
            v = simm & 0xFFFFFFFF
            if v >= 2:
                ints.add(v)
        elif op in (10, 28):  # cmplwi, andi.
            if imm >= 2:
                ints.add(imm)
        if op in LS_D or op in LS_PSQ:
            if ra not in (0, 1, 2, 13) and ra not in lis:
                d = simm if op in LS_D else ((w & 0xFFF) - 0x1000 if w & 0x800 else w & 0xFFF)
                if 0 <= d < 0x8000:
                    disps.add(d)
            if op in LS_D and op < 36 or op in (40, 41, 42, 43, 46, 48, 49, 50, 51, 56, 57):
                lis.pop(rd, None)  # a load writes rD
        elif op in (14, 15, 24, 25, 28, 29, 7, 8, 12, 13):
            # addi/addis/ori/oris/andi./andis./mulli/subfic/addic write rD (ori/andi: rA)
            lis.pop(ra if op in (24, 25, 28, 29) else rd, None)
        elif op == 31 or op == 21 or op == 20 or op == 23:
            lis.pop(ra, None)
            lis.pop(rd, None)
    return ints, disps, c32


def gc_side(dol_path, jsonl):
    D = Dol(dol_path)
    rows = [json.loads(l) for l in open(jsonl)]
    strings = {int(k): v for k, v in rows[-1]['#strings'].items()}
    rows = rows[:-1]
    funcs = {r['a']: r for r in rows}
    out = {}
    for r in rows:
        a = r['a']
        calls = sorted((o, t) for o, t in r['calls'] if t in funcs and t != a)
        strs, glob, floats, stra, fla = set(), [], set(), {}, {}
        for off, txt, t in r['refs']:
            if t in funcs:
                continue  # a function pointer
            m = txt.split(' ')[0].lower()
            b = D.read(t, 256)
            if m.startswith(('lfs', 'lfd', 'psq_l')) and b:
                if m.startswith('lfd') and len(b) >= 8:
                    v = struct.unpack_from('>d', b)[0]
                    if v == 0 or 1e-12 < abs(v) < 1e12:
                        floats.add(F32.unpack(F32.pack(v))[0] if abs(v) < 3e38 else v)
                        fla[t] = v
                elif len(b) >= 4:
                    bits = struct.unpack_from('>I', b)[0]
                    if bits == 0 or nice_float(bits) or (bits >> 23) & 0xFF in range(0x60, 0xA0):
                        floats.add(f32(bits))
                        fla[t] = f32(bits)
                continue
            st = printable(b) if b else None
            if st is None and t in strings and len(strings[t]) >= 3:
                st = strings[t]
            if st is not None:
                strs.add(st)
                stra[t] = st
                continue
            glob.append(t)
        ints, disps, c32 = decode_consts(D.words(a, r['e']))
        out[a] = dict(a=a, e=r['e'], n=r['ni'], calls=calls, strs=strs, glob=glob, floats=floats,
                      ints=ints, disps=disps, c32=c32,
                      sec='.text' if D.is_text(a) else r['sec'], name=r['n'], stra=stra, fla=fla)
    return D, out


def main():
    args = sys.argv[1:]
    holdout = '--holdout' in args
    if holdout:
        args.remove('--holdout')
    if '--layers' in args:
        i = args.index('--layers')
        import fmatch3
        e0, g0, s0 = (int(v, 16) for v in args[i + 1].split(':'))
        fmatch3.AI_ENGINE = (e0, g0)
        fmatch3.AI_SDK_START = s0
        del args[i:i + 2]
    if len(args) != 6:
        sys.exit(__doc__)
    elf, pcsv, sce, dol, jl, out = args
    P, PS = ps2_side(elf, pcsv, sce)
    D, GS = gc_side(dol, jl)
    if holdout:
        return xmatch.check_holdout(PS, GS)
    M = Matcher(PS, GS)
    M.run()
    inl = M.inlined()
    pairs = {}
    for p in sorted(M.m):
        x = M.m[p]
        pairs['0x%08X' % p] = dict(x='0x%08X' % x, xname=GS[x]['name'], how=M.how[p][0],
                                   detail=M.how[p][1], conf=M.confidence(p),
                                   score=round(M.score(p, x), 3),
                                   votes=M.how[p][1] if M.how[p][0] == 'call' else
                                   round(M.lastvotes.get((p, x), 0), 2),
                                   ps2_n=PS[p]['n'], x_n=GS[x]['n'], layer=PS[p]['layer'],
                                   shared=sorted(t for t in M.TP[p] & M.TX[x] if t in M.w)[:40])
    res = dict(log=M.log, pairs=pairs, xbox_lib_start='0x%08X' % M.xlib_start,
               inlined={'0x%08X' % p: v for p, v in sorted(inl.items())},
               xbox_only=['0x%08X' % x for x in sorted(GS) if x not in M.rx],
               xbox_names={'0x%08X' % x: GS[x]['name'] for x in sorted(GS)},
               ps2_layers={'0x%08X' % p: f['layer'] for p, f in sorted(PS.items())},
               ps2_sizes={'0x%08X' % p: f['n'] for p, f in sorted(PS.items())},
               xbox_sections={'0x%08X' % x: GS[x]['sec'] for x in sorted(GS)},
               readable={},
               globals={'0x%08X' % g: '0x%08X' % h for g, h in sorted(M.gm.items())},
               names=names_from_xbox(M, GS, P.elf.data),
               platform='gamecube')
    json.dump(res, open(out, 'w'), indent=0)
    for l in M.log:
        print(*l)
    summary(res)
    # size agreement of the unique-string pairs (the fit behind SIZE_LOG_MEAN)
    import math
    rs = [math.log(max(GS[x]['n'], 1) / max(PS[p]['n'], 1)) for p, x in unique_string_pairs(PS, GS)]
    if rs:
        mu = sum(rs) / len(rs)
        sd = (sum((r - mu) ** 2 for r in rs) / len(rs)) ** 0.5
        print('size log ratio over %d unique-string pairs: mean %.3f sd %.3f' % (len(rs), mu, sd))


if __name__ == '__main__':
    main()
