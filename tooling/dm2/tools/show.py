"""Print decompiled functions (by containing address) or list callers. Data only.
Usage: show.py <decomp.c|BUILD> addr... | show.py <decomp.c|BUILD> --callers addr | --grep regex [--base]
BUILD (ai_ps2, dm2_xbox, ...): that build's decompilation from tools/exports.py, the readable one
unless --base."""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exports  # noqa: E402
readable = exports.base_flag(sys.argv)
text = open(exports.resolve(sys.argv[1], 'decomp', readable), encoding='latin1').read()
ms = list(re.finditer(r'^//// FUNCTION ([0-9a-f]+) (\S+)$', text, re.M))
funcs = [(int(m.group(1), 16), m.group(2), text[m.start(): ms[i + 1].start() if i + 1 < len(ms) else len(text)]) for i, m in enumerate(ms)]
args = sys.argv[2:]
if args[0] == '--callers':
    for a in args[1:]:
        # a function the readable stage named is called by its name
        name = next((n for s0, n, b in funcs if s0 == int(a, 16)), 'FUN_%08x' % int(a, 16))
        print(a, 'callers:', ' '.join('%x' % s for s, n, b in funcs if name + '(' in b and n != name))
elif args[0] == '--grep':
    r = re.compile(args[1])
    for s, n, b in funcs:
        if r.search(b):
            print('%x %s' % (s, n))
else:
    lim = 120
    for a in args:
        if a.startswith('-n'):
            lim = int(a[2:]); continue
        v = int(a, 16)
        best = max((f for f in funcs if f[0] <= v), key=lambda f: f[0])
        print('\n'.join(best[2].splitlines()[:lim]))
