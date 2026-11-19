#!/usr/bin/env python3
"""DM2's rider code mapped for the rider physics port: the functions the
rider's frame, its state tables and the physics task reach, with their counterparts in DM2's Xbox
and GameCube builds and in BMX XXX's three, and what each is for (tools/ridermap_roles.txt).

    python -I tools/ridermap.py pair OUT.json            DM2 -> BMX XXX PS2 match (fmatch3's passes)
    python -I tools/ridermap.py map PAIRS.json RESULTS_DIR [--table FILE.md] [--run NAME]

`pair` runs fmatch3.match_pair on the two PS2 ELFs (MATCH3.md's copies; fmatch3.py makes this
match only as its triangle check and keeps none of it) and writes every pair. `map` reads the
work database (read-only), the DM2 ELF's state tables, the pairs and the roles file, and writes
two result files the work database ingests (MATCH3.md section 6, kinds `map` and `pair`):
RESULTS_DIR/rider-map.jsonl (one line per rider function) and RESULTS_DIR/dm2-bmx-pairs.jsonl
(every DM2 -> BMX XXX pair). `--table` writes the map as a readable table.

Inputs are disc derived (the ELF copies, the database): outputs go to scratch, never to git.
(`python` = $ZAXIS_SCRATCH/venv/bin/python, which has duckdb; re-run with it otherwise.)
"""
import collections, datetime, json, os, struct, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import zpaths  # noqa: E402  (tooling/paths.env)
VENV_PY = os.path.join(zpaths.ZAXIS_SCRATCH, 'venv', 'bin', 'python')
try:
    import duckdb
except ImportError:
    if os.path.exists(VENV_PY):
        os.execv(VENV_PY, [VENV_PY, '-I', os.path.abspath(__file__)] + sys.argv[1:])
    sys.exit('ridermap: needs the duckdb module (%s)' % VENV_PY)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
HOME = os.path.expanduser('~')
SCRATCH = os.environ.get('AI_SCRATCH', zpaths.ZAXIS_SCRATCH)
DB = os.environ.get('WORKDB', SCRATCH + '/workdb/work.duckdb')
M3 = SCRATCH + '/match3'
DM2_ELF = M3 + '/elf/SLUS_201.59'
DM2_CSV = os.environ.get('DM2_CSV', os.path.join(zpaths.DM2_WORKSPACE, 'recomp', 'ghidra-functions.csv'))
BMX_ELF = M3 + '/elf/SLUS_204.15'
BMX_CSV = M3 + '/ghidra-bmxxxx/out/ghidra-functions.csv'
ROLES = os.path.join(HERE, 'ridermap_roles.txt')
GAME = (0x150000, 0x1C2728)  # DM2's game layer (MATCH3.md section 4's bounds)

# The roots: the rider setup, the action frame, the rider frame and the physics state change, and
# every callback of the state tables: physics exits, enters, turn,
# forces, move, after (11 states each), the action states, the riding and air sub-states.
ROOTS = (0x1A2348, 0x1643D8, 0x1A33F0, 0x192180, 0x191DC0)
TABLES = ((0x241D80, 12), (0x241DB0, 12), (0x241DE0, 11), (0x241E0C, 11), (0x241E38, 11),
          (0x241E64, 11), (0x241E90, 8), (0x241EB0, 6), (0x259530, 9))
# What PS2Recomp's dm2rider2 round ported (runtime/dm2_rider.h dr::MotionPicker, removed 2026-11-05): the motion choice
# of these, driven by AI's skater; not their physics, and not as registered readable functions.
MOTION_CHOICE = {0x1A2348, 0x1A33F0, 0x1A3C50, 0x1A41D0, 0x1A4620, 0x1A5240, 0x1A5360, 0x1A5498,
                 0x1A56E8, 0x1A5A00, 0x1A5E68, 0x1A65A8, 0x1A6808, 0x1A6F18, 0x1A7470, 0x1A7520,
                 0x1A8230, 0x1A8918, 0x1A9168, 0x1A9A90, 0x1A9B68, 0x1A9C90, 0x1A9D30}
CONSTANT_ONLY = {0x19A110}  # the hard landing's threshold (0x2593A0 x 0x254DA8)
CLS_CONF = {'identical': 'high', 'close': 'high', 'drifted': 'medium', 'weak': 'low'}
RANK = {'high': 3, 'medium': 2, 'low': 1}


def h(a):
    return '0x%08X' % a


def generator():
    try:
        c = subprocess.run(['git', '-C', HERE, 'log', '-1', '--format=%h'], capture_output=True,
                           text=True).stdout.strip()
    except OSError:
        c = ''
    return 'ridermap.py ' + (c or 'uncommitted')


def cmd_pair(argv):
    import fmatch3
    from fmatch import Prog
    out = argv[0]
    D = Prog(DM2_ELF, DM2_CSV)
    X = Prog(BMX_ELF, BMX_CSV)
    m = fmatch3.match_pair(D, X, lambda s: print(s, file=sys.stderr, flush=True))
    res = {h(a): dict(v, addr=h(v['addr'])) for a, v in m.items()}
    json.dump(res, open(out, 'w'), indent=0)
    print('%d DM2 -> BMX XXX pairs: %s' % (len(res), dict(collections.Counter(v['cls'] for v in res.values()))))


def read_roles():
    roles = {}
    for ln in open(ROLES):
        if ln.startswith('#') or not ln.strip():
            continue
        a, role, name, port, conf, what = ln.rstrip('\n').split('|', 5)
        roles[int(a, 16)] = dict(role=role, name=name, port=port, confidence=conf, what=what)
    return roles


def cmd_map(argv):
    pairs_path, results = argv[0], argv[1]
    table_out = argv[argv.index('--table') + 1] if '--table' in argv else None
    run = argv[argv.index('--run') + 1] if '--run' in argv else 'riderplan'
    con = duckdb.connect(DB, read_only=True)
    elf = open(DM2_ELF, 'rb').read()
    word = lambda va: struct.unpack_from('<I', elf, va - 0x100000 + 0x1000)[0]
    F = collections.defaultdict(dict)
    for b, a, size, insns in con.execute("SELECT build, addr, bytes, insns FROM functions").fetchall():
        F[b][a] = (size, insns)
    callees = collections.defaultdict(list)
    callers = collections.defaultdict(list)
    for c, t in con.execute("SELECT caller, callee FROM calls WHERE build = 'dm2_ps2' "
                            "ORDER BY caller, call_offset").fetchall():
        callees[c].append(t)
        callers[t].append(c)
    M = collections.defaultdict(dict)
    for ab, aa, bb, ba, conf, meth in con.execute(
            "SELECT a_build, a_addr, b_build, b_addr, confidence, method FROM matches").fetchall():
        M[(ab, aa)][bb] = (ba, conf, meth)
    ai_of = {d['dm2_ps2'][0]: (a, d['dm2_ps2'][1]) for (ab, a), d in M.items()
             if ab == 'ai_ps2' and 'dm2_ps2' in d}
    rd = {a: (n, s) for a, n, s in con.execute("SELECT addr, name, status FROM readable").fetchall()}
    bn = {a: n for a, n in con.execute("SELECT addr, name FROM best_names WHERE build = 'ai_ps2'").fetchall()}
    names = collections.defaultdict(list)
    for b, a, n, src in con.execute("SELECT build, addr, name, source FROM names").fetchall():
        names[(b, a)].append('%s (%s %s)' % (n, b, src))
    strs = collections.defaultdict(list)
    for b, f, t in con.execute("SELECT build, func, text FROM func_strings "
                               "WHERE build IN ('dm2_ps2', 'bmx_ps2')").fetchall():
        strs[(b, f)].append(t)
    pairs = json.load(open(pairs_path))

    roots = list(ROOTS) + [word(base + 4 * i) for base, n in TABLES for i in range(n)]
    seen, stack = set(), [r for r in roots if r in F['dm2_ps2']]
    while stack:
        f = stack.pop()
        if f in seen or not (GAME[0] <= f < GAME[1]):
            continue
        seen.add(f)
        stack.extend(callees.get(f, []))
    roles = read_roles()
    missing = sorted(seen - set(roles))
    if missing:
        print('no role for: ' + ' '.join(h(a) for a in missing), file=sys.stderr)

    gen, ts = generator(), datetime.datetime.now().strftime('%Y-%m-%dT%H:%M:%S')
    os.makedirs(results, exist_ok=True)
    rows = []
    for a in sorted(seen):
        r = roles.get(a, dict(role='', name='', port='', confidence='', what=''))
        cp = {}
        for b in ('dm2_xbox', 'dm2_gc'):
            if b in M[('dm2_ps2', a)]:
                ba, conf, meth = M[('dm2_ps2', a)][b]
                cp[b] = dict(addr=h(ba), confidence=conf, method=meth)
        x = pairs.get(h(a))
        if x:
            xa, xc = int(x['addr'], 16), CLS_CONF[x['cls']]
            cp['bmx_ps2'] = dict(addr=x['addr'], confidence=xc, method=x['method'], cls=x['cls'],
                                 ratio=x['ratio'])
            for b in ('bmx_xbox', 'bmx_gc'):
                if b in M[('bmx_ps2', xa)]:
                    ba, conf, meth = M[('bmx_ps2', xa)][b]
                    lo = conf if RANK[conf] <= RANK[xc] else xc
                    cp[b] = dict(addr=h(ba), confidence=lo, method=meth, via='bmx_ps2')
        if a in ai_of:
            aa, conf = ai_of[a]
            cp['ai_ps2'] = dict(addr=h(aa), confidence=conf, name=(rd.get(aa) or (bn.get(aa), None))[0],
                                status=(rd.get(aa) or (None, None))[1])
        nm = [r['name']] if r['name'] else []
        for b, d in cp.items():
            nm += names.get((b, int(d['addr'], 16)), [])
        nm += names.get(('dm2_ps2', a), [])
        engine = [t for t in dict.fromkeys(callees.get(a, [])) if t < GAME[0]]
        covered = 'motion choice' if a in MOTION_CHOICE else ('constant' if a in CONSTANT_ONLY else '')
        rec = dict(kind='map', build='dm2_ps2', addr=h(a), run=run, generator=gen, ts=ts,
                   agent='riderplan', name=r['name'], role=r['role'], port=r['port'],
                   confidence=r['confidence'], module=module_of(a), covered=covered,
                   size=F['dm2_ps2'][a][0], insns=F['dm2_ps2'][a][1],
                   callers=len(set(callers.get(a, []))), callees=len(set(callees.get(a, []))),
                   note=r['what'], counterparts=cp, name_candidates=nm,
                   caller_list=[h(c) for c in dict.fromkeys(callers.get(a, []))],
                   callee_list=[h(c) for c in dict.fromkeys(callees.get(a, []))],
                   engine_callees=[dict(addr=h(t), ai=h(ai_of[t][0]) if t in ai_of else None,
                                        ai_name=(rd.get(ai_of[t][0]) or (bn.get(ai_of[t][0]),))[0]
                                        if t in ai_of else None) for t in engine],
                   bmx_strings=strs.get(('bmx_ps2', int(x['addr'], 16)), [])[:12] if x else [])
        rows.append(rec)
    with open(os.path.join(results, 'rider-map.jsonl'), 'w') as f:
        for rec in rows:
            f.write(json.dumps(rec) + '\n')
    with open(os.path.join(results, 'dm2-bmx-pairs.jsonl'), 'w') as f:
        for k in sorted(pairs):
            v = pairs[k]
            f.write(json.dumps(dict(kind='pair', build='dm2_ps2', addr=k, b_build='bmx_ps2', b_addr=v['addr'],
                                    run=run, generator=gen, ts=ts, agent='riderplan', method=v['method'],
                                    cls=v['cls'], confidence=CLS_CONF[v['cls']], ratio=v['ratio'])) + '\n')
    summary(rows)
    if table_out:
        write_table(rows, table_out)


# DM2's game modules by the __FILE__ anchors (MATCH3.md section 3): a function belongs to the
# module whose anchor is the last at or before it; physics.c's state tables (.data 0x241D80 ..
# 0x241EC4) hold the action states too, so physics.c runs to the rider's trick tables.
ANCHORS = ((0x150000, 'zgui.c (engine end)'), (0x163468, 'action.c'), (0x168378, 'camera.c'),
           (0x16E3B0, 'fx.c'), (0x173110, 'game.c'), (0x186580, 'overlay.c'),
           (0x1907B0, 'physics.c'), (0x1AA000, 'sound and surface (no anchor)'),
           (0x1B3820, 'zoverlay.c'), (0x1B81E0, 'editGui.c'))


def module_of(a):
    m = ''
    for start, name in ANCHORS:
        if a >= start:
            m = name
    return m


def summary(rows):
    c = collections.Counter
    print('%d functions, %d bytes' % (len(rows), sum(r['size'] for r in rows)))
    print('by port: %s' % dict(c(r['port'] for r in rows)))
    for b in ('dm2_xbox', 'dm2_gc', 'bmx_ps2', 'bmx_xbox', 'bmx_gc', 'ai_ps2'):
        k = c(r['counterparts'][b]['confidence'] for r in rows if b in r['counterparts'])
        p = c(r['counterparts'][b]['confidence'] for r in rows if b in r['counterparts'] and r['port'] == 'port')
        print('%-8s all %3d %s | port %3d %s' % (b, sum(k.values()), dict(k), sum(p.values()), dict(p)))
    print('covered by the motion-choice port: %d' % sum(1 for r in rows if r['covered']))


def cp_cell(r, b):
    d = r['counterparts'].get(b)
    if not d:
        return '-'
    return '%s %s' % (d['addr'][2:].lstrip('0').upper().rjust(6, '0') if b.endswith('ps2') else d['addr'][2:].lstrip('0').upper(), d['confidence'][0])


def write_table(rows, out):
    order = ['setup', 'frame', 'state', 'physics', 'input', 'tricks', 'ground', 'crouch', 'manual', 'fakie',
             'air', 'landing', 'grind', 'stall', 'lip', 'wallride', 'bail', 'respawn', 'contact', 'collision',
             'surface', 'pose', 'sound', 'fx', 'camera', 'hud', 'scoring', 'goals', 'replay', 'debug', 'util']
    rows = sorted(rows, key=lambda r: (order.index(r['role']) if r['role'] in order else 99, int(r['addr'], 16)))
    with open(out, 'w') as f:
        f.write('| DM2 | bytes | role | name | port | in / out | DM2 Xbox | DM2 GC | BMX XXX PS2 | BMX Xbox | BMX GC | covered | what |\n')
        f.write('|---|---|---|---|---|---|---|---|---|---|---|---|---|\n')
        for r in rows:
            f.write('| %s | %d | %s | %s | %s | %d / %d | %s | %s | %s | %s | %s | %s | %s |\n' % (
                r['addr'][4:], r['size'], r['role'], r['name'], r['port'], r['callers'], r['callees'],
                cp_cell(r, 'dm2_xbox'), cp_cell(r, 'dm2_gc'), cp_cell(r, 'bmx_ps2'), cp_cell(r, 'bmx_xbox'),
                cp_cell(r, 'bmx_gc'), r['covered'] or '', r['note'].replace('|', '/')))


if __name__ == '__main__':
    cmds = dict(pair=cmd_pair, map=cmd_map)
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        sys.exit(__doc__)
    cmds[sys.argv[1]](sys.argv[2:])
