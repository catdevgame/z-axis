"""Second pass: neighbour-constrained candidates for wanted AI functions.

Reads match.json (fmatch.py) for anchors. For each wanted AI function F:
  - votes: aligned jal targets in the DM2 counterparts of F's matched callers
  - candidates: those votes, callees of matched callers' counterparts,
    callers of matched callees' counterparts
  - scores: masked-token ratio, opcode-class ratio, shared strings
Also translates data addresses through all matched function pairs (any score).
Data only.

Usage: fmatch2.py <ai.elf> <ai.csv> <dm2.elf> <dm2.csv> <match.json> <out.json> addr...
"""
import collections, difflib, json, sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
from fmatch import Prog, align_pairs


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


def opseq(p, s):
    ws = p.elf.words(s, p.funcs[s])
    return [opclass(w) for w in ws]


def callers_map(p):
    c = collections.defaultdict(set)
    for s, i in p.info.items():
        for _, t in i['calls']:
            c[t].add(s)
    return c


def main():
    A = Prog(sys.argv[1], sys.argv[2])
    B = Prog(sys.argv[3], sys.argv[4])
    mj = json.load(open(sys.argv[5]))
    match = {int(a, 16): (int(b[0], 16), b[1], b[2]) for a, b in mj['matches'].items()}
    wanted = [int(x, 16) for x in sys.argv[7:]]
    ca, cb = callers_map(A), callers_map(B)
    inv = {v[0]: k for k, v in match.items()}
    ops_cache = {}

    def ops(p, s):
        k = (id(p), s)
        if k not in ops_cache:
            ops_cache[k] = opseq(p, s)
        return ops_cache[k]

    res = {}
    for w in wanted:
        f = A.func_of(w)
        if f is None:
            continue
        off = w - f
        ia = A.info[f]
        votes = collections.Counter()
        cand = set()
        for c in ca.get(f, ()):
            if c not in match:
                continue
            c2 = match[c][0]
            r, pairs = align_pairs(A.info[c], B.info[c2])
            ra, rb = dict(A.info[c]['refs']), dict(B.info[c2]['refs'])
            for x, y in pairs:
                if x in ra and ra[x] == ('call', f) and y in rb and rb[y][0] == 'call':
                    votes[rb[y][1]] += 1
            for _, t in B.info[c2]['calls']:
                cand.add(t)
        for _, t in ia['calls']:
            if t in match:
                for c in cb.get(match[t][0], ()):
                    cand.add(c)
        cand |= set(votes)
        cand = {c for c in cand if c in B.info}
        scored = []
        for s in cand:
            ib = B.info[s]
            r = difflib.SequenceMatcher(None, ia['toks'], ib['toks'], autojunk=False).ratio()
            r2 = difflib.SequenceMatcher(None, ops(A, f), ops(B, s), autojunk=False).ratio()
            st = len(ia['strs'] & ib['strs'])
            # callee overlap: matched callees of F whose counterparts B calls
            ca_f = {match[t][0] for _, t in ia['calls'] if t in match}
            cb_s = {t for _, t in ib['calls']}
            co = len(ca_f & cb_s) / max(1, len(ca_f)) if ca_f else 0.0
            score = 0.35 * r + 0.35 * r2 + 0.2 * co + 0.1 * min(st, 3) / 3 + 0.15 * min(votes[s], 2) / 2
            scored.append((round(score, 3), round(r, 3), round(r2, 3), round(co, 2), st, votes[s], s, ib['n']))
        scored.sort(reverse=True)
        cur = match.get(f)
        res['0x%08X' % w] = dict(
            ai_func='0x%08X' % f, ai_len=ia['n'], ai_strs=sorted(ia['strs'])[:5],
            ai_callers=len(ca.get(f, ())), matched_callers=sum(1 for c in ca.get(f, ()) if c in match),
            first_pass=None if not cur else ['0x%08X' % cur[0], cur[1], cur[2]],
            candidates=[dict(dm2='0x%08X' % (x[6] + off), func='0x%08X' % x[6], score=x[0], tok=x[1], ops=x[2],
                             callees=x[3], strs=x[4], votes=x[5], len=x[7]) for x in scored[:4]])

    # data translation through every matched pair
    dm = collections.defaultdict(collections.Counter)
    for a, (b, how, sc) in match.items():
        ia, ib = A.info[a], B.info[b]
        r, pairs = align_pairs(ia, ib)
        if r < 0.5:
            continue
        ra, rb = dict(ia['refs']), dict(ib['refs'])
        for x, y in pairs:
            if x in ra and y in rb and ra[x][0] == 'data' and rb[y][0] == 'data':
                dm[ra[x][1]][rb[y][1]] += 1
    json.dump(dict(wanted=res, datamap={'0x%08X' % k: [['0x%08X' % b, n] for b, n in v.most_common(3)] for k, v in dm.items()}),
              open(sys.argv[6], 'w'), indent=1)


if __name__ == '__main__':
    main()
