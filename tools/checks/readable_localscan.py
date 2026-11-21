#!/usr/bin/env python3
"""readable_localscan.py: ee::Local arguments that a plain call hands on by value.

Through GAME_CALL and the ee::game stand-ins an ee::Local argument went as its address. A plain function's entry
has the function's own C++ signature, so a Local handed to a parameter that is not a pointer, an ee::Ptr or an
ee::Address converts through Local's `operator T &` and goes as its VALUE. This lists every such call:

  - the callees: every function registered (READABLE_FUNCTION*) in a file under src/game/plain_sources.txt, with
    its definition's parameter types;
  - the calls: `name(...)`, `ns::name(...)` and `game::tail(name, ...)` anywhere in the host's sources (HOST_FOLDERS) and tests
    (member calls `.name(` / `->name(` and other namespaces' qualified names are not these functions);
  - the arguments: a bare name (parentheses stripped) whose latest declaration in the calling function, its
    parameters included, is an ee::Local (or FloatLocal), unless a later plain declaration shadows it; or
    `x.m` / `x->m` / a bare `m` with no declaration in the function, where m is a member declared as an ee::Local
    in some class (marked `member?`);
  - the kinds: `value`  a parameter that is not a pointer, ee::Ptr, ee::Address, ee::VarArg or reference;
               `ptr-value` an ee::Local<ee::Ptr<U>> (or <U*>) to an ee::Ptr / pointer parameter: Local's
                       operator T & gives the pointer it holds, not the Local's own address;
               `ref`    a reference parameter (binds to the Local's own storage: listed with -v only).

A call that resolves to a same-named function the calling file declares itself (a stand-in, a static helper) is
marked `local:` and listed with -v only.

A call resolves only to the functions of that name the calling file's builds have: a game folder's (dm2/, bmx/)
its own folder's and its instance's shared sources (game/<game>/instance_sources.txt), a shared source's AI's and
each instance that compiles it, a header's every build. So BMX XXX's sphereQueryRaw is not DM2's rider's callee.
Files outside src/game (the unit tests) see every function, as before.

    readable_localscan.py [-v] [--all]          exit 1 when there is a value / ptr-value hit not explained
    readable_localscan.py --explained FILE     default readable_localscan_explained.txt beside this tool: lines
                                               `path:callee:argument  why`, hits reviewed and kept
    readable_localscan.py --root WORKTREE      another checkout (default: the one this tool is in)
"""
import argparse
import os
import re
import sys

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
HERE = os.path.dirname(os.path.abspath(__file__))
ap.add_argument('--root', help='the z-axis tree (default: ztree.py\'s)')
ap.add_argument('--explained', default=os.path.join(HERE, 'readable_localscan_explained.txt'))
ap.add_argument('-v', action='store_true')
ap.add_argument('--all', action='store_true', help='also list the Local arguments that go as addresses')
A = ap.parse_args()

sys.path.insert(0, HERE)
import zlayout  # noqa: E402  (the tree's folders, old or new)
if A.root is None:
    import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
    A.root = ztree.TREE
ROOT = os.path.abspath(A.root)
GAME = ROOT     # (the tree: its readable code, zlayout.readable_walk; a source by its key)
sys.path.insert(0, zlayout.path(ROOT, 'tools', 'readable'))
import readable_entries as rt  # noqa: E402
# The host's sources the calls are looked for in (what runtime/src was before layout1), and the unit tests
HOST_FOLDERS = ('engine/overrides', 'engine/game_module', 'ps2/runtime', 'ps2/kernel', 'ps2/gs', 'ps2/vu1', 'collection',
                'platform') + tuple('games/%s/overrides' % g for g in zlayout.GAMES)


def key_of(rel):
    """A tree path's key in the readable code (zge/render/draw.cpp), None outside it."""
    return zlayout.readable_key(ROOT, os.path.join(ROOT, rel))

KEYWORDS = {'return', 'case', 'else', 'throw', 'co_return', 'delete', 'new', 'goto', 'sizeof', 'do', 'typedef',
            'using', 'if', 'while', 'for', 'switch'}
LOCAL_RE = re.compile(r'(?<![\w:])(?:ee::)?(?:Float)?Local\s*<')
QUAL_SKIP = {'tail', 'ee', 'game', 'sym', 'std', 'detail', 'readable'}


def plain_files():
    pre = [l.strip() for l in open(zlayout.path(ROOT, 'games', 'plain_sources.txt')) if l[:1].isalpha()]
    out = []
    for dp, _, fs in zlayout.readable_walk(ROOT):
        for x in fs:
            if x.endswith('.cpp'):
                rel = zlayout.readable_key(ROOT, os.path.join(dp, x))
                if any(rel == p or (p.endswith('/') and rel.startswith(p)) for p in pre):
                    out.append(rel)
    return sorted(out)


def namespaces_at(code, pos):
    """The namespace names open at pos (anonymous ones as '')."""
    stack = []
    for m in re.finditer(r'[{}]', code[:pos]):
        if m.group() == '{':
            before = code[max(0, m.start() - 200):m.start()]
            n = re.search(r'\bnamespace(\s+([\w:]+))?\s*$', before)
            stack.append(n.group(2) or '' if n else None)
        elif stack:
            stack.pop()
    return [s for s in stack if s is not None]


def param_type(p):
    decl, name, had = rt.param_name(p)
    t = decl[:decl.rfind(name)] if had else decl
    return ' '.join(t.replace('&', ' & ').replace('*', ' * ').split())


def instance_sources():
    """Each game's shared sources (src/game-relative), its instance_sources.txt."""
    out = {}
    for g in rt.GAMES:
        p = zlayout.readable_path(ROOT, g + '/instance_sources.txt')
        out[g] = set(l.strip() for l in open(p) if l[:1].isalpha()) if os.path.exists(p) else set()
    return out


def builds_of(rel):
    """The builds a source under src/game is compiled in ('ai' and the games of rt.GAMES): a game folder's file its
    game's alone, a shared .cpp AI's and each instance listing it, a header every build's (any may include it)."""
    top = rel.split('/', 1)[0]
    if top in rt.GAMES:
        return {top}
    if rel.endswith('.cpp'):
        return {'ai'} | {g for g in rt.GAMES if rel in INSTANCES[g]}
    return {'ai'} | set(rt.GAMES)


def collect_callees():
    sig = {}  # name -> [(rel, ns, [types])]
    for rel in plain_files():
        code = rt.mask(open(zlayout.readable_path(ROOT, rel), encoding='utf-8').read())
        blocks = rt.namespace_scope(code)
        # the file's aliases of a pointer type (`using FloatParts = ee::Ptr<FpNumberF>;`): a parameter so typed is one
        aliases = dict(re.findall(r'\busing\s+(\w+)\s*=\s*((?:ee::)?Ptr\s*<[^;]*>)\s*;', code))
        for _, _, args, _, _, own in rt.registrations(code):
            # (the _OWN form: the shared function and each version's own body)
            names = [args[0]] + args[3::2] if own else [args[0] if len(args) == 2 else args[1]]
            for name in names:
                for m in re.finditer(r'(?<![\w.>:~])%s\s*\(' % re.escape(name), code):
                    if rt.inside(blocks, m.start()):
                        continue
                    close = rt.match_close(code, m.end() - 1, '(', ')')
                    if not re.match(r'\s*(?:const\s*)?(?:noexcept\s*)?\{', code[close:]):
                        continue
                    params = rt.split_top(code[m.end():close - 1])
                    if len(params) == 1 and params[0].strip() in ('', 'void'):
                        params = []
                    ns = '::'.join(n for n in namespaces_at(code, m.start()) if n)
                    types = [param_type(p) for p in params]
                    types = [aliases.get(t.replace('const ', '').strip(), t) for t in types]
                    sig.setdefault(name, []).append((rel, ns, types))
    return sig


def brace_pairs(code):
    pairs, stack = [], []
    for m in re.finditer(r'[{}]', code):
        if m.group() == '{':
            stack.append(m.start())
        elif stack:
            pairs.append((stack.pop(), m.end()))
    return pairs


def opener_kind(code, brace):
    before = code[max(0, brace - 300):brace]
    if re.search(r'\bnamespace(\s+[\w:]+)?\s*$|\bextern\s+""\s*$', before):
        return 'ns'
    if re.search(r'\b(struct|class|union|enum)\b[^;{}()]*$', before):
        return 'class'
    if re.search(r'\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->\s*[\w:<>*&\s]+)?(?:mutable\s*)?$', before):
        return 'fn'
    return 'block'


def function_span(code, pairs, kinds, pos):
    """(start of the parameter list, body end) of the outermost function body around pos, or None."""
    best = None
    for (s, e), k in zip(pairs, kinds):
        if s < pos < e and k == 'fn' and (best is None or s < best[0]):
            best = (s, e)
    if best is None:
        return None
    s = best[0]
    k = s - 1
    while k >= 0 and code[k] != ')':
        k -= 1
    depth = 0
    while k >= 0:
        if code[k] == ')':
            depth += 1
        elif code[k] == '(':
            depth -= 1
            if depth == 0:
                break
        k -= 1
    return (max(k, 0), best[1])


def local_decls(text, base):
    """[(pos, name, T)] of the Local declarations (variables and parameters) in text."""
    out = []
    for m in LOCAL_RE.finditer(text):
        lt = m.end() - 1
        try:
            gt = rt.match_close(text, lt, '<', '>')
        except rt.Fail:
            continue
        targs = rt.split_top(text[lt + 1:gt - 1])
        t = ' '.join(targs[-1].split()) if targs else ''
        k = m.start() - 1
        while k >= 0 and text[k] in ' \t\n':
            k -= 1
        if text[max(0, k - 4):k + 1] == 'const':
            k -= 5
            while k >= 0 and text[k] in ' \t\n':
                k -= 1
        is_param = k >= 0 and text[k] in '(,'
        # declarators up to ; (a statement) or , / ) (a parameter)
        j, depth, cur, decls = gt, 0, [], []
        while j < len(text):
            c = text[j]
            if c in '(<[{':
                depth += 1
            elif c in ')>]}':
                if depth == 0:
                    break
                depth -= 1
            if depth == 0 and (c == ';' or (c == ',' and is_param)):
                break
            if depth == 0 and c == ',':
                decls.append((''.join(cur), j - len(cur)))
                cur = []
            else:
                cur.append(c)
            j += 1
        decls.append((''.join(cur), j - len(cur)))
        for d, at in decls:
            nm = re.match(r'\s*[&*]?\s*([A-Za-z_]\w*)', rt.split_top(d, '=')[0] if d.strip() else d)
            if nm and nm.group(1) not in ('const', 'operator'):
                out.append((base + at + nm.start(1), nm.group(1), t))
    return out


def plain_decl_positions(text, base, name):
    """Positions of non-Local declarations of name (variables, parameters, structured bindings excluded)."""
    out = []
    pat = re.compile(r'(?:^|[;{}(,])\s*(?:const\s+|static\s+|constexpr\s+)*([A-Za-z_][\w:]*(?:\s*<[^;(){}]*?>)?)'
                     r'\s*[*&]*\s*\b%s\s*[;=,)\[{(:]' % re.escape(name))
    for m in pat.finditer(text):
        ty = m.group(1)
        if ty in KEYWORDS or 'Local' in ty:
            continue
        out.append(base + m.end() - 1)
    return out


def class_local_members(files):
    names = {}
    for rel, code in files.items():
        if 'Local' not in code:
            continue
        pairs = brace_pairs(code)
        cls = [(s, e) for s, e in pairs if opener_kind(code, s) == 'class']
        for pos, nm, t in local_decls(code, 0):
            inner = [(s, e) for s, e in pairs if s < pos < e]
            if inner and max(inner)[0] in [s for s, _ in cls]:
                names.setdefault(nm, []).append((rel, t))
    return names


def classify(ptype, ltype):
    p = ptype.replace('const ', '').strip()
    holds_ptr = bool(re.match(r'(ee::)?Ptr\s*<', ltype)) or ltype.endswith('*')
    if p.endswith('&'):
        return 'ref'
    if '*' in p or re.match(r'(ee::)?Ptr\s*<', p):
        # a Local<Ptr<U>> to a Ptr<U> parameter hands on the pointer it holds; to Ptr<Ptr<U>>, its own address
        norm = lambda t: re.sub(r'\s+', '', re.sub(r'\b(ee|zge)::|\bconst\b', '', t))
        return 'ptr-value' if holds_ptr and norm(p) == norm(ltype) else None
    if re.fullmatch(r'(ee::)?(Address|VarArg)', p):  # (a variadic word takes a Local as its address: ee/plain.h)
        return None
    return 'value'


def strip_parens(a):
    a = a.strip()
    while a.startswith('(') and a.endswith(')'):
        try:
            if rt.match_close(a, 0, '(', ')') != len(a):
                break
        except rt.Fail:
            break
        a = a[1:-1].strip()
    return a


def main():
    sig = collect_callees()
    walks = [zlayout.readable_walk(ROOT)] + [os.walk(os.path.join(ROOT, d)) for d in HOST_FOLDERS] + [
        os.walk(zlayout.path(ROOT, 'tests'))]
    files = {}
    for w in walks:
        for dp, dn, fs in w:
            dn[:] = [d for d in dn if d not in ('out', 'build', 'gs_cache')]
            for x in fs:
                if x.endswith(('.cpp', '.h', '.inl', '.hpp')):
                    p = os.path.join(dp, x)
                    try:
                        files[os.path.relpath(p, ROOT)] = rt.mask(open(p, encoding='utf-8').read())
                    except (UnicodeDecodeError, StopIteration, AttributeError):
                        pass
    members = class_local_members(files)
    explained = {}
    if os.path.exists(A.explained):
        for line in open(A.explained):
            line = line.strip()
            if line and not line.startswith('#'):
                key, _, why = line.partition(' ')
                explained[key] = why.strip()
    # (the same matches as one alternation of every name in sig, which took ten CPU minutes a run once the tables
    # passed a few thousand names: any identifier before a '(' is matched, and kept when sig has it)
    class CallMatches:
        _re = re.compile(r'(?<![\w.>])((?:[A-Za-z_]\w*::)*)([A-Za-z_]\w*)\s*\(')

        def finditer(self, code):
            return (m for m in self._re.finditer(code) if m.group(2) in sig)

        def search(self, code):
            return next(self.finditer(code), None)
    call_re = CallMatches()
    tail_re = re.compile(r'\bgame::tail\s*\(\s*(?:[\w:]*::)?(\w+)\s*,')
    hits, quiet, seen_keys = [], [], set()
    for rel, code in sorted(files.items()):
        if not call_re.search(code) and 'game::tail' not in code:
            continue
        pairs = brace_pairs(code)
        kinds = [opener_kind(code, s) for s, _ in pairs]
        own = set()  # names this file declares itself outside the plain definitions
        is_plain_def = key_of(rel) is not None and key_of(rel) in plain_files_set
        calls = []
        for m in call_re.finditer(code):
            q = m.group(1).strip(':')
            if q and not any(q == cns or cns.endswith('::' + q) or q in cns.split('::') for _, cns, _ in sig[m.group(2)]):
                continue  # another namespace's function of the same name
            calls.append((m.start(), m.end() - 1, m.group(2), 0))
        for m in tail_re.finditer(code):
            if m.group(1) in sig:
                calls.append((m.start(), code.index('(', m.start()), m.group(1), 1))
        if not calls:
            continue
        for nm in {c[2] for c in calls}:
            if re.search(r'(?m)(?<![\w.>:])%s\s*\([^;{}]*\)\s*(?:const\s*)?[{;]' % re.escape(nm),
                         re.sub(r'\b(?:zge|aggressive|dm2|bmx)::', '', code)):
                # declared or defined here: a header's declaration is fine, a stand-in body is not the plain one (in a
                # plain source, a one-statement stand-in of another file's function of the same name)
                for m in re.finditer(r'(?<![\w.>:])%s\s*\([^;{}]*\)\s*(?:const\s*)?\{' % re.escape(nm), code):
                    body = code[m.end():m.end() + 400].split('}')[0]
                    if ('ee::game' in body or 'game::call' in body) and (not is_plain_def or body.count(';') <= 1):
                        own.add(nm)
        # (the builds this file is in: its calls resolve to the functions those builds have)
        caller_builds = builds_of(key_of(rel)) if key_of(rel) is not None else None
        for start, paren, nm, skip in calls:
            span = function_span(code, pairs, kinds, start)
            if span is None:
                continue
            try:
                close = rt.match_close(code, paren, '(', ')')
            except rt.Fail:
                continue
            if re.match(r'\s*(?:const\s*)?\{', code[close:]):
                continue  # a definition
            args = rt.split_top(code[paren + 1:close - 1]) if code[paren + 1:close - 1].strip() else []
            args = args[skip:]
            ftext = code[span[0]:start]
            decls = None
            for i, a in enumerate(args):
                a = strip_parens(a)
                kind_note, ltype = None, None
                mm = re.fullmatch(r'(?:[A-Za-z_]\w*(?:\[[^\]]*\])?\s*(?:\.|->)\s*)+([A-Za-z_]\w*)', a)
                if re.fullmatch(r'[A-Za-z_]\w*', a):
                    if decls is None:
                        decls = local_decls(ftext, span[0])
                    ld = [d for d in decls if d[1] == a]
                    if ld:
                        lpos, _, ltype = ld[-1]
                        shadow = [p for p in plain_decl_positions(ftext, span[0], a) if p > lpos]
                        if shadow:
                            continue
                    elif a in members and not plain_decl_positions(ftext, span[0], a):
                        ltype = members[a][0][1]
                        kind_note = 'member?'
                    else:
                        continue
                elif mm and mm.group(1) in members:
                    ltype = members[mm.group(1)][0][1]
                    kind_note = 'member?'
                else:
                    continue
                for crel, cns, ptypes in sig[nm]:
                    if i >= len(ptypes):
                        continue
                    if caller_builds is not None and not (builds_of(crel) & caller_builds):
                        continue  # another game's function of that name: not this call's callee
                    kind = classify(ptypes[i], ltype)
                    if kind is None:
                        if A.all:
                            print('  address: %s:%d: %s(`%s`: Local<%s>) -> `%s`' % ((key_of(rel) or rel),
                                  code.count('\n', 0, start) + 1, nm, a, ltype, ptypes[i]))
                        continue
                    line = code.count('\n', 0, start) + 1
                    key = '%s:%s:%s' % ((key_of(rel) or rel), nm, a)
                    desc = '%s:%d: %s(arg %d `%s`: Local<%s>) -> `%s` [%s%s] (%s%s)' % (
                        (key_of(rel) or rel), line, nm, i + skip, a, ltype, ptypes[i], kind,
                        ', ' + kind_note if kind_note else '', crel, ' ' + cns if cns else '')
                    if nm in own:
                        quiet.append('local: ' + desc)
                    elif kind == 'ref':
                        quiet.append(desc)
                    elif key in explained:
                        seen_keys.add(key)
                        quiet.append('explained: %s  -- %s' % (desc, explained[key]))
                    else:
                        hits.append(desc)
    for h in hits:
        print(h)
    if A.v:
        for h in quiet:
            print('  ' + h)
    stale = sorted(set(explained) - seen_keys)
    for k in stale:
        print('stale explanation (no such hit now): %s' % k)
    print('readable_localscan: %d plain functions, %d hits unexplained, %d explained, %d listed with -v only'
          % (len(sig), len(hits), len(seen_keys), len(quiet) - len(seen_keys)), file=sys.stderr)
    return 1 if hits else 0


plain_files_set = set(plain_files())
INSTANCES = instance_sources()
if __name__ == '__main__':
    sys.exit(main())
