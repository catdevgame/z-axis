#!/usr/bin/env python3
"""Module map of Aggressive Inline's executable: which source file (object) each
function comes from, from the leftover build information of the three games on
the engine, carried to AI's addresses through fmatch3.py's match. Data only;
the output holds the games' addresses: write it to scratch, never commit it.

    python3 -I tools/modmap.py <match3.json> [--readable readable-carry.json] \
        <ai.elf> <ai.csv> <ai sce-matches.csv> <dm2.elf> <dm2.csv> <dm2 sce-matches.csv> \
        <bmx.elf> <bmx.csv> <bmx sce-matches.csv> <out.json> [xbe-paths.json ...]

Evidence, strongest first:
  file      the function references a __FILE__ path string (assert / error
            report), in AI or in its DM2 or BMX XXX counterpart
  sdk       the SCE signature scan names it (library from the scan)
  between   it lies between two functions of the same module in AI's text: the
            linker keeps an object's .text contiguous, so it is that module too
  block     it is in one block with an anchored function. Blocks: functions
            that must come from one object, in any of the three games, carried
            to AI through the match. Two functions are linked when they use the
            same data item (any of .data .sdata .bss .sbss .rodata) or one calls
            the other, with every user within W bytes of text (local: statics
            and a module's own globals; farther users mean an extern); a link
            covers the text between them (an object's .text is contiguous).
            Adjacent blocks whose local data is out of link order (median of a
            section higher in the first) are one object too. A block holding
            anchors of two modules is a clash and gets no module.
  gap       unresolved: between X and Y; candidates are the objects the DM2
            link-map fragment lists between X and Y (when both are in it)

DM2's link-map fragment (SLUS_201.59's ELF header padding, file 0x51-0x1000):
`.rodata` of about 33 engine objects in link order (another build's addresses).
Its order is checked against the anchored modules' order in each executable.
The Xbox builds' __FILE__ paths (tools/xbe_paths.py) only add module names;
their .rdata order is not the PS2 link order.
"""
import bisect, collections, csv, json, os, re, sys

sys.path.insert(0, os.path.dirname(__file__))
from fmatch import Prog

PATH_RE = re.compile(r'^(?:\.\./|\./|[A-Za-z]:\\|\\).*\.(?:c|cpp)$', re.I)
FRAG_RE = re.compile(rb'\.rodata\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+\S*?([A-Za-z0-9_]+)\.o')
EXTERN_SPAN = 0x20000  # .rodata used farther apart than this is an extern table (ctype etc.)
W = 0x3000  # link window (bytes of text): no anchor clash in AI or DM2 at this size
SDK_START = 0x2021E0  # AI's SDK libraries (fmatch3.py's layers)
DATA_SECS = ('.rodata', '.data', '.sdata', '.bss', '.sbss')


def modname(path):
    p = path.replace('\\', '/')
    base = os.path.splitext(os.path.basename(p))[0]
    return base


def modpath(path):
    p = path.replace('\\', '/')
    for k in ('ngengine/', 'NgEngine/'):
        if k in p:
            p = p[p.index(k) + len(k):]
    p = re.sub(r'^(ZGE\d|Zge\d)/', 'zge/', p, flags=re.I)
    return p.lstrip('./')


def fragment(dm2_elf):
    d = open(dm2_elf, 'rb').read()[0x51:0x1000]
    objs = []
    head = re.match(rb'\s*\\?([A-Za-z0-9_]+)\.o', d)
    if head:
        objs.append(head.group(1).decode())
    for m in FRAG_RE.finditer(d):
        objs.append(m.group(3).decode())
    return objs


def sce(csv_path):
    out = {}
    for r in csv.DictReader(open(csv_path)):
        out[int(r['Address'], 16)] = (r['Name'], r['Library'])
    return out


class Game:
    def __init__(self, elf, fcsv, scsv):
        self.P = Prog(elf, fcsv)
        self.sce = sce(scsv)
        e = self.P.elf
        ro = e.section('.rodata')
        self.ro = (ro['addr'], ro['addr'] + ro['size'])
        self.anchor = {}   # function -> module (from a path string)
        self.path_ro = {}  # module -> rodata address of its path string
        self.paths = {}
        for f, i in self.P.info.items():
            for _, (k, a) in i['refs']:
                if k != 'data':
                    continue
                s = self.P.string_at(a)
                if s and PATH_RE.match(s):
                    m = modname(s)
                    self.anchor[f] = m
                    self.paths[m] = modpath(s)
                    self.path_ro[m] = min(self.path_ro.get(m, a), a)
        users = collections.defaultdict(set)
        for f, i in self.P.info.items():
            for _, (k, a) in i['refs']:
                if k == 'data' and self.ro[0] <= a < self.ro[1]:
                    users[a].add(f)
        self.rodata = collections.defaultdict(list)  # function -> own .rodata addresses
        for a, u in users.items():
            if max(u) - min(u) > EXTERN_SPAN:
                continue
            for f in u:
                self.rodata[f].append(a)


def blocks(P):
    """Segment ids per function index: see `block` in the docstring."""
    import statistics
    secs = {sc['name']: (sc['addr'], sc['addr'] + sc['size']) for sc in P.elf.sections if sc['name'] in DATA_SECS}

    def sec(a):
        for n, (lo, hi) in secs.items():
            if lo <= a < hi:
                return n
        return None
    users = collections.defaultdict(set)
    for f, i in P.info.items():
        for _, (k, a) in i['refs']:
            if k == 'data' and sec(a):
                users[a].add(f)
    callers = collections.defaultdict(set)
    for f, i in P.info.items():
        for _, t in i['calls']:
            if t in P.info:
                callers[t].add(f)
    idx = {f: k for k, f in enumerate(P.starts)}
    N = len(P.starts)
    cover = [0] * (N + 1)
    local = collections.defaultdict(list)

    def link(fs):
        ks = sorted(idx[f] for f in fs)
        cover[ks[0] + 1] += 1
        cover[ks[-1] + 1] -= 1
    for a, u in users.items():
        if max(u) - min(u) <= W:
            for f in u:
                local[f].append((sec(a), a))
            if len(u) > 1:
                link(u)
    for t, c in callers.items():
        u = c | {t}
        if max(u) - min(u) <= W:
            link(u)
    bl, run = [[0]], 0
    for k in range(1, N):
        run += cover[k]
        if run == 0:
            bl.append([k])
        else:
            bl[-1].append(k)

    def data(b):
        d = collections.defaultdict(list)
        for k in b:
            for sn, a in local.get(P.starts[k], ()):
                d[sn].append(a)
        return d
    changed = True
    while changed:
        changed = False
        out = [bl[0]]
        for b in bl[1:]:
            da, db = data(out[-1]), data(b)
            if any(sn in db and statistics.median(da[sn]) > statistics.median(db[sn]) for sn in da):
                out[-1] = out[-1] + b
                changed = True
            else:
                out.append(b)
        bl = out
    seg = [0] * N
    for i, b in enumerate(bl):
        for k in b:
            seg[k] = i
    return seg


def main():
    args = [a for a in sys.argv[1:]]
    readable = None
    if '--readable' in args:  # readable_carry.py's output: readable names and folders per block
        k = args.index('--readable')
        readable = json.load(open(args[k + 1]))
        del args[k:k + 2]
    mj = json.load(open(args[0]))['functions']
    AI = Game(args[1], args[2], args[3])
    DM = Game(args[4], args[5], args[6])
    BX = Game(args[7], args[8], args[9])
    out_path = args[10]
    xbe = [json.load(open(p)) for p in args[11:]]
    frag = fragment(args[4])
    P = AI.P
    starts = P.starts
    mod = {}     # AI function -> (module, how)
    paths = dict(AI.paths)
    for g in (DM, BX):
        for k, v in g.paths.items():
            paths.setdefault(k, v)

    # 1. file anchors in AI and through the match from DM2 and BMX XXX
    conflicts = []
    for f, m in AI.anchor.items():
        mod[f] = (m, 'file')
    for key, g in (('dm2', DM), ('bmx', BX)):
        for a, e in mj.items():
            if key not in e or e[key]['cls'] == 'weak':
                continue
            b = int(e[key]['addr'], 16)
            if b in g.anchor:
                a = int(a, 16)
                if a in mod and mod[a][0] != g.anchor[b]:
                    conflicts.append(('0x%08X' % a, mod[a][0], key, g.anchor[b]))
                    continue
                mod.setdefault(a, (g.anchor[b], 'file'))
    # 2. SDK libraries from the scan (AI's own, then the others' through identical matches)
    for a, (n, lib) in AI.sce.items():
        if a in P.info and a not in mod:
            mod[a] = ('sdk:' + lib, 'sdk')
    for key, g in (('dm2', DM), ('bmx', BX)):
        for a, e in mj.items():
            a = int(a, 16)
            if key in e and e[key]['cls'] in ('identical', 'close') and a not in mod:
                b = int(e[key]['addr'], 16)
                if b in g.sce:
                    mod[a] = ('sdk:' + g.sce[b][1], 'sdk')

    def fill_between():
        n = 0
        known = [f for f in starts if f in mod]
        for x, y in zip(known, known[1:]):
            if mod[x][0] != mod[y][0] or (mod[x][1] == 'sdk' and x < SDK_START):
                continue
            i, j = bisect.bisect_right(starts, x), bisect.bisect_left(starts, y)
            for f in starts[i:j]:
                if f not in mod:
                    mod[f] = (mod[x][0], 'between')
                    n += 1
        return n

    # blocks: AI's own, and the other games' carried through the match
    idx = {f: k for k, f in enumerate(starts)}
    N = len(starts)
    cover = [0] * (N + 1)
    segA = blocks(P)
    for k in range(1, N):
        if segA[k] == segA[k - 1]:
            cover[k] += 1
            cover[k + 1] -= 1
    carried = 0
    for key, g in (('dm2', DM), ('bmx', BX)):
        segB = blocks(g.P)
        bidx = {f: k for k, f in enumerate(g.P.starts)}
        groups = collections.defaultdict(list)
        for a, e in mj.items():
            if key in e and e[key]['cls'] != 'weak':
                groups[segB[bidx[int(e[key]['addr'], 16)]]].append(int(a, 16))
        for fs in groups.values():
            if len(fs) > 1 and max(fs) - min(fs) <= W:
                ks = sorted(idx[f] for f in fs)
                cover[ks[0] + 1] += 1
                cover[ks[-1] + 1] -= 1
                carried += 1
    seg, run, sid = [0] * N, 0, 0
    for k in range(1, N):
        run += cover[k]
        if run == 0:
            sid += 1
        seg[k] = sid
    members = collections.defaultdict(list)
    for k in range(N):
        members[seg[k]].append(starts[k])
    clashes = []

    def block_pass():
        n = 0
        for sidx, fs in members.items():
            # SCE names inside the engine and game are small chance hits: they never seed a block
            ms = {mod[f][0] for f in fs if f in mod and (mod[f][1] == 'file' or (mod[f][1] == 'sdk' and f >= SDK_START))}
            if len(ms) > 1:
                if sidx not in [c[0] for c in clashes]:
                    clashes.append((sidx, sorted(ms), '0x%08X' % fs[0], '0x%08X' % fs[-1]))
                continue
            if len(ms) == 1:
                m = ms.pop()
                for f in fs:
                    if f not in mod:
                        mod[f] = (m, 'block')
                        n += 1
        return n

    rounds = []
    for _ in range(10):
        b = block_pass()
        a = fill_between()
        rounds.append((a, b))
        if not (a or b):
            break

    # module order in AI's text vs the fragment's
    order = []
    for f in starts:
        if f in mod and mod[f][1] in ('file', 'between', 'block') and (not order or order[-1] != mod[f][0]):
            order.append(mod[f][0])
    fl = [x.lower() for x in frag]
    in_frag = [m for m in order if m.lower() in fl]
    frag_pos = [fl.index(m.lower()) for m in in_frag]
    order_ok = all(x <= y for x, y in zip(frag_pos, frag_pos[1:]))

    # gaps: name candidates from the fragment
    res = {}
    known = [f for f in starts if f in mod]
    for f in starts:
        e = dict(name=P.names.get(f), n=P.info[f]['n'], layer=mj['0x%08X' % f]['layer'], block=seg[idx[f]])
        if f in mod:
            e['module'], e['how'] = mod[f]
            e['path'] = paths.get(mod[f][0])
        else:
            k = bisect.bisect_left(known, f)
            mx = mod[known[k - 1]][0] if k > 0 else None
            my = mod[known[k]][0] if k < len(known) else None
            e['how'] = 'gap'
            e['between'] = [mx, my]
            if mx and my and mx.lower() in fl and my.lower() in fl:
                i, j = fl.index(mx.lower()), fl.index(my.lower())
                if i < j:
                    e['candidates'] = frag[i:j + 1]
        res['0x%08X' % f] = e

    # summary
    by_layer = collections.defaultdict(collections.Counter)
    words = collections.defaultdict(collections.Counter)
    for e in res.values():
        by_layer[e['layer']][e['how']] += 1
        words[e['layer']][e['how']] += e['n']
    mods = collections.OrderedDict()
    for f in starts:
        e = res['0x%08X' % f]
        if 'module' in e:
            m = mods.setdefault(e['module'], dict(path=e.get('path'), first='0x%08X' % f, last='0x%08X' % f,
                                                  functions=0, words=0, how=collections.Counter()))
            m['last'] = '0x%08X' % f
            m['functions'] += 1
            m['words'] += e['n']
            m['how'][e['how']] += 1
    xbe_names = sorted({modname(r['path']) for x in xbe for r in x})
    blk = collections.OrderedDict()
    for f in starts:
        e = res['0x%08X' % f]
        b = blk.setdefault(e['block'], dict(first='0x%08X' % f, last='0x%08X' % f, functions=0, words=0,
                                            layer=e['layer'], modules=collections.Counter()))
        b['last'] = '0x%08X' % f
        b['functions'] += 1
        b['words'] += e['n']
        b['modules'][e.get('module', '?')] += 1
    if readable:
        for r in readable:
            e = res.get(r['ai'])
            if e and r['status'] != 'Planned':
                b = blk[e['block']]
                b.setdefault('readable', []).append(r['name'])
                b.setdefault('folders', collections.Counter())[os.path.dirname(r['file'])] += 1
    blocks_out = {str(k): dict(v, modules=dict(v['modules']), folders=dict(v.get('folders', {})))
                  for k, v in blk.items()}
    multi = collections.Counter(v['layer'] for v in blk.values() if v['functions'] > 1)
    named = collections.Counter(v['layer'] for v in blk.values() if v['functions'] > 1 and set(v['modules']) != {'?'})
    json.dump(dict(fragment=frag, order=order, fragment_order_kept=order_ok, conflicts=conflicts,
                   rounds=rounds, blocks=sid + 1, carried_links=carried,
                   clashes=clashes, coverage={k: dict(v) for k, v in by_layer.items()},
                   coverage_words={k: dict(v) for k, v in words.items()},
                   modules={k: dict(v, how=dict(v['how'])) for k, v in mods.items()},
                   xbe_modules=xbe_names, block_ranges=blocks_out, functions=res), open(out_path, 'w'), indent=1)
    print('fragment objects (%d): %s' % (len(frag), ' '.join(frag)))
    print('anchored module order in AI text:', ' '.join(order))
    print('fragment order kept by the anchored modules:', order_ok, '(%d of them in the fragment)' % len(in_frag))
    print('anchor conflicts:', len(conflicts), conflicts[:5])
    print('AI blocks %d (links carried from DM2 / BMX XXX %d); clashes %d %s' % (sid + 1, carried, len(clashes), clashes[:6]))
    print('fill rounds (between, block):', rounds)
    print('blocks of 2+ functions by layer:', dict(multi), 'with a module:', dict(named))
    hows = ('file', 'sdk', 'block', 'between', 'gap')
    print('%-7s %6s  %s' % ('layer', 'fns', '  '.join('%8s' % h for h in hows)))
    for layer in ('sdk', 'engine', 'game'):
        c = by_layer[layer]
        print('%-7s %6d  %s' % (layer, sum(c.values()), '  '.join('%8d' % c[h] for h in hows)))
    print('\n%-28s %-44s %-10s %-10s %5s %6s  %s' % ('module', 'path', 'first', 'last', 'fns', 'words', 'how'))
    for k, v in mods.items():
        print('%-28s %-44s %-10s %-10s %5d %6d  %s' % (k, (v['path'] or '')[:44], v['first'], v['last'],
                                                     v['functions'], v['words'], dict(v['how'])))
    if readable:
        print('\nblocks holding readable functions (most first):')
        rows = sorted((v for v in blk.values() if v.get('readable')), key=lambda v: -len(v['readable']))
        for v in rows:
            print('  %s-%s %4d fns %3d readable  module %-18s folders %s' % (
                v['first'], v['last'], v['functions'], len(v['readable']),
                ','.join(sorted(m for m in v['modules'] if m != '?')) or '-', dict(v['folders'])))
    in_xbe_only = [n for n in xbe_names if n not in mods]
    print('\nXbox __FILE__ modules not placed in AI:', ' '.join(in_xbe_only))


if __name__ == '__main__':
    main()
