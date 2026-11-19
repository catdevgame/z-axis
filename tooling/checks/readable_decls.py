#!/usr/bin/env python3
"""Two declarations of one function that disagree, found without a compiler.

Clang on the Mac links `int32_t f_(...)` declared in one file against `void f_(...)` defined in
another, because Itanium mangling leaves the return type out of a plain function's name. The
Windows linker (lld-link, MSVC mangling) does not, and a desktop build is 30 minutes to learn it.
Lost this way: saveAnchor_, findWorldNodeByHash, zguiPad_, vertexUv_.

This reads every non-member function declaration and definition at namespace scope under
src/game (namespaces and `namespace a::b {` included, one-liners too) and reports, per fully
qualified name:

  return   the same parameter types with more than one return type.
  params   a declaration whose parameter types match no definition of that name, while a definition
           with the same parameter count exists: the redeclaration that links nowhere.

Overloads (different parameters, each declared and defined) are fine. Skipped, being per file or
not plain functions: static, anonymous namespaces, templates, members (a qualifier that is a class),
operators, macro invocations (GAME_CALL, READABLE_FUNCTION have no return type), trailing return
types, and two entries in different branches of one #if. Keeping false positives out beats
completeness: a parameter that is a function pointer, or a type through an alias this tree defines
twice, compares as anything. Aliases (using, typedef) are resolved, builtin integer spellings and
namespace qualifiers are normalised, top-level const and parameter names are dropped.

  readable_decls.py [--root DIR] [-v]       exit 1 when anything is reported
"""
import argparse
import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)

TOKEN = re.compile(r'''
    (?P<id>[A-Za-z_][A-Za-z_0-9]*)|(?P<num>\d[\w.']*)|(?P<str>"(?:\\.|[^"\\])*")|(?P<chr>'(?:\\.|[^'\\])*')
    |(?P<op>::|->|.)''', re.X | re.S)
COMMENTS = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)
RAW = re.compile(r'R"([^(\s]*)\(.*?\)\1"', re.S)

BUILTIN = {'unsigned', 'signed', 'int', 'long', 'short', 'char', 'bool', 'float', 'double', 'void', 'wchar_t'}
CANON = {
    ('int',): 'int32_t', ('signed',): 'int32_t', ('signed', 'int'): 'int32_t',
    ('unsigned',): 'uint32_t', ('unsigned', 'int'): 'uint32_t',
    ('char',): 'char', ('signed', 'char'): 'int8_t', ('unsigned', 'char'): 'uint8_t',
    ('short',): 'int16_t', ('short', 'int'): 'int16_t', ('signed', 'short'): 'int16_t',
    ('unsigned', 'short'): 'uint16_t', ('unsigned', 'short', 'int'): 'uint16_t',
    ('long',): 'int64_t', ('long', 'int'): 'int64_t', ('long', 'long'): 'int64_t',
    ('long', 'long', 'int'): 'int64_t', ('unsigned', 'long'): 'uint64_t',
    ('unsigned', 'long', 'int'): 'uint64_t', ('unsigned', 'long', 'long'): 'uint64_t',
    ('unsigned', 'long', 'long', 'int'): 'uint64_t',
}
DROP_SPEC = {'inline', 'constexpr', 'consteval', 'extern', 'virtual', 'explicit', '[', ']', 'noexcept',
             '__forceinline', 'nodiscard', 'maybe_unused', 'noreturn', 'FORCE_INLINE', 'static_cast'}
NOT_DECL = {'struct', 'class', 'enum', 'union', 'typedef', 'using', 'template', 'return', 'friend',
            'static', 'namespace', 'static_assert', 'public', 'private', 'protected', 'concept',
            'thread_local', 'asm', '__asm__', 'if', 'else', 'for', 'while', 'switch', 'do', 'case', 'goto'}
QUALIFIERS = {'const', 'volatile', 'struct', 'class', 'enum', 'typename', 'union'}


def strip(text):
    text = RAW.sub('""', text)
    # comments, but not inside strings: tokenise strings first by a combined pass
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(c + 'S' + c if c == '"' else "'c'")
            i = j + 1
        elif text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            out.append('\n' * text.count('\n', i, n if j < 0 else j))
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def tokenize(path):
    """Tokens (text, line, cond) with preprocessor lines out; cond identifies the #if branch."""
    with open(path, encoding='utf-8', errors='replace') as f:
        text = strip(f.read())
    toks, cond, group = [], [], [0]
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.lstrip().startswith('#'):
            d = line.lstrip()[1:].lstrip()
            while line.endswith('\\') and i + 1 < len(lines):
                i += 1
                line = lines[i]
            if re.match(r'(if|ifdef|ifndef)\b', d):
                group[0] += 1
                cond.append([group[0], 0])
            elif re.match(r'(else|elif|elifdef|elifndef)\b', d) and cond:
                cond[-1][1] += 1
            elif re.match(r'endif\b', d) and cond:
                cond.pop()
            i += 1
            continue
        c = tuple(map(tuple, cond))
        for m in TOKEN.finditer(line):
            t = m.group(0)
            if t.isspace():
                continue
            if m.lastgroup == 'str':
                t = '"S"'
            toks.append((t, i + 1, c))
        i += 1
    return toks


def match_close(toks, i, open_, close):
    depth = 0
    while i < len(toks):
        if toks[i][0] == open_:
            depth += 1
        elif toks[i][0] == close:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return len(toks) - 1


class Scan:
    def __init__(self):
        self.entries = []   # dict(name, ret, params, body, file, line, cond)
        self.aliases = {}   # short name -> set of definition token tuples
        self.classes = set()

    def file(self, path, rel, full):
        toks = tokenize(path)
        ns = []             # (names tuple or None for anonymous or transparent)
        ts = []
        i, n = 0, len(toks)
        while i < n:
            t, line, cond = toks[i]
            if t == '{':
                head = [x[0] for x in ts]
                j = match_close(toks, i, '{', '}')
                if head.count('(') > head.count(')'):
                    # (a brace inside an open parenthesis is an initializer, a default argument's `= {}` or a
                    # braced argument, never a body: taken as one, it ended the declaration early and joined
                    # the rest to the next function's head)
                    ts.append(('{}', line, cond))
                    i = j + 1
                    continue
                if head[:1] == ['namespace'] or head[:2] == ['inline', 'namespace']:
                    names = [x for x in head if re.match(r'[A-Za-z_]', x) and x not in ('namespace', 'inline')]
                    ns.append(tuple(names) if names else None)
                    ts = []
                    i += 1
                    continue
                if len(head) == 2 and head[0] == 'extern':
                    ns.append(())
                    ts = []
                    i += 1
                    continue
                if head[:1] and head[0] in ('struct', 'class', 'union') or (
                        head[:1] == ['enum']):
                    names = [x for x in head if re.match(r'[A-Za-z_]', x) and x not in QUALIFIERS | {'final'}]
                    if names and full:
                        self.classes.add(names[0])
                if self.is_function(head):
                    self.record(ts, ns, rel, True, full)
                    ts = []
                    i = j + 1
                    continue
                ts.append(('{}', line, cond))
                i = j + 1
                continue
            if t == '}':
                if ns:
                    ns.pop()
                ts = []
                i += 1
                continue
            if t == ';':
                self.statement(ts, ns, rel, full)
                ts = []
                i += 1
                continue
            ts.append(toks[i])
            i += 1

    @staticmethod
    def is_function(head):
        if not head or head[0] in NOT_DECL:
            return False
        if '(' not in head:
            return False
        p = head.index('(')
        return '=' not in head[:p]

    def statement(self, ts, ns, rel, full):
        head = [x[0] for x in ts]
        if not head:
            return
        if head[0] == 'using' and len(head) > 3 and head[2] == '=':
            self.aliases.setdefault(head[1], set()).add(tuple(head[3:]))
            return
        if head[0] == 'typedef' and '(' not in head and len(head) > 2:
            self.aliases.setdefault(head[-1], set()).add(tuple(head[1:-1]))
            return
        if head[0] in NOT_DECL or not full:
            return
        if self.is_function(head):
            self.record(ts, ns, rel, False, full)

    def record(self, ts, ns, rel, body, full):
        if not full or any(x is None for x in ns):
            return
        head = [x[0] for x in ts]
        p = head.index('(')
        q = match_close(head, p, '(', ')')
        if q >= len(head) or head[q] != ')':
            return
        after = head[q + 1:]
        if '->' in after:
            return
        # name: identifier run (with ::) before the '('
        k = p - 1
        if k < 0 or not re.match(r'[A-Za-z_]', head[k]):
            return
        name = [head[k]]
        while k >= 2 and head[k - 1] == '::' and re.match(r'[A-Za-z_]', head[k - 2]):
            name = [head[k - 2], '::'] + name
            k -= 2
        if k >= 1 and head[k - 1] == '::':       # leading ::
            k -= 1
        if (k >= 1 and head[k - 1] == '~') or 'operator' in head[:p]:
            return
        ret = [x for x in head[:k] if x not in DROP_SPEC]
        if ret[:1] == ['"S"']:
            ret = ret[1:]
        # drop attributes [[...]]
        if not ret:
            return
        if ret[0] in NOT_DECL or 'static' in head[:k] or 'template' in head[:k]:
            return
        if ret == ['auto'] or 'decltype' in ret:
            return
        qual = [x for x in name if x != '::']
        if len(qual) > 1 and any(c in self.classes for c in qual[:-1]):
            return
        qname = '::'.join([c for part in ns for c in part] + qual)
        qname = qname.lstrip(':')
        self.entries.append(dict(
            name=qname, ret=ret, params=head[p + 1:q], body=body, file=rel,
            line=ts[p][1], cond=ts[p][2]))


class Norm:
    def __init__(self, aliases):
        self.alias = {}
        for k, v in aliases.items():
            if len(v) == 1:
                self.alias[k] = list(v)[0]
            else:
                self.alias[k] = None     # two meanings: compares as anything

    def types(self, toks, depth=0):
        out = []
        for t in toks:
            if t in self.alias and depth < 6:
                a = self.alias[t]
                if a is None:
                    out.append('?')
                else:
                    out.extend(self.types(list(a), depth + 1))
            else:
                out.append(t)
        # drop qualifiers of namespaces: a::b::c -> c
        res, i = [], 0
        while i < len(out):
            if i + 1 < len(out) and out[i + 1] == '::' and re.match(r'[A-Za-z_]', out[i]):
                i += 2
                continue
            if out[i] == '::':
                i += 1
                continue
            res.append(out[i])
            i += 1
        out = [x for x in res if x not in ('struct', 'class', 'enum', 'typename', 'union')]
        # builtin runs
        res, i = [], 0
        while i < len(out):
            if out[i] in BUILTIN:
                j = i
                while j < len(out) and out[j] in BUILTIN:
                    j += 1
                run = tuple(out[i:j])
                if run in CANON:
                    res.append(CANON[run])
                elif tuple(sorted(run)) in {tuple(sorted(k)): 1 for k in CANON}:
                    res.append(next(v for k, v in CANON.items() if sorted(k) == sorted(run)))
                else:
                    res.append(' '.join(run))
                i = j
            else:
                res.append(out[i])
                i += 1
        return res

    def top_const(self, toks):
        if not any(x in ('*', '&', '&&') for x in toks):
            return [x for x in toks if x not in ('const', 'volatile')]
        # const after the last * or & is top-level
        last = max(i for i, x in enumerate(toks) if x in ('*', '&', '&&'))
        return toks[:last + 1] + [x for x in toks[last + 1:] if x not in ('const', 'volatile')]

    def ret(self, toks):
        t = self.top_const(self.types(toks))
        return ' '.join(t) if '?' not in t else '?'

    def params(self, toks):
        if not toks or toks == ['void']:
            return []
        parts, cur, d = [], [], 0
        for t in toks:
            if t in '([<' and len(t) == 1:
                d += 1
            elif t in ')]>' and len(t) == 1:
                d -= 1
            if t == ',' and d == 0:
                parts.append(cur)
                cur = []
            else:
                cur.append(t)
        parts.append(cur)
        out = []
        for p in parts:
            if '=' in p:
                p = p[:p.index('=')]
            if '(' in p or '...' in p or p == ['.', '.', '.'] or '.' in p:
                out.append('?')
                continue
            array = '[' in p                 # name[N] is a pointer, after the name goes
            if array:
                p = p[:p.index('[')]
            t = self.types(p)
            body = [x for x in t if x not in QUALIFIERS]
            # drop the parameter name: a trailing identifier after some type tokens
            if len(body) >= 2 and re.match(r'[A-Za-z_]\w*$', body[-1]) and body[-1] not in BUILTIN \
                    and not re.match(r'u?int(8|16|32|64)_t$|char$|bool$|float$|double$|void$', body[-1]):
                if t[-1] == body[-1]:
                    t = t[:-1]
            elif len(body) >= 2 and body[-1] == '?':
                t = t[:-1]
            if '?' in t:
                out.append('?')
                continue
            out.append(' '.join(self.top_const(t)) + (' *' if array else ''))
        return out


def exclusive(a, b):
    for x in a['cond']:
        for y in b['cond']:
            if x[0] == y[0] and x[1] != y[1] and a['file'] == b['file']:
                return True
    return False


def same_params(a, b):
    return len(a) == len(b) and all(x == y or x == '?' or y == '?' for x, y in zip(a, b))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ztree.RUNTIME)
    ap.add_argument('-v', action='store_true')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    scan = Scan()
    for sub, full in (('include', False), ('src', False), ('src/game', True)):
        base = os.path.join(root, sub)
        for dp, dns, fns in os.walk(base):
            dns[:] = [d for d in dns if d not in ('generated', 'out', 'build')]
            if sub != 'src/game' and os.path.join(dp).startswith(os.path.join(root, 'src', 'game')):
                continue
            for fn in sorted(fns):
                if fn.endswith(('.h', '.hpp', '.cpp', '.cc', '.inl')):
                    p = os.path.join(dp, fn)
                    scan.file(p, os.path.relpath(p, root), full)
    norm = Norm(scan.aliases)
    groups = {}
    for e in scan.entries:
        e['r'] = norm.ret(e['ret'])
        e['p'] = norm.params(e['params'])
        groups.setdefault(e['name'], []).append(e)
    bad = []
    for name, es in sorted(groups.items()):
        # return: same parameters, different return
        seen = set()
        for i, a in enumerate(es):
            for b in es[i + 1:]:
                if same_params(a['p'], b['p']) and a['r'] != b['r'] and '?' not in (a['r'], b['r']) \
                        and not exclusive(a, b):
                    key = (a['file'], a['line'], b['file'], b['line'])
                    if key not in seen:
                        seen.add(key)
                        bad.append((name, 'return', a, b))
        # params: a declaration with no matching definition, others of its arity do exist
        defs = [e for e in es if e['body']]
        for d in es:
            if d['body'] or any(same_params(d['p'], e['p']) for e in defs):
                continue
            same_count = [e for e in defs if len(e['p']) == len(d['p']) and not exclusive(d, e)]
            # an overload set: the declaration may belong to a definition we cannot see; report only
            # when every same-count definition differs in a way no overload would
            if same_count and not any(same_params(d['p'], e['p']) for e in es if e is not d and e['body']):
                bad.append((name, 'params', d, same_count[0]))
    for name, kind, a, b in bad:
        print('%s: %s\n    %s:%d  %s %s(%s)%s\n    %s:%d  %s %s(%s)%s' % (
            name, kind,
            a['file'], a['line'], a['r'], name.split('::')[-1], ', '.join(a['p']), '' if not a['body'] else ' {}',
            b['file'], b['line'], b['r'], name.split('::')[-1], ', '.join(b['p']), '' if not b['body'] else ' {}'))
    print('readable_decls: %d functions, %d names, %d mismatches' % (
        len(scan.entries), len(groups), len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
