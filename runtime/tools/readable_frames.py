#!/usr/bin/env python3
"""readable_frames.py: the frame table of the readable functions (ee/abi.h: the original's stack frame of every
function, its register saves, and each callee's call site, ee::frameLayout, ee::siteReturn) as committed data, one
file per game in runtime/tools/frames/<game>.tsv, so a build with no generated code (the app, made before the
player's disc gives it its code) has the table. They are made from the generated code by fncheck_traced_gen.py's
scan (the same functions, unchanged), as runtime/tools/bind is made from the work database: the integration lane
writes them when generated code or the registrations change, a build that scans a game's code compares them, and
merge-check runs `check`. Proof scaffolding: as the readable functions stop needing the original's frames, these
shrink (SCAFFOLDING.md).

    readable_frames.py write --list traced_blocks.list --scan GAME=DIR [--scan ...] [--dir DIR]
        the scanned games' tables into DIR (default runtime/tools/frames)
    readable_frames.py check --list traced_blocks.list --scan GAME=DIR [--scan ...] [--dir DIR]
        exit 1, naming the games, when the committed tables are not what the scan makes
    readable_frames.py cpp --out readable_frames.cpp [--dir DIR]
        the frame table's source (fncheck_traced_gen.frame_table, with readable_locals.tsv) from the tables

A table's rows: `frame ADDRESS SIZE SAVES` (SAVES: reg:bytes:offset,... in fncheck_traced_gen's order; - for none) and
`call FUNCTION TARGET RETURN` (the one site's return address, 0 for several), sorted, hexadecimal.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fncheck_traced_gen as traced  # noqa: E402  (the scan and the table's source: unchanged, pinned)

GAMES = {0: 'ai', 1: 'dm2', 2: 'bmx'}
DEFAULT_DIR = os.path.join(HERE, 'frames')


def listed(path):
    """{game: {address: [files]}} and {game: {address}} of the list, as fncheck_traced_gen.main reads it: the
    functions of several entry_ chunks and the registered single entry_ chunks (their rows are the whole function's)."""
    chunked, framed = {}, {}
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        game, addr, gen, _out, *rest = line.split('\t')
        chunks = rest[0].split('|') if rest and rest[0] else []
        if chunks:
            chunked.setdefault(int(game), {})[int(addr, 16)] = [gen] + chunks
        elif traced.FUNC_FILE.search(os.path.basename(gen)) and os.path.basename(gen).startswith('entry_'):
            framed.setdefault(int(game), set()).add(int(addr, 16))
    return chunked, framed


def scanned(list_path, scans):
    """{game: text of its table} for each GAME=DIR."""
    chunked, framed = listed(list_path)
    out = {}
    for item in scans:
        game, folder = item.split('=', 1)
        game = int(game)
        frames, calls = [], []
        traced.scan(game, folder, set(), {}, [], frames, calls, {}, chunked.get(game), set(), framed.get(game))
        out[game] = table_text(game, frames, calls)
    return out


def table_text(game, frames, calls):
    lines = ['# readable_frames.py write: %s\'s frame table from its generated code (%d functions, %d call sites)'
             % (GAMES[game], len(frames), len(set(calls))),
             '# kind\taddress\tsize or target\tsaves or return']
    for g, address, size, regs in sorted(frames):
        saves = ','.join('%d:%d:0x%x' % r for r in regs) or '-'
        lines.append('frame\t0x%08x\t0x%x\t%s' % (address, size, saves))
    for g, function, target, ret in sorted(set(calls)):
        lines.append('call\t0x%08x\t0x%08x\t0x%08x' % (function, target, ret))
    return '\n'.join(lines) + '\n'


def load(directory):
    """(frames, calls) of every committed table, as fncheck_traced_gen's scan gives them."""
    frames, calls = [], []
    for game, name in sorted(GAMES.items()):
        path = os.path.join(directory, name + '.tsv')
        if not os.path.exists(path):
            continue
        for line in open(path, encoding='utf-8'):
            if line.startswith('#') or not line.strip():
                continue
            kind, a, b, c = line.rstrip('\n').split('\t')
            if kind == 'frame':
                regs = [] if c == '-' else [tuple(int(x, 0) for x in s.split(':')) for s in c.split(',')]
                frames.append((game, int(a, 16), int(b, 16), regs))
            elif kind == 'call':
                calls.append((game, int(a, 16), int(b, 16), int(c, 16)))
            else:
                sys.exit('readable_frames.py: %s: a row of no kind: %s' % (path, line.rstrip()))
    return frames, calls


def write_if_changed(path, text):
    old = open(path, encoding='utf-8').read() if os.path.exists(path) else None
    if old != text:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(text)
        return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('write', 'check'):
        p = sub.add_parser(name)
        p.add_argument('--list', required=True)
        p.add_argument('--scan', action='append', default=[], required=True)
        p.add_argument('--dir', default=DEFAULT_DIR)
    p = sub.add_parser('cpp')
    p.add_argument('--out', required=True)
    p.add_argument('--dir', default=DEFAULT_DIR)
    a = ap.parse_args()
    if a.cmd == 'cpp':
        frames, calls = load(a.dir)
        write_if_changed(a.out, traced.frame_table(frames, calls))
        return 0
    tables = scanned(a.list, a.scan)
    if a.cmd == 'write':
        os.makedirs(a.dir, exist_ok=True)
        for game, text in sorted(tables.items()):
            changed = write_if_changed(os.path.join(a.dir, GAMES[game] + '.tsv'), text)
            print('readable_frames: %s %s' % (GAMES[game], 'written' if changed else 'unchanged'))
        return 0
    stale = []
    for game, text in sorted(tables.items()):
        path = os.path.join(a.dir, GAMES[game] + '.tsv')
        if not os.path.exists(path) or open(path, encoding='utf-8').read() != text:
            stale.append(GAMES[game])
    if stale:
        print('readable_frames: the committed frame tables of %s are not what the generated code makes: '
              'readable_frames.py write, then commit runtime/tools/frames' % ', '.join(stale))
        return 1
    print('readable_frames: %s as the generated code makes them' % ', '.join(GAMES[g] for g in sorted(tables)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
