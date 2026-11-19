#!/usr/bin/env python3
"""readable_plaincalls.py: the calls in the readable game that are not plain C++ calls yet, by kind and folder.

Game code calls a game function by its name, as a typed C++ call: `moveObjectBox(box, delta)`, the function
defined under that name and declared with the types its callers pass (docs/READABLE.md, "Calls"). What is
counted here is every other way of making a call, and the bodies still named with a trailing underscore:

  game_call       GAME_CALL(name) declarations: a variadic entry that converts what it is given
  game_call_va    GAME_CALL_VARIADIC(name)
  game_call_t     game::call<&fn>, game::callAt<&fn, address>
  game_tail_t     game::tailCall<&fn>, game::tailCallAt<&fn, address>, game::tail(fn, ...)
  route           GAME_ROUTE / GAME_TABLE_ROUTE lines
  addr_readable   ee::game / ee::call / ee::tailCall to a function with a readable body here (by its symbol)
  addr_runtime    the same to a function without one (a library's, an unported one), or through a pointer
  body_           a registered body named with a trailing underscore (its plain name is GAME_CALL's)
  to_plain        a function of a plain source (plain_sources.txt) handed on by its address: `&name` there is
                  its entry (tools/readable_entries.py), not the body the tables register. game::call<&name>
                  and game::tailCall<&name> find no registration for it and go to address 0; callAt,
                  detail::direct, Route and ee::entry give the entry a frame of its own around the one it
                  makes, or register it in place of the body. So every `&name` (and a bare `name` as a
                  template argument) counts: a template argument (game::call<&name>, stepDone<&name>(),
                  a helper's `template <auto Fn>` that hands it to game::call), a value handed to a call,
                  stored or returned. Not counted: what calls it as a host function and only that, which
                  reaches the entry as a plain call does: `game::tail(name, ...)`, `(&name)(...)`, and a
                  variable `f = &name` whose every other use in the file is a call `f(...)`. A name that
                  the file also declares as a variable is that variable; a qualifier names one of the
                  function's namespaces, not a class. Always 0. A game folder (dm2/, bmx/) is counted with
                  its own plain functions and the engine's (its instance's), AI's folders with theirs: a name
                  both have is each game's own function.

    readable_plaincalls.py                 the table, by folder (aggressive/x, zge/x, dm2/x, sce)
    readable_plaincalls.py --json FILE     the counts as JSON
    readable_plaincalls.py --check         exit 1 if any kind in any folder is above the baseline
    readable_plaincalls.py --rebaseline    write the baseline (only ever down: it refuses a rise)
    readable_plaincalls.py --to-plain      list the to_plain places (file:line: what hands the address on)

Comments and strings are left out. Folders are the first two path parts under src/game.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
GAME = os.path.normpath(ztree.GAME)
BASELINE = os.path.join(HERE, 'readable_plaincalls_baseline.json')
KINDS = ['game_call', 'game_call_va', 'game_call_t', 'game_tail_t', 'route', 'addr_readable', 'addr_runtime', 'body_',
         'to_plain']
SKIP_DIRS = {'ee'}  # the dispatch itself


def strip(code):
    """Code with comments and string/char literals blanked (newlines kept)."""
    out, i, n = [], 0, len(code)
    while i < n:
        c = code[i]
        if code.startswith('//', i):
            j = code.find('\n', i)
            j = n if j < 0 else j
            i = j
        elif code.startswith('/*', i):
            j = code.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * code.count('\n', i, j))
            i = j
        elif c == '"' or c == "'":
            if c == "'" and i and (code[i - 1].isalnum()):  # 0x1'000 digit separators
                out.append(c)
                i += 1
                continue
            j = i + 1
            while j < n and code[j] != c:
                j += 2 if code[j] == '\\' else 1
            out.append(c + c)
            i = j + 1
        elif code.startswith('R"', i) and (i == 0 or not code[i - 1].isalnum()):
            m = re.match(r'R"([^(]*)\(', code[i:])
            end = code.find(')' + m.group(1) + '"', i)
            out.append('""')
            i = end + len(m.group(1)) + 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def folder(rel):
    parts = rel.split('/')
    return '/'.join(parts[:2]) if len(parts) > 2 else parts[0]


def sources():
    for root, dirs, files in os.walk(GAME):
        dirs[:] = sorted(d for d in dirs if not (root == GAME and d in SKIP_DIRS))
        for f in sorted(files):
            if f.endswith(('.cpp', '.h')):
                p = os.path.join(root, f)
                yield os.path.relpath(p, GAME), p


# (a registration READABLE_EXCEPT(GAME, ...) wraps, _OWN too, is read as the inner one: its arguments stop at their
# own closing parenthesis, the wrapper's after it)
REG = re.compile(r'\bREADABLE_FUNCTION(?:_WAITS|_VU0)?(_IN|_OWN)?\s*\(([^;)]*)\)\s*(?:\)\s*)?;')
CONST = re.compile(r'\b(k\w+)\s*=\s*sym::(\w+)\b')
ADDR_CALL = re.compile(r'\bee::(game|call|tailCall)\s*(?:<[^;(]*?>)?\s*\(\s*([\w:]+)')


def registered_names(texts):
    """The names of functions with a readable body (a registration's symbol and C++ name)."""
    names, underscored = set(), []
    for rel, code in texts.items():
        for m in REG.finditer(code):
            args = [a.strip() for a in m.group(2).split(',')]
            if m.group(1) == '_OWN':  # (FN, STATUS, GAME, OWN[, GAME, OWN]): the shared function and a version's own body
                names.add(args[0])
                names.update(args[3::2])
                continue
            if args and args[0] in ('dm2',):
                args = args[1:]
            if len(args) == 2:  # (fn, STATUS)
                names.add(args[0])
            elif len(args) >= 3:  # (sym::name, fn, STATUS)
                if args[0].startswith('sym::'):
                    names.add(args[0][5:])
                fn = args[1].split('::')[-1]
                names.add(fn.rstrip('_'))
                if fn.endswith('_'):
                    underscored.append((rel, fn))
    return names, underscored


GAME_FOLDERS = ('dm2', 'bmx')  # a game's own folder (its instance only): its names are that game's functions


def count():
    texts = {rel: strip(open(p, encoding='utf-8', errors='replace').read()) for rel, p in sources()}
    # (a game folder calls its game's functions: those with a body there are its own registrations)
    names, underscored = registered_names({r: c for r, c in texts.items() if r.split('/')[0] not in GAME_FOLDERS})
    folder_names = {g: registered_names({r: c for r, c in texts.items() if r.split('/')[0] == g})[0] for g in GAME_FOLDERS}
    underscored += [u for g in GAME_FOLDERS for u in registered_names({r: c for r, c in texts.items() if r.split('/')[0] == g})[1]]
    table = {}

    def add(rel, kind, n=1):
        if n:
            row = table.setdefault(folder(rel), {k: 0 for k in KINDS})
            row[kind] += n

    everywhere = {}     # (a game folder's constants are its game's names: kept apart, by folder)
    for rel, code in texts.items():
        side = rel.split('/')[0] if rel.split('/')[0] in GAME_FOLDERS else ''
        for k, v in CONST.findall(code):
            everywhere.setdefault((side, k), set()).add(v)
    for rel, code in texts.items():
        add(rel, 'game_call', len(re.findall(r'(?<![\w#])GAME_CALL\s*\(', code)))
        add(rel, 'game_call_va', len(re.findall(r'(?<![\w#])GAME_CALL_VARIADIC\s*\(', code)))
        add(rel, 'game_call_t', len(re.findall(r'\bgame::call(?:At)?\s*<', code)))
        add(rel, 'game_tail_t', len(re.findall(r'\bgame::tail(?:Call(?:At)?\s*<|\s*\()', code)))
        add(rel, 'route', len(re.findall(r'(?<![\w#])GAME_(?:TABLE_)?ROUTE\s*\(', code)))
        consts = dict(CONST.findall(code))
        for m in ADDR_CALL.finditer(code):
            target = m.group(2)
            key = target.split('::')[-1]
            sym = target[5:] if target.startswith('sym::') else consts.get(key)
            side = rel.split('/')[0] if rel.split('/')[0] in GAME_FOLDERS else ''
            if sym is None and len(everywhere.get((side, key), ())) == 1:
                sym = next(iter(everywhere[(side, key)]))  # a constant from a header
            add(rel, 'addr_readable' if sym in folder_names.get(rel.split('/')[0], names) else 'addr_runtime')
    for rel, fn in underscored:
        add(rel, 'body_')
    for rel, code in texts.items():
        add(rel, 'to_plain', len(plain_addresses(code, plain_of(texts, rel))))
    return table


_PLAIN_SIDES = {}


def plain_of(texts, rel):
    """The plain functions a file's `&name` can be: its game folder's and the engine's (zge, sce) for a game folder's
    file (its instance), else every plain source but the game folders' (AI's)."""
    head = rel.split('/')[0]
    side = head if head in GAME_FOLDERS else ''
    if side not in _PLAIN_SIDES:
        keep = (side, 'zge', 'sce') if side else None
        _PLAIN_SIDES[side] = plain_functions({r: c for r, c in texts.items()
                                              if (r.split('/')[0] in keep if keep else r.split('/')[0] not in GAME_FOLDERS)})
    return _PLAIN_SIDES[side]


def namespaces_at(code, pos):
    """The named namespaces open at pos."""
    stack = []
    for m in re.finditer(r'[{}]', code[:pos]):
        if m.group() == '{':
            n = re.search(r'\bnamespace(?:\s+([\w:]+))?\s*$', code[max(0, m.start() - 200):m.start()])
            stack.append((n.group(1) or '').split('::') if n else None)
        elif stack:
            stack.pop()
    return [part for s in stack if s for part in s if part]


def plain_functions(texts):
    """{name: the namespace names around its registrations} of the functions registered in a plain source."""
    plain_list = os.path.join(GAME, 'plain_sources.txt')
    prefixes = [l.strip() for l in open(plain_list) if l[:1].isalpha()] if os.path.exists(plain_list) else []
    plain = {}
    for rel, code in texts.items():
        if rel.endswith('.cpp') and any(rel.startswith(p) for p in prefixes):
            for m in REG.finditer(code):
                for name in registered_names({rel: m.group(0)})[0]:
                    plain.setdefault(name, set()).update(namespaces_at(code, m.start()))
    return plain


UNARY_AFTER = {'return', 'case', 'co_return', 'throw'}


def plain_addresses(code, plain):
    """[(offset, text)] of the places code hands on a plain function's address (the to_plain kind)."""
    if not plain:
        return []
    names = '|'.join(sorted(plain, key=len, reverse=True))
    code = REG.sub(lambda r: re.sub(r'[^\n]', ' ', r.group(0)), code)  # its registrations name it
    found = []
    variables = {}  # name -> whether code declares a variable of that name

    def declares(before):
        """Whether text ending in a name declares it: a type, then * or &."""
        t = re.search(r'(\w+|[>\]])\s*[*&]*\s*$', before)
        return t is not None and t.group(1) not in UNARY_AFTER and not t.group(1).isdigit()

    def in_body(pos):
        """Whether pos is inside a function's body (a block), not at namespace or class scope."""
        depth, k = 0, pos
        while k > 0:
            k -= 1
            c = code[k]
            if c == '}':
                depth += 1
            elif c == '{':
                if depth:
                    depth -= 1
                    continue
                head = code[max(0, k - 300):k]
                if re.search(r'\bnamespace(\s+[\w:]+)?\s*$|\bextern\s+""\s*$', head) or \
                        re.search(r'\b(struct|class|union|enum)\b[^;{}()]*$', head):
                    return False
                return True
        return False

    def is_variable(name):
        if name not in variables:
            variables[name] = any(declares(code[:v.start()]) for v in re.finditer(
                r'(?<![\w.>:])%s\s*(?:[=;,)\[{]|:(?!:))' % name, code)) or \
                re.search(r'(?<![\w.>:])%s\s*(?:[-+*/|&^]|<<|>>)?=(?!=)' % name, code) is not None or \
                any(declares(code[:v.start()]) and in_body(v.start()) for v in re.finditer(
                    r'(?<![\w.>:])%s\s*\(' % name, code))  # `T name(init);` in a body: a local's direct init
        return variables[name]

    for m in re.finditer(r'(?<![\w.>:])(::)?((?:\w+::)*)(%s)\b(?!\s*(?:\(|\.|->|\[|::))' % names, code):
        name, qual = m.group(3), [q for q in m.group(2).split('::') if q]
        if qual and qual[-1] not in plain[name]:
            continue  # another namespace's or a class's function of that name
        before = code[:m.start()].rstrip()
        address = before.endswith('&') and not before.endswith('&&')
        if address:
            ahead = before[:-1].rstrip()
            word = re.search(r'(\w+)$', ahead)
            if ahead[-1:] in (')', ']') or (word and word.group(1) not in UNARY_AFTER):
                continue  # a binary &, or a reference declarator
            start = len(ahead)
        else:
            # a bare name: only as a template argument (`helper<name>`) or a value handed on
            ahead = before
            if not (re.search(r'\w\s*<$', ahead) and re.match(r'\s*[,>]', code[m.end():])) \
                    and not (ahead[-1:] in ('=', '(', ',', '?', ':', '{') and re.match(r'\s*[;,)}:]', code[m.end():])):
                continue
            if re.search(r'\bgame::tail\s*\($', ahead):
                continue  # game::tail(name, ...): the entry, called
            if re.search(r'\bGAME_\w+\s*\($', ahead):
                continue  # GAME_ROUTE(name, ...): the name a route gives
            if re.search(r'\boffsetof\s*\([^()]*$', ahead):
                continue  # a member's name
            if re.search(r'\boperator\s*\W*$|\btemplate\s*<$|\btypename\s*$', ahead):
                continue
            start = len(ahead)
        if not qual and is_variable(name):
            continue  # the file's own variable of that name
        if address and ahead.endswith('(') and re.match(r'\s*\)\s*\(', code[m.end():]):
            continue  # (&name)(...): called
        held = re.search(r'(?<![\w.>:])(\w+)\s*(?:=|\{)\s*$', ahead)
        if held and not re.search(r'\b(?:return|case)$', ahead[:held.start(1)].rstrip()):
            var = held.group(1)
            uses = [u for u in re.finditer(r'(?<![\w.>:])%s\b' % re.escape(var), code) if u.start() != held.start(1)]
            if uses and all(re.match(r'\s*\(', code[u.end():]) or declares(code[:u.start()]) for u in uses):
                continue  # f = &name; f(...): a host function pointer, called
        found.append((start, code[start:m.end()]))
    return found


def totals(table):
    t = {k: 0 for k in KINDS}
    for row in table.values():
        for k in KINDS:
            t[k] += row.get(k, 0)
    return t


def show(table):
    width = max([len(f) for f in table] + [6])
    print('%-*s %s' % (width, 'folder', ' '.join('%13s' % k for k in KINDS)))
    for f in sorted(table):
        print('%-*s %s' % (width, f, ' '.join('%13d' % table[f][k] for k in KINDS)))
    t = totals(table)
    print('%-*s %s' % (width, 'total', ' '.join('%13d' % t[k] for k in KINDS)))


def rises(table, base):
    out = []
    for f, row in table.items():
        for k in KINDS:
            was = base.get(f, {}).get(k, 0)
            if row[k] > was:
                out.append('%s %s: %d, baseline %d' % (f, k, row[k], was))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--json', metavar='FILE')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--rebaseline', action='store_true')
    ap.add_argument('--baseline', default=BASELINE)
    ap.add_argument('--to-plain', action='store_true')
    a = ap.parse_args()
    if a.to_plain:
        texts = {rel: strip(open(p, encoding='utf-8', errors='replace').read()) for rel, p in sources()}
        n = 0
        for rel, code in sorted(texts.items()):
            for at, what in plain_addresses(code, plain_of(texts, rel)):
                n += 1
                print('%s:%d: %s' % (rel, code.count('\n', 0, at) + 1, ' '.join(what.split())))
        print('readable_plaincalls: to_plain %d' % n)
        return 1 if n else 0
    table = count()
    if a.json:
        with open(a.json, 'w') as f:
            json.dump({'folders': table, 'total': totals(table)}, f, indent=1, sort_keys=True)
    base = json.load(open(a.baseline)) if os.path.exists(a.baseline) else None
    if a.check:
        if base is None:
            print('readable_plaincalls: no baseline (%s)' % a.baseline)
            return 2
        bad = rises(table, base)
        t, b = totals(table), totals(base)
        print('readable_plaincalls: %s' % ', '.join('%s %d (%+d)' % (k, t[k], t[k] - b[k]) for k in KINDS))
        for line in bad:
            print('  rose: ' + line)
        return 1 if bad else 0
    if a.rebaseline:
        if base is not None and rises(table, base):
            for line in rises(table, base):
                print('  rose: ' + line)
            print('readable_plaincalls: not rebaselined: counts only go down')
            return 1
        with open(a.baseline, 'w') as f:
            json.dump(table, f, indent=1, sort_keys=True)
            f.write('\n')
        print('readable_plaincalls: baseline written (%s)' % ', '.join('%s %d' % kv for kv in totals(table).items()))
        return 0
    if not a.json:
        show(table)
    return 0


if __name__ == '__main__':
    sys.exit(main())
