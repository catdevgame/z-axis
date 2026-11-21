#!/usr/bin/env python3
"""A callee-saved register read by real logic after its publisher's save line went.
usage: survivors.py TREE BASE_TREE BUILD
Lists every ee::callerRegister/callerFloat of a saved register still in TREE, how its function is reached, and which
direct callers in BASE_TREE (before `saves`) published that register. The register may be held in a variable: a loop's
(`for (const int reg : {saved::s0, saved::s1})`, `for (int f = 0; f < 32; ++f)`), a constant's (kS6), a helper's
parameter (held.keepCallers(reg): a row per register at each call of it), or a class's (`HeldRegisters held`: a member
initialiser or constructor reads them, a row per register in each function that declares one). A read none of these
resolves is printed as `?` and makes the exit status 1."""
import argparse, collections, os, re, sys
sys.dont_write_bytecode = True
tree, base, build = sys.argv[1:4]
os.environ['ZAXIS_TREE'] = os.path.abspath(tree)   # (readable_args reads this tree: ztree.py)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import readable_args as R
SAVED = {'s0': 16, 's1': 17, 's2': 18, 's3': 19, 's4': 20, 's5': 21, 's6': 22, 's7': 23, 'fp': 30, 'ra': 31}
NUM = {v: k for k, v in SAVED.items()}
READ = re.compile(r'\bee::caller(Register|Float)\(\s*((?:[^()]|\([^()]*\))*?)\s*\)')


def load(path):
    a = argparse.Namespace(tree=path, build=build, game='ai', generated=None)
    t, _ = R.build_tree(a)
    return t


def reached_by(t):
    direct = collections.defaultdict(set)
    names = {f.short: f for f in t.functions if f.addresses}
    names.update({n[:-1]: f for n, f in list(names.items()) if n.endswith('_')})
    for s in t.sites:
        if s.kind == 'direct':
            target = s.target if isinstance(s.target, int) else None
            f = t.by_address.get(target) if target else (t.by_name.get(str(s.target).split('::')[-1]) or [None])[0]
            if f is not None:
                direct[id(f)].add(s.fn)
    for f in t.functions:
        for name in f.plain:
            g = names.get(name)
            if g is not None and g is not f:
                direct[id(g)].add(f)
    return direct


t = load(tree)
b = load(base)
direct = reached_by(t)
base_direct = reached_by(b)
published = {}   # (function short name, reg) -> True
for f in b.functions:
    body = b.files[f.path][1][f.start:f.end]
    for m in re.finditer(r'\{\s*(\d+)\s*,', ' '.join(re.findall(r'calleeSaves\(\{(.*?)\}\);', body, re.S))):
        published[(f.short.rstrip('_'), int(m.group(1)))] = True


def number(expr, code):
    """A register expression's number, None if it is not one."""
    e = expr.replace('ee::reg::', '').replace('saved::', '').strip()
    if re.fullmatch(r'\d+', e):
        return int(e)
    if e in SAVED:
        return SAVED[e]
    if e in R.GPR_NAMES:
        return R.GPR_NAMES.index(e)
    m = re.search(r'\b' + re.escape(e) + r'\s*=\s*(\d+)\b', code)
    return int(m.group(1)) if m and e.startswith('k') else None


def values(expr, code, start, at):
    """The registers an expression can hold at `at` in code[start:]: a number, or the variable of a loop around it."""
    n = number(expr, code)
    if n is not None:
        return [n]
    head = code[start:at]
    found = None
    for m in re.finditer(r'for\s*\(\s*(?:const\s+)?\w+\s+' + re.escape(expr) + r'\s*:\s*\{([^}]*)\}', head):
        found = [number(x, code) for x in m.group(1).split(',')]
    for m in re.finditer(r'for\s*\(\s*\w+\s+' + re.escape(expr) + r'\s*=\s*(\d+)\s*;\s*\w+\s*<\s*(\d+)', head):
        found = list(range(int(m.group(1)), int(m.group(2))))
    return found


def enclosing_class(code, at):
    """The struct or class whose braces hold offset `at`."""
    for m in reversed(list(re.finditer(r'\b(?:struct|class)\s+(\w+)[^;{()]*\{', code[:at]))):
        depth = 0
        for c in code[m.end() - 1:at]:
            depth += c == '{'
            depth -= c == '}'
        if depth > 0:
            return m.group(1)
    return None


def row(f, kind, reg, line):
    if kind == 'Float':
        if reg < 20:
            return None
        number_, name = 32 + reg, 'f%d' % reg
    else:
        if reg not in NUM:
            return None
        number_, name = reg, NUM[reg]
    callers = {c.short.rstrip('_') for c in direct.get(id(f), ())}
    old = None
    for g in b.functions:
        if g.short.rstrip('_') == f.short.rstrip('_') and g.path.split('game/')[-1] == f.path.split('game/')[-1]:
            old = g
    old_callers = {c.short.rstrip('_') for c in base_direct.get(id(old), ())} if old else set()
    pub = sorted(c for c in old_callers | callers if (c, number_) in published)
    return (R.ztree.zlayout.readable_key(R.ztree.TREE, f.path), line, f.short.rstrip('_'), name, len(callers), ','.join(pub))


rows = []
unresolved = []
by_file = collections.defaultdict(list)
for f in t.functions:
    by_file[f.path].append(f)
real = [f for f in t.functions if not f.wrapper]
helpers = {}    # (method name) -> (kind, parameter's index)
holders = {}    # class -> [(kind, register)]
# (Drafts do not run: their files lie under the build's readable_sweep.)
files = {path: text for path, text in t.files.items() if 'readable_sweep' not in path}
for path, (_, code) in files.items():
    lines = R.LineIndex(code)
    for m in READ.finditer(code):
        kind, expr = m.group(1), m.group(2)
        f = next((g for g in by_file[path] if g.start <= m.start() < g.end), None)
        cls = enclosing_class(code, m.start())
        regs = values(expr, code, f.start if f else 0, m.start())
        if f is not None and regs is None:
            params = [p.split()[-1] if p.split() else '' for p in f.params]
            if expr in params:
                helpers[f.short] = (kind, params.index(expr))
                continue
        # (A constructor's or a class's; with an initialiser list the parser names a constructor after its last member.)
        if f is not None and (f.short != cls and not f.short.startswith('m_') or f.addresses):
            if regs is None:
                unresolved.append((path, lines.at(m.start()), f.short, expr))
            for reg in regs or ():
                r = row(f, kind, reg, lines.at(m.start()))
                if r:
                    rows.append(r)
        elif cls is not None and regs is not None:
            holders.setdefault(cls, []).extend((kind, reg, path) for reg in regs)
        else:
            unresolved.append((path, lines.at(m.start()), f.short if f else '(no function)', expr))
for path, (_, code) in files.items():
    lines = R.LineIndex(code)
    for f in by_file[path]:
        if f.wrapper:
            continue
        body = code[f.start:f.end]
        for name, (kind, index) in helpers.items():
            for m in re.finditer(r'\.' + name + r'\(', body):
                args = R.split_args(body[m.end():R.matching(body, m.end() - 1) - 1])
                regs = values(args[index], code, f.start, f.start + m.start())
                if regs is None:
                    unresolved.append((path, lines.at(f.start + m.start()), f.short, args[index]))
                for reg in regs or ():
                    r = row(f, kind, reg, lines.at(f.start + m.start()))
                    if r:
                        rows.append(r)
        for cls, reads in holders.items():
            for m in re.finditer(r'\b' + cls + r'\s+\w+\s*[;({]', body):
                if f.short == cls:
                    continue
                for kind, reg, where in reads:
                    if where == path:
                        r = row(f, kind, reg, lines.at(f.start + m.start()))
                        if r:
                            rows.append(r)
rows = list(dict.fromkeys(rows))
rows.sort()
for r in rows:
    print('\t'.join(map(str, r)))
for path, line, name, expr in unresolved:
    print('%s\t%d\t%s\t?\t%s' % (R.ztree.zlayout.readable_key(R.ztree.TREE, path), line, name, expr))
print('reads %d, in %d functions; reached as C++ %d; with a direct caller that published that register %d; not resolved %d' % (
    len(rows), len({r[2] for r in rows}), sum(1 for r in rows if r[4]), sum(1 for r in rows if r[5]), len(unresolved)), file=sys.stderr)
sys.exit(1 if unresolved else 0)
