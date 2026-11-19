#!/usr/bin/env python3
"""readable_entries.py: a readable source as the build compiles it, with each registered function's entry.

Game code calls a game function by its name, `moveObjectBox(box, delta)`, and the source defines it under that
name. The call still has to do what a call of a game function does: under ps2xFnCheck the replay answers it
from the recording, a function switched off (PS2X_READABLE) or hooked by an override is reached through the
function table, a function that waits gets its fiber, and a readable callee runs in its own frame
(game::detail::Route, ee/readable.h). The game and ps2xFnCheck link the same objects, so that cannot be decided
at the definition: the name callers call and the body have to be two functions.

This tool makes them, at build time and only in the build dir. It copies SOURCE to OUT with, for every
registered function (READABLE_FUNCTION and its _WAITS and _VU0 forms; _OWN, a version's own body under the shared
symbol; _ONLY, a function one engine version has, at that game's own symbol sym::GAME::NAME) defined in it:

    the definition renamed NAME_                          the body
    the registration pointed at NAME_                      (its name in the tables stays NAME)
    NAME declared before the body and defined after it     the entry: Route<&NAME_>::at<its symbol>(arguments)

and in a registered function's body, each `ee::LeftByCaller<T>` (ee/plain.h: a local the original never sets, as
its caller left it) gets its register, `ee::LeftByCaller<T, R>`: the function's row of readable_entry_reads.tsv (a row with a game column
is that game's instance's or own body's alone: R is then the instance's choice),
beside this tool, one register for each declaration, in order. The source never names it; a declaration
anywhere else, one that names its register, or a count unlike the row's stops the build.

A per-version parameter (ee/readable_waits.h: READABLE_PARAM_ONLY(GAME, TYPE NAME = VALUE), _EXCEPT, written after
the parameter before it) is the form without its value in the copy's parameter lists, the value a constant at the top
of the body for the versions without it (READABLE_PARAM_*_VALUE), and the entry passes it on with the form; that,
in any definition at namespace scope, registered or not (a declaration names it without the value).

Everything else, every call of NAME included, is left as written, so callers reach the entry. Text is only
inserted within lines and the copy starts with `#line 1 "SOURCE"`, so diagnostics, __FILE__ and the debugger
name the source's own lines. SOURCE is written relative to --line-root (the build's top folder, where the compiler
runs and the debugger looks from; default the working folder), so no copy names the folder a checkout is in. A definition it cannot read with certainty (an overload set, a variadic or
function-pointer parameter, a registration with no definition in the file) stops the build.

    readable_entries.py [--line-root DIR] SOURCE OUT
                                             (writes OUT only when its text changes)
    readable_entries.py --own-inline SOURCE OUT
                                             the source as written but for its own bodies (_OWN), each defined
                                             READABLE_OWN_INLINE(GAME) as in the build copy: what a build that
                                             compiles the source itself (tests/' ps2_test_lib) needs, so an own
                                             body that calls its game's _ONLY functions is not emitted there either
    readable_entries.py --check SOURCE...    parse only: every registration has its definition
"""
import argparse
import os
import re
import sys

REG = re.compile(r'\bREADABLE_FUNCTION(_WAITS|_VU0)?(_OWN|_ONLY)?\s*\(')
GAMES = ('dm2', 'bmx')     # (a game with an instance: the _ONLY form's first argument)
# A per-version parameter (ee/readable_waits.h): READABLE_PARAM_ONLY(GAME, TYPE NAME = VALUE), _EXCEPT.
VERSION_PARAM = re.compile(r'\bREADABLE_PARAM_(ONLY|EXCEPT)\s*\(')
VERSION_RESULT = re.compile(r'\bREADABLE_RESULT_(ONLY|EXCEPT)\s*\(')
ALL_GAMES = {'ai', 'dm2', 'bmx'}
VERSION_GAMES = ('ai', 'dm2', 'bmx')
LEFT_BY_CALLER = re.compile(r'\bLeftByCaller\s*<')
ENTRY_READS = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'readable_entry_reads.tsv')
# The callee-saved registers, by name, in ee::callerRegister's numbering (32 + n for FPR n).
SAVED = dict({'s%d' % i: 16 + i for i in range(8)}, gp=28, fp=30, ra=31, **{'f%d' % i: 32 + i for i in range(20, 32)})
TYPE_WORDS = {'int', 'char', 'short', 'long', 'unsigned', 'signed', 'float', 'double', 'bool', 'void', 'const',
              'volatile', 'auto'}


class Fail(Exception):
    pass


def mask(text):
    """The text with comments, string and char literals and preprocessor lines blanked: same length, newlines kept."""
    out = list(text)
    i, n = 0, len(text)
    line_start = True
    while i < n:
        c = text[i]
        if line_start and c in ' \t':
            i += 1
            continue
        if line_start and c == '#':
            j = i
            while j < n:
                k = text.find('\n', j)
                k = n if k < 0 else k
                if k > 0 and text[k - 1] == '\\':
                    j = k + 1
                    continue
                break
            for x in range(i, k):
                if out[x] != '\n':
                    out[x] = ' '
            i = k
            continue
        line_start = False
        if c == '\n':
            line_start = True
            i += 1
        elif text.startswith('//', i):
            k = text.find('\n', i)
            k = n if k < 0 else k
            for x in range(i, k):
                out[x] = ' '
            i = k
        elif text.startswith('/*', i):
            k = text.find('*/', i + 2)
            k = n if k < 0 else k + 2
            for x in range(i, k):
                if out[x] != '\n':
                    out[x] = ' '
            i = k
        elif text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
            m = re.match(r'R"([^(]*)\(', text[i:])
            k = text.find(')' + m.group(1) + '"', i) + len(m.group(1)) + 2
            for x in range(i + 1, k - 1):
                if out[x] != '\n':
                    out[x] = ' '
            i = k
        elif c == '"' or (c == "'" and not (i and (text[i - 1].isalnum() or text[i - 1] == '_'))):
            k = i + 1
            while k < n and text[k] != c:
                k += 2 if text[k] == '\\' else 1
            for x in range(i + 1, k):
                out[x] = ' '
            i = k + 1
        else:
            i += 1
    return ''.join(out)


def match_close(code, i, open_ch, close_ch):
    """Index just past the bracket matching the one at i."""
    depth = 0
    for k in range(i, len(code)):
        if code[k] == open_ch:
            depth += 1
        elif code[k] == close_ch:
            depth -= 1
            if depth == 0:
                return k + 1
    raise Fail('unbalanced %s at offset %d' % (open_ch, i))


def namespace_scope(code):
    """For every offset, whether it is at namespace scope: a list of (start, end) spans of non-namespace braces."""
    blocks = []  # (open offset, close offset) of braces that are not namespaces / extern "C"
    stack = []
    for m in re.finditer(r'[{}]', code):
        if m.group() == '{':
            before = code[max(0, m.start() - 200):m.start()]
            is_ns = re.search(r'(\bnamespace(\s+[\w:]+)?|\bextern\s+"")\s*$', before) is not None
            stack.append((m.start(), is_ns))
        else:
            if not stack:
                raise Fail('unbalanced } at offset %d' % m.start())
            start, is_ns = stack.pop()
            # only the outermost non-namespace block matters
            if not is_ns and all(ns for _, ns in stack):
                blocks.append((start, m.end()))
    return blocks


def inside(blocks, pos):
    for s, e in blocks:
        if s <= pos < e:
            return True
    return False


def split_top(text, sep=','):
    parts, depth, cur = [], 0, []
    for c in text:
        if c in '(<[{':
            depth += 1
        elif c in ')>]}':
            depth -= 1
        if c == sep and depth == 0:
            parts.append(''.join(cur))
            cur = []
        else:
            cur.append(c)
    if ''.join(cur).strip():
        parts.append(''.join(cur))
    return parts


def split_spans(code, start, end):
    """The (start, end) of each top-level comma-separated piece of code[start:end]."""
    spans, depth, at = [], 0, start
    for i in range(start, end):
        c = code[i]
        if c in '(<[{':
            depth += 1
        elif c in ')>]}':
            depth -= 1
        elif c == ',' and depth == 0:
            spans.append((at, i))
            at = i + 1
    spans.append((at, end))
    return spans


def param_name(param):
    """(the parameter's declaration with a name, its name, whether it had one); default argument dropped, and a
    per-version parameter after it (version_params) left out: the parameter's own."""
    if VERSION_PARAM.search(param):
        param = version_params(param)[0]
    decl = split_top(param, '=')[0].strip() if '=' in param else param.strip()
    if '(' in decl or '...' in decl:
        raise Fail('a parameter this tool does not read: %r' % param.strip())
    tokens = re.findall(r'[A-Za-z_]\w*|::|\S', decl)
    named = (len(tokens) >= 2 and re.fullmatch(r'[A-Za-z_]\w*', tokens[-1]) and tokens[-1] not in TYPE_WORDS
             and tokens[-2] != '::')
    if named and len(tokens) >= 2 and all(t in TYPE_WORDS for t in tokens):
        named = False
    if named:
        return decl, tokens[-1], True
    return decl, None, False


class VersionParam:
    """One per-version parameter form: kind 'ONLY' or 'EXCEPT', the game, the declaration without its value, the
    parameter's name, the value (None in a declaration), and where the form is in its text (start, end)."""

    def __init__(self, kind, game, decl, name, value, start, end):
        self.kind, self.game, self.decl, self.name, self.value = kind, game, decl, name, value
        self.start, self.end = start, end

    def games(self):
        return {self.game} if self.kind == 'ONLY' else ALL_GAMES - {self.game}

    def param(self):
        return 'READABLE_PARAM_%s(%s, %s)' % (self.kind, self.game, self.decl)

    def arg(self):
        return 'READABLE_PARAM_%s(%s, %s)' % (self.kind, self.game, self.name)

    def constant(self):
        return 'READABLE_PARAM_%s_VALUE(%s, %s = %s)' % (self.kind, self.game, self.decl, self.value)


def version_params(text, path='', offset=0):
    """(text with its per-version forms taken out, [VersionParam]) of a parameter's text (or a whole list's); a form's
    start and end are offsets into text, plus offset."""
    found, plain, at = [], [], 0
    for m in VERSION_PARAM.finditer(text):
        if m.start() < at:
            continue
        close = match_close(text, m.end() - 1, '(', ')')
        args = split_top(text[m.end():close - 1])
        if len(args) < 2 or args[0].strip() not in VERSION_GAMES:
            raise Fail('%s: a per-version parameter this tool does not read: %r' % (path, text[m.start():close]))
        game = args[0].strip()
        rest = text[m.end():close - 1].split(',', 1)[1]
        parts = split_top(rest, '=')
        decl = parts[0].strip()
        value = '='.join(parts[1:]).strip() if len(parts) > 1 else None
        decl_named, name, had = param_name(decl)
        if not had:
            raise Fail('%s: a per-version parameter with no name: %r' % (path, decl))
        plain.append(text[at:m.start()])
        if not ''.join(plain).strip():
            raise Fail('%s: %s: a per-version parameter follows the parameter before it, with no comma between (a '
                       'first one that a version has not is an own body\'s, ee/readable_waits.h)' % (path, name))
        found.append(VersionParam(m.group(1), game, decl, name, value, offset + m.start(), offset + close))
        at = close
    plain.append(text[at:])
    return ''.join(plain), found


def version_definitions(code, path, blocks):
    """The edits that make each namespace-scope definition with per-version parameters one each version compiles: the
    forms' values out of the parameter list (a default argument in the version that has it), and each value a
    constant at the top of the body (READABLE_PARAM_*_VALUE: the versions without the parameter). A declaration
    with a value stops: the value is the definition's."""
    edits, done = [], set()
    for m in VERSION_PARAM.finditer(code):
        if inside(blocks, m.start()):
            continue
        # The parameter list the form is in: the innermost ( still open before it.
        depth, k = 0, m.start() - 1
        while k >= 0:
            if code[k] == ')':
                depth += 1
            elif code[k] == '(':
                if depth == 0:
                    break
                depth -= 1
            k -= 1
        if k < 0:
            raise Fail('%s: a per-version parameter outside a parameter list' % path)
        if k in done:
            continue
        done.add(k)
        close = match_close(code, k, '(', ')')
        rest = re.match(r'\s*(?:const\s*)?(?:noexcept\s*)?(\{|;|=|:)', code[close:])
        plain_params, forms = [], []
        try:
            for start, end in split_spans(code, k + 1, close - 1):
                own, found = version_params(code[start:end], path, start)
                plain_params.append(own)
                forms += found
        except Fail:
            if rest and rest.group(1) == '{':
                raise
            continue    # (a call's argument at namespace scope, a static's initialiser: no parameter list)
        # A name every version has (a plain parameter, or forms that cover every game: another order) has no value.
        covered = {param_name(p)[1] for p in plain_params if p.strip() and p.strip() != 'void'}
        games = {}
        for f in forms:
            games.setdefault(f.name, set()).update(f.games())
        covered |= {n for n, g in games.items() if g == ALL_GAMES}
        if rest and rest.group(1) == '{':
            brace = close + rest.end() - 1
            constants = []
            for f in forms:
                if f.name in covered:
                    if f.value is not None:
                        raise Fail('%s: %s: every version has this parameter (in another place): no value' % (path, f.name))
                elif f.value is None:
                    raise Fail('%s: %s: a per-version parameter of a definition gives the value the other versions '
                               'have in its place (TYPE NAME = VALUE)' % (path, f.name))
                else:
                    constants.append(f.constant())
                edits.append((f.start, f.end - f.start, f.param()))
            if constants:
                edits.append((brace + 1, 0, ' ' + ' '.join(constants)))
        elif any(f.value is not None for f in forms):
            raise Fail('%s: a per-version parameter\'s value in a declaration: it is the definition\'s' % path)
    return edits


def registrations(code):
    """[(start, end of the argument list, args, form, macro start, kind)] of the registrations at namespace scope;
    kind: '' the plain form, '_OWN' (ee/readable_waits.h: a version's own body under the shared symbol) or '_ONLY'
    (a function only one engine version has)."""
    out = []
    for m in REG.finditer(code):
        close = match_close(code, m.end() - 1, '(', ')')
        args = [a.strip() for a in split_top(code[m.end():close - 1])]
        out.append((m.end(), close - 1, args, m.group(1) or '', m.start(), m.group(2) or ''))
    return out


_entry_reads = None


def entry_reads():
    """{(function, game or None): [register number, ...]} of readable_entry_reads.tsv's rows that name callee-saved
    registers one by one (a range or `*` is a row for readable_pass.py's mirrors, not a place). A row whose second
    column is a game (ai, dm2, bmx; the registers then the third) is that game's alone; one without is every game's."""
    global _entry_reads
    if _entry_reads is None:
        _entry_reads = {}
        for line in open(ENTRY_READS, encoding='utf-8'):
            if line.startswith('#') or not line.strip():
                continue
            fields = line.rstrip('\n').split('\t')
            game = fields.pop(1) if len(fields) > 1 and fields[1] in ALL_GAMES else None
            name, registers = fields[:2]
            parts = registers.split(',')
            if all(p in SAVED for p in parts):
                _entry_reads[(name, game)] = [SAVED[p] for p in parts]
    return _entry_reads


def body_registers(name, count, own, only):
    """A body's ee::LeftByCaller registers, one text each in declaration order: a number, or the instance's choice
    where the table's rows differ by game; None when a game the body runs in has no row of count registers. own:
    {own body: (its game, the function it is the body of)}, only: {an _ONLY function: its game}."""
    rows = entry_reads()
    if name in own:
        # (an own body runs in its game alone: its own row, else its function's row for that game)
        game, function = own[name]
        per = {game: rows.get((name, None)) or rows.get((name, game)) or rows.get((function, game))}
    else:
        games = (only[name],) if name in only else VERSION_GAMES
        per = {g: rows.get((name, g)) or rows.get((name, None)) for g in games}
    if any(r is None or len(r) != count for r in per.values()):
        return None
    games = [g for g in VERSION_GAMES if g in per]
    out = []
    for i in range(count):
        text = '%d' % per[games[0]][i]
        if any(per[g][i] != per[games[0]][i] for g in games):
            # (the last game's is the else: the copy is one text, compiled once per instance)
            text = '%d' % per[games[-1]][i]
            for g in reversed(games[:-1]):
                if per[g][i] != per[games[-1]][i]:
                    text = '::game::readable::kInstanceGame == ::game::games::%s ? %d : %s' % (g, per[g][i], text)
            text = '(%s)' % text
        out.append(text)
    return out


def left_by_caller(code, path, bodies, own=None, only=None):
    """The edits that give each ee::LeftByCaller in a registered body (name, brace, end) its register."""
    edits, placed = [], set()
    for name, brace, end in bodies:
        found = [m for m in LEFT_BY_CALLER.finditer(code, brace, end)]
        if not found:
            continue
        registers = body_registers(name, len(found), own or {}, only or {})
        if registers is None:
            mine = sorted('%s%s: %d' % (n, ' (%s)' % g if g else '', len(r)) for (n, g), r in entry_reads().items()
                          if n == name or n == (own or {}).get(name, ('', ''))[1])
            raise Fail('%s: %s: %d ee::LeftByCaller, and readable_entry_reads.tsv names %s for it' %
                       (path, name, len(found), ', '.join(mine) if mine else 'no register'))
        for m, register in zip(found, registers):
            close = match_close(code, m.end() - 1, '<', '>')
            if len(split_top(code[m.end():close - 1])) != 1:
                raise Fail('%s: %s: an ee::LeftByCaller that names its register: the place is the table\'s' % (path, name))
            edits.append((close - 1, 0, ', %s' % register))
            placed.add(m.start())
    for m in LEFT_BY_CALLER.finditer(code):
        if m.start() not in placed:
            raise Fail('%s: an ee::LeftByCaller outside a registered function\'s body: it has no place' % path)
    return edits


def rewrite(text, path, own_inline=False):
    """The build copy's text and the entries' names; own_inline: only the own bodies' READABLE_OWN_INLINE."""
    code = mask(text)
    bodies = []
    blocks = namespace_scope(code)
    edits = []  # (offset, length to replace, new text)
    inline_edits = []   # (the own bodies' READABLE_OWN_INLINE among them, for --own-inline)
    names = []
    symbols = {}
    reg_at = {}     # name -> where its registration starts (the copy names its body NAME_ there)
    only = set()    # the _ONLY form's functions: their bodies are inline (AI's build emits nothing of them)
    own_game = {}   # an own body (the _OWN form) -> its game: inline in another build, its entry that game's alone
    own_of = {}     # an own body -> (its game, the function it is the body of): its row of readable_entry_reads.tsv
    only_game = {}  # an _ONLY function -> its game
    for args_start, args_end, args, form, macro_at, kind in registrations(code):
        if inside(blocks, args_start):
            raise Fail('%s: a registration inside a block' % path)
        if kind == '_ONLY':
            # _ONLY(GAME, FN): FN_ the body, FN its entry at the game's own symbol; the copy's form is
            # _ONLY_AT(GAME, FN, FN_). (Only that game's code calls it: another instance has no such function, and
            # the entry, inline as the body is, is emitted only where it is called.) Its status is the game's table's
            # (a status word after FN, the form before tools19, is read by nothing and kept as written.)
            if len(args) not in (2, 3) or args[0] not in GAMES or not re.fullmatch(r'[A-Za-z_]\w*', args[1]):
                raise Fail('%s: an _ONLY registration this tool does not read: %r' % (path, args))
            game, name = args[:2]
            if name.endswith('_'):
                raise Fail('%s: %s: a body named with an underscore' % (path, name))
            edits.append((macro_at, args_start - 1 - macro_at, 'READABLE_FUNCTION%s_ONLY_AT' % form))
            edits.append((args_start, args_end - args_start, ', '.join([game, name, name + '_'] + args[2:])))
            names.append(name)
            symbols[name] = 'sym::%s::%s' % (game, name)
            reg_at[name] = macro_at
            only.add(name)
            only_game[name] = game
            continue
        if kind == '_OWN':
            # _OWN(FN, STATUS, GAME, OWN[, GAME, OWN]): FN's body and each own body get an entry at FN's symbol (in
            # GAME's instance the function there is OWN); the copy's form is _OWN_AT(FN, FN_, STATUS, GAME, OWN_, ...).
            if len(args) not in (4, 6) or not all(re.fullmatch(r'[A-Za-z_]\w*', a) for a in args):
                raise Fail('%s: an _OWN registration this tool does not read: %r' % (path, args))
            name, symbol, status = args[0], 'sym::' + args[0], args[1]
            owns = args[3::2]
            if any(n.endswith('_') for n in [name] + owns):
                raise Fail('%s: %s: a body named with an underscore' % (path, name))
            rewritten = [name, name + '_', status] + [x for g, o in zip(args[2::2], owns) for x in (g, o + '_')]
            edits.append((macro_at, args_start - 1 - macro_at, 'READABLE_FUNCTION%s_OWN_AT' % form))
            edits.append((args_start, args_end - args_start, ', '.join(rewritten)))
            for n in [name] + owns:
                names.append(n)
                symbols[n] = symbol
                reg_at[n] = macro_at
            for g, o in zip(args[2::2], owns):
                if g not in GAMES:
                    raise Fail('%s: %s: an own body of %r, a game with no instance' % (path, o, g))
                own_game[o] = g
                own_of[o] = (g, name)
            continue
        if len(args) == 2:
            name, symbol = args[0], 'sym::' + args[0]
        elif len(args) == 3:
            symbol, name = args[0], args[1]
        else:
            raise Fail('%s: a registration this tool does not read: %r' % (path, args))
        if not re.fullmatch(r'[A-Za-z_]\w*', name):
            raise Fail('%s: registration of %r: a plain name only' % (path, name))
        if name.endswith('_'):
            raise Fail('%s: %s: the body is named with an underscore; a plain source names it plainly' % (path, name))
        status = args[-1]
        edits.append((args_start, args_end - args_start, '%s, %s_, %s' % (symbol, name, status)))
        names.append(name)
        symbols[name] = symbol
        reg_at[name] = macro_at
    if len(set(names)) != len(names):
        raise Fail('%s: a function registered twice' % path)

    for name in names:
        found = []
        for m in re.finditer(r'(?<![\w.>:~])%s\s*\(' % re.escape(name), code):
            if inside(blocks, m.start()):
                continue
            open_paren = m.end() - 1
            close_paren = match_close(code, open_paren, '(', ')')
            rest = re.match(r'\s*(?:const\s*)?(?:noexcept\s*)?(\{|;|=|:)', code[close_paren:])
            if not rest:
                continue
            if rest.group(1) != '{':
                continue  # a declaration (or a call at namespace scope: a static's initialiser)
            found.append((m.start(), open_paren, close_paren, close_paren + rest.end() - 1))
        if len(found) != 1:
            raise Fail('%s: %s: %d definitions at namespace scope (want exactly one)' % (path, name, len(found)))
        name_at, open_paren, close_paren, brace = found[0]
        body_end = match_close(code, brace, '{', '}')
        # The declaration's start: after the last ; } { at namespace scope before the name.
        k = name_at - 1
        while k >= 0 and code[k] not in ';{}':
            k -= 1
        prefix = ' '.join(code[k + 1:name_at].split())
        # (a per-version result, READABLE_RESULT_ONLY(GAME, TYPE) / _EXCEPT: the return type, a form of its own)
        bare = prefix
        for m in reversed(list(VERSION_RESULT.finditer(prefix))):
            bare = bare[:m.start()] + 'TYPE' + bare[match_close(prefix, m.end() - 1, '(', ')'):]
        if not prefix or '(' in bare or '=' in bare or 'template' in prefix.split():
            raise Fail('%s: %s: a declaration this tool does not read: %r' % (path, name, prefix))
        if name in only and 'inline' not in prefix.split():
            raise Fail('%s: %s: an _ONLY function is defined inline (AI\'s build must emit nothing of it)' % (path, name))
        params = split_top(code[open_paren + 1:close_paren - 1])
        if len(params) == 1 and params[0].strip() in ('', 'void'):
            params = []
        decls, call_names, firsts = [], [], []
        for i, p in enumerate(params):
            # (a per-version parameter, ee/readable_waits.h: the form follows the parameter before it, and each
            # declaration of the entry names it as that form, and the entry's call passes it with the form)
            p, forms = version_params(p, path)
            if forms and not p.strip():
                raise Fail('%s: %s: a per-version parameter follows the parameter before it (a first one that a '
                           'version has not is an own body\'s, ee/readable_waits.h)' % (path, name))
            decl, pname, had = param_name(p)
            if not had:
                pname = 'argument%d' % i
                decl = decl + ' ' + pname
            trail = ''.join(' ' + f.param() for f in forms)
            decls.append(decl + trail)
            call_names.append(pname + ''.join(' ' + f.arg() for f in forms))
            firsts.append(p.strip() + trail)
        has_defaults = any(len(split_top(p, '=')) > 1 for p in params)
        first_params = ', '.join(firsts) if has_defaults else ', '.join(decls)
        qualifiers = ' '.join(code[close_paren:brace].split())
        # An _ONLY function's entry is an external function of GAME's instance (READABLE_ONLY_DEFINE: defined there
        # only), so that another file calls it by name as it calls any engine function, declared in the header as
        # any is (matrix.h: ee::Ptr<Vec4> transformPoint(...);); the inline body stays the file's. In another build
        # there is no entry: its callers are steps that build discards (if constexpr on kIsDm2), which need none.
        # An own body (_OWN) is the same: its body inline but in its game's instance (another build emits nothing of
        # it), its entry that game's alone.
        entry_prefix = ' '.join(w for w in prefix.split() if w != 'inline') if name in only or name in own_game else prefix
        forward = '%s %s(%s)%s; ' % (entry_prefix, name, first_params, ' ' + qualifiers if qualifiers else '')
        call = '::game::detail::Route<&%s_>::template at<%s, ::game::readable::kInstanceGame>' % (name, symbols[name])
        entry = (' %s %s(%s)%s { return %s(%s); }'
                 % (entry_prefix, name, ', '.join(decls), ' ' + qualifiers if qualifiers else '', call, ', '.join(call_names)))
        if name in only:    # (ee/readable_waits.h: in the game's instance only)
            entry = ' READABLE_ONLY_DEFINE(%s,%s)' % (symbols[name].split('::')[1], entry)
        elif name in own_game:
            entry = ' READABLE_ONLY_DEFINE(%s,%s)' % (own_game[name], entry)
        # The forward declaration goes where the declaration starts (on its line, after leading blanks).
        start = k + 1
        while start < name_at and code[start] in ' \t\n':
            start += 1
        body_prefix = prefix
        if name in own_game and 'inline' not in prefix.split():
            # (an own body is inline outside its game's instance, so that build emits nothing of it, and in its own
            # instance as written: READABLE_OWN_INLINE, ee/readable_waits.h. Before the forward declaration's insertion
            # at the same place: the copy reads `forward; READABLE_OWN_INLINE(GAME) body`)
            edits.append((start, 0, 'READABLE_OWN_INLINE(%s) ' % own_game[name]))
            inline_edits.append(edits[-1])
            body_prefix = 'READABLE_OWN_INLINE(%s) %s' % (own_game[name], prefix)
        edits.append((start, 0, forward))
        if reg_at[name] < start:
            # A registration before the definition (d4collision4's query.cpp: _OWN(collisionQuery, ..., dm2,
            # collisionQueryUncached) above collisionQueryUncached): the source names the function as its header
            # declares it, the copy names the renamed body, so the body is declared there too (no default arguments:
            # the definition keeps them), on the registration's line.
            edits.append((reg_at[name], 0, '%s %s_(%s)%s; ' % (body_prefix, name, ', '.join(decls),
                                                               ' ' + qualifiers if qualifiers else '')))
        edits.append((name_at, len(name), name + '_'))
        edits.append((body_end, 0, entry))
        bodies.append((name, brace, body_end))
    edits += left_by_caller(code, path, bodies, own_of, only_game)
    edits += version_definitions(code, path, blocks)
    if own_inline:
        edits = inline_edits
    out = text
    for offset, length, new in sorted(edits, key=lambda e: e[0], reverse=True):
        out = out[:offset] + new + out[offset + length:]
    return out, names


def line_path(source, root):
    """The copy's #line name for SOURCE: relative to ROOT, '/' separators (absolute only where no relative path
    exists: another drive on Windows)."""
    try:
        path = os.path.relpath(os.path.abspath(source), os.path.abspath(root))
    except ValueError:
        path = os.path.abspath(source)
    return path.replace('\\', '/')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--own-inline', action='store_true', help='the source with only its own bodies made READABLE_OWN_INLINE')
    ap.add_argument('--line-root', help='the folder the copy\'s #line is relative to (the build\'s top: the compiler\'s '
                    'working folder); default the working folder')
    ap.add_argument('paths', nargs='+')
    a = ap.parse_args()
    try:
        if a.check:
            for p in a.paths:
                _, names = rewrite(open(p, encoding='utf-8').read(), p)
                print('%s: %d entries' % (p, len(names)))
            return 0
        if len(a.paths) != 2:
            ap.error('SOURCE OUT')
        source, out = a.paths
        text = open(source, encoding='utf-8').read()
        body, _ = rewrite(text, source, a.own_inline)
        made = '#line 1 "%s"\n%s' % (line_path(source, a.line_root or os.getcwd()), body)
        if not os.path.exists(out) or open(out, encoding='utf-8').read() != made:
            os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
            with open(out, 'w', encoding='utf-8') as f:
                f.write(made)
        return 0
    except Fail as e:
        print('readable_entries: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
