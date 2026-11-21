"""Pair data addresses of two matched functions by order of first use in the
decompiled C (only when both lists have the same length). Data only.
Usage: datzip.py <ai-decomp.c|BUILD> <dm2-decomp.c|BUILD> ai:dm2 ... [--readable]
BUILD (ai_ps2, dm2_ps2, ...): that build's decompilation from tools/exports.py. This tool alone
takes the base files by default: it pairs DAT_ addresses, and the readable AI PS2 file names its
typed globals (g_...), which drops them from the lists."""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exports  # noqa: E402
def load(p):
    t = open(p, encoding='latin1').read()
    ms = list(re.finditer(r'^//// FUNCTION ([0-9a-f]+) (\S+)$', t, re.M))
    return {int(m.group(1), 16): t[m.end(): ms[i + 1].start() if i + 1 < len(ms) else len(t)] for i, m in enumerate(ms)}
readable = '--readable' in sys.argv
if readable:
    sys.argv.remove('--readable')
A = load(exports.resolve(sys.argv[1], 'decomp', readable))
B = load(exports.resolve(sys.argv[2], 'decomp', readable))
R = re.compile(r'\b(?:DAT|PTR_DAT|_DAT|PTR_FUN|PTR)_(00[0-9a-f]{6})\b')
for pair in sys.argv[3:]:
    a, b = (int(x, 16) for x in pair.split(':'))
    la = list(dict.fromkeys(R.findall(A.get(a, ''))))
    lb = list(dict.fromkeys(R.findall(B.get(b, ''))))
    ok = len(la) == len(lb)
    print('%x:%x %s (%d/%d)' % (a, b, 'zip' if ok else 'LEN-DIFF', len(la), len(lb)),
          ' '.join('%s>%s' % (x[2:], y[2:]) for x, y in zip(la, lb)) if ok else '')
