"""Decompiler-feature matcher: TF-IDF cosine over constants, strings and
known callees in Ghidra's decompiled C of both programs. Data only.

Usage: dmatch.py <ai-decomp.c|BUILD> <dm2-decomp.c|BUILD> <match.json> <out.json> addr... [--base]
BUILD (ai_ps2, dm2_ps2, ...): that build's decompilation from tools/exports.py, the readable one
unless --base. A call by a readable name counts as the call to its address (the file's own
function names), so the readable files give the same call features as the base ones.
"""
import collections, json, math, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exports  # noqa: E402

FUNC_RE = re.compile(r'^//// FUNCTION ([0-9a-f]+) (\S+)$', re.M)
HEX_RE = re.compile(r'\b0x([0-9a-f]+)\b')
NUM_RE = re.compile(r'(?<![\w.])(\d+\.\d+|\d{2,})(?![\w.])')
STR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
CALL_RE = re.compile(r'\b(FUN_[0-9a-f]{8}|[A-Za-z_]\w*)\s*\(')
DAT_RE = re.compile(r'\b(?:DAT|PTR|_DAT|uRam|iRam|fRam)_?([0-9a-f]{8})\b')


def parse(path):
    text = open(path, encoding='latin1').read()
    out = {}
    ms = list(FUNC_RE.finditer(text))
    for i, m in enumerate(ms):
        body = text[m.end(): ms[i + 1].start() if i + 1 < len(ms) else len(text)]
        out[int(m.group(1), 16)] = (m.group(2), body)
    return out


def features(body, fmap, own_names):
    f = collections.Counter()
    for s in STR_RE.findall(body):
        f['s:' + s] += 3
    for h in HEX_RE.findall(body):
        v = int(h, 16)
        if 0x100000 <= v < 0x400000:
            continue  # addresses differ
        f['h:%x' % v] += 1
    for n in NUM_RE.findall(body):
        f['n:' + n] += 1
    for c in CALL_RE.findall(body):
        if c.startswith('FUN_'):
            a = int(c[4:], 16)
            if a in fmap:
                f['c:%x' % fmap[a]] += 2
        elif own_names and c in own_names and own_names[c] in fmap:
            f['c:%x' % fmap[own_names[c]]] += 2
        elif c not in ('if', 'while', 'for', 'switch', 'return', 'sizeof') and not c.startswith(('undefined', 'int', 'uint')):
            f['c:' + c] += 2
    return f


def main():
    readable = exports.base_flag(sys.argv)
    ai = parse(exports.resolve(sys.argv[1], 'decomp', readable))
    dm = parse(exports.resolve(sys.argv[2], 'decomp', readable))
    an = {n: a for a, (n, b) in ai.items() if not n.startswith('FUN_')}
    dn = {n: a for a, (n, b) in dm.items() if not n.startswith('FUN_')}
    mj = json.load(open(sys.argv[3]))
    amap = {int(a, 16): int(b[0], 16) for a, b in mj['matches'].items() if b[1] == 'exact' or b[2] >= 0.8}
    ident = {b: b for b in amap.values()}
    fa = {a: features(b, amap, an) for a, (n, b) in ai.items()}
    fd = {a: features(b, ident, dn) for a, (n, b) in dm.items()}
    df = collections.Counter()
    for f in list(fa.values()) + list(fd.values()):
        for k in f:
            df[k] += 1
    N = len(fa) + len(fd)
    idf = {k: math.log(N / (1 + v)) for k, v in df.items()}

    def vec(f):
        v = {k: c * idf[k] for k, c in f.items() if idf[k] > 1.0}
        n = math.sqrt(sum(x * x for x in v.values())) or 1.0
        return v, n
    vd = {a: vec(f) for a, f in fd.items()}
    res = {}
    for w in sys.argv[5:]:
        w = int(w, 16)
        fs = [a for a in ai if a <= w]
        if not fs:
            continue
        a = max(fs)
        va, na = vec(fa[a])
        sc = []
        for d, (vv, nd) in vd.items():
            dot = sum(x * vv.get(k, 0.0) for k, x in va.items())
            if dot:
                sc.append((dot / (na * nd), d))
        sc.sort(reverse=True)
        shared = []
        if sc:
            vv = vd[sc[0][1]][0]
            shared = sorted((k for k in va if k in vv), key=lambda k: -va[k])[:8]
        res['0x%08X' % w] = dict(ai_func='0x%08X' % a, top=[['0x%08X' % d, round(s, 3)] for s, d in sc[:4]], shared=shared)
    json.dump(res, open(sys.argv[4], 'w'), indent=1)
    for k, v in res.items():
        print(k, v['ai_func'], v['top'][:3], v['shared'][:6])


if __name__ == '__main__':
    main()
