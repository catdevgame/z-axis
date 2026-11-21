#!/usr/bin/env python3
"""Reads fncheck recordings (<address>.fnrec, ps2/ee/fncheck.h)
without replaying them: per file the version, the function, how many calls
it keeps, how many callee calls those made, and the vsyncs they span.

    fnrec_index.py FOLDER|FILE ...          a table
    fnrec_index.py --json FOLDER ...        the same as JSON

Used by readable_corpus.py for the corpus index.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fnrec_io  # noqa: E402  (plain or compressed: ps2/ee/fnrec_file.h)

HEADER = struct.Struct('<8sIIII')       # magic, version, address, context size, game (reserved before 6)
GAMES = ('ai', 'dm2')                   # ee/game_id.h
LINE = 24                               # fncheck::Line
KEEP_CALL_HEAD = struct.Struct('<III')  # target, sp, flags


def read_file(path):
    """{'address', 'version', 'calls', 'callee_calls', 'max_callee_calls', 'vsync_first', 'vsync_last',
    'bytes', 'damaged'}"""
    out = {'path': path, 'bytes': fnrec_io.plain_size(path), 'calls': 0, 'callee_calls': 0, 'max_callee_calls': 0,
           'vsync_first': None, 'vsync_last': None, 'damaged': False, 'special': 0}
    with fnrec_io.open_plain(path) as f:
        head = f.read(HEADER.size)
        if len(head) < HEADER.size:
            out['damaged'] = True
            return out
        magic, version, address, ctx_size, game = HEADER.unpack(head)
        if not magic.startswith(b'FNREC1'):
            out['damaged'] = True
            return out
        out['address'] = address
        out['version'] = version
        out['game'] = GAMES[game] if version >= 6 and game < len(GAMES) else ('ai' if version < 6 else str(game))
        out['context_size'] = ctx_size
        size = out['bytes']

        def lines():
            raw = f.read(4)
            if len(raw) < 4:
                raise EOFError
            n = struct.unpack('<I', raw)[0]
            if n > (1 << 24):
                raise EOFError
            f.seek(n * LINE, 1)

        while f.tell() < size:
            start = f.tell()
            try:
                raw = f.read(4 + 4 + 8 + 8 + 4)
                if len(raw) < 28:
                    break
                magic_r, flags, seq, vsync, dead_low = struct.unpack('<IIQQI', raw)
                if magic_r != 0x4C4C4143:
                    out['damaged'] = True
                    break
                if version >= 3:
                    f.seek(8, 1)          # cycles
                f.seek(2 * ctx_size, 1)   # entry, exit
                lines()
                lines()
                raw = f.read(4)
                if len(raw) < 4:
                    raise EOFError
                ncalls = struct.unpack('<I', raw)[0]
                for _ in range(ncalls):
                    f.seek(12 if version >= 2 else 8, 1)
                    f.seek(8 * 8, 1)          # a[8]
                    if version >= 5:
                        f.seek(8 * 8, 1)      # aHigh[8]
                    f.seek(8 * 4, 1)          # f[8]
                    f.seek(ctx_size, 1)       # after
                    lines()                   # effects
                    if version >= 7:
                        lines()               # reads: what the callee read of the caller's frame
                if version >= 5 and flags & 2:  # kIoLog: the I/O accesses (fncheck.h IoAccess, 24 bytes)
                    raw = f.read(4)
                    if len(raw) < 4:
                        raise EOFError
                    f.seek(24 * struct.unpack('<I', raw)[0], 1)
                if f.tell() > size:
                    raise EOFError
            except EOFError:
                out['damaged'] = True  # cut short (a run ended by a kill mid-write)
                f.seek(start)
                break
            out['calls'] += 1
            out['callee_calls'] += ncalls
            out['max_callee_calls'] = max(out['max_callee_calls'], ncalls)
            if flags & 1:
                out['special'] += 1
            out['vsync_first'] = vsync if out['vsync_first'] is None else min(out['vsync_first'], vsync)
            out['vsync_last'] = vsync if out['vsync_last'] is None else max(out['vsync_last'], vsync)
    return out


def read_folder(folder):
    res = []
    for name in sorted(os.listdir(folder)):
        if name.endswith('.fnrec'):
            res.append(read_file(os.path.join(folder, name)))
    return res


def main(argv):
    as_json = '--json' in argv
    paths = [a for a in argv[1:] if a != '--json']
    rows = []
    for p in paths:
        rows += read_folder(p) if os.path.isdir(p) else [read_file(p)]
    if as_json:
        json.dump(rows, sys.stdout, indent=1)
        return 0
    print('%-10s %3s %7s %9s %8s %10s  %s' % ('address', 'v', 'calls', 'callees', 'max', 'MB', 'vsyncs'))
    for r in sorted(rows, key=lambda r: -r['callee_calls']):
        game = r.get('game', 'ai')
        print('%s0x%08x %3s %7d %9d %8d %10.1f  %s-%s%s' % ('' if game == 'ai' else game + ':', r.get('address', 0), r.get('version'), r['calls'], r['callee_calls'],
                                                      r['max_callee_calls'], r['bytes'] / 1e6, r['vsync_first'], r['vsync_last'],
                                                      ' (cut short)' if r['damaged'] else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
