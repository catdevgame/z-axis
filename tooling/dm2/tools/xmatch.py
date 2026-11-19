#!/usr/bin/env python3
"""Aggressive Inline's PS2 functions paired with its Xbox build's (InlineXBox, x86, MSVC with
link-time code generation). Data only: reads AI's PS2 ELF with its Ghidra function CSV and SCE
scan, and the XBE with tools/ghidra/xbe_pipeline.sh's export; writes a JSON in scratch (the
games' addresses: never commit it).

    python3 -I tools/xmatch.py <ai.elf> <ai.csv> <ai sce-matches.csv> <default.xbe> \
        <xbe.jsonl> <out.json> [--readable <z-axis>/runtime/src/game]

x86 against MIPS, so nothing is compared as machine code. Each function becomes a set of
tokens both builds share:
  s:<text>   a string it references (asserts, __FILE__ paths, formats, asset names)
  f:<value>  a float constant (MIPS lui/ori + mtc1, lwc1 from data, a float word stored; x87
             and SSE loads from .rdata, doubles narrowed to float, float-looking immediates)
  i:<n>      an integer immediate >= 2 (MIPS li, andi, slti; x86 immediates)
  d:<n>      a memory displacement off a register other than sp/gp/esp/ebp (struct offsets:
             both builds are 32-bit and the engine's structs mostly keep their layout)
  c:<k>      a callee, once the callee itself is paired (k = the pair)
  g:<k>      a global, once paired (globals are paired by the matched functions that use them)
weighted by rarity (idf over both builds). Only tokens seen in both builds count, so
platform-only constants do not count against a pair. score(p, x) = 0.5 Dice + 0.5 the share of
p's weight found in x (the Xbox function may hold callees LTCG inlined into it), times
1 - exp(-shared weight / 5) (a few common tokens shared by two small functions say little).

Passes:
  name     the SCE scan's libc/libm names against Function ID's MSVC names (memcpy, strcmp ...)
  string   functions sharing rare strings (used by at most 4 functions a side), mutual best
  call     call sites (a small unpaired callee's own calls follow it in the list, as either
           build may have inlined it): in each pair, callees between two paired callees (in call order) that
           are alone on both sides vote for each other (weight 2); longer stretches are aligned
           in call order (as the order pass below) and aligned callees vote 1; the pair's
           unpaired callers vote the same way, unordered. A candidate is taken when it is the
           mutual best and score and votes are enough (see accept()).
  order    link order: inside a module the Xbox keeps the PS2's function order; between two
           consecutive pairs (in PS2 order) about as far apart in both builds, the unpaired
           functions on both sides are aligned by gapscore (0.6 score + 0.4 size agreement;
           x86 against MIPS instruction counts, the PS2 function alone or with its small
           callees), gaps free
  folded   a PS2 function whose masked MIPS body equals an already paired one's, voted for the
           same Xbox function (MSVC folds identical functions: many to one)
The passes repeat until nothing changes. Then inlining is inferred: a PS2 function with no
counterpart whose paired callers' Xbox counterparts have no call left at its place, and whose
own tokens are found in them, is 'inlined' into those Xbox functions.

Confidence: high (a name; a shared string unique on both sides with score >= 0.2; call votes
>= 4 with score >= 0.3; score >= 0.6), medium (votes >= 2 with score >= 0.2; score >= 0.4; a
rare string with score >= 0.3), low (the rest that were accepted). Checked (MATCH3.md section 4):
high and medium dependable in a hand sample, low about half right; --holdout hides every second
pair a unique string makes and reports how the other passes pair them.

    python3 -I tools/xmatch.py --holdout <the same six arguments>

For DM2's or BMX XXX's PS2 build against its own Xbox build, --layers ENGINE:GAME:SDK (hex
addresses: engine start, game start, SDK start) replaces AI's layer bounds, so the other
game's SDK stays out of the pool and its game code in it (BMX XXX: 100100:170000:21DAA8).
"""
import bisect, collections, csv, json, math, os, re, struct, sys

sys.path.insert(0, os.path.dirname(__file__))
from fmatch import Prog, LOADSTORE
from fmatch3 import sce_names, layer_of

F32 = struct.Struct('<f')
# x86 instruction count / MIPS instruction count of a pair: log mean and spread (fitted on the
# pairs found by unique strings: median 1.03, log sd 0.48)
SIZE_LOG_MEAN, SIZE_LOG_SD = 0.06, 0.48
ORDER_MIN = 0.45
CALL_MIN = 0.4
# callees this small (instructions) are looked through in the call pass (wrappers one build inlined)
WRAPPER_N = 16
# shared token weight at which a score keeps 63 % of its value (a unique string weighs about 8.6,
# an offset or constant found in a fifth of the functions about 1.9)
EVIDENCE_W = 5.0
PATH_RE = re.compile(r'^[A-Za-z0-9_.:\\/ -]+[\\/][A-Za-z0-9_. -]+\.(c|cpp|h)$', re.I)


def f32(bits):
    return F32.unpack(struct.pack('<I', bits & 0xFFFFFFFF))[0]


def fkey(v):
    if v != v or v in (float('inf'), float('-inf')):
        return None
    return 'f:%.5g' % v


def nice_float(bits):
    """A word that looks like a float constant a programmer wrote (not an address or count)."""
    if bits == 0:
        return False
    e = (bits >> 23) & 0xFF
    return 0x66 <= e <= 0x98 and (bits & 0x3FF) == 0


def printable(b):
    n = b.find(b'\0')
    if n < 3:
        return None
    s = b[:n]
    if all(32 <= c < 127 or c in (9, 10, 13) for c in s):
        return s.decode('latin1')
    return None


# ---------------------------------------------------------------- PS2 side

def ps2_side(elf, csv_path, sce_csv):
    P = Prog(elf, csv_path)
    named = sce_names(csv_path)
    sce = {}
    for r in csv.DictReader(open(sce_csv)):
        sce[int(r['Address'], 16)] = r['Name']
    out = {}
    for s in P.starts:
        e = P.funcs[s]
        ws = P.elf.words(s, e)
        info = P.info[s]
        refmap = {}
        calls = []
        for i, (kind, t) in info['refs']:
            if kind == 'call' and t in P.funcs:
                calls.append((i, t))
            elif kind == 'jump' and t in P.funcs and t != s and not (s <= t < e):
                calls.append((i, t))
            elif kind == 'data':
                refmap[i] = t
        calls.sort()
        strs, glob, stra = set(), [], {}
        for i, a in sorted(refmap.items()):
            st = P.string_at(a)
            if st:
                strs.add(st)
                stra[a] = st
            else:
                glob.append(a)
        floats, ints, disps, fla = set(), set(), set(), {}
        luiv = {}
        for i, w in enumerate(ws):
            op = w >> 26
            rs = (w >> 21) & 31
            rt = (w >> 16) & 31
            imm = w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            if op == 0x0F:
                luiv[rt] = imm << 16
                continue
            if op == 0x0D and rs == rt and rt in luiv:
                luiv[rt] |= imm
                continue
            if op == 0x11 and rs == 4:  # mtc1
                v = 0 if rt == 0 else luiv.get(rt)
                if v is not None and (v == 0 or nice_float(v)):
                    floats.add(f32(v))
            elif op == 0x31 and i in refmap:  # lwc1 from data
                b = P.elf.read(refmap[i], 4)
                if b and len(b) == 4:
                    v = struct.unpack('<I', b)[0]
                    fla[refmap[i]] = f32(v)
                    if v == 0 or nice_float(v):
                        floats.add(f32(v))
            elif op == 0x2B and rt in luiv and nice_float(luiv[rt]) and i not in refmap:
                floats.add(f32(luiv[rt]))
            if op in LOADSTORE and i not in refmap and rs not in (28, 29) and rs not in luiv:
                if 0 <= simm < 0x8000:
                    disps.add(simm)
            if op in (0x09, 0x0D) and rs == 0:
                v = (simm if op == 0x09 else imm) & 0xFFFFFFFF
                if v >= 2:
                    ints.add(v)
            elif op in (0x0A, 0x0B, 0x0C) and rs != 0:  # slti sltiu andi
                v = (imm if op == 0x0C else simm) & 0xFFFFFFFF
                if v >= 2:
                    ints.add(v)
            # register writes forget lui values
            if op == 0:
                luiv.pop((w >> 11) & 31, None)
            elif 0x08 <= op <= 0x0E or op in (0x20, 0x21, 0x23, 0x24, 0x25, 0x27, 0x37, 0x1E):
                luiv.pop(rt, None)
        lay = layer_of(s, named)
        out[s] = dict(a=s, e=e, n=len(ws), calls=calls, strs=strs, glob=glob, floats=floats,
                      ints=ints, disps=disps, layer=lay, name=sce.get(s), hash=info['hash'],
                      stra=stra, fla=fla)
    return P, out


# ---------------------------------------------------------------- Xbox side

class Xbe:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        assert d[:4] == b'XBEH'
        self.base = struct.unpack_from('<I', d, 0x104)[0]
        n = struct.unpack_from('<I', d, 0x11C)[0]
        sh = struct.unpack_from('<I', d, 0x120)[0] - self.base
        self.secs = []
        for i in range(n):
            fl, va, vs, ra, rs, name = struct.unpack_from('<IIIIII', d, sh + i * 56)
            nm = d[name - self.base:d.index(b'\0', name - self.base)].decode('latin1')
            self.secs.append((nm, va, vs, ra, rs, fl))

    def read(self, va, n):
        for nm, sva, vs, ra, rs, fl in self.secs:
            if sva <= va < sva + rs:
                o = ra + va - sva
                return self.d[o:o + min(n, sva + rs - va)]
        return None

    def section(self, va):
        for nm, sva, vs, ra, rs, fl in self.secs:
            if sva <= va < sva + vs:
                return nm
        return None


X87 = ('FLD', 'FMUL', 'FADD', 'FSUB', 'FDIV', 'FCOM', 'FST', 'FSTP', 'FIDIV', 'FSUBR', 'FDIVR', 'FCOMP')
SSE = ('MOVSS', 'MULSS', 'ADDSS', 'SUBSS', 'DIVSS', 'COMISS', 'MAXSS', 'MINSS', 'UCOMISS')


def xbox_side(xbe_path, jsonl):
    X = Xbe(xbe_path)
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
            st = None
            b = X.read(t, 256)
            if b:
                st = printable(b)
            if st is None and t in strings and len(strings[t]) >= 3:
                st = strings[t]
            m = txt.split(' ')[0].upper()
            if st is not None and not m.startswith(X87 + SSE):
                strs.add(st)
                stra[t] = st
                continue
            if m.startswith(X87 + SSE) and b:
                if 'qword' in txt and len(b) >= 8:
                    v = struct.unpack_from('<d', b)[0]
                    if v == 0 or 1e-12 < abs(v) < 1e12:
                        floats.add(F32.unpack(F32.pack(v))[0] if abs(v) < 3e38 else v)
                        fla[t] = v
                    continue
                if len(b) >= 4:
                    bits = struct.unpack_from('<I', b)[0]
                    if bits == 0 or nice_float(bits) or (bits >> 23) & 0xFF in range(0x60, 0xA0):
                        floats.add(f32(bits))
                        fla[t] = f32(bits)
                    continue
            glob.append(t)
        ints = set()
        for v in r['imms']:
            if nice_float(v):
                floats.add(f32(v))
            elif v >= 2:
                ints.add(v)
        disps = {v for v in r['disp'] if v < 0x8000}
        out[a] = dict(a=a, e=r['e'], n=r['ni'], calls=calls, strs=strs, glob=glob, floats=floats,
                      ints=ints, disps=disps, sec=r['sec'], name=r['n'], stra=stra,
                      fla=fla)
    return X, out


# ---------------------------------------------------------------- matching

def libname(n):
    if not n:
        return None
    n = n.split('::')[-1]
    if n.startswith(('FUN_', 'thunk_', 'FID_conflict', 'switchD', 'LAB_', 'entry')):
        return None
    while n.startswith('_'):
        n = n[1:]
    return n.split('@')[0].lower() or None


def unique_string_pairs(PS, XS):
    """(ps2, xbox) pairs that share a string no other function of either build uses."""
    up, ux = collections.defaultdict(set), collections.defaultdict(set)
    for a, f in PS.items():
        if f['layer'] != 'sdk':
            for t in f['strs']:
                up[t].add(a)
    for a, f in XS.items():
        for t in f['strs']:
            ux[t].add(a)
    got = collections.defaultdict(set)
    for t, ps in up.items():
        xs = ux.get(t, ())
        if len(ps) == 1 and len(xs) == 1:
            got[next(iter(ps))].add(next(iter(xs)))
    return sorted((p, next(iter(xs))) for p, xs in got.items() if len(xs) == 1)


class Matcher:
    def __init__(self, PS, XS, holdout=()):
        self.PS, self.XS = PS, XS
        # held-out pairs (--holdout): their strings are hidden and the string pass skips them
        self.hp = {p for p, x in holdout}
        self.hx = {x for p, x in holdout}
        self.m = {}      # ps2 -> xbox
        self.how = {}    # ps2 -> (pass, detail)
        self.rx = collections.defaultdict(set)  # xbox -> ps2s
        self.gm = {}     # ps2 global -> xbox global
        self.votes = collections.defaultdict(float)
        self.callers_p = collections.defaultdict(set)
        self.callers_x = collections.defaultdict(set)
        for a, f in PS.items():
            for _, t in f['calls']:
                self.callers_p[t].add(a)
        for a, f in XS.items():
            for _, t in f['calls']:
                self.callers_x[t].add(a)
        # engine and game code on both sides: the PS2's SDK (Sony libraries, newlib) pairs only
        # by name with the Xbox's libraries (XDK sections, Function ID / XbSymbolDatabase names)
        self.ppool = {a for a, f in PS.items() if f['layer'] != 'sdk'}
        # the Xbox libraries in .text (XAPI, the CRT) follow the game's objects: from the first
        # function XbSymbolDatabase or Function ID names there (D3D8LTCG's are compiled into
        # the game's objects by LTCG and do not count)
        libs = [a for a, f in XS.items() if f['sec'] == '.text' and (
            f['name'].startswith(('XAPILIB::', 'xboxkrnl.exe::', '_', '?')))]
        self.xlib_start = min(libs) if libs else 1 << 32
        self.xpool = {a for a, f in XS.items() if f['sec'] == '.text' and a < self.xlib_start and
                      f['name'].split('::')[-1].startswith(('FUN_', 'thunk_FUN_'))}
        self.static_tokens()

    # tokens that do not change with the match
    def static_tokens(self):
        def base(f):
            t = set()
            for s in f['strs']:
                if PATH_RE.match(s):  # __FILE__: the builds' roots differ (../ngengine, C:\NgEngine)
                    t.add('p:' + s.replace('\\', '/').rsplit('/', 1)[-1].lower())
                else:
                    t.add('s:' + s)
            t.update(k for k in (fkey(v) for v in f['floats']) if k)
            t.update('i:%d' % v for v in f['ints'])
            t.update('d:%d' % v for v in f['disps'])
            return t
        self.tp0 = {a: base(f) for a, f in self.PS.items()}
        self.tx0 = {a: base(f) for a, f in self.XS.items()}
        for d, hide in ((self.tp0, self.hp), (self.tx0, self.hx)):
            for a in hide:
                d[a] = {t for t in d[a] if not t.startswith(('s:', 'p:'))}
        self.recount()

    def tokens_p(self, a):
        t = set(self.tp0[a])
        f = self.PS[a]
        t.update('c:%x' % c for _, c in f['calls'] if c in self.m)
        t.update('g:%x' % g for g in f['glob'] if g in self.gm)
        return t

    def tokens_x(self, a):
        t = set(self.tx0[a])
        f = self.XS[a]
        for _, c in f['calls']:
            for p in self.rx.get(c, ()):
                t.add('c:%x' % p)
        inv = self.gx
        t.update('g:%x' % inv[g] for g in f['glob'] if g in inv)
        return t

    def recount(self):
        self.gx = {v: k for k, v in self.gm.items()}
        self.TP = {a: self.tokens_p(a) for a in self.PS}
        self.TX = {a: self.tokens_x(a) for a in self.XS}
        dp, dx = collections.Counter(), collections.Counter()
        for t in self.TP.values():
            dp.update(t)
        for t in self.TX.values():
            dx.update(t)
        n = len(self.PS) + len(self.XS)
        self.w = {k: math.log(1 + n / (dp[k] + dx[k])) for k in dp if k in dx}
        self.dp, self.dx = dp, dx
        self.sc = {}
        self.wp = {a: sum(self.w.get(k, 0) for k in t) for a, t in self.TP.items()}
        self.wx = {a: sum(self.w.get(k, 0) for k in t) for a, t in self.TX.items()}

    def score(self, p, x):
        k = (p, x)
        v = self.sc.get(k)
        if v is None:
            tp, tx = self.TP[p], self.TX[x]
            common = sum(self.w[t] for t in (tp & tx) if t in self.w)
            wp, wx = self.wp[p], self.wx[x]
            if wp <= 0 or wx <= 0:
                v = 0.0
            else:
                v = 0.5 * (2 * common / (wp + wx)) + 0.5 * (common / wp)
                # little shared evidence is little evidence, however well it covers two small
                # functions (one common offset is not a pair)
                v *= 1 - math.exp(-common / EVIDENCE_W)
            # size: x86 has fewer instructions than MIPS for the same code; far off is unlikely
            np_, nx = self.PS[p]['n'], self.XS[x]['n']
            r = max(nx, 1) / max(np_, 1)
            if r < 0.15 or r > 8:
                v *= 0.5
            self.sc[k] = v
        return v

    def add(self, p, x, how, detail=None):
        self.m[p] = x
        self.rx[x].add(p)
        self.how[p] = (how, detail)

    # ---- passes
    def pass_name(self):
        xn = collections.defaultdict(list)
        for a, f in self.XS.items():
            n = libname(f['name'])
            if n:
                xn[n].append(a)
        n = 0
        for a, f in self.PS.items():
            ln = libname(f['name'])
            if ln and len(xn.get(ln, ())) == 1 and xn[ln][0] not in self.rx:
                self.add(a, xn[ln][0], 'name', ln)
                n += 1
        return n

    def pass_string(self):
        up, ux = collections.defaultdict(set), collections.defaultdict(set)
        for a, f in self.PS.items():
            if a not in self.m and a in self.ppool and a not in self.hp:
                for s in f['strs']:
                    up[s].add(a)
        for a, f in self.XS.items():
            if a not in self.rx and a in self.xpool and a not in self.hx:
                for s in f['strs']:
                    ux[s].add(a)
        cand = collections.defaultdict(set)
        uniq = set()
        for s, ps in up.items():
            xs = ux.get(s)
            if not xs or len(ps) > 4 or len(xs) > 4:
                continue
            for p in ps:
                for x in xs:
                    cand[p].add(x)
                    if len(ps) == 1 and len(xs) == 1:
                        uniq.add((p, x))
        best_x = {}
        for p, xs in cand.items():
            sc = sorted(((self.score(p, x), x) for x in xs), reverse=True)
            best_x[p] = sc
        rev = collections.defaultdict(list)
        for p, sc in best_x.items():
            for v, x in sc:
                rev[x].append((v, p))
        n = 0
        for p, sc in best_x.items():
            v, x = sc[0]
            if len(sc) > 1 and sc[1][0] > v - 0.05:
                continue
            bp = max(rev[x])
            if bp[1] != p or v < 0.2:
                continue
            others = sorted(rev[x], reverse=True)
            if len(others) > 1 and others[1][0] > v - 0.05:
                continue
            self.add(p, x, 'string', 'unique' if (p, x) in uniq else 'rare')
            n += 1
        return n

    def pair_globals(self):
        votes = collections.defaultdict(collections.Counter)
        for p, x in self.m.items():
            gp, gx = self.PS[p]['glob'], self.XS[x]['glob']
            if not gp or not gx:
                continue
            sx = set(gx)
            for g in set(gp):
                for h in sx:
                    votes[g][h] += 1.0 / len(sx)
        best = {}
        for g, c in votes.items():
            (h, v), = c.most_common(1)
            users = sum(1 for u in self.callers_glob_p(g))
            if v >= 1.5 or (v >= 0.99 and users <= 2):
                best[g] = (h, v)
        rev = collections.defaultdict(list)
        for g, (h, v) in best.items():
            rev[h].append((v, g))
        gm = {}
        for h, l in rev.items():
            l.sort(reverse=True)
            if len(l) == 1 or l[0][0] > l[1][0] + 0.5:
                gm[l[0][1]] = h
        self.gm = gm

    def callers_glob_p(self, g):
        if not hasattr(self, '_gusers'):
            self._gusers = collections.defaultdict(set)
            for a, f in self.PS.items():
                for x in f['glob']:
                    self._gusers[x].add(a)
        return self._gusers.get(g, ())

    def collect_votes(self):
        votes = collections.defaultdict(float)

        def segments(A, B):
            # anchors: positions of paired callees in both lists, kept in order (LIS on B index)
            pos = []
            bidx = collections.defaultdict(list)
            for j, b in enumerate(B):
                bidx[b].append(j)
            for i, a in enumerate(A):
                x = self.m.get(a)
                if x is not None and x in bidx:
                    for j in bidx[x]:
                        pos.append((i, j))
            # longest increasing subsequence over j
            pos.sort()
            tails, prev, idx = [], [None] * len(pos), []
            for k, (i, j) in enumerate(pos):
                t = bisect.bisect_left([pos[q][1] for q in idx], j)
                if t > 0:
                    prev[k] = idx[t - 1]
                if t == len(idx):
                    idx.append(k)
                else:
                    idx[t] = k
            chain = []
            k = idx[-1] if idx else None
            while k is not None:
                chain.append(pos[k])
                k = prev[k]
            chain.reverse()
            cuts = [(-1, -1)] + chain + [(len(A), len(B))]
            for (i0, j0), (i1, j1) in zip(cuts, cuts[1:]):
                ua = []
                for a in A[i0 + 1:i1]:
                    if a not in self.m and a not in ua and a in self.ppool:
                        ua.append(a)
                ub = []
                for b in B[j0 + 1:j1]:
                    if b not in self.rx and b not in ub and b in self.xpool:
                        ub.append(b)
                if ua and ub:
                    yield ua, ub

        def expand(S, f, a, rev, depth=2):
            """The call list, with each small unpaired callee followed by its own callees: one
            build often inlined a wrapper the other kept (LTCG inlines more, GCC some)."""
            out = []
            for _, t in S[a]['calls']:
                out.append(t)
                if depth and t not in rev and S[t]['n'] <= WRAPPER_N and t != a:
                    out.extend(expand(S, f, t, rev, depth - 1))
            return out

        for p, x in self.m.items():
            if p not in self.ppool:
                continue
            A = expand(self.PS, None, p, self.m)
            B = expand(self.XS, None, x, self.rx)
            for ua, ub in segments(A, B):
                if len(ua) == 1 and len(ub) == 1:
                    votes[(ua[0], ub[0])] += 2
                elif len(ua) <= 60 and len(ub) <= 60:
                    # callees keep their call order: align the segment (inlined ones are gaps)
                    for a, b, g in self.align(ua, ub, CALL_MIN):
                        votes[(a, b)] += 1
            ca = [c for c in self.callers_p.get(p, ()) if c not in self.m and c in self.ppool]
            cb = [c for c in self.callers_x.get(x, ()) if c not in self.rx and c in self.xpool]
            if ca and cb:
                if len(ca) == 1 and len(cb) == 1:
                    votes[(ca[0], cb[0])] += 2
                elif len(ca) * len(cb) <= 36:
                    for a in ca:
                        for b in cb:
                            votes[(a, b)] += 1.0 / max(len(ca), len(cb))
        return votes

    def accept(self, p, x, v, s):
        g = self.gapscore(p, x)
        if s < 0.1:  # nothing shared: the call sites and the size alone, two of them at least
            return v >= 2 and g >= 0.45
        return (v >= 4 and s >= 0.1) or (v >= 2 and (s >= 0.2 or g >= 0.45)) or \
            (v >= 1 and (s >= 0.3 or g >= 0.55))

    def pass_calls(self):
        votes = self.collect_votes()
        self.lastvotes = votes
        byp = collections.defaultdict(list)
        byx = collections.defaultdict(list)
        for (p, x), v in votes.items():
            s = self.score(p, x)
            byp[p].append((s, v, x))
            byx[x].append((s, v, p))
        n = 0
        for p, l in byp.items():
            l.sort(reverse=True)
            s, v, x = l[0]
            if len(l) > 1 and l[1][0] > s - 0.05 and l[1][1] >= v:
                continue
            lx = sorted(byx[x], reverse=True)
            if lx[0][2] != p or (len(lx) > 1 and lx[1][0] > s - 0.05 and lx[1][1] >= lx[0][1]):
                continue
            if x in self.rx or p in self.m:
                continue
            if self.accept(p, x, v, s) or (v >= 4 and self.tiny(p, x)):
                self.add(p, x, 'call', round(v, 2))
                n += 1
        return n

    def tiny(self, p, x):
        """Functions with nothing to compare (leaf getters): only the call sites decide; their
        sizes must still agree."""
        if self.wp[p] > 0 and self.wx[x] > 0:
            return False
        r = self.XS[x]['n'] / max(self.PS[p]['n'], 1)
        return 0.3 <= r <= 2.5

    def pass_folded(self):
        votes = getattr(self, 'lastvotes', {})
        byhash = collections.defaultdict(list)
        for p in self.m:
            byhash[self.PS[p]['hash']].append(p)
        n = 0
        for p, f in self.PS.items():
            if p in self.m:
                continue
            same = byhash.get(f['hash'])
            if not same:
                continue
            x = self.m[same[0]]
            if votes.get((p, x), 0) >= 1 or any(self.m.get(c) in {t for _, t in self.XS[x]['calls']}
                                                 for c in ()):
                self.add(p, x, 'folded', '%x' % same[0])
                n += 1
        return n

    def sizesim(self, p, x):
        """How well the sizes agree: x86 against MIPS instruction counts, the PS2 function
        alone or with its small callees (which LTCG may have inlined into x), the better."""
        if not hasattr(self, '_incl'):
            self._incl = {a: f['n'] + sum(self.PS[t]['n'] for t in {t for _, t in f['calls']}
                                          if t != a and self.PS[t]['n'] <= WRAPPER_N)
                          for a, f in self.PS.items()}
        nx = max(self.XS[x]['n'], 1)
        best = 0.0
        for n in (self.PS[p]['n'], self._incl[p]):
            r = nx / max(n, 1)
            best = max(best, math.exp(-(math.log(r) - SIZE_LOG_MEAN) ** 2 / (2 * SIZE_LOG_SD ** 2)))
        return best

    def gapscore(self, p, x):
        t = self.score(p, x)
        z = self.sizesim(p, x)
        if self.wp[p] <= 0 or self.wx[x] <= 0:
            return 0.6 * z  # nothing to compare but the size
        return 0.6 * t + 0.4 * z

    def align(self, up, ux, floor):
        """Order-keeping alignment of two lists (dynamic programming, gaps free) maximising the
        sum of gapscore - floor; returns the aligned (p, x, gapscore) with gapscore >= floor."""
        g = [[self.gapscore(p, x) - floor for x in ux] for p in up]
        H = [[0.0] * (len(ux) + 1) for _ in range(len(up) + 1)]
        for i in range(1, len(up) + 1):
            for j in range(1, len(ux) + 1):
                H[i][j] = max(H[i - 1][j], H[i][j - 1], H[i - 1][j - 1] + max(g[i - 1][j - 1], -9))
        i, j = len(up), len(ux)
        got = []
        while i > 0 and j > 0:
            if H[i][j] == H[i - 1][j]:
                i -= 1
            elif H[i][j] == H[i][j - 1]:
                j -= 1
            else:
                if g[i - 1][j - 1] >= 0:
                    got.append((up[i - 1], ux[j - 1], g[i - 1][j - 1] + floor))
                i -= 1
                j -= 1
        return got

    def pass_order(self):
        """Link order: within a module the Xbox build keeps the PS2's function order (modules
        themselves are placed elsewhere). Between two consecutive pairs (in PS2 order) whose
        Xbox counterparts are in the same order and about as far apart, the unpaired functions
        on both sides are aligned (dynamic programming, gaps free: functions LTCG inlined or
        the linker dropped) by gapscore; aligned pairs above ORDER_MIN are taken."""
        ps = sorted(self.ppool)
        xs = sorted(self.xpool)
        pi = {a: i for i, a in enumerate(ps)}
        xi = {a: i for i, a in enumerate(xs)}
        anchors = sorted((p, self.m[p]) for p in self.m if p in pi and self.m[p] in xi)
        n = 0

        def plausible(a, b):
            db, dx = b[0] - a[0], b[1] - a[1]
            return dx > 0 and (0.2 <= dx / db <= 3.0 or (db < 0x800 and dx < 0x800))

        spans = []
        for k, a in enumerate(anchors):
            # the next anchor in the same run (a wrong or moved pair in between is stepped over)
            for b in anchors[k + 1:k + 5]:
                if plausible(a, b):
                    spans.append((a, b))
                    break
        for (p0, x0), (p1, x1) in spans:
            up = [p for p in ps[pi[p0] + 1:pi[p1]] if p not in self.m]
            ux = [x for x in xs[xi[x0] + 1:xi[x1]] if x not in self.rx]
            if not up or not ux or len(up) > 80 or len(ux) > 80:
                continue
            for p, x, v in self.align(up, ux, ORDER_MIN):
                if v >= ORDER_MIN and p not in self.m and x not in self.rx:
                    self.add(p, x, 'order', round(v, 2))
                    n += 1
        return n

    def run(self):
        log = []
        log.append(('name', self.pass_name()))
        self.recount()
        log.append(('string', self.pass_string()))
        for it in range(30):
            self.pair_globals()
            self.recount()
            a = self.pass_calls()
            b = self.pass_folded()
            self.recount()
            c = self.pass_string()
            d = self.pass_order()
            log.append(('round %d' % it, a, b, c, d))
            if a + b + c + d == 0:
                break
        self.log = log

    def confidence(self, p):
        how, det = self.how[p]
        x = self.m[p]
        s = self.score(p, x)
        v = det if how == 'call' else getattr(self, 'lastvotes', {}).get((p, x), 0)
        if how == 'name' or (how == 'string' and det == 'unique' and s >= 0.2) or \
                (v >= 4 and s >= 0.3) or s >= 0.6:
            return 'high'
        if (v >= 2 and s >= 0.2) or s >= 0.4 or (how == 'string' and s >= 0.3):
            return 'medium'
        return 'low'

    def inlined(self):
        """PS2 functions with no counterpart whose callers' counterparts hold them."""
        res = {}
        for p, f in self.PS.items():
            if p in self.m or p not in self.ppool:
                continue
            callers = [c for c in self.callers_p.get(p, ()) if c in self.m]
            if not callers:
                continue
            hosts = []
            for c in callers:
                x = self.m[c]
                A = [t for _, t in self.PS[c]['calls']]
                B = {t for _, t in self.XS[x]['calls']}
                # no unpaired Xbox callee left that could be p
                left = [b for b in B if b not in self.rx]
                tok = self.tp0[p]
                wp = sum(self.w.get(k, 0) for k in tok)
                found = sum(self.w.get(k, 0) for k in tok & self.TX[x])
                share = found / wp if wp > 0 else None
                hosts.append((x, len(left), share))
            evid = [h for h in hosts if (h[2] is not None and h[2] >= 0.5) or h[1] == 0]
            if evid and len(evid) * 2 >= len(hosts):
                strong = any(h[2] is not None and h[2] >= 0.5 for h in evid)
                res[p] = dict(hosts=['0x%X' % h[0] for h in evid], callers=len(callers),
                              evidence='tokens' if strong else 'no call left')
        return res


# identifiers in the Xbox build's messages: function names ("zguiScript_Load: ...",
# "zfileRegisterErrorMessage(): ..."), types and enums ("EzsceneObjType=%d", "kzobjType")
NAME_RE = re.compile(r'\b((?:z|inline|edit|XB|Ez|kz)[A-Za-z0-9]*?[A-Z_][A-Za-z0-9_]*)\b\s*(\(|:|=)?')


def names_from_xbox(M, XS, ps2_data=b''):
    """Per Xbox function, the identifiers its strings name, with the PS2 counterpart. A name
    followed by ( or : in a message is most likely the function itself, or one LTCG inlined
    into it (kind 'function'); others are types, enums or tags (kind 'other')."""
    out = {}
    for x, f in XS.items():
        if x not in M.xpool:
            continue
        got = []
        for a, st in sorted(f['stra'].items()):
            if PATH_RE.match(st) or ' ' not in st and len(st) > 40:
                continue
            for m in NAME_RE.finditer(st):
                nm, tail = m.group(1), m.group(2)
                if len(nm) < 6 or nm.isupper() or nm.startswith('XBOX'):
                    continue
                kind = 'function' if tail in ('(', ':') and m.start() < 3 else 'other'
                got.append(dict(name=nm, kind=kind, string=st[:120],
                                ps2_has=nm.encode('latin1') in ps2_data))
        if got:
            ps = sorted(M.rx.get(x, ()))
            out['0x%08X' % x] = dict(ps2=['0x%08X' % p for p in ps], names=got)
    return out


def main():
    args = sys.argv[1:]
    readable = None
    if '--readable' in args:
        i = args.index('--readable')
        readable = args[i + 1]
        del args[i:i + 2]
    holdout = '--holdout' in args
    if holdout:
        args.remove('--holdout')
    if '--layers' in args:
        # another game's PS2 build: its own layer bounds (ENGINE:GAME:SDK, hex), AI's otherwise
        i = args.index('--layers')
        import fmatch3
        e0, g0, s0 = (int(v, 16) for v in args[i + 1].split(':'))
        fmatch3.AI_ENGINE = (e0, g0)
        fmatch3.AI_SDK_START = s0
        del args[i:i + 2]
    if len(args) != 6:
        sys.exit(__doc__)
    elf, pcsv, sce, xbe, jl, out = args
    P, PS = ps2_side(elf, pcsv, sce)
    X, XS = xbox_side(xbe, jl)
    if holdout:
        return check_holdout(PS, XS)
    M = Matcher(PS, XS)
    M.run()
    inl = M.inlined()
    regs = {}
    if readable:
        from readable_carry import registrations
        for r in registrations(readable):
            regs[r['addr']] = r
    pairs = {}
    for p in sorted(M.m):
        x = M.m[p]
        pairs['0x%08X' % p] = dict(x='0x%08X' % x, xname=XS[x]['name'], how=M.how[p][0],
                                   detail=M.how[p][1], conf=M.confidence(p),
                                   score=round(M.score(p, x), 3),
                                   votes=M.how[p][1] if M.how[p][0] == 'call' else
                                   round(M.lastvotes.get((p, x), 0), 2),
                                   ps2_n=PS[p]['n'], x_n=XS[x]['n'], layer=PS[p]['layer'],
                                   shared=sorted(t for t in M.TP[p] & M.TX[x] if t in M.w)[:40])
    res = dict(log=M.log, pairs=pairs, xbox_lib_start='0x%08X' % M.xlib_start,
               inlined={'0x%08X' % p: v for p, v in sorted(inl.items())},
               xbox_only=['0x%08X' % x for x in sorted(XS) if x not in M.rx],
               xbox_names={'0x%08X' % x: XS[x]['name'] for x in sorted(XS)},
               ps2_layers={'0x%08X' % p: f['layer'] for p, f in sorted(PS.items())},
               ps2_sizes={'0x%08X' % p: f['n'] for p, f in sorted(PS.items())},
               xbox_sections={'0x%08X' % x: XS[x]['sec'] for x in sorted(XS)},
               readable={'0x%08X' % a: dict(name=r['name'], status=r['status'], file=r['file'])
                         for a, r in sorted(regs.items())},
               globals={'0x%08X' % g: '0x%08X' % h for g, h in sorted(M.gm.items())},
               names=names_from_xbox(M, XS, P.elf.data),
               ps2_strings={'0x%08X' % a: t for a, t in sorted(
                   {a: t for f in PS.values() for a, t in f['stra'].items()}.items())},
               ps2_floats={'0x%08X' % a: v for a, v in sorted(
                   {a: v for f in PS.values() for a, v in f['fla'].items()}.items())},
               xbox_floats={'0x%08X' % a: v for a, v in sorted(
                   {a: v for f in XS.values() for a, v in f['fla'].items()}.items())},
               xbox_strings={'0x%08X' % a: t for a, t in sorted(
                   {a: t for f in XS.values() for a, t in f['stra'].items()}.items())})
    json.dump(res, open(out, 'w'), indent=0)
    for l in M.log:
        print(*l)
    summary(res)


def check_holdout(PS, XS):
    """Precision check: every second pair found by a unique string is held out (its strings
    hidden on both sides, the string pass skips it); the other passes then pair it or not, and
    the pairs they make are compared with the string's."""
    truth = unique_string_pairs(PS, XS)
    held = truth[1::2]
    M = Matcher(PS, XS, holdout=held)
    M.run()
    c = collections.Counter()
    for p, x in held:
        if p not in M.m:
            c['unpaired'] += 1
        else:
            k = 'right' if M.m[p] == x else 'wrong'
            if k == 'wrong':
                print('  wrong: PS2 0x%X -> Xbox 0x%X (the string says 0x%X), %s %s %s, score %.2f'
                      % (p, M.m[p], x, M.confidence(p), M.how[p][0], M.how[p][1], M.score(p, M.m[p])))
            c[k] += 1
            c[k + '_' + M.confidence(p)] += 1
            c[k + '_' + M.how[p][0]] += 1
    print('unique-string pairs', len(truth), 'held out', len(held))
    for k in sorted(c):
        print('  %-16s %d' % (k, c[k]))


def summary(res):
    pairs, inl, lay, size = res['pairs'], res['inlined'], res['ps2_layers'], res['ps2_sizes']
    rows = collections.OrderedDict()
    for k in ('sdk', 'engine', 'game'):
        rows[k] = collections.Counter()
    for p, l in lay.items():
        c = rows[l]
        c['n'] += 1
        c['words'] += size[p]
        if p in pairs:
            c[pairs[p]['conf']] += 1
            c['wpaired'] += size[p]
        elif p in inl:
            c['inlined'] += 1
        else:
            c['none'] += 1
    print('%-7s %6s %6s %6s %6s %8s %6s %8s' % ('layer', 'fns', 'high', 'medium', 'low', 'inlined',
                                             'none', 'by size'))
    for k, c in rows.items():
        print('%-7s %6d %6d %6d %6d %8d %6d %7.1f%%' % (k, c['n'], c['high'], c['medium'], c['low'],
                                                     c['inlined'], c['none'],
                                                     100.0 * c['wpaired'] / max(c['words'], 1)))
    hows = collections.Counter(v['how'] for v in pairs.values())
    print('by pass:', dict(hows))
    if res['readable']:
        c = collections.Counter()
        for a, r in res['readable'].items():
            st = 'Planned' if r['status'] == 'Planned' else 'readable'
            c[(st, pairs[a]['conf'] if a in pairs else ('inlined' if a in inl else 'none'))] += 1
        print('readable registrations:', {'%s/%s' % k: v for k, v in sorted(c.items())})


if __name__ == '__main__':
    main()
