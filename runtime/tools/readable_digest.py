#!/usr/bin/env python3
"""Digests of the readable functions' source (docs/READABLE.md, "How a
function is proven"): a zx-fncheck pass names the digest of the readable function
it was made against (impl=), and the work database's stale_verified view lists
Verified functions whose current digest has no passing check.

A function's digest is the sha256 (first 16 hex digits) of its definition and
of every function of the same file it calls (by name, transitively; all
overloads of a name), each with comments and spacing taken out, in name order.
What it does not see: inline functions of headers, struct layouts, and
functions of other files (readable callees are checked themselves; a header
change that matters shows in the recordings of the functions using it).

    readable_digest.py [--cpp OUT.cpp] [--json] [--bind-dir DIR] FILE_OR_DIR ...

--cpp writes the table zx-fncheck is built with (written only when its text
changes); --json prints [{game, addr, name, file, status, status_in, digest}]
(status_in: `source`, the registration's own word, or `table`, an _ONLY
registration's, whose status is its game's game/<game>/symbols_status.tsv
row); with neither, one line a registration. --bind-dir: the bind tables a game folder's registration of an AI name
is read through (readable_symbols.literal_view; the build's ZX_BIND_DIR, empty: none), else the tree's own
(tools/bind).
"""
import argparse
import hashlib
import json
import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import readable_symbols

GAMES = {'ai': 0, 'dm2': 1, 'bmx': 2}
REG = re.compile(r'\bREADABLE_(FUNCTION|FUNCTION_WAITS|FUNCTION_VU0)\(\s*(0x[0-9A-Fa-f]+)\s*,\s*([A-Za-z_][\w:]*)\s*,\s*(\w+)\s*\)')
REG_IN = re.compile(r'\bREADABLE_FUNCTION(?:_WAITS|_VU0)?_IN\(\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*([A-Za-z_][\w:]*)\s*,\s*(\w+)\s*\)')
# (a keyword before a parenthesis is never a function's name: `if constexpr (c) {` is a step, not a
# definition named constexpr)
KEYWORDS = {'if', 'for', 'while', 'switch', 'catch', 'return', 'sizeof', 'alignof', 'decltype', 'static_assert',
            'defined', 'noexcept', 'requires', 'operator', 'constexpr', 'consteval', 'alignas', 'typeid', 'throw',
            'case', 'else', 'do'}
IDENT_CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
IDENT = re.compile(r'\b[A-Za-z_]\w*\b')


def strip(text):
    """Comments out; string and character literals kept whole, their contents
    included (a changed message is a changed digest), lengths kept for nothing:
    the result is only searched and hashed. match() steps over the literals, so
    a '{' in one is no brace; an identifier scan of the text sees the words in
    them (more reached, never less: in the tree today only the runtime's and the
    checker's own files, which register nothing, reach a definition that way)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
            out.append(' ')
        elif c in '"\'':
            # raw strings are rare in game code; plain escapes handled
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def match(text, i, open_, close):
    """Index after the bracket matching text[i] (== open_), or -1. A bracket in a string or character literal is
    not one (strip keeps their contents: screen_space.cpp's scanName compares with '{', and its definition ran to
    the end of the file, so the level walk charged it the symbols of every function after it)."""
    depth = 0
    j, n = i, len(text)
    while j < n:
        c = text[j]
        if c in '"\'' and not (c == "'" and j and text[j - 1].isalnum()):     # (1'000: a digit separator)
            k = j + 1
            while k < n and text[k] != c and text[k] != '\n':
                k += 2 if text[k] == '\\' else 1
            j = k + 1
            continue
        if c == open_:
            depth += 1
        elif c == close:
            depth -= 1
            if depth == 0:
                return j + 1
        j += 1
    return -1


def definitions(code):
    """{name: [definition text, ...]}: every `name(...) [qualifiers] {...}`."""
    defs = {}
    for m in IDENT_CALL.finditer(code):
        name = m.group(1)
        if name in KEYWORDS or name.isupper():
            continue
        p = m.end() - 1
        q = match(code, p, '(', ')')
        if q < 0:
            continue
        rest = code[q:q + 200]
        t = re.match(r'\s*(?:const\s*|noexcept\s*|override\s*|final\s*|->\s*[\w:<>,\s*&]+?\s*)*\{', rest)
        if not t:
            continue
        b = q + t.end() - 1
        e = match(code, b, '{', '}')
        if e < 0:
            continue
        defs.setdefault(name, []).append(code[m.start():e])
    return defs


def normal(text):
    return ' '.join(text.split())


def file_digests(path):
    source = open(path, encoding='utf-8', errors='replace').read()
    # (an _ONLY registration's status is its game's table's, game/<game>/symbols_status.tsv: literal_view gives it; a
    # game folder's registration and AI's say theirs in the source)
    only = {(m.group(2), m.group(3)) for m in readable_symbols._REG_ONLY.finditer(source)}
    raw = readable_symbols.literal_view(source)
    regs = []
    for m in REG.finditer(raw):
        regs.append(('ai', int(m.group(2), 16), m.group(3), m.group(4)))
    for m in REG_IN.finditer(raw):
        regs.append((m.group(1), int(m.group(2), 16), m.group(3), m.group(4)))
    if not regs:
        return []
    code = strip(raw)
    defs = definitions(code)
    out = []
    for game, addr, fn, status in regs:
        short = fn.split('::')[-1]
        seen, todo = set(), [short]
        while todo:
            n = todo.pop()
            if n in seen or n not in defs:
                continue
            seen.add(n)
            for d in defs[n]:
                for ident in set(IDENT.findall(d)):
                    if ident in defs and ident not in seen:
                        todo.append(ident)
                    elif ident + '_' in defs and ident + '_' not in seen:
                        todo.append(ident + '_')  # (a GAME_CALL name: its body has the marker)
        h = hashlib.sha256()
        for n in sorted(seen):
            for d in sorted(normal(x) for x in defs[n]):
                h.update(d.encode('utf-8'))
                h.update(b'\n')
        out.append({'game': game, 'addr': addr, 'name': fn, 'file': path, 'status': status,
                    'status_in': 'table' if (game, short) in only else 'source',
                    'digest': h.hexdigest()[:16] if seen else None})
    return out


def sources(items):
    for item in items:
        if os.path.isdir(item):
            for root, dirs, files in os.walk(item):
                dirs.sort()
                for f in sorted(files):
                    if f.endswith('.cpp'):
                        yield os.path.join(root, f)
        else:
            yield item


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('paths', nargs='+')
    p.add_argument('--cpp')
    p.add_argument('--json', action='store_true')
    p.add_argument('--relative-to', help='file paths in --json relative to this folder')
    p.add_argument('--bind-dir', help="the bind tables (<dir>/<game>.tsv); empty: none")
    a = p.parse_args()
    if a.bind_dir is not None:
        readable_symbols.BIND_DIR = a.bind_dir
    rows = []
    for path in sources(a.paths):
        rows += file_digests(path)
    rows.sort(key=lambda r: (GAMES.get(r['game'], 99), r['addr']))
    if a.cpp:
        body = ''.join('    {%du, 0x%08xu, "%s"},\n' % (GAMES.get(r['game'], 99), r['addr'], r['digest'] or '-')
                       for r in rows)
        text = ('// Generated by tools/readable_digest.py: the readable functions\' source digests\n'
                '// (fncheck_build.h), %d.\n#include "ee/fncheck_build.h"\n\nnamespace fncheck\n{\n'
                '    const Digest g_readableDigests[] = {\n%s        {0u, 0u, nullptr}};\n'
                '    const size_t g_readableDigestCount = %d;\n}\n' % (len(rows), body, len(rows)))
        try:
            same = open(a.cpp, encoding='utf-8').read() == text
        except OSError:
            same = False
        if not same:
            os.makedirs(os.path.dirname(os.path.abspath(a.cpp)), exist_ok=True)
            with open(a.cpp + '.tmp', 'w', encoding='utf-8', newline='\n') as f:
                f.write(text)
            os.replace(a.cpp + '.tmp', a.cpp)
        missing = sum(1 for r in rows if not r['digest'])
        print('readable_digest: %d registrations%s' % (len(rows), ', %d definitions not found' % missing if missing else ''))
    elif a.json:
        for r in rows:
            r['addr'] = '0x%08X' % r['addr']
            if a.relative_to:
                r['file'] = os.path.relpath(r['file'], a.relative_to)
        json.dump(rows, sys.stdout, indent=0)
    else:
        for r in rows:
            print('%s 0x%08X %-10s %s %s' % (r['game'], r['addr'], r['status'], r['digest'] or '-', r['name']))


if __name__ == '__main__':
    main()
