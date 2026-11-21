#!/usr/bin/env python3
"""readable_names_recover.py SWEEP OUT.json [K]: struct recovery across functions over the drafts (docs/READABLE.md,
"Names"): a weighted Steensgaard unification of pointers (a variable = a class + a delta), fields as class members,
call edges merged lowest fan-in first and only where the two classes' known accesses agree in width; a parameter with
more than K callers is not merged through. OUT.json: the classes with their parameters, results, globals, variables,
offsets and accesses, read with `readable_names_recover.py --cards OUT.json ID...` and turned into table rows by
readable_names_specs.py. A class is a lead, not a type: a generic function or a union can merge two structs."""
import collections, json, os, re, sys, pickle
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
sys.path.append(os.environ.get('RN_DIR', ztree.TOOLS))
import readable_names as rn, readable_draft as rd, readable_symbols
if sys.argv[1] == '--cards':
    args = sys.argv[3:]
    rec = json.load(open(sys.argv[2]))
    N = 1; W = 92
    UNK = '-u' in args
    if UNK: args.remove('-u')
    for flag in ('-n', '-w'):
        if flag in args:
            i = args.index(flag); v = int(args[i + 1]); del args[i:i + 2]
            if flag == '-n': N = v
            else: W = v
    C = {c['id']: c for c in rec['classes']}
    FS = rn.struct_fields()
    fn = rec['fname']
    def nm(a): return fn.get(str(a), hex(a))
    def squeeze(t):
        t = re.sub(r'static_cast<\w+>', '', t)
        t = re.sub(r'\*ee::at<int32_t>\(', 'i(', t)
        t = re.sub(r'\*ee::at<uint32_t>\(', 'u(', t)
        t = re.sub(r'\*ee::at<ee::Float>\(', 'f(', t)
        t = re.sub(r'\*ee::at<(u?)int(\d+)_t>\(', r'\1\2(', t)
        t = re.sub(r'ee::at<ee::Float>\(', '&f(', t)
        t = t.replace('ee::Float', 'F').replace('const ', '').replace('int32_t ', '').replace('ee::call', 'call').replace('offsetof', 'of').replace('aggressive::', '').replace('zge::', '').replace('game::', '')
        return t
    for i in map(int, args):
        c = C[i]
        print('#%d raw=%d %s' % (i, c['raw'], [l[0] for l in c['labels'][:3]]))
        for l in c['labels'][:1]:
            if FS.get(l[0]) and not UNK: print(' known %s:' % l[0], ' '.join('%x=%s' % (o, f[0]) for o, f in sorted(FS[l[0]].items())))
        if c['params']: print(' P:', ' '.join('%s:%d%s' % (nm(p[0]), p[1], '%+x' % p[2] if p[2] else '') for p in c['params'][:24]), '..%d' % len(c['params']) if len(c['params']) > 24 else '')
        if c['rets']: print(' R:', ' '.join('%s%s' % (nm(p[0]), '%+x' % p[1] if p[1] else '') for p in c['rets'][:12]))
        if c['globs']: print(' G:', [hex(a) if isinstance(a, int) else a for a, d in c['globs'][:6]])
        if c['fieldof']: print(' in:', [('#%s' % a, hex(o)) for a, o, d in c['fieldof'][:6]])
        for o in sorted(c['offs'], key=int):
            ks = c['offs'][o]
            tot = sum(len(v) for v in ks.values())
            fns = collections.Counter(nm(x[0]) for v in ks.values() for x in v)
            seen = []
            for v in ks.values():
                for x in v:
                    t = squeeze(x[1])
                    if t not in seen: seen.append(t)
            seen.sort(key=lambda t: -('=' in t and bool(re.search(r'[a-z]{4}', t))))
            ptr = c['fld'].get(o)
            if UNK and c['labels'] and FS.get(c['labels'][0][0]) and int(o) in FS[c['labels'][0][0]]:
                continue
            print('%+#x %s n%d %s%s | %s' % (int(o), '/'.join(sorted(ks)), tot, ','.join(k[:18] for k, _ in fns.most_common(2)), ' >#%s' % ptr if ptr is not None else '', ' ; '.join(t[:W] for t in seen[:N])))

    sys.exit(0)
sweep, out = sys.argv[1], sys.argv[2]
K = int(sys.argv[3]) if len(sys.argv) > 3 else 6
spans = json.load(open(os.path.join(sweep, 'spans.json')))
FS = rn.struct_fields()

parent = {}; delta = {}; fields = {}; acc = {}; labels = {}; refused = collections.Counter()
def new(n):
    if n not in parent:
        parent[n] = n; delta[n] = 0; fields[n] = {}; acc[n] = {}; labels[n] = collections.Counter()
    return n
def find(n):
    new(n)
    path = []
    while parent[n] != n:
        path.append(n); n = parent[n]
    # compress
    d = 0
    for p in reversed(path):
        d += delta[p]; delta[p] = d; parent[p] = n
    return n
def rd_(n):
    r = find(n)
    return r, (delta[n] if n != r else 0)
def width(t):
    if 'Float' in t or t == 'float': return 'f'
    if '8_t' in t and '128' not in t: return '1'
    if '16' in t: return '2'
    if '64' in t: return '8'
    if '128' in t or 'Vec' in t or 'uint128' in t: return 'q'
    return '4'
def compat(a, b):
    return a == b or {a, b} <= {'4', 'f'} and False
def conflicts(ra, rb, s):
    """accesses of ra shifted by s against rb's"""
    A, B = acc[ra], acc[rb]
    if len(A) > len(B):
        A, B = B, A; s = -s
    c = 0
    for o, kinds in A.items():
        k2 = B.get(o + s)
        if k2 and not (set(kinds) & set(k2)):
            c += 1
    return c
def field(n, off):
    r, d = rd_(n)
    o = d + off
    f = fields[r].get(o)
    if f is None:
        f = ('f', r, o, len(parent)); new(f); fields[r][o] = f
    return f
def union(a, b, k=0, strict=False):
    """a = b + k."""
    work = [(a, b, k)]
    ok = True
    first = True
    while work:
        a, b, k = work.pop()
        ra, da = rd_(a); rb, db = rd_(b)
        s = db + k - da   # ra's offset o is rb's o + s... ra + da = rb + db + k
        if ra == rb:
            continue
        if first and strict and conflicts(ra, rb, s):
            refused['conflict'] += 1
            return False
        first = False
        # ra into rb
        if len(fields[ra]) + len(acc[ra]) > len(fields[rb]) + len(acc[rb]):
            ra, rb, s = rb, ra, -s
        parent[ra] = rb; delta[ra] = s
        for o, f in fields[ra].items():
            o2 = o + s
            if o2 in fields[rb]:
                work.append((f, fields[rb][o2], 0))
            else:
                fields[rb][o2] = f
        for o, kinds in acc[ra].items():
            d = acc[rb].setdefault(o + s, {})
            for k_, v in kinds.items():
                d.setdefault(k_, []).extend(v)
        labels[rb].update(labels[ra])
        fields[ra] = {}; acc[ra] = {}; labels[ra] = collections.Counter()
    return True
def access(n, off, t, site, rawlit):
    r, d = rd_(n)
    acc[r].setdefault(d + off, {}).setdefault(width(t), []).append(site + (rawlit,))

CAST_RE = re.compile(r'^(?:static_cast<[\w:]+>|ee::Float|reinterpret_cast<[\w:*]+>|uint32_t|int32_t)\((.*)\)$', re.S)
def strip(e):
    e = e.strip()
    while True:
        if e.startswith('(') and rn._balanced(e, 1) == e[1:-1]:
            e = e[1:-1].strip(); continue
        m = CAST_RE.match(e)
        if m and rn._balanced(e, e.index('(') + 1) == m.group(1):
            e = m.group(1).strip(); continue
        return e
def top_split(e, ops='+-'):
    """split at top-level + / -: [(sign, term)]"""
    terms = []; depth = 0; cur = ''; sign = 1; i = 0
    angle = 0
    while i < len(e):
        c = e[i]
        if c in '([{': depth += 1
        elif c in ')]}': depth -= 1
        if depth == 0 and c in '+-' and cur.strip() and not cur.rstrip().endswith(('<', '>', '(', ',')):
            terms.append((sign, cur.strip())); cur = ''; sign = 1 if c == '+' else -1
        else:
            cur += c
        i += 1
    if cur.strip(): terms.append((sign, cur.strip()))
    return terms
LIT = re.compile(r'^(0x[0-9A-Fa-f]+|\d+)(?:u|ull|ll|U)?$')
OFFSETOF = re.compile(r'^offsetof\(\s*([\w:]+)\s*,\s*(\w+)\s*\)$')
stats = collections.Counter()

class Fn: pass
def parse_fn(a, code, ctx):
    """intra-function unification"""
    fn = int(a, 16)
    consts = ctx['consts']; fconsts = ctx['fconsts']; gl = ctx['globals']
    defs, rhss = rn._defs_of(code)
    vn = lambda v: ('v', fn, v)
    names = rn.signature_params(code[0]) or []
    for i, n in enumerate(names, 1):
        if n:
            union(vn(n), ('p', fn, i))
    vconst = {}
    for v, rs in rhss.items():
        vals = set()
        for r in rs:
            r2 = strip(r)
            if re.match(r'^0u?$', r2): continue
            if r2 in consts: vals.add(consts[r2])
            elif re.match(r'^0x[0-9A-Fa-f]{8}u$', r2): vals.add(int(r2[:-1], 16))
            else: vals.add(None)
        if len(vals) == 1 and None not in vals and defs.get(v, 0) == len(rs):
            vconst[v] = next(iter(vals))
    def known_offset(t):
        """(offset, struct) of a term that names an offset"""
        m = LIT.match(t)
        if m: return int(m.group(1), 0), None
        m = OFFSETOF.match(t)
        if m:
            q = m.group(1)
            for qq, fs in FS.items():
                if qq and (qq == q or qq.endswith('::' + q)):
                    for o, (f, _h) in fs.items():
                        if f == m.group(2): return o, qq
            return None
        if t in fconsts:
            return fconsts[t][1], fconsts[t][0]
        return None
    def node_of(e):
        """(node, delta) of an expression, None"""
        e = strip(e)
        ts = top_split(e)
        base = None; off = 0; label = None
        for sign, t in ts:
            k = known_offset(strip(t))
            if k is not None:
                off += sign * k[0]; label = k[1] or label
                continue
            if sign < 0 or base is not None:
                st = strip(t)
                m = re.match(r'^(?:\w+)\s*(?:\*|<<)\s*(?:0x[0-9A-Fa-f]+|\d+)u?$', st)
                if sign > 0 and base is None: continue
                stats['two bases'] += 1
                return None
            base = strip(t)
        if base is None:
            return None
        if label:
            pass
        n = term(base)
        if n is None: return None
        if n[0] == 'abs':
            return ('abs', n[1] + off), 0
        if label:
            labels[find(n)][label] += 1
        return n, off
    def term(t):
        t = strip(t)
        m = re.match(r'^[A-Za-z_]\w*$', t)
        if m:
            if t in consts: return ('abs', consts[t])
            if t in vconst: return ('abs', vconst[t])
            if t.startswith('k'): return None
            return vn(t)
        m = re.match(r'^0x([0-9A-Fa-f]{8})u$', t)
        if m:
            return ('abs', int(m.group(1), 16))
        m = re.match(r'^\*(g_\w+)$', t)
        if m:
            return ('g', gl.get(m.group(1), m.group(1)))
        m = re.match(r'^(?:\*ee::at|ee::io::loadAny)<(.+?)>\(', t)
        if m and t.endswith(')'):
            inner = rn._balanced(t, m.end())
            if m.end() + len(inner) + 1 != len(t): return None
            ty = m.group(1)
            q = re.match(r'^ee::Ptr<([\w:]+)>$', ty)
            k = node_of(inner)
            if k is None: return None
            return field(k[0], k[1])
        m = re.match(r'^ee::Ptr<([\w:]+)>\((.*)\)$', t)
        if m:
            n = term(m.group(2))
            if n is not None: labels[find(n)][m.group(1)] += 1
            return n
        return None
    site = (fn,)
    for j, c in enumerate(code):
        if j == 0: continue
        # memory accesses
        for m in re.finditer(r'\bee::(?:at|io::loadAny|io::storeAny)<([^<>()]+(?:<[^<>()]+>)?)>\(', c):
            inner = rn._balanced(c, m.end())
            # storeAny's value after the comma
            parts = rn.split_args(inner)
            k = node_of(parts[0]) if parts else None
            if k is None:
                stats['access unresolved'] += 1
                continue
            lit = 'raw' if re.search(r'\+\s*(0x[0-9A-Fa-f]+|\d+)u?\s*$', parts[0].strip()) else 'named'
            q = re.match(r'^(?:\w+::)*[A-Z]\w*$', m.group(1))
            if q and 'Float' not in m.group(1):
                labels[find(k[0])][m.group(1)] += 1
            access(k[0], k[1], m.group(1), site + (c.strip()[:110], ), lit)
            stats['access'] += 1
        for m in re.finditer(r'\bee::Ptr<([\w:]+)>\(\s*(?:static_cast<uint32_t>\()?([A-Za-z_]\w*)\)?\s*\)', c):
            labels[find(vn(m.group(2)))][m.group(1)] += 1
        # assignments
        am = rn.ASSIGN.match(c)
        if am:
            v, rhs = am.group(1), am.group(2).strip()
            e = strip(rhs)
            calls = rn.calls_in(e)
            if calls and (e.startswith(('ee::call', 'ee::callSuspendable', 'game::call', 'game::callAt'))):
                callee = calls[0][0]
                ctx['rets'].append((v, fn, callee, ctx['consts'].get(callee)))
            else:
                k = node_of(rhs)
                if k is not None:
                    # a counter stepped: no delta edge
                    bn, off = k
                    if bn == vn(v):
                        ctx['steps'].append((fn, v, off))
                    elif off == 0 or defs.get(v, 0) <= 1:
                        union(vn(v), bn, off)
        sm = re.match(r'^\s*\*ee::at<([^<>()]+(?:<[^<>()]+>)?)>\(', c)
        if sm:
            inner = rn._balanced(c, sm.end())
            rest = c[sm.end() + len(inner) + 1:].strip()
            if rest.startswith('=') and not rest.startswith('=='):
                val = rest[1:].rstrip().rstrip(';')
                k1 = node_of(inner)
                k2 = node_of(val) if 'call' not in val else None
                if k1 and k2 and k2[1] == 0:
                    union(field(k1[0], k1[1]), k2[0])
        gm = re.match(r'^\s*\*(g_\w+)\s*=\s*(.*);\s*$', c)
        if gm:
            k2 = node_of(gm.group(2))
            if k2 and k2[1] == 0:
                union(('g', gl.get(gm.group(1), gm.group(1))), k2[0])
        rm = re.match(r'^\s*return\s+(.*);\s*$', c)
        if rm:
            k = node_of(rm.group(1)) if 'call' not in rm.group(1) else None
            if k and k[1] == 0:
                union(('r', fn), k[0])
        for callee, args in rn.calls_in(c):
            for i, arg in enumerate(args, 1):
                k = node_of(arg)
                if k is not None:
                    ctx['args'].append((fn, callee, ctx['consts'].get(callee), i, k[0], k[1]))

ctx_args = []; ctx_rets = []; steps = []
fname = {}
known_globals = rn._known_globals()
files = sorted(spans.items())
sys.stderr.write('parsing %d files\n' % len(files))
cpat = re.compile(r'constexpr\s+uint32_t\s+(k\w+)\s*=\s*0x([0-9A-Fa-f]+)u?\s*;(?:\s*//\s*([\w:]+)(?:::\w+)? \+0x([0-9A-Fa-f]+))?')
for f, dd in files:
    text = readable_symbols.literal_view(open(os.path.join(sweep, f), encoding='utf-8').read())
    consts = {}; fconsts = {}
    for m in cpat.finditer(text):
        if m.group(3):
            fconsts[m.group(1)] = (m.group(3), int(m.group(4), 16))
        else:
            consts[m.group(1)] = int(m.group(2), 16)
    gl = {m.group(3): int(m.group(2), 16) for m in re.finditer(r'ee::Global<\s*(.+?)\s*,\s*0x([0-9A-Fa-f]+)u?\s*>\s*(\w+)\s*;', text)}
    lines = text.split('\n')
    ctx = dict(consts=consts, fconsts=fconsts, globals=gl, args=ctx_args, rets=ctx_rets, steps=steps)
    for a, (s0, s1) in dd.items():
        body = lines[s0 - 1:s1]
        start = next((j for j, l in enumerate(body) if re.match(r'^    [^\s/].*?\b\w+\((.*)\)$', l)), None)
        if start is None: continue
        code = [rn.code_part(l)[0] for l in body[start:]]
        mm = re.match(r'^    [^\s/].*?\b(\w+)\(', code[0])
        fname[int(a, 16)] = mm.group(1)
        parse_fn(a, code, ctx)
sys.stderr.write('%s\n' % dict(stats))
# qualified readable names -> address
readable = rd.read_readable(rn.GAME_SRC)
qual = {}
for a, r in readable.items():
    if r.get('kind') != 'PLANNED':
        ns = r.get('namespace') or ''
        qual[(ns + '::' + r['name']) if ns else r['name']] = a
        fname.setdefault(a, r['name'])
def callee_addr(callee, a):
    return a if a is not None else qual.get(callee)
fanin = collections.Counter()
sites = collections.defaultdict(list)
for fn, callee, ca, i, n, d in ctx_args:
    addr = callee_addr(callee, ca)
    if addr is None: continue
    fanin[(addr, i)] += 1
    sites[(addr, i)].append((fn, n, d))
for v, fn, callee, ca in ctx_rets:
    addr = callee_addr(callee, ca)
    if addr is None: continue
    sites[(addr, 'r')].append((fn, ('v', fn, v), 0))
LIB_FROM = 0x0020C000  # sce / libc: never unified through
order = sorted(sites, key=lambda k: len(sites[k]))
blocked = collections.Counter()
hand = {a for a in readable if a not in fname or True}
drafted = set(int(a, 16) for _, dd in files for a in dd)
for key in order:
    addr, i = key
    ss = sites[key]
    node = ('r', addr) if i == 'r' else ('p', addr, i)
    if addr not in drafted:
        blocked['undrafted'] += 1; continue
    if addr >= LIB_FROM:
        blocked['library'] += 1; continue
    if len(set(s[0] for s in ss)) > K:
        r = find(node)
        blocked['fan-in'] += 1
        continue
    # a parameter with no field use is nothing to go on above 2 callers
    r = find(node)
    if not acc[r] and len(ss) > 3:
        blocked['no fields'] += 1; continue
    for fn, n, d in ss:
        if i == 'r':
            if d == 0: union(n, node, 0, strict=True)
        else:
            union(node, n, d, strict=True)
sys.stderr.write('blocked %s refused %s\n' % (dict(blocked), dict(refused)))
# collect
members = collections.defaultdict(lambda: dict(params=[], rets=[], globs=[], addrs=[], fieldof=[]))
for n in list(parent):
    r, d = rd_(n)
    if n[0] == 'p': members[r]['params'].append((n[1], n[2], d))
    elif n[0] == 'r': members[r]['rets'].append((n[1], d))
    elif n[0] == 'g': members[r]['globs'].append((n[1], d))
    elif n[0] == 'abs': members[r]['addrs'].append((n[1], d))
for n in list(parent):
    if n[0] == 'v':
        r, d = rd_(n)
        members[r].setdefault('vars', []).append((n[1], n[2], d))
res = []
parentof = collections.defaultdict(list)
for r0, fs in fields.items():
    for o, f in fs.items():
        r, d = rd_(f)
        parentof[r].append((r0, o, d))
ids = {}
for r in acc:
    if parent[r] == r and acc[r]:
        ids[r] = len(ids)
for r, i in ids.items():
    m = members[r]
    offs = {}
    for o, kinds in acc[r].items():
        offs[o] = {k: [list(x) for x in v] for k, v in kinds.items()}
    raw = sum(1 for o, ks in acc[r].items() for k, v in ks.items() for x in v if x[-1] == 'raw')
    fld = {o: ids.get(rd_(f)[0]) for o, f in fields[r].items()}
    res.append(dict(id=i, raw=raw, offs=offs, vars=m.get('vars', []), params=m['params'], rets=m['rets'], globs=m['globs'], addrs=m['addrs'], labels=labels[r].most_common(6),
                    fld=fld, fieldof=[(ids.get(a), o, d) for a, o, d in parentof[r]]))
res.sort(key=lambda c: -c['raw'])
json.dump(dict(classes=res, fname={str(k): v for k, v in fname.items()}, fanin={'%d:%s' % k: len(v) for k, v in sites.items()}), open(out, 'w'))
tot = sum(c['raw'] for c in res)
print('classes', len(res), 'raw', tot, 'top', [(k, sum(c['raw'] for c in res[:k])) for k in (10, 50, 100, 200, 500, 1000)])
for c in res[:40]:
    print(c['id'], c['raw'], 'offs', len(c['offs']), 'params', len(c['params']), 'rets', len(c['rets']), c['labels'][:3], [fname.get(p[0]) for p in c['params'][:5]])
