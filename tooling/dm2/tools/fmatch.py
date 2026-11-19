"""Function matcher between two PS2 EE ELFs (Aggressive Inline -> Dave Mirra 2).

Data only: reads the two ELFs and their Ghidra function CSVs.

Normalisation per instruction: jal/j targets masked, lui immediates masked,
the low half of lui pairs and gp-relative offsets masked (absolute addresses
differ between builds); struct/stack offsets, branch offsets, registers kept.

Passes:
  1. exact normalised-body hash, unique on both sides
  2. propagation: aligned instruction pairs of matched functions give
     (callee, callee) and (data address, data address) pairs; callees are
     matched when the vote is unambiguous, then their bodies are compared
  3. fuzzy: for requested addresses not yet matched, best SequenceMatcher
     ratio over candidates (length +-50 %), plus string-reference overlap

Usage: fmatch.py <ai.elf> <ai.csv> <dm2.elf> <dm2.csv> <out.json> [addr ...]
"""
import csv, difflib, hashlib, json, struct, sys, collections

sys.path.insert(0, __import__('os').path.dirname(__file__))
from elfinfo import Elf

GP = 28
LOADSTORE = set(range(0x20, 0x30)) | {0x31, 0x35, 0x36, 0x37, 0x39, 0x3D, 0x3E, 0x3F, 0x1E, 0x1F}


class Prog:
    def __init__(self, elf_path, csv_path):
        self.elf = Elf(elf_path)
        ri = self.elf.section('.reginfo')
        self.gp = struct.unpack_from('<I', self.elf.data, ri['off'] + 20)[0]
        t = self.elf.section('.text')
        self.text = (t['addr'], t['addr'] + t['size'])
        self.funcs = {}
        names = {}
        for r in csv.DictReader(open(csv_path)):
            if r['Name'].startswith('entry_'):
                continue
            s, e = int(r['Start'], 16), int(r['End'], 16)
            if self.text[0] <= s < self.text[1] and e > s:
                self.funcs[s] = e
                names[s] = r['Name']
        self.names = names
        self.starts = sorted(self.funcs)
        self.info = {}
        for s in self.starts:
            self.info[s] = self.analyse(s, self.funcs[s])

    def string_at(self, a):
        b = self.elf.read(a, 96)
        if not b:
            return None
        n = b.find(b'\0')
        if n < 3:
            return None
        s = b[:n]
        if all(32 <= c < 127 or c in (9, 10, 13) for c in s):
            return s.decode('latin1')
        return None

    def analyse(self, start, end):
        ws = self.elf.words(start, end)
        toks, calls, refs = [], [], []
        hi = {}
        for i, w in enumerate(ws):
            op = w >> 26
            rs = (w >> 21) & 31
            rt = (w >> 16) & 31
            imm = w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            ref = None
            if op in (2, 3):
                tok = op << 26
                tgt = ((start + 4 * i) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                if op == 3:
                    calls.append((i, tgt))
                ref = ('call' if op == 3 else 'jump', tgt)
            elif op == 0x0F:
                tok = w & 0xFFFF0000
                hi[rt] = imm << 16
            elif op in (0x09, 0x0D) or op in LOADSTORE:
                if rs == GP:
                    tok = w & 0xFFFF0000
                    ref = ('data', (self.gp + simm) & 0xFFFFFFFF)
                elif rs in hi:
                    tok = w & 0xFFFF0000
                    a = (hi[rs] + (imm if op == 0x0D else simm)) & 0xFFFFFFFF
                    ref = ('data', a)
                else:
                    tok = w
                if op in (0x09, 0x0D) and rt != rs:
                    hi.pop(rt, None)
                elif op in (0x20, 0x21, 0x23, 0x24, 0x25, 0x27, 0x37, 0x1E):
                    hi.pop(rt, None)
            else:
                tok = w
                # R-type writes rd; clear knowledge conservatively
                if op == 0:
                    hi.pop((w >> 11) & 31, None)
            toks.append(tok)
            if ref:
                refs.append((i, ref))
        strs = set()
        for i, (kind, a) in refs:
            if kind == 'data':
                s = self.string_at(a)
                if s:
                    strs.add(s)
        h = hashlib.sha1(struct.pack('<%dI' % len(toks), *toks)).hexdigest()
        return dict(toks=toks, calls=calls, refs=refs, strs=strs, hash=h, n=len(toks))

    def func_of(self, a):
        import bisect
        i = bisect.bisect_right(self.starts, a) - 1
        if i >= 0 and a < self.funcs[self.starts[i]]:
            return self.starts[i]
        return None


def align_pairs(ia, ib):
    sm = difflib.SequenceMatcher(None, ia['toks'], ib['toks'], autojunk=False)
    pairs = []
    for a, b, n in sm.get_matching_blocks():
        for k in range(n):
            pairs.append((a + k, b + k))
    return sm.ratio(), pairs


def main():
    A = Prog(sys.argv[1], sys.argv[2])
    B = Prog(sys.argv[3], sys.argv[4])
    out_path = sys.argv[5]
    wanted = [int(x, 16) for x in sys.argv[6:]]
    print('AI funcs', len(A.starts), 'DM2 funcs', len(B.starts), file=sys.stderr)

    match = {}   # A start -> (B start, method, score)
    byhash_a = collections.defaultdict(list)
    byhash_b = collections.defaultdict(list)
    for s in A.starts:
        if A.info[s]['n'] >= 6:
            byhash_a[A.info[s]['hash']].append(s)
    for s in B.starts:
        if B.info[s]['n'] >= 6:
            byhash_b[B.info[s]['hash']].append(s)
    for h, la in byhash_a.items():
        lb = byhash_b.get(h)
        if lb and len(la) == 1 and len(lb) == 1:
            match[la[0]] = (lb[0], 'exact', 1.0)
    print('exact unique', len(match), file=sys.stderr)

    # string-anchored: functions whose referenced-string set is distinctive and equal
    def strkey(p, s):
        st = p.info[s]['strs']
        return frozenset(st) if st else None
    sa = collections.defaultdict(list)
    sb = collections.defaultdict(list)
    for s in A.starts:
        k = strkey(A, s)
        if k:
            sa[k].append(s)
    for s in B.starts:
        k = strkey(B, s)
        if k:
            sb[k].append(s)
    taken_b = {v[0] for v in match.values()}
    for k, la in sa.items():
        lb = sb.get(k)
        if lb and len(la) == 1 and len(lb) == 1 and la[0] not in match and lb[0] not in taken_b:
            r, _ = align_pairs(A.info[la[0]], B.info[lb[0]])
            if r >= 0.5:
                match[la[0]] = (lb[0], 'strings', round(r, 3))
                taken_b.add(lb[0])
    print('after strings', len(match), file=sys.stderr)

    # propagation through aligned call targets
    datamap = collections.defaultdict(collections.Counter)
    done = set()
    for rnd in range(8):
        votes = collections.defaultdict(collections.Counter)
        for a, (b, how, sc) in list(match.items()):
            if (a, b) in done:
                continue
            done.add((a, b))
            ia, ib = A.info[a], B.info[b]
            r, pairs = align_pairs(ia, ib)
            ra = dict(ia['refs'])
            rb = dict(ib['refs'])
            for x, y in pairs:
                if x in ra and y in rb and ra[x][0] == rb[y][0]:
                    ka, va = ra[x]
                    kb, vb = rb[y]
                    if ka in ('call', 'jump'):
                        votes[va][vb] += 1
                    else:
                        datamap[va][vb] += 1
                        fa, fb = A.func_of(va), B.func_of(vb)
                        if fa == va and fb == vb:
                            votes[va][vb] += 1  # function pointer
        taken_b = {v[0] for v in match.values()}
        added = 0
        for va, c in votes.items():
            if va in match or va not in A.info:
                continue
            (vb, n), = c.most_common(1)
            if vb not in B.info or vb in taken_b:
                continue
            if len(c) > 1 and c.most_common(2)[1][1] * 2 >= n:
                continue
            r, _ = align_pairs(A.info[va], B.info[vb])
            match[va] = (vb, 'callgraph', round(r, 3))
            taken_b.add(vb)
            added += 1
        print('round', rnd, 'added', added, 'total', len(match), file=sys.stderr)
        if not added:
            break

    # fuzzy for wanted functions
    taken_b = {v[0] for v in match.values()}
    results = {}
    for w in wanted:
        f = A.func_of(w)
        entry = {'ai': '0x%08X' % w, 'ai_func': '0x%08X' % f if f else None,
                 'ai_name': A.names.get(f) if f else None}
        if f is None:
            entry['status'] = 'not a function in AI map'
            results[entry['ai']] = entry
            continue
        off = w - f
        if f in match:
            b, how, sc = match[f]
            entry.update(dm2='0x%08X' % (b + off), method=how, score=sc,
                         ai_len=A.info[f]['n'], dm2_len=B.info[b]['n'])
        else:
            ia = A.info[f]
            best = []
            for s in B.starts:
                ib = B.info[s]
                if not (0.5 * ia['n'] <= ib['n'] <= 2.0 * ia['n']):
                    continue
                sm = difflib.SequenceMatcher(None, ia['toks'], ib['toks'], autojunk=False)
                if sm.real_quick_ratio() < 0.4 or sm.quick_ratio() < 0.4:
                    continue
                r = sm.ratio()
                st = len(ia['strs'] & ib['strs'])
                best.append((r + 0.1 * min(st, 3), r, st, s))
            best.sort(reverse=True)
            top = best[:3]
            if top:
                sc, r, st, s = top[0]
                second = top[1][0] if len(top) > 1 else 0
                entry.update(dm2='0x%08X' % (s + off), method='fuzzy', score=round(r, 3), strings=st,
                             margin=round(sc - second, 3), ai_len=ia['n'], dm2_len=B.info[s]['n'],
                             alternatives=['0x%08X:%.3f' % (x[3], x[1]) for x in top[1:]])
            else:
                entry['status'] = 'no candidate'
        entry['ai_strs'] = sorted(A.info[f]['strs'])[:6]
        results[entry['ai']] = entry

    dm = {}
    for va, c in datamap.items():
        (vb, n), = c.most_common(1)
        dm['0x%08X' % va] = ['0x%08X' % vb, n, sum(c.values())]
    stats = dict(ai_funcs=len(A.starts), dm2_funcs=len(B.starts), matched=len(match),
                 by_method=collections.Counter(v[1] for v in match.values()),
                 ai_bytes=sum(A.info[s]['n'] * 4 for s in A.starts),
                 matched_bytes=sum(A.info[s]['n'] * 4 for s in match))
    json.dump(dict(stats=stats, wanted=results, datamap=dm,
                   matches={'0x%08X' % a: ['0x%08X' % b, h, s] for a, (b, h, s) in match.items()}),
              open(out_path, 'w'), indent=1)
    print(json.dumps(stats), file=sys.stderr)


if __name__ == '__main__':
    main()
