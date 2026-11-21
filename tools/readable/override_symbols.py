#!/usr/bin/env python3
"""The games' addresses as names in the host's game overrides (engine/overrides, games/<game>/overrides), and in the
collection's extras converters (collection/extras: the data tables they read from a game's ELF).

An override reaches its game's functions and data as the readable code does, by name, from the same table
(readable_symbols.tsv, made from the work database) and the same bind tables:

    #include "sym/game_overrides/dave_mirra_gameplay.cpp.h"
    kSym.cameraSettings = sym::dm2::g_cameraSettings;   // DM2's own row
    runtime.lookupFunction(sym::dm2::drawWidget);       // AI's name, where DM2's bind table puts it
    runtime.lookupFunction(sym::actionFrame);           // Aggressive Inline's own
    ra == points::dm2::showPauseScreen::itemsSet        // a resume point of the game's own (a return address)

The overrides of every game are built into one runner (a unity build puts several into one unit), so a game's
names are always qualified: sym::dm2::NAME, sym::bmx::NAME; a bare sym::NAME is Aggressive Inline's. A name
resolves to the game's own row first (the game column of readable_symbols.tsv), else to AI's row of that name
bound through the game's table (<bind dir>/<game>.tsv with games/<game>/bound.tsv, readable_symbols.py's Bind).
The bind tables are the tree's own (tools/readable/bind, committed), so every build resolves a bound name the same
way, whether or not it compiles that game's code: an override and a converter are compiled into every runner and
tool, and the panel's captures read a game's RAM from a dump in any build. A bound name the game's table does not
bind, or a game with no table, fails the build, naming the symbol: no name becomes an address nothing has.

    headers --out DIR [--bind-dir DIR]   one header per override source that includes "sym/game_overrides/..."
                                         (a converter: "sym/collection/extras/..."), written only when its text
                                         changes
    lint                                 an address literal in any override or converter fails (every source
                                         of the two folders, whether it includes its header or not), and so
                                         does a machine reference in a comment (FUN_, DAT_, LAB_, fn_ or D_ and
                                         an address: write the name)

This is not a checker: it reads readable_symbols.py's table and bind code and changes neither.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import readable_symbols as rs  # noqa: E402

# The folders whose sources name a game's addresses: (the symbol headers' folder, its path). The overrides are four
# folders (the engine's and each game's: zlayout.overrides_dirs, in the runner's order), their headers one folder,
# sym/game_overrides, as when they were one.
FOLDERS = tuple(('game_overrides', at) for _, at in rs.zaxis_paths.zlayout.overrides_dirs(rs.TREE)) + (
    ('collection/extras', os.path.join(rs.TREE, 'collection', 'extras')),)
GAMES = ('dm2', 'bmx')
# The games' bind tables, the tree's own (readable_symbols.py bind --write).
BIND_DIR = os.path.join(rs.HERE, 'bind')
_NAME = re.compile(r'\bsym::(?:(dm2|bmx)::)?(?!level::|status::)(\w+)')
_POINT = re.compile(r'\bpoints::(dm2|bmx)::(\w+)::(\w+)')   # a game's own resume point (a return address)
_COMMENT = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)
_STRING = re.compile(r'"(?:\\.|[^"\\\n])*"')
_ADDRESS = re.compile(r'\b0x00(?:1[0-9A-Fa-f]|2[0-9A-Fa-f]|3[01])[0-9A-Fa-f]{4}u?\b')
_MACHINE = re.compile(r'\b(?:FUN|DAT|LAB|fn|D)_(?:00)?[0-9A-Fa-f]{6}\b')   # in a comment (readable_symbols' rule, and D_)


def sources(every=False):
    """(folder, file, text) of each source that includes its symbol header (every: each source)."""
    for folder, at in FOLDERS:
        for f in sorted(os.listdir(at)) if os.path.isdir(at) else ():
            if f.endswith(('.cpp', '.h')):
                text = open(os.path.join(at, f), encoding='utf-8', errors='replace').read()
                if every or '#include "sym/%s/' % folder in text:
                    yield folder, f, text


def code_of(text):
    return _COMMENT.sub('', _STRING.sub('""', text))


def resolve(t, binds, game, name):
    """(address or None, why): the game's own row, else AI's bound there."""
    if game is None:
        entry = t.syms.get(name)
        return (entry[1], None) if entry else (None, 'no row of readable_symbols.tsv')
    own = t.own(game).syms.get(name)
    if own:
        return own[1], None
    entry = t.syms.get(name)
    if not entry:
        return None, 'no %s row and no AI row of readable_symbols.tsv (readable_symbols.py add --game %s)' % (game, game)
    bind = binds.get(game)
    if bind is None:
        return None, "AI's %s, bound through %s's table, which the bind folder does not have" % (name, game)
    value = bind.function(entry[1]) if entry[0] == 'function' else bind.datum(entry[1])
    return (value, None) if value is not None else (None, "AI's %s, which %s's bind table does not bind" % (name, game))


def cmd_headers(args):
    t = rs.table()
    binds = {}
    bind_dir = args.bind_dir or BIND_DIR
    for game in GAMES:
        path = os.path.join(bind_dir, game + '.tsv')
        if os.path.exists(path):
            binds[game] = rs.Bind(path)
    failed = []
    written = 0
    for folder, f, text in sources():
        names = sorted(set(_NAME.findall(code_of(text))), key=lambda n: (n[0] or '', n[1]))
        lines = ['#pragma once',
                 '// Generated by tools/override_symbols.py headers: the symbols %s/%s names, at each '
                 "game's addresses." % (folder, f), '#include <cstdint>']
        for game, name in names:
            value, why = resolve(t, binds, game or None, name)
            if value is None:
                failed.append('%s: sym::%s%s: %s' % (f, game + '::' if game else '', name, why))
                continue
            guard = 'SYM_%s_%s' % (game, name) if game else 'SYM_%s' % name
            space = 'sym::%s' % game if game else 'sym'
            lines += ['#ifndef %s' % guard, '#define %s' % guard,
                      'namespace %s { inline constexpr uint32_t %s = 0x%08Xu; }' % (space, name, value), '#endif']
        for game, function, name in sorted(set(_POINT.findall(code_of(text)))):
            address = t.own(game).points.get((function, name))
            if address is None:
                failed.append('%s: points::%s::%s::%s is in no row of readable_symbols.tsv (readable_symbols.py add '
                              'point %s ADDRESS %s --game %s)' % (f, game, function, name, name, function, game))
                continue
            guard = 'POINTS_%s_%s_%s' % (game, function, name)
            lines += ['#ifndef %s' % guard, '#define %s' % guard,
                      'namespace points::%s::%s { inline constexpr uint32_t %s = 0x%08Xu; }' % (game, function, name, address),
                      '#endif']
        if rs.write_if_changed(os.path.join(args.out, 'sym', folder, f + '.h'), '\n'.join(lines) + '\n'):
            written += 1
    if failed:
        raise SystemExit('override_symbols.py:\n  ' + '\n  '.join(failed))
    return 0


def cmd_lint(args):
    bad = []
    files = 0
    refs = 0
    for folder, f, text in sources(every=True):
        files += 1
        for n, line in enumerate(code_of(text).split('\n'), 1):
            for m in _ADDRESS.finditer(line):
                bad.append('%s/%s:%d: %s' % (folder, f, n, m.group(0)))
        for m in _COMMENT.finditer(text):
            for ref in _MACHINE.finditer(m.group(0)):
                refs += 1
                bad.append('%s/%s:%d: %s: a machine reference in a comment: write the function\'s name' %
                           (folder, f, text.count('\n', 0, m.start()) + 1, ref.group(0)))
    for b in bad:
        print(b)
    print('override_symbols lint: %d address literals, %d machine references in comments, in %d sources '
          '(game_overrides, collection/extras)' % (len(bad) - refs, refs, files))
    return 1 if bad else 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    h = sub.add_parser('headers')
    h.add_argument('--out', required=True)
    h.add_argument('--bind-dir', default='', help='the bind tables (default: the tree\'s tools/readable/bind)')
    sub.add_parser('lint')
    args = p.parse_args()
    return {'headers': cmd_headers, 'lint': cmd_lint}[args.cmd](args)


if __name__ == '__main__':
    sys.exit(main())
