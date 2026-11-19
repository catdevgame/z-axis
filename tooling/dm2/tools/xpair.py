#!/usr/bin/env python3
"""A PS2 function of Aggressive Inline beside its Xbox counterpart's decompilation (the pairing
from tools/xmatch.py). Data only; prints to stdout or writes into a scratch folder (the output
holds both builds' decompiled code: never commit it).

    python3 -I tools/xpair.py <xmatch.json> <ps2-decomp.c> <xbox-decomp.c> <PS2 address> ...
        [--side [width]]   two columns instead of one after the other
    python3 -I tools/xpair.py ai_xbox|dm2_xbox|bmx_xbox <PS2 address> ... [--base]
        the build's pairing and the two builds' decompilations (tools/exports.py: the readable
        ones, --base for the files as analysed); a decompilation argument above may also be a
        build name (ai_ps2, ai_xbox, ...)
    python3 -I tools/xpair.py <xmatch.json> <ps2-decomp.c> <xbox-decomp.c> \
        --slices <z-axis>/docs/READABLE.md --out <dir> [--side [width]]

Per address: the pair's confidence and evidence (shared strings, floats, offsets, paired
callees), the PS2 decompilation, then the Xbox one with its calls and globals annotated with the
PS2 counterparts (`FUN_000a2cb0/*PS2 0x13F1D8 name*/`); on both sides string and float constant
addresses get their text or value (`_DAT_001a7e8c/*0.01f*/`). A
PS2 function with no standalone counterpart that xmatch found inlined shows the Xbox functions
that hold it. --slices takes every address in READABLE.md's "Next slices" section (and every function in
an address range there, "0x1793B4 .. 0x17AFEC"), and every Planned registration, writes one file per function (<addr>_<name>.txt) and an index.txt.
"""
import json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exports  # noqa: E402

# the Xbox builds' pairings with their PS2 builds (xmatch.py's output, as the work database reads them)
XBOX_PAIRING = {'ai_xbox': exports.SCRATCH + '/aixbox/xmatch.json',
                'dm2_xbox': exports.WORKDB + '/dm2xbox/xmatch.json',
                'bmx_xbox': exports.WORKDB + '/bmxxbox/xmatch.json'}
PS2_OF = {'ai_xbox': 'ai_ps2', 'dm2_xbox': 'dm2_ps2', 'bmx_xbox': 'bmx_ps2'}

FN_RE = re.compile(r'^//// FUNCTION ([0-9a-fA-F]+) (\S+)')


XNAMES = {}  # the Xbox decompilation's {name: address}, filled by main
NAMES = {}  # decompilation path -> {function name: address} (readable exports name functions)


def load_decomp(path):
    out, cur, buf = {}, None, []
    names = NAMES.setdefault(path, {})
    for ln in open(path, encoding='utf-8', errors='replace'):
        m = FN_RE.match(ln)
        if m:
            if cur is not None:
                out[cur] = ''.join(buf).strip('\n')
            cur, buf = int(m.group(1), 16), []
            if not m.group(2).startswith(('FUN_', 'thunk_FUN_')):
                names[m.group(2)] = cur
            continue
        if cur is not None:
            buf.append(ln)
    if cur is not None:
        out[cur] = ''.join(buf).strip('\n')
    return out


class Pairs:
    def __init__(self, path):
        R = self.R = json.load(open(path))
        self.pairs = {int(k, 16): v for k, v in R['pairs'].items()}
        self.inl = {int(k, 16): v for k, v in R['inlined'].items()}
        self.rx = {}
        for p, v in self.pairs.items():
            self.rx.setdefault(int(v['x'], 16), []).append(p)
        self.readable = {int(k, 16): v for k, v in R['readable'].items()}
        self.gx = {int(v, 16): int(k, 16) for k, v in R.get('globals', {}).items()}
        self.xstrings = {int(k, 16): v for k, v in R.get('xbox_strings', {}).items()}
        self.pstrings = {int(k, 16): v for k, v in R.get('ps2_strings', {}).items()}
        self.xfloats = {int(k, 16): v for k, v in R.get('xbox_floats', {}).items()}
        self.pfloats = {int(k, 16): v for k, v in R.get('ps2_floats', {}).items()}

    def pname(self, p):
        r = self.readable.get(p)
        if r and not r['name'].startswith('fn_'):
            return r['name']
        return None


def annotate(text, P, names=None):
    """names: the Xbox decompilation's {name: address}, so a function the readable stage named
    (tools/ghidra/readable_pass.sh) gets its PS2 counterparts as FUN_ ones do."""
    def fun(m, a=None):
        a = int(m.group(1), 16) if a is None else a
        ps = P.rx.get(a)
        if not ps:
            return m.group(0)
        tag = ', '.join('0x%X' % p + (' ' + P.pname(p) if P.pname(p) else '') for p in sorted(ps))
        return '%s/*PS2 %s*/' % (m.group(0), tag)

    def dat(m):
        a = int(m.group(2), 16)
        tags = []
        if a in P.gx:
            tags.append('PS2 0x%X' % P.gx[a])
        if a in P.xfloats:
            tags.append(fmt_float(P.xfloats[a]))
        s = P.xstrings.get(a)
        if s is not None:
            tags.append(quote(s))
        return '%s/*%s*/' % (m.group(0), ', '.join(tags)) if tags else m.group(0)

    text = re.sub(r'\bFUN_([0-9a-fA-F]{8})\b', fun, text)
    named = {n: a for n, a in (names or {}).items() if a in P.rx}
    if named:
        text = re.sub(r'(?<!readable: )\b(%s)\b(?=\s*\()' % '|'.join(map(re.escape, sorted(named, key=len, reverse=True))),
                      lambda m: fun(m, named[m.group(1)]), text)
    text = re.sub(r'\b(_?DAT|PTR_DAT|s_\w*?|u_\w*?|PTR)_([0-9a-fA-F]{8})\b', dat, text)
    return text


def fmt_float(v):
    return ('%.7g' % v) + 'f'


def quote(s):
    s = s if len(s) < 60 else s[:57] + '...'
    return '"%s"' % s.replace('*/', '* /').replace('\n', '\\n')


def annotate_ps2(text, P):
    """PS2 decompilation: string addresses Ghidra left as numbers, float constants in data."""
    def num(m):
        a = int(m.group(1), 16)
        s = P.pstrings.get(a)
        return '%s/*%s*/' % (m.group(0), quote(s)) if s is not None else m.group(0)

    def dat(m):
        a = int(m.group(1), 16)
        if a in P.pfloats:
            return '%s/*%s*/' % (m.group(0), fmt_float(P.pfloats[a]))
        s = P.pstrings.get(a)
        return '%s/*%s*/' % (m.group(0), quote(s)) if s is not None else m.group(0)

    text = re.sub(r'\b0x([0-9a-fA-F]{6})\b', num, text)
    text = re.sub(r'\b_?DAT_([0-9a-fA-F]{8})\b', dat, text)
    return text


def side_by_side(a, b, width):
    la, lb = a.split('\n'), b.split('\n')
    n = max(len(la), len(lb))
    out = []
    for i in range(n):
        x = la[i] if i < len(la) else ''
        y = lb[i] if i < len(lb) else ''
        x = x.replace('\t', '  ')
        if len(x) > width:
            x = x[:width - 1] + '>'
        out.append('%-*s | %s' % (width, x, y))
    return '\n'.join(out)


def show(p, P, D2, DX, side=None):
    out = []
    r = P.readable.get(p)
    title = 'PS2 0x%08X' % p
    if r:
        title += '  %s (%s, %s)' % (r['name'], r['status'], r['file'])
    lay = P.R['ps2_layers'].get('0x%08X' % p, '?')
    out.append('=' * 100)
    out.append(title + '  [%s, %s MIPS instructions]' % (lay, P.R['ps2_sizes'].get('0x%08X' % p, '?')))
    v = P.pairs.get(p)
    hosts = []
    if v:
        x = int(v['x'], 16)
        hosts = [x]
        folded = [q for q in P.rx.get(x, []) if q != p]
        out.append('Xbox 0x%08X %s  confidence %s (pass %s %s, score %.2f, call votes %s, %d x86 '
                   'instructions)' % (x, v['xname'], v['conf'], v['how'], v['detail'], v['score'],
                                      v['votes'], v['x_n']))
        if folded:
            out.append('  the same Xbox function is also the counterpart of: ' +
                       ', '.join('0x%X' % q for q in folded))
        if v['shared']:
            out.append('  shared: ' + ' '.join(v['shared']))
    elif p in P.inl:
        i = P.inl[p]
        hosts = [int(h, 16) for h in i['hosts']]
        out.append('no standalone Xbox function: inlined (%s) into %s (Xbox counterparts of %d of '
                   'its callers)' % (i['evidence'], ', '.join(i['hosts']), i['callers']))
    else:
        out.append('no Xbox counterpart found')
    ps2 = annotate_ps2(D2.get(p, '// not in the PS2 decompilation'), P)
    for x in hosts[:3]:
        xb = annotate(DX.get(x, '// not in the Xbox decompilation'), P, XNAMES)
        hdr_x = '// Xbox 0x%08X' % x
        if side:
            out.append(side_by_side('// PS2 0x%08X\n' % p + ps2, hdr_x + '\n' + xb, side))
        else:
            out.append('---- PS2 0x%08X' % p)
            out.append(ps2)
            out.append('---- Xbox 0x%08X' % x)
            out.append(xb)
    if not hosts:
        out.append('---- PS2 0x%08X' % p)
        out.append(ps2)
    return '\n'.join(out) + '\n'


def slice_addresses(readable_md, P):
    txt = open(readable_md, encoding='utf-8').read()
    i = txt.find('## Next slices, in order')
    sec = txt[i:] if i >= 0 else ''
    j = sec.find('\n## ', 5)
    if j > 0:
        sec = sec[:j]
    want = []
    for m in re.finditer(r'\b(?:FUN_|0x)([0-9A-Fa-f]{6,8})\b', sec):
        a = int(m.group(1), 16)
        if 0x100000 <= a < 0x300000 and a not in want:
            want.append(a)
    known = sorted(int(k, 16) for k in P.R['ps2_layers'])
    # ranges ("the draw handlers 0x1793B4 .. 0x17AFEC"): every function in them
    for m in re.finditer(r'\b(?:FUN_|0x)([0-9A-Fa-f]{6,8})\s*(?:\.\.|-)\s*(?:FUN_|0x)([0-9A-Fa-f]{6,8})\b', sec):
        a, b = int(m.group(1), 16), int(m.group(2), 16)
        if 0 < b - a < 0x10000:
            want.extend(f for f in known if a <= f <= b and f not in want)
    for p, r in sorted(P.readable.items()):
        if r['status'] == 'Planned' and p not in want:
            want.append(p)
    known = set(known)
    return [a for a in want if a in known]


def main():
    a = sys.argv[1:]
    side = None
    if '--side' in a:
        i = a.index('--side')
        side = 100
        if i + 1 < len(a) and a[i + 1].isdigit():
            side = int(a[i + 1])
            del a[i + 1]
        del a[i]
    slices = out = None
    if '--slices' in a:
        i = a.index('--slices')
        slices = a[i + 1]
        del a[i:i + 2]
    if '--out' in a:
        i = a.index('--out')
        out = a[i + 1]
        del a[i:i + 2]
    readable = exports.base_flag(a)
    if a and a[0] in XBOX_PAIRING:
        # a build: its pairing, its PS2 build's decompilation and its own
        b = a.pop(0)
        a[:0] = [XBOX_PAIRING[b], PS2_OF[b], b]
    if len(a) < 3:
        sys.exit(__doc__)
    a[1], a[2] = exports.resolve(a[1], 'decomp', readable), exports.resolve(a[2], 'decomp', readable)
    P = Pairs(a[0])
    D2, DX = load_decomp(a[1]), load_decomp(a[2])
    XNAMES.update(NAMES[a[2]])
    if slices:
        os.makedirs(out, exist_ok=True)
        idx = []
        for p in slice_addresses(slices, P):
            r = P.readable.get(p)
            nm = r['name'] if r else 'FUN_%08X' % p
            fn = '%08X_%s.txt' % (p, re.sub(r'[^A-Za-z0-9_]', '_', nm))
            open(os.path.join(out, fn), 'w').write(show(p, P, D2, DX, side))
            v = P.pairs.get(p)
            st = ('%s 0x%s %s' % (v['conf'], v['x'][2:], v['how'])) if v else \
                ('inlined into ' + ','.join(P.inl[p]['hosts']) if p in P.inl else 'none')
            idx.append('%08X %-28s %-8s %s' % (p, nm, r['status'] if r else '-', st))
        open(os.path.join(out, 'index.txt'), 'w').write('\n'.join(idx) + '\n')
        print('\n'.join(idx))
        print(len(idx), 'functions ->', out)
        return
    for s in a[3:]:
        sys.stdout.write(show(int(s, 16), P, D2, DX, side))


if __name__ == '__main__':
    main()
