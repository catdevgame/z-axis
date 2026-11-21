#!/usr/bin/env python3
"""Three-way function match on Z-Axis's engine: Aggressive Inline (ZGE3, the key),
Dave Mirra BMX 2 (ZGE2) and BMX XXX (ZGE4). Data only: reads the three ELFs and
their Ghidra function CSVs (tools/ghidra/pipeline.sh); writes a JSON in scratch
(it holds the games' addresses: never commit it).

    python3 -I tools/fmatch3.py <ai.elf> <ai.csv> <dm2.elf> <dm2.csv> <bmx.elf> <bmx.csv> <out.json>

Per pair (AI -> DM2, AI -> BMX XXX), the passes of fmatch.py (address-masked
exact hash, unique string sets, call-graph propagation through aligned call
sites) and two more:
  - equal-hash groups: functions whose masked bodies are equal but not unique
    (small getters, thunks), paired when the group is the same size on both
    sides and their neighbours' matches keep address order, or a call site
    votes for them;
  - order: an unmatched AI function between two matched neighbours whose
    counterparts are in the same order is compared with the unmatched functions
    between those counterparts (link order is mostly kept between versions);
    the best similarity wins when it is >= 0.5 and clearly ahead (equal bodies
    go to the candidate at the same relative place in the gap);
  - neighbour: when nothing else adds, candidates from the call graph around
    the function (fmatch2.py's), best similarity >= 0.6 and 0.1 ahead.
Similarity = 0.4 masked-token ratio + 0.4 opcode-class ratio + 0.2 share of
matched callees called by both (0.5/0.5 without callees). Call-graph votes are
accepted at similarity >= 0.45 (>= 0.3 with two votes). The passes repeat until
nothing changes. A DM2 -> BMX XXX match is made the same way as a check: for AI
functions matched in both games, does it pair the same two (`triangle`)?

Each pair is then classed, with the masked-token ratio r of the two bodies:
  identical  the address-masked bodies are equal (only jal/j targets, lui/lo
             addresses and gp offsets differ)
  close      r >= 0.85
  drifted    0.5 <= r < 0.85
  weak       matched by call graph or order only, r < 0.5
  none       no counterpart found
For identical pairs, `callees_consistent` says whether every aligned call goes
to the matched counterpart of AI's callee (the per-game address table's check).

Layers by AI address (docs/READABLE.md "Layout"): sdk (an SCE-scanned name,
crt0 below 0x100100, or 0x2021E0 and up: libm, libgraph, libkernel ...),
engine (0x100100-0x171000, ZGE3), game (0x171000-0x2021E0).
"""
import bisect, collections, csv, difflib, json, sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
from fmatch import Prog, align_pairs

AI_ENGINE = (0x100100, 0x171000)
AI_SDK_START = 0x2021E0
CLASSES = ('identical', 'close', 'drifted', 'weak')


def sce_names(csv_path):
    names = set()
    for r in csv.DictReader(open(csv_path)):
        n = r['Name']
        if not n.startswith(('FUN_', 'entry', 'thunk_', 'LAB_', 'switch')):
            names.add(int(r['Start'], 16))
    return names


def layer_of(a, named_sdk):
    if a in named_sdk or a < AI_ENGINE[0] or a >= AI_SDK_START:
        return 'sdk'
    if a < AI_ENGINE[1]:
        return 'engine'
    return 'game'


def opclass(w):
    op = w >> 26
    if op == 0:
        return (0, w & 0x3F)
    if op == 1:
        return (1, (w >> 16) & 31)
    if op in (0x10, 0x11, 0x12):
        return (op, (w >> 21) & 31, w & 0x3F)
    if op == 0x1C:
        return (op, w & 0x3F, (w >> 6) & 31)
    return (op,)


class Ratio:
    """Cached similarities of an A function and a B function.

    tok(a, b): SequenceMatcher ratio of the address-masked words (registers,
    offsets, immediates kept): what decides identical / close / drifted.
    ops(a, b): ratio of the opcode classes only (survives register allocation
    and offset changes), used with callee overlap to accept a pair.
    """
    def __init__(self, A, B):
        self.A, self.B = A, B
        self.tc, self.oc, self.seq = {}, {}, {}
        self.match = None

    def opseq(self, P, s):
        k = (id(P), s)
        if k not in self.seq:
            self.seq[k] = [opclass(w) for w in P.elf.words(s, P.funcs[s])]
        return self.seq[k]

    def tok(self, a, b, quick=True):
        k = (a, b)
        v = self.tc.get(k)
        if v is None or (v < 0 and not quick):
            ia, ib = self.A.info[a], self.B.info[b]
            if ia['hash'] == ib['hash']:
                v = 1.0
            else:
                sm = difflib.SequenceMatcher(None, ia['toks'], ib['toks'], autojunk=False)
                if quick and (sm.real_quick_ratio() < 0.3 or sm.quick_ratio() < 0.3):
                    v = -1.0  # below 0.3, not computed
                else:
                    v = sm.ratio()
            self.tc[k] = v
        return max(v, 0.0) if quick else v

    def ops(self, a, b):
        k = (a, b)
        if k not in self.oc:
            sa, sb = self.opseq(self.A, a), self.opseq(self.B, b)
            sm = difflib.SequenceMatcher(None, sa, sb, autojunk=False)
            if sm.real_quick_ratio() < 0.3 or sm.quick_ratio() < 0.3:
                self.oc[k] = 0.0
            else:
                self.oc[k] = sm.ratio()
        return self.oc[k]

    def callees(self, a, b):
        """Share of a's matched callees whose counterparts b calls too."""
        m = self.match
        ca = {m[t][0] for _, t in self.A.info[a]['calls'] if t in m}
        if not ca:
            return None
        cb = {t for _, t in self.B.info[b]['calls']}
        return len(ca & cb) / len(ca)

    def sim(self, a, b):
        t = self.tok(a, b)
        if t == 1.0:
            return 1.0
        o = self.ops(a, b)
        c = self.callees(a, b)
        if c is None:
            return 0.5 * t + 0.5 * o
        return 0.4 * t + 0.4 * o + 0.2 * c


def match_pair(A, B, log, seed=None):
    """seed: {a: (b, how)}, pairs taken as made after the exact and string passes (one whose
    either side is matched by then is skipped), for the later passes to grow from."""
    R = Ratio(A, B)
    match = {}
    R.match = match
    taken = set()

    def add(a, b, how):
        match[a] = (b, how)
        taken.add(b)

    byh_a, byh_b = collections.defaultdict(list), collections.defaultdict(list)
    for s in A.starts:
        if A.info[s]['n'] >= 6:
            byh_a[A.info[s]['hash']].append(s)
    for s in B.starts:
        if B.info[s]['n'] >= 6:
            byh_b[B.info[s]['hash']].append(s)
    for h, la in byh_a.items():
        lb = byh_b.get(h)
        if lb and len(la) == 1 and len(lb) == 1:
            add(la[0], lb[0], 'exact')
    log('  exact unique %d' % len(match))

    sa, sb = collections.defaultdict(list), collections.defaultdict(list)
    for s in A.starts:
        if A.info[s]['strs']:
            sa[frozenset(A.info[s]['strs'])].append(s)
    for s in B.starts:
        if B.info[s]['strs']:
            sb[frozenset(B.info[s]['strs'])].append(s)
    for k, la in sa.items():
        lb = sb.get(k)
        if lb and len(la) == 1 and len(lb) == 1 and la[0] not in match and lb[0] not in taken:
            if R.tok(la[0], lb[0]) >= 0.5:
                add(la[0], lb[0], 'strings')
    log('  after strings %d' % len(match))
    if seed:
        for a, (b, how) in sorted(seed.items()):
            if a in A.info and b in B.info and a not in match and b not in taken:
                add(a, b, how)
        log('  after seeds %d' % len(match))

    done = set()
    callers_b = collections.defaultdict(set)
    for s, i in B.info.items():
        for _, t in i['calls']:
            callers_b[t].add(s)

    def propagate():
        votes = collections.defaultdict(collections.Counter)
        for a, (b, how) in list(match.items()):
            if (a, b) in done:
                continue
            done.add((a, b))
            ia, ib = A.info[a], B.info[b]
            _, pairs = align_pairs(ia, ib)
            ra, rb = dict(ia['refs']), dict(ib['refs'])
            for x, y in pairs:
                if x in ra and y in rb and ra[x][0] == rb[y][0]:
                    va, vb = ra[x][1], rb[y][1]
                    if ra[x][0] in ('call', 'jump') or (A.func_of(va) == va and B.func_of(vb) == vb):
                        votes[va][vb] += 1
        added = 0
        for va, c in votes.items():
            if va in match or va not in A.info:
                continue
            (vb, n), = c.most_common(1)
            if vb not in B.info or vb in taken:
                continue
            if len(c) > 1 and c.most_common(2)[1][1] * 2 >= n:
                continue
            sm = R.sim(va, vb)
            if sm < 0.45 and not (n >= 2 and sm >= 0.3):
                continue
            add(va, vb, 'callgraph')
            added += 1
        return added

    def order_pass():
        added = 0
        ma = sorted(match)
        unm_b = sorted(s for s in B.starts if s not in taken)
        for i in range(len(ma) - 1):
            a0, a1 = ma[i], ma[i + 1]
            lo, hi = bisect.bisect_right(A.starts, a0), bisect.bisect_left(A.starts, a1)
            gap_a = [s for s in A.starts[lo:hi] if s not in match]
            if not gap_a:
                continue
            b0, b1 = match[a0][0], match[a1][0]
            if not b0 < b1:
                continue
            j0, j1 = bisect.bisect_right(unm_b, b0), bisect.bisect_left(unm_b, b1)
            gap_b = [s for s in unm_b[j0:j1] if s not in taken]
            if not gap_b or len(gap_a) > 40 or len(gap_b) > 40:
                continue
            for ka, fa in enumerate(gap_a):
                # ties (equal bodies) go to the candidate at the same relative place in the gap
                pa = (ka + 0.5) / len(gap_a)
                sc = sorted(((round(R.sim(fa, fb), 6), -abs((kb + 0.5) / len(gap_b) - pa), fb)
                             for kb, fb in enumerate(gap_b) if fb not in taken), reverse=True)
                if not sc:
                    continue
                r, _, fb = sc[0]
                second = sc[1][0] if len(sc) > 1 and sc[1][0] < r else (sc[1][0] if len(sc) > 1 else 0.0)
                if len(sc) > 1 and sc[1][0] == r and r < 0.9:
                    continue  # a real tie below 0.9: leave it
                if r >= 0.5 and (r - second >= 0.1 or r >= 0.9):
                    add(fa, fb, 'order')
                    added += 1
                elif len(gap_a) == 1 and len(gap_b) == 1 and r >= 0.35:
                    add(fa, fb, 'order')
                    added += 1
        return added

    def group_pass():
        added = 0
        ma = sorted(match)
        for h, la in byh_a.items():
            lb = byh_b.get(h)
            if not lb or (len(la) == 1 and len(lb) == 1):
                continue
            la2 = [a for a in la if a not in match]
            lb2 = [b for b in lb if b not in taken]
            if not la2 or not lb2:
                continue
            for a in la2:
                # neighbours' counterparts bracket a candidate
                k = bisect.bisect_left(ma, a)
                prev = match[ma[k - 1]][0] if k > 0 else None
                nxt = match[ma[k]][0] if k < len(ma) else None
                if prev is None or nxt is None or not prev < nxt:
                    continue
                c = [b for b in lb2 if prev < b < nxt and b not in taken]
                if len(c) == 1:
                    add(a, c[0], 'group')
                    added += 1
        return added

    callers_a = collections.defaultdict(set)
    for s_, i in A.info.items():
        for _, t in i['calls']:
            callers_a[t].add(s_)

    def neighbour_pass():
        """Candidates from the call graph around a (fmatch2.py's), best sim >= 0.6 and clearly ahead."""
        added = 0
        for a in A.starts:
            if a in match or A.info[a]['n'] < 4:
                continue
            cand = set()
            for c in callers_a.get(a, ()):
                if c in match:
                    cand.update(t for _, t in B.info[match[c][0]]['calls'])
            for _, t in A.info[a]['calls']:
                if t in match:
                    cand.update(callers_b.get(match[t][0], ()))
            na = A.info[a]['n']
            cand = [b for b in cand if b in B.info and b not in taken and 0.5 * na <= B.info[b]['n'] <= 2 * na]
            if not cand or len(cand) > 200:
                continue
            sc = sorted(((R.sim(a, b), b) for b in cand), reverse=True)
            r, b = sc[0]
            second = sc[1][0] if len(sc) > 1 else 0.0
            if r >= 0.6 and r - second >= 0.1:
                add(a, b, 'neighbour')
                added += 1
        return added

    for rnd in range(30):
        n1 = propagate()
        n2 = group_pass()
        n3 = order_pass()
        n4 = neighbour_pass() if not (n1 or n2 or n3) else 0
        log('  round %d: callgraph +%d, group +%d, order +%d, neighbour +%d, total %d'
            % (rnd, n1, n2, n3, n4, len(match)))
        if not (n1 or n2 or n3 or n4):
            break

    out = {}
    for a, (b, how) in match.items():
        ia, ib = A.info[a], B.info[b]
        r = R.tok(a, b, quick=False)
        if ia['hash'] == ib['hash']:
            cls = 'identical'
        elif r >= 0.85:
            cls = 'close'
        elif r >= 0.5:
            cls = 'drifted'
        else:
            cls = 'weak'
        out[a] = dict(addr=b, method=how, ratio=round(r, 4), ops=round(R.ops(a, b), 4), cls=cls, n=ib['n'])
    # callee consistency for identical pairs
    for a, e in out.items():
        if e['cls'] != 'identical':
            continue
        ca = [t for _, t in A.info[a]['calls']]
        cb = [t for _, t in B.info[e['addr']]['calls']]
        ok = len(ca) == len(cb) and all(x in match and match[x][0] == y for x, y in zip(ca, cb))
        e['callees_consistent'] = ok
    return out


def main():
    A = Prog(sys.argv[1], sys.argv[2])
    D = Prog(sys.argv[3], sys.argv[4])
    X = Prog(sys.argv[5], sys.argv[6])
    out_path = sys.argv[7]
    log = lambda s: print(s, file=sys.stderr, flush=True)
    log('functions: AI %d, DM2 %d, BMX XXX %d' % (len(A.starts), len(D.starts), len(X.starts)))
    sdk = sce_names(sys.argv[2])
    log('AI -> DM2')
    md = match_pair(A, D, log)
    log('AI -> BMX XXX')
    mx = match_pair(A, X, log)
    log('DM2 -> BMX XXX (triangle check)')
    mdx = match_pair(D, X, log)
    tri = collections.Counter()
    tri_cls = collections.defaultdict(collections.Counter)
    for a in A.starts:
        if a in md and a in mx:
            d, x = md[a]['addr'], mx[a]['addr']
            ok = 'agree' if d in mdx and mdx[d]['addr'] == x else ('disagree' if d in mdx else 'dm2 unmatched')
            tri[ok] += 1
            worst = max((md[a]['cls'], mx[a]['cls']), key=CLASSES.index)
            tri_cls[worst][ok] += 1
    log('triangle: ' + str(dict(tri)))
    for c in CLASSES:
        log('  worse of the two %-9s %s' % (c, dict(tri_cls[c])))
    res = {}
    for a in A.starts:
        e = dict(name=A.names.get(a), layer=layer_of(a, sdk), n=A.info[a]['n'])
        for key, m, P in (('dm2', md, D), ('bmx', mx, X)):
            if a in m:
                v = dict(m[a])
                v['name'] = P.names.get(v['addr'])
                v['addr'] = '0x%08X' % v['addr']
                e[key] = v
        res['0x%08X' % a] = e
    # per layer table
    table = {}
    for layer in ('sdk', 'engine', 'game', 'all'):
        rows = [e for e in res.values() if layer == 'all' or e['layer'] == layer]
        t = dict(functions=len(rows), words=sum(e['n'] for e in rows))
        for key in ('dm2', 'bmx'):
            c = collections.Counter(e[key]['cls'] if key in e else 'none' for e in rows)
            w = collections.Counter()
            for e in rows:
                w[e[key]['cls'] if key in e else 'none'] += e['n']
            t[key] = dict(count=dict(c), words=dict(w))
        both = sum(1 for e in rows if e.get('dm2', {}).get('cls') == 'identical' and e.get('bmx', {}).get('cls') == 'identical')
        t['identical_in_both'] = both
        table[layer] = t
    for a in A.starts:
        e = res['0x%08X' % a]
        if a in md and a in mx:
            d = md[a]['addr']
            e['triangle'] = d in mdx and mdx[d]['addr'] == mx[a]['addr']
    json.dump(dict(table=table, triangle={k: dict(v) for k, v in tri_cls.items()}, functions=res),
              open(out_path, 'w'), indent=1)
    print_table(table)


def print_table(table):
    cls = ('identical', 'close', 'drifted', 'weak', 'none')
    print('%-7s %6s | %s | %s | %s' % ('layer', 'AI fns', 'DM2: ' + ' '.join('%9s' % c for c in cls),
                                       'BMX XXX: ' + ' '.join('%9s' % c for c in cls), 'ident both'))
    for layer, t in table.items():
        print('%-7s %6d | %s | %s | %d' % (
            layer, t['functions'],
            '     ' + ' '.join('%9d' % t['dm2']['count'].get(c, 0) for c in cls),
            '         ' + ' '.join('%9d' % t['bmx']['count'].get(c, 0) for c in cls),
            t['identical_in_both']))


if __name__ == '__main__':
    main()
