#!/usr/bin/env python3
"""DM2 -> BMX XXX PS2: the function match between Dave Mirra BMX 2 (ZGE2) and BMX XXX (ZGE4) for
the work database (MATCH3.md section 6: kind `pair`, which `build` copies into `matches`).

    python -I tools/dm2bmx.py OUT.json RESULTS_DIR [--run NAME]

fmatch3.match_pair on the two PS2 ELFs (MATCH3.md's copies, DM2's current function map), as
`ridermap.py pair` makes it; then a second run seeded with those pairs and the triangle's: a DM2
and a BMX XXX function the first run left unpaired whose AI counterparts (the work database's
match3 links) are the same AI function, identical or close on both sides. The second run's passes
grow from the seeds; its pairs are the result. Each pair keeps fmatch3's class (identical, close,
drifted, weak), method (exact, strings, callgraph, group, order, neighbour, or triangle for a
seed), masked-token ratio, opcode ratio and callee consistency, and the triangle's verdict
(agree: AI's two links name the same two functions; disagree: AI's links name another BMX XXX
function; none: AI has no link to one of them).

Writes OUT.json (every pair) and RESULTS_DIR/dm2-bmx-pairs.jsonl. Inputs are disc derived: both
outputs go to scratch, never to git. (`python` = $ZAXIS_SCRATCH/venv/bin/python.)
"""
import collections, datetime, json, os, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import zpaths  # noqa: E402  (tooling/paths.env)
VENV_PY = os.path.join(zpaths.ZAXIS_SCRATCH, 'venv', 'bin', 'python')
try:
    import duckdb
except ImportError:
    if os.path.exists(VENV_PY):
        os.execv(VENV_PY, [VENV_PY, '-I', os.path.abspath(__file__)] + sys.argv[1:])
    sys.exit('dm2bmx: needs the duckdb module (%s)' % VENV_PY)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fmatch3  # noqa: E402
from fmatch import Prog  # noqa: E402
from ridermap import DM2_ELF, DM2_CSV, BMX_ELF, BMX_CSV, DB  # noqa: E402

CLS_CONF = {'identical': 'high', 'close': 'high', 'drifted': 'medium', 'weak': 'low'}


def h(a):
    return '0x%08X' % a


def generator():
    try:
        c = subprocess.run(['git', '-C', HERE, 'log', '-1', '--format=%h'], capture_output=True,
                           text=True).stdout.strip()
        if c and subprocess.run(['git', '-C', HERE, 'status', '--porcelain', '--', 'dm2bmx.py', 'fmatch3.py'],
                                capture_output=True, text=True).stdout.strip():
            c += '+changes'
    except OSError:
        c = ''
    return 'dm2bmx.py ' + (c or 'uncommitted')


def ai_links(con):
    """{ai: {build: (addr, cls)}} of match3's AI -> DM2 and AI -> BMX XXX links."""
    out = collections.defaultdict(dict)
    for a, b, x, cls in con.execute("SELECT a_addr, b_build, b_addr, cls FROM matches WHERE a_build = 'ai_ps2' "
                                    "AND b_build IN ('dm2_ps2', 'bmx_ps2')").fetchall():
        out[a][b] = (x, cls)
    return out


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    out, results = argv[0], argv[1]
    run = argv[argv.index('--run') + 1] if '--run' in argv else 'dm2bmx'
    log = lambda s: print(s, file=sys.stderr, flush=True)
    con = duckdb.connect(DB, read_only=True)
    links = ai_links(con)
    D = Prog(DM2_ELF, DM2_CSV)
    X = Prog(BMX_ELF, BMX_CSV)
    log('DM2 %d, BMX XXX %d functions; first run' % (len(D.starts), len(X.starts)))
    m1 = fmatch3.match_pair(D, X, log)
    taken = {v['addr'] for v in m1.values()}
    seed = {a: (v['addr'], v['method']) for a, v in m1.items()}
    tri = 0
    for a, d in sorted(links.items()):
        if 'dm2_ps2' not in d or 'bmx_ps2' not in d:
            continue
        (da, dc), (xa, xc) = d['dm2_ps2'], d['bmx_ps2']
        if dc in ('identical', 'close') and xc in ('identical', 'close') and da not in seed and xa not in taken \
                and da in D.info and xa in X.info:
            seed[da] = (xa, 'triangle')
            taken.add(xa)
            tri += 1
    log('%d triangle seeds; second run' % tri)
    m = fmatch3.match_pair(D, X, log, seed=seed)
    via = {}
    for a, d in links.items():
        if 'dm2_ps2' in d and 'bmx_ps2' in d:
            via[d['dm2_ps2'][0]] = d['bmx_ps2'][0]
    res = {}
    for a, v in sorted(m.items()):
        e = dict(v, addr=h(v['addr']))
        if a in via:
            e['triangle'] = 'agree' if via[a] == v['addr'] else 'disagree'
        else:
            e['triangle'] = 'none'
        res[h(a)] = e
    json.dump(res, open(out, 'w'), indent=0)
    gen, ts = generator(), datetime.datetime.now().replace(microsecond=0).isoformat()
    os.makedirs(results, exist_ok=True)
    with open(os.path.join(results, 'dm2-bmx-pairs.jsonl'), 'w') as f:
        for k, v in res.items():
            f.write(json.dumps(dict(kind='pair', build='dm2_ps2', addr=k, b_build='bmx_ps2', b_addr=v['addr'],
                                    run=run, generator=gen, ts=ts, agent=run, method=v['method'], cls=v['cls'],
                                    confidence=CLS_CONF[v['cls']], ratio=v['ratio'], ops=v['ops'], insns=v['n'],
                                    callees_consistent=v.get('callees_consistent'),
                                    triangle=v['triangle'])) + '\n')
    c = collections.Counter(v['cls'] for v in res.values())
    print('%d DM2 -> BMX XXX pairs (first run %d, %d triangle seeds): %s' % (
        len(res), len(m1), tri, ', '.join('%s %d' % (k, c[k]) for k in fmatch3.CLASSES)))
    print('triangle: %s' % dict(collections.Counter(v['triangle'] for v in res.values())))
    print('methods: %s' % dict(collections.Counter(v['method'] for v in res.values())))


if __name__ == '__main__':
    main(sys.argv[1:])
