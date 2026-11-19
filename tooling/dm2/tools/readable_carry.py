#!/usr/bin/env python3
"""How much of PS2Recomp's readable C++ carries over to DM2 and BMX XXX.

    python3 -I tools/readable_carry.py <match3.json> <ai.elf> <ai.csv> <dm2.elf> <dm2.csv> \
        <bmx.elf> <bmx.csv> <z-axis>/runtime/src/game <out.json> [--list]

Reads every READABLE_FUNCTION / _WAITS / _VU0 / READABLE_PLANNED registration
(AI address, name, status) and, for each game, its counterpart from
fmatch3.py's match with what differs:

  identical  address-masked body equal; `table_ok` when every call goes to the
             matched counterpart of AI's callee and every global maps the way
             the rest of the match maps it (then a per-game address table is
             all the readable function needs)
  drifted    matched, bodies differ; the aligned instructions are classed:
               offsets   same instruction, other load/store offset off a non-sp
                         base (struct field offsets)
               stack     same, off sp (frame layout)
               regs      same operation, other registers only
               consts    other immediates (constants, masks, counts)
               globals   a global that maps elsewhere than the rest of the match
               calls     a call to something other than the callee's counterpart,
                         or calls added or dropped
               code      instructions added, dropped or changed in kind
  none       no counterpart

The output holds the games' addresses: write it to scratch, never commit it.
"""
import collections, difflib, json, os, re, sys

sys.path.insert(0, os.path.dirname(__file__))
from fmatch import Prog, LOADSTORE
from fmatch3 import opclass

# Sources name their addresses (PS2Recomp tools/readable_symbols.py): read with the literals written back.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'checks'))
import zlayout  # noqa: E402  (the tree's folders, old or new)
_ZAXIS = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
sys.path.insert(0, os.environ.get('READABLE_TOOLS', zlayout.path(_ZAXIS, 'runtime', 'tools')))
import readable_symbols

REG_RE = re.compile(r'READABLE_(FUNCTION(?:_WAITS|_VU0)?|PLANNED)\(\s*(0x[0-9A-Fa-f]+)\s*,\s*([^,)]+?)\s*(?:,\s*(\w+))?\s*\)')


def registrations(src):
    out = []
    for root, _, files in os.walk(src):
        for f in sorted(files):
            if not f.endswith(('.cpp', '.h')):
                continue
            p = os.path.join(root, f)
            for ln in readable_symbols.literal_view(open(p, encoding='utf-8', errors='replace').read()).split('\n'):
                s = ln.strip()
                if s.startswith(('//', '#', '*')):
                    continue
                m = REG_RE.search(s)
                if not m:
                    continue
                kind, addr, name, status = m.groups()
                out.append(dict(addr=int(addr, 16), name=name.strip('"& '), kind=kind,
                                status=status or 'Planned', file=os.path.relpath(p, src)))
    return out


def datamap(A, B, pairs):
    """AI data address -> most voted B address, over aligned refs of matched pairs;
    and AI callee -> B callee for callees the match left unmatched, when every
    aligned call site agrees."""
    votes = collections.defaultdict(collections.Counter)
    cvotes = collections.defaultdict(collections.Counter)
    for a, b in pairs:
        ia, ib = A.info[a], B.info[b]
        sm = difflib.SequenceMatcher(None, ia['toks'], ib['toks'], autojunk=False)
        if sm.ratio() < 0.5:
            continue
        ra, rb = dict(ia['refs']), dict(ib['refs'])
        for x0, y0, n in sm.get_matching_blocks():
            for k in range(n):
                x, y = x0 + k, y0 + k
                if x in ra and y in rb and ra[x][0] == rb[y][0] == 'data':
                    votes[ra[x][1]][rb[y][1]] += 1
                elif x in ra and y in rb and ra[x][0] == rb[y][0] == 'call':
                    cvotes[ra[x][1]][rb[y][1]] += 1
    return ({k: v.most_common(1)[0][0] for k, v in votes.items()},
            {k: next(iter(v)) for k, v in cvotes.items() if len(v) == 1})


def diff(A, B, a, b, fmap, dmap, cmap):
    ia, ib = A.info[a], B.info[b]
    wa, wb = A.elf.words(a, A.funcs[a]), B.elf.words(b, B.funcs[b])
    ra, rb = dict(ia['refs']), dict(ib['refs'])
    kinds = collections.Counter()
    sm = difflib.SequenceMatcher(None, ia['toks'], ib['toks'], autojunk=False)
    for tag, i0, i1, j0, j1 in sm.get_opcodes():
        if tag == 'equal':
            for k in range(i1 - i0):
                x, y = i0 + k, j0 + k
                if x in ra and y in rb:
                    (ka, va), (kb, vb) = ra[x], rb[y]
                    if ka == 'call' and fmap.get(va) != vb:
                        # an unmatched callee whose call sites all name one B function
                        kinds['callsite' if va not in fmap and cmap.get(va) == vb else 'calls'] += 1
                    elif ka == 'data' and va in dmap and dmap[va] != vb:
                        kinds['globals'] += 1
            continue
        if tag == 'replace' and i1 - i0 == j1 - j0:
            for k in range(i1 - i0):
                x, y = wa[i0 + k], wb[j0 + k]
                if opclass(x) != opclass(y):
                    if (x >> 26) == 3 or (y >> 26) == 3:
                        kinds['calls'] += 1
                    else:
                        kinds['code'] += 1
                    continue
                op = x >> 26
                if (x ^ y) & 0xFFFF0000 == 0:  # only the immediate
                    if op in LOADSTORE:
                        kinds['stack' if (x >> 21) & 31 == 29 else 'offsets'] += 1
                    else:
                        kinds['consts'] += 1
                elif (x ^ y) & 0xFFFF == 0 or op == 0:
                    kinds['regs'] += 1
                else:
                    kinds['regs' if op in LOADSTORE and (x & 0xFFFF) == (y & 0xFFFF) else 'consts'] += 1
            continue
        calls = sum(1 for k in range(i0, i1) if (wa[k] >> 26) == 3) + sum(1 for k in range(j0, j1) if (wb[k] >> 26) == 3)
        if calls:
            kinds['calls'] += calls
        kinds['code'] += max(i1 - i0, j1 - j0) - calls
    return kinds


def main():
    args = [x for x in sys.argv[1:] if not x.startswith('--')]
    mj = json.load(open(args[0]))['functions']
    A = Prog(args[1], args[2])
    games = dict(dm2=Prog(args[3], args[4]), bmx=Prog(args[5], args[6]))
    regs = registrations(args[7])
    out_path = args[8]
    print('registrations', len(regs), collections.Counter(r['status'] for r in regs), file=sys.stderr)
    fmaps, dmaps, cmaps = {}, {}, {}
    for g, B in games.items():
        fm = {int(a, 16): int(e[g]['addr'], 16) for a, e in mj.items() if g in e}
        fmaps[g] = fm
        dmaps[g], cmaps[g] = datamap(A, B, fm.items())
    res = []
    for r in regs:
        f = A.func_of(r['addr'])
        e = dict(r, ai='0x%08X' % r['addr'], func_start=f == r['addr'], n=A.info[f]['n'] if f else 0)
        del e['addr']
        if f is None:
            e['error'] = 'not in the AI function map'
            res.append(e)
            continue
        m = mj['0x%08X' % f]
        e['layer'] = m['layer']
        for g, B in games.items():
            if g not in m:
                e[g] = dict(cls='none')
                continue
            b = int(m[g]['addr'], 16)
            k = diff(A, B, f, b, fmaps[g], dmaps[g], cmaps[g])
            d = dict(addr=m[g]['addr'], ratio=m[g]['ratio'], method=m[g]['method'])
            if m[g]['cls'] == 'identical':
                d['cls'] = 'identical'
                d['table_ok'] = not k
                d['table_ok_callsite'] = set(k) <= {'callsite'}
                if k:
                    d['differs'] = dict(k)
            else:
                d['cls'] = 'drifted' if m[g]['cls'] in ('close', 'drifted') else 'weak'
                d['match_cls'] = m[g]['cls']
                d['differs'] = dict(k)
            e[g] = d
        res.append(e)
    json.dump(res, open(out_path, 'w'), indent=1)
    summary(res, '--list' in sys.argv)


def summary(res, listing):
    groups = collections.OrderedDict()
    for st in ('Verified', 'Tested', 'Draft', 'Planned'):
        groups[st] = [r for r in res if r['status'] == st]
    groups['readable (not Planned)'] = [r for r in res if r['status'] != 'Planned']
    groups['all registered'] = res
    print('%-24s %5s | %-46s | %-46s' % ('status', 'n', 'DM2: ident (table ok/+callsite) drifted weak none', 'BMX XXX: same'))
    for name, rows in groups.items():
        cells = []
        for g in ('dm2', 'bmx'):
            c = collections.Counter(r[g]['cls'] if g in r else 'none' for r in rows)
            ok = sum(1 for r in rows if g in r and r[g].get('table_ok'))
            ok2 = sum(1 for r in rows if g in r and r[g].get('table_ok_callsite'))
            cells.append('%5d (%4d/%4d) %7d %5d %5d' % (c['identical'], ok, ok2, c['drifted'], c['weak'], c['none']))
        print('%-24s %5d | %-46s | %-46s' % (name, len(rows), cells[0], cells[1]))
    for layer in ('engine', 'game', 'sdk'):
        rows = [r for r in res if r.get('layer') == layer and r['status'] != 'Planned']
        cells = []
        for g in ('dm2', 'bmx'):
            c = collections.Counter(r[g]['cls'] if g in r else 'none' for r in rows)
            ok = sum(1 for r in rows if g in r and r[g].get('table_ok'))
            ok2 = sum(1 for r in rows if g in r and r[g].get('table_ok_callsite'))
            cells.append('%5d (%4d/%4d) %7d %5d %5d' % (c['identical'], ok, ok2, c['drifted'], c['weak'], c['none']))
        print('%-24s %5d | %-46s | %-46s' % ('readable, ' + layer, len(rows), cells[0], cells[1]))
    for g in ('dm2', 'bmx'):
        k = collections.Counter()
        n = collections.Counter()
        for r in res:
            if r['status'] != 'Planned' and g in r and r[g]['cls'] == 'drifted':
                for kk in r[g]['differs']:
                    n[kk] += 1
                k[tuple(sorted(r[g]['differs']))] += 1
        print(g, 'drifted readable functions with each kind of difference:', dict(n.most_common()))
    if listing:
        for g in ('dm2', 'bmx'):
            print('\n== identical in', g)
            for r in res:
                if g in r and r[g]['cls'] == 'identical' and r['status'] != 'Planned':
                    print('  %s %-40s %-9s %s%s' % (r['ai'], r['name'], r['status'], r[g]['addr'],
                                                   '' if r[g]['table_ok'] else '  (%s)' % r[g].get('differs')))


if __name__ == '__main__':
    main()
