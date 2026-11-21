#!/usr/bin/env python3
"""readable_locals_order.py: each readable_locals*.tsv row against the Local ee::frameLocal gives its place to.

ee::frameLocal (ee/abi.cpp) places the running function's next ee::Local by an ordinal, Frame::localCount, and each
Local gives the count back at the end of its scope (ee::LocalScope, ee/plain.h). So a Local's ordinal is the number
of the function's Locals alive when it is made: those declared before it in the blocks around it. A Local in a block
that has ended counts no more, and Locals in sibling blocks (a switch's cases, an if's branches, the code after an
if whose block made one) share an ordinal and so a row:

    default: { ee::Local<char[0x40]> text; ... return {}; }      text          ordinal 0
    if (moved) { ee::Local<Vec4> rootPosition; ee::Local<Vec4> position; ... }
                                                                 rootPosition  ordinal 0, position 1

Rows counted in the source's order (text 0, rootPosition 1, position 2) put position at rootPosition's place: BMX XXX
setPathManState kept the root bone's height (tested2, 585d35e71). The ordinal is fixed by the blocks, not by the
path the run takes, so this tool works every Local's out from the source and fails where the rows say otherwise:

  - every row names a Local the function makes at that ordinal;
  - every Local the function makes at an ordinal that has rows has a row of its own. Locals sharing an ordinal share
    its place, so each is listed (consecutive rows of one ordinal; fncheck_traced_gen.py stops on two places for one
    ordinal: where the original keeps them apart, the body has to make them at different ordinals);
  - a Local made in a lambda counts from where the lambda is called, and one made in a plain helper (a function with
    no registration: it runs in its caller's frame) from where the helper is called: each call's ordinal is checked
    as the body's own are (the row names the Local as the lambda or helper declares it). A lambda this tool cannot
    follow to its calls stops it where its Locals could reach a row.

A Local is an ee::Local<...> (or a template alias of one) or a type that is an ee::Local by its base, declared by its
plain name or one a `using` gives it (zge/collision/closest.cpp's VectorOnStack and SquaredVector: int30, the five
closestPointOnTriangle rows locals1 dropped as naming no Local); -v lists those types.

The body of a row: the registered function of its name, defined in the file that registers it, in its game (an own
body, READABLE_FUNCTION_OWN, in that game's; a game folder's definition before a shared one). Source the tool cannot
read with certainty (a Local in a for's or an if's parentheses, a type in a body holding Locals, a Local temporary)
stops it too.

    readable_locals_order.py [--root WORKTREE] [-v]     exit 1 on any row or Local out of its place
    readable_locals_order.py --list FUNCTION            the ordinals of a function's Locals, as the run makes them
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from readable_entries import mask, match_close, split_top  # noqa: E402
import zaxis_paths  # noqa: E402  (the tree's readable folders: walk, key_of)

GAMES = {0: 'ai', 1: 'dm2', 2: 'bmx'}
ALL = frozenset(GAMES.values())
# (`if constexpr (game::readable::kIsDm2) {`, `(!...kIsBmx)`: a block only some games compile; its else the others)
GAME_IF = re.compile(r'\bif\s+constexpr\s*\(\s*(!?)\s*(?:game::readable::)?kIs(Dm2|Bmx)\s*\)\s*$')
TABLES = ('readable_locals.tsv', 'readable_locals_dm2.tsv', 'readable_locals_bmx.tsv')
REG = re.compile(r'\bREADABLE_FUNCTION(?:_WAITS|_VU0)?(_OWN|_ONLY)?(_AT)?\s*\(')
ALIAS = re.compile(r'\busing\s+(\w+)\s*=[^;]*\bee::Local\s*<')
# (a type that is an ee::Local by its base, `struct VectorOnStack : ee::Local<Vec4>` (zge/collision/closest.cpp), and
# a plain name given to one, `using SquaredVector = VectorOnStack;`: each declaration by its name makes a Local)
DERIVED = re.compile(r'\b(?:struct|class)\s+(\w+)\s*(?:final\s*)?:\s*(?:public\s+)?(?:ee::)?Local\s*<')
RENAME = re.compile(r'\busing\s+(\w+)\s*=\s*(?:typename\s+)?(?:\w+::)*(\w+)\s*;')
KEYWORDS = {'if', 'for', 'while', 'switch', 'catch', 'return', 'sizeof', 'alignof', 'decltype', 'static_assert',
            'defined', 'requires', 'noexcept', 'alignas', 'operator', 'READABLE_PARAM_ONLY', 'READABLE_PARAM_EXCEPT'}
TYPE_HEAD = re.compile(r'\b(struct|class|union|enum)\b[^;{}()]*$')
CALL = re.compile(r'(?<![\w.>])(?:[A-Za-z_]\w*::)*([A-Za-z_]\w*)\s*(?:<[^;(){}<>]*(?:<[^;(){}<>]*>[^;(){}<>]*)*>\s*)?\(')


class Fail(Exception):
    pass


class Local:
    """A Local declaration: its name, its ordinal from its context's start (the function's entry, or a lambda's), its
    offset in the source."""

    def __init__(self, name, ordinal, at, end, games, optional=False):
        self.name, self.ordinal, self.at, self.end, self.games = name, ordinal, at, end, games
        self.optional = optional    # (declared by an alias that is an ee::Local in some builds only)


class Context:
    """A function's body or a lambda's: where it is, the Locals and calls made in it directly (not in a lambda
    inside it), each with the count of its own Locals alive there."""

    def __init__(self, name, brace, end, parent=None):
        self.name, self.brace, self.end, self.parent = name, brace, end, parent
        self.locals = []        # [Local]
        self.calls = []         # [(callee name, count alive, offset, games)]
        self.lambdas = {}       # name -> Context of the lambdas defined in it
        self.lambda_calls = []  # [(lambda Context, count alive, offset)]: immediate calls, `[&] { ... }()`
        self.games = ALL        # the games it is compiled for (an `if constexpr` on the game around it)
        self.path = None


class Body(Context):
    def __init__(self, name, path, brace, end):
        Context.__init__(self, name, brace, end)
        self.path = path
        self.problems = []
        self.anonymous = []     # [(lambda Context, count alive at its definition)] with no name to follow
        self.game_spans = []    # [(start, end, games)] of its `if constexpr` blocks on the game (and their else)


def line_of(text, at):
    return text.count('\n', 0, at) + 1


def template_end(code, i):
    """Index just past the > that closes the < at i."""
    depth, k = 0, i
    while k < len(code):
        c = code[k]
        if c == '<':
            depth += 1
        elif c == '>':
            depth -= 1
            if depth == 0:
                return k + 1
        elif c in '({[':
            k = match_close(code, k, c, {'(': ')', '{': '}', '[': ']'}[c]) - 1
        elif c == ';':
            return None
        k += 1
    return None


def open_before(code, close, open_ch, close_ch):
    """Index of the bracket that the one at close closes."""
    depth, k = 0, close
    while k >= 0:
        if code[k] == close_ch:
            depth += 1
        elif code[k] == open_ch:
            depth -= 1
            if depth == 0:
                return k
        k -= 1
    return -1


def back_word(code, i):
    """The identifier (qualified with ::) that ends at i, spaces before i skipped."""
    k = i
    while k > 0 and code[k - 1].isspace():
        k -= 1
    end = k
    while k > 0 and (code[k - 1].isalnum() or code[k - 1] in '_:~'):
        k -= 1
    return code[k:end]


def brace_kind(code, brace):
    """What the { at brace opens: ('function', name), ('lambda', start of its [), ('type',), ('namespace',) or
    ('block',) (a statement's block, an initialiser's braces)."""
    before = code[max(0, brace - 600):brace]
    base = brace - len(before)
    if re.search(r'(\bnamespace(\s+[\w:]+)?|\bextern\s+"")\s*$', before):
        return ('namespace',)
    stripped = before.rstrip()
    if stripped.endswith(']'):
        return ('lambda', open_before(code, base + len(stripped) - 1, '[', ']'))
    # (`) const`, `) noexcept`, `) mutable`, `) -> T`: a function's or a lambda's)
    m = re.search(r'\)\s*(?:const\s*|noexcept\s*|override\s*|final\s*|mutable\s*|&\s*)*(?:->\s*[\w:<>,\s*&]+?\s*)?$',
                  stripped)
    if m is None:
        # (a constructor's member initialisers: `) : a(x), b{y} {`)
        m2 = re.search(r'\)\s*:\s*[\w:]+\s*[({][^;]*$', stripped)
        if m2 and TYPE_HEAD.search(stripped) is None:
            return function_at(code, base + m2.start())
        if TYPE_HEAD.search(stripped):
            return ('type',)
        return ('block',)
    return function_at(code, base + m.start())


def function_at(code, close):
    """What a ) at close followed by a { closes: a function's parameter list, a lambda's, or a statement's head."""
    k = open_before(code, close, '(', ')')
    if k < 0:
        return ('block',)
    head = code[:k].rstrip()
    if head.endswith(']'):
        return ('lambda', open_before(code, len(head) - 1, '[', ']'))
    name = back_word(code, k)
    if not name or name.split('::')[-1] in KEYWORDS or head.endswith('>') and not name:
        return ('block',)
    return ('function', name)


def declarators(code, i):
    """[(name, is an array)] of the declarators from i (just past the Local's type) to the declaration's ;."""
    end, depth = i, 0
    while end < len(code):
        c = code[end]
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        elif c == ';' and depth == 0:
            break
        end += 1
    out = []
    for part in split_top(code[i:end]):
        m = re.match(r'\s*([A-Za-z_]\w*)\s*(\[)?', part)
        if m:
            out.append((m.group(1), bool(m.group(2))))
    return out




def braces(code):
    """{offset of each { : offset of its }}."""
    out, stack = {}, []
    for m in re.finditer(r'[{}]', code):
        if m.group() == '{':
            stack.append(m.start())
        elif stack:
            out[stack.pop()] = m.start()
    return out


def derived_locals(codes):
    """{name: where} of the types that are an ee::Local by their base, and the plain names given to them, over the
    masked sources (a header's type is declared by its name in other files)."""
    found = {}
    for path, code in codes:
        for m in DERIVED.finditer(code):
            found.setdefault(m.group(1), '%s:%d' % (path, line_of(code, m.start())))
    while True:
        more = {m.group(1): '%s:%d' % (path, line_of(code, m.start())) for path, code in codes
                for m in RENAME.finditer(code) if m.group(2) in found and m.group(1) not in found}
        if not more:
            return found
        found.update(more)


def scan_file(path, text, derived=()):
    """[Body] of every function defined in the file (at namespace scope or in a class). derived: the names of the
    types that are an ee::Local by their base (derived_locals)."""
    code = mask(text)
    close_of = braces(code)
    aliases = sorted(set(ALIAS.findall(code)))
    local_re = re.compile(r'(?<![\w:])(?:(?:ee::)?Local%s)\s*<' % ''.join('|' + re.escape(a) for a in aliases))
    events = [(m.start(), m.group()) for m in re.finditer(r'[{}]', code)]
    events += [(m.start(), 'L') for m in local_re.finditer(code)]
    named = sorted(d for d in derived if re.search(r'\b%s\b' % re.escape(d), code))
    if named:  # (a declaration by such a type's name, `const VectorOnStack alongEdge0 = edge0 * s;`: 'P' + its end)
        plain_re = re.compile(r'(?<![\w:.>])(?:\w+::)*(?:%s)(?=\s+[A-Za-z_])' % '|'.join(re.escape(d) for d in named))
        events += [(m.start(), 'P%d' % m.end()) for m in plain_re.finditer(code)]
        local_re = re.compile('%s|%s' % (local_re.pattern, plain_re.pattern))
    events.sort()
    bodies = []
    stack = []      # (kind, outer context, count before, games before, its } offset)
    body = context = None
    count = 0
    games = ALL
    closed = (None, None)   # (offset of the last } of an `if constexpr` block on the game, its games)
    skip_until = -1
    for at, what in events:
        if at < skip_until:
            continue
        if what == '{':
            end = close_of.get(at, len(code))
            kind = brace_kind(code, at)
            if body is None:
                if kind[0] == 'function':
                    body = context = Body(kind[1].split('::')[-1], path, at, end)
                    bodies.append(body)
                    stack.append(('function', None, count, games, end))
                    count, games = 0, ALL
                else:
                    stack.append((kind[0], None, count, games, end))
                continue
            if kind[0] == 'type':
                if local_re.search(code, at, end):
                    body.problems.append((path, at, 'a type in the body holds an ee::Local'))
                skip_until = end + 1
                continue
            if kind[0] == 'lambda':
                m = re.search(r'\b([A-Za-z_]\w*)\s*=\s*$', code[:kind[1]])
                lam = Context(m.group(1) if m else None, at, end, context)
                lam.games, lam.path = games, path
                if m:
                    context.lambdas[lam.name] = lam
                elif re.match(r'\s*\(', code[end + 1:]):
                    context.lambda_calls.append((lam, count, at))
                else:
                    body.anonymous.append((lam, count))
                stack.append(('lambda', context, count, games, end))
                context, count = lam, 0
                continue
            before = games
            head = code[max(0, at - 120):at]
            g = GAME_IF.search(head)
            cond = None
            if g:
                one = frozenset([g.group(2).lower()])
                cond = ALL - one if g.group(1) else one
            elif re.search(r'\belse\s*$', head) and closed[0] is not None and \
                    not code[closed[0] + 1:at].replace('else', '').strip():
                cond = ALL - closed[1]
            if cond is not None:
                games = games & cond
                body.game_spans.append((at, end, games))
            stack.append(('game' if g else 'block', None, count, before, end, cond))
            continue
        if what == '}':
            if not stack:
                continue
            kind, outer, before, games_before, _, cond = (stack.pop() + (None,))[:6]
            closed = (at, cond) if kind == 'game' else (None, None)
            if kind == 'lambda':
                context = outer
            count, games = before, games_before
            if kind == 'function':
                for c in contexts_of(body):
                    find_calls(code, c, body)
                body = context = None
            continue
        if body is None:
            continue
        close = int(what[1:]) if what.startswith('P') else template_end(code, code.index('<', at))
        if close is None:
            continue
        rest = code[close:close + 200]
        m = re.match(r'\s*([A-Za-z_]\w*)\s*(\[|;|\{|\(|,|=|\))', rest)
        if not m or m.group(1) in ('const', 'operator'):
            if re.match(r'\s*[({]', rest):
                body.problems.append((path, at, 'an ee::Local made as a temporary'))
            continue
        prev = re.sub(r'\bconst$', '', code[:at].rstrip()).rstrip()
        if prev.endswith(('(', ',')) and m.group(2) in (',', ')'):
            continue    # (a parameter, never one by value)
        if paren_open(code, at):
            body.problems.append((path, at, 'an ee::Local declared in parentheses (a for\'s or an if\'s): its scope '
                                            'is no block this tool reads'))
            continue
        scope_end = stack[-1][4] if stack else len(code)
        optional = not what.startswith('P') and not re.match(r'(?:ee::)?Local\b', code[at:at + 9])
        for name, _array in declarators(code, close):
            context.locals.append(Local(name, count, at, scope_end, games, optional))
            count += 1
    return bodies


def contexts_of(context):
    yield context
    for lam in list(context.lambdas.values()) + [c for c, _, _ in context.lambda_calls]:
        yield from contexts_of(lam)
    if isinstance(context, Body):
        for lam, _ in context.anonymous:
            yield from contexts_of(lam)


def paren_open(code, at):
    """Whether at is inside a ( still open on its statement (a for's or an if's head)."""
    depth, k = 0, at - 1
    while k >= 0 and code[k] not in ';{}':
        if code[k] == ')':
            depth += 1
        elif code[k] == '(':
            if depth == 0:
                return True
            depth -= 1
        k -= 1
    return False


def find_calls(code, context, body):
    """The calls made in the context itself (not in a lambda inside it), each with the count of its Locals alive and
    the games it is compiled for."""
    inner = [(c.brace, c.end) for c in list(context.lambdas.values()) + [c for c, _, _ in context.lambda_calls]]
    if isinstance(context, Body):
        inner += [(c.brace, c.end) for c, _ in context.anonymous]
    for m in CALL.finditer(code, context.brace, context.end):
        if m.group(1) in KEYWORDS or any(s <= m.start() < e for s, e in inner):
            continue
        alive = [l for l in context.locals if l.at < m.start() < l.end]
        n = max([l.ordinal + 1 for l in alive], default=0)
        games = ALL
        for start, end, g in body.game_spans:
            if start < m.start() < end:
                games = games & g
        context.calls.append((m.group(1), n, m.start(), games))


def registrations(code):
    out = []
    for m in REG.finditer(code):
        close = match_close(code, m.end() - 1, '(', ')')
        out.append((m.group(1) or '', bool(m.group(2)), [a.strip() for a in split_top(code[m.end():close - 1])]))
    return out


def game_of(rel):
    """The game a source under src/game is that game's alone ('dm2', 'bmx', 'ai'), or None (shared)."""
    return {'dm2': 'dm2', 'bmx': 'bmx', 'aggressive': 'ai'}.get(rel.split('/')[0])


class Tree:
    def __init__(self, root):
        self.root = root
        self.game = root    # (its readable code: zaxis_paths.walk, key_of)
        self.tools = os.path.join(root, 'tools', 'readable')
        self.bodies = {}            # name -> [Body]
        self.registered = set()     # names with a frame of their own (registered bodies, own bodies)
        self.own = {}               # (function, game) -> its own body's name in that game
        self.registers = {}         # file -> the names it registers
        self.texts = {}
        self.memo = {}
        sources = []
        for top, _, files in zaxis_paths.walk(self.game):
            for f in sorted(files):
                if f.endswith(('.cpp', '.h')):
                    path = os.path.join(top, f)
                    sources.append((zaxis_paths.key_of(path, self.game), open(path, encoding='utf-8', errors='replace').read()))
        self.derived = derived_locals([(rel, mask(text)) for rel, text in sources if 'Local' in text])
        for rel, text in sources:
            if 'Local' not in text and 'READABLE_FUNCTION' not in text and \
                    not any(re.search(r'\b%s\b' % re.escape(d), text) for d in self.derived):
                continue
            self.texts[rel] = text
            mine = self.registers.setdefault(rel, set())
            for kind, at_form, args in registrations(mask(text)):
                if kind == '_ONLY':
                    fn = args[2] if at_form and len(args) > 2 else args[1] if len(args) > 1 else None
                else:
                    fn = (args[1] if at_form and len(args) > 1 else args[0]) if args else None
                if not fn:
                    continue
                self.registered.add(fn)
                mine.add(fn)
                if kind == '_OWN':
                    pairs = (args[2:] if at_form else args[1:])[1:]     # (after the status: GAME, OWN, ...)
                    for g, o in zip(pairs[0::2], pairs[1::2]):
                        self.own[(fn, g)] = o
                        self.registered.add(o)
                        mine.add(o)
            for body in scan_file(rel, text, self.derived):
                self.bodies.setdefault(body.name, []).append(body)

    def line(self, path, at):
        return '%s:%d' % (path, line_of(self.texts[path], at))

    def body_of(self, name, game):
        """(the body a row's function runs in that game, None) or (None, why)."""
        name = self.own.get((name, game), name)
        found = [b for b in self.bodies.get(name, []) if name in self.registers.get(b.path, ())]
        if not found:
            return None, 'no registered body of that name'
        if len(found) > 1:
            found = [b for b in found if game_of(b.path) == game] or [b for b in found if game_of(b.path) is None] or found
        if len(found) > 1:
            return None, 'bodies in %s' % ', '.join(sorted(self.line(b.path, b.brace) for b in found))
        return found[0], None

    def helpers(self, name, game):
        """The bodies of a plain helper `name` that runs in its caller's frame in that game."""
        if name in self.registered:
            return []
        return [b for b in self.bodies.get(name, []) if game_of(b.path) in (None, game)]

    def made(self, context, game, seen=()):
        """[(name, ordinal from the context's start, path, offset, via)] of every Local the context makes in the
        running frame in that game: its own, its lambdas' at each call, its helpers' at each call (via: the calls on
        the way, innermost last). Kept per context and game."""
        key = (id(context), game)
        if key in self.memo:
            return self.memo[key]
        out = []
        for local in context.locals:
            if game in local.games:
                out.append((local.name, local.ordinal, context.path, local.at, ('',) if local.optional else ()))
        for callee, n, at, games in context.calls:
            if game not in games:
                continue
            lam = find_lambda(context, callee)
            targets = [lam] if lam is not None else self.helpers(callee, game)
            for target in targets:
                if target in seen or target is context:
                    continue
                step = ('%s() at %s' % (callee, self.line(context.path, at)),)
                out += [(l, o + n, p, a, v + step) for l, o, p, a, v in self.made(target, game, seen + (context,))]
        for lam, n, at in context.lambda_calls:
            if game in lam.games:
                step = ('the lambda at %s' % self.line(context.path, at),)
                out += [(l, o + n, p, a, v + step) for l, o, p, a, v in self.made(lam, game, seen + (context,))]
        self.memo[key] = out
        return out


def find_lambda(context, name):
    """The lambda named `name` that a call in context reaches: its own or an enclosing context's."""
    while context is not None:
        if name in context.lambdas:
            return context.lambdas[name]
        context = context.parent
    return None


def load_rows(tools):
    """{(game, address): (name, [(ordinal, local, offset, table:line)])}"""
    rows = {}
    for table in TABLES:
        path = os.path.join(tools, table)
        if not os.path.exists(path):
            continue
        for n, line in enumerate(open(path, encoding='utf-8'), 1):
            if line.startswith('#') or not line.strip():
                continue
            game, address, name, ordinal, local, offset = line.rstrip('\n').split('\t')
            key = (int(game), int(address, 16))
            rows.setdefault(key, (name, []))[1].append((int(ordinal), local, int(offset, 16), '%s:%d' % (table, n)))
    return rows


def check_function(tree, game, name, places):
    """([problem], [(local, ordinal, where)]) of one function's rows."""
    body, why = tree.body_of(name, game)
    if body is None and name.endswith('_'):
        body, why = tree.body_of(name.rstrip('_'), game)
    if body is None:
        return ['%s (%s)' % (why, places[0][3])], None
    problems = ['%s: %s' % (tree.line(p, a), what) for p, a, what in body.problems]
    count = max(o for o, _, _, _ in places) + 1
    made, optional = [], set()
    for l, o, p, a, v in tree.made(body, game):
        if '' in v:
            optional.add((o, l))
        made.append((l, o, tree.line(p, a) + ''.join(', from ' + x for x in v if x)))
    first = min([o for o, _ in optional], default=None)
    if first is not None and any(o > first and (o, l) not in optional and o < count for l, o, _ in made):
        problems.append('a Local some builds do not make (%s) comes before others that reach a row: their ordinals '
                        'differ by build' % ', '.join(sorted(l for _, l in optional)))
    for lam, n in body.anonymous:
        if game not in lam.games:
            continue
        for l, o, p, a, v in tree.made(lam, game):
            if o + n < count:
                problems.append('%s (%s) is made in a lambda this tool cannot follow to its calls (passed on, or '
                                'kept): its ordinal is at least %d, a row\'s' % (l, tree.line(p, a), o + n))
    at = {}
    for local, ordinal, _ in made:
        at.setdefault(local, set()).add(ordinal)
    rows = {(o, l) for o, l, _, _ in places}
    for ordinal, local, _, where in places:
        if local not in at:
            problems.append('row %s names %s, which the function makes nowhere' % (where, local))
        elif ordinal not in at[local]:
            problems.append('row %s puts %s at ordinal %d; the function makes it at %s' %
                            (where, local, ordinal, '/'.join(map(str, sorted(at[local])))))
    for local, ordinal, where in made:
        if ordinal < count and (ordinal, local) not in rows and (ordinal, local) not in optional:
            taken = sorted({l for o, l, _, _ in places if o == ordinal})
            problems.append('%s (%s) is made at ordinal %d, the place of the row%s for %s: it needs a row of its own'
                            % (local, where, ordinal, 's' if len(taken) > 1 else '', ', '.join(taken) or 'none'))
    return problems, made


def check(root, verbose=False, only=None, tree=None):
    """[problem] of every function with rows (only: of the functions of these names)."""
    tree = tree or Tree(root)
    problems = []
    for (game_number, address), (name, places) in sorted(load_rows(tree.tools).items()):
        if only and name not in only:
            continue
        where = '%s 0x%08X %s' % (GAMES[game_number], address, name)
        found, made = check_function(tree, GAMES[game_number], name, places)
        problems += ['%s: %s' % (where, p) for p in found]
        if verbose and made is not None:
            print('%s: %s' % (where, ', '.join('%s %d' % (l, o) for l, o, _ in made)))
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', default=os.path.dirname(os.path.dirname(HERE)))
    ap.add_argument('--list', metavar='FUNCTION', action='append')
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    tree = Tree(a.root)
    problems = check(a.root, a.v or bool(a.list), set(a.list or ()), tree)
    for p in problems:
        print(p)
    if a.v:
        for name, where in sorted(tree.derived.items()):
            print('an ee::Local by its base: %s (%s)' % (name, where))
    print('readable_locals_order: %d problems; %d type names an ee::Local by their base (%s)'
          % (len(problems), len(tree.derived), ', '.join(sorted(tree.derived)) or 'none'))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
