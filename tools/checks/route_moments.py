#!/usr/bin/env python3
"""Which game states a recording folder reached, read from its records (routes4): the audit of whether each
route's folder holds the moment the route is named for (a level, a screen, a game over, a save, a goal).

Every record (ps2/ee/fncheck.h Record) keeps the memory its call read (inputs) and wrote (outputs), 16-byte lines
with a mask of the bytes known, and the vsync it began at. A word the game keeps its state in (its mode, the screen
on show, the level, the menu) is in the lines of every call that read or wrote it, so the folder's records together
are a timeline of that word over the route: which values it took, from which vsync to which. No replay, no runner:
the folder alone.

    route_moments.py scan --game ai|dm2|bmx [--jobs N] [--out FILE.json] FOLDER ...
        each folder's timeline of its game's state words (STATE below) and its functions' vsync spans
    route_moments.py show FILE.json [ROUTE ...]
        the timelines, readable

The folder's files are read in either form (fnrec_io: plain or compressed). Values come from the lines whose mask
covers all of the word's bytes; a word no recorded call touched has no timeline (a targeted folder, a few functions:
its moment is the functions it targets, which the vsync spans show).
"""
import argparse
import json
import os
import struct
import sys
from concurrent.futures import ProcessPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fnrec_io  # noqa: E402  (plain or compressed: ps2/ee/fnrec_file.h)

HEADER = struct.Struct('<8sIIII')
LINE = 24
FULL_LIST = 500   # a folder with this many files or more was recorded with every function (or most): its state is there

# The state words of each game: name -> (address, bytes). The anchors the route tools close their loops on
# (routes.py/sweep.py, dm2routes.py, bmxroutes.py) and the work database's names for the rest.
STATE = {
    'ai': {
        'mode': (0x276FE4, 4),          # sweep.MODE: 1 front end, 2 level play, 3 paused
        'panel': (0x240A20, 4),         # sweep.PANEL: the zgui panel on top (0x831150 the main menu)
        'level': (0x24C9B4, 4),         # sweep.LEVEL: the level loaded
        'frame': (0x231CAC, 4),         # sweep.FRAME: frames since the level started
        'quit': (0x23FD64, 4),          # routes.GAME_QUIT: engineMain's loop runs while 0
        'timer': (0x277CCC, 4),         # routes.VISIBLE_TIMER: the HUD's countdown (0 when none)
        'editor': (0x278808, 4),        # routes.PE_EDITOR: the park editor object while it runs
    },
    'dm2': {
        'mode': (0x259770, 4),          # dm2routes.MODE: 0 boot and movies, 1 front end, 2 level, 3 paused
        'level_frame': (0x25BCC4, 4),   # dm2routes.LEVEL_FRAME
        'single_mode': (0x2566C9, 1),   # g_sessionSinglePlayerMode
        'session_level': (0x2566CA, 1), # g_sessionLevel
        'players': (0x2566CB, 1),       # dm2routes.SESSION +3: players
        'current_level': (0x25A1A8, 4), # g_currentLevel (dm2routes.LEVEL_CURSOR)
        'menu_mode': (0x25A1C0, 4),     # g_menuMode
        'game_screen': (0x25BBE4, 1),   # g_gameScreen (overlay.h GameScreen: 1 pause, 7 exit confirm, 9 round over, 10 game over, 11 save game)
        'quit': (0x2073B0, 4),          # dm2routes.QUIT_FLAG: main's loop flag
    },
    'bmx': {
        'mode': (0x29D040, 4),          # bmxroutes.MODE: 1 from the title on
        'level_frame': (0x2A2DE0, 4),   # bmxroutes.LEVEL_FRAME
        'session_kind': (0x29C5D9, 1),  # g_sessionKind
        'current_level': (0x29C5DE, 1), # g_currentLevel
        'menu': (0x2A2BB0, 4),          # g_menu: the front end menu's id (front_end_menus.h)
        'game_screen': (0x2A2D70, 4),   # g_gameScreen: 22 the game over screen
        'game_frames': (0x29C5D4, 4),   # g_gameFrames
        'quit': (0x252A94, 4),          # bmxroutes.QUIT_FLAG: engineMain's loop flag
    },
}


def _watch(game):
    """{line address: [(name, offset, size)]}"""
    out = {}
    for name, (addr, size) in STATE[game].items():
        out.setdefault(addr & ~15, []).append((name, addr & 15, size))
    return out


def _values(blob, watch, seen, vsync):
    """The watched words in a lines blob (n * 24 bytes): seen[name] gets (vsync, value)."""
    for line_addr, words in watch.items():
        key = struct.pack('<I', line_addr)
        at = blob.find(key)
        while at >= 0:
            if at % LINE == 0:
                mask = struct.unpack_from('<H', blob, at + 4)[0]
                data = blob[at + 8:at + 24]
                for name, off, size in words:
                    need = ((1 << size) - 1) << off
                    if mask & need == need:
                        seen.append((name, vsync, int.from_bytes(data[off:off + size], 'little')))
                break
            at = blob.find(key, at + 1)


def scan_file(args):
    """One file: (address, game number, calls, first vsync, last vsync, observations [(name, vsync, value)])."""
    path, game = args
    watch = _watch(game)
    seen = []
    calls, first, last = 0, None, None
    address = gamenum = None
    try:
        f = fnrec_io.open_plain(path, buffering=1 << 20)
    except Exception as e:  # (a file this Python cannot open: no zstd)
        return {'path': path, 'error': str(e)}
    with f:
        head = f.read(HEADER.size)
        if len(head) < HEADER.size or not head.startswith(b'FNREC1'):
            return {'path': path, 'error': 'not a recording'}
        _, version, address, ctx, gamenum = HEADER.unpack(head)
        head_bytes = 28 + (8 if version >= 3 else 0)
        call_bytes = (12 if version >= 2 else 8) + 64 + (64 if version >= 5 else 0) + 32 + ctx

        def u32():
            raw = f.read(4)
            if len(raw) < 4:
                raise EOFError
            return struct.unpack('<I', raw)[0]

        def blob():
            n = u32()
            if n > (1 << 24):
                raise EOFError
            data = f.read(n * LINE)
            if len(data) < n * LINE:
                raise EOFError
            return data

        def skip(n):
            if n and len(f.read(n)) < n:
                raise EOFError

        try:
            while True:
                raw = f.read(head_bytes)
                if len(raw) < head_bytes:
                    break
                magic, flags, _, vsync, _ = struct.unpack_from('<IIQQI', raw)
                if magic != fnrec_io.RECORD_MAGIC:
                    break
                skip(2 * ctx)
                _values(blob(), watch, seen, vsync)            # inputs: as the call found them
                _values(blob(), watch, seen, vsync)            # outputs: as it left them
                for _ in range(u32()):
                    skip(call_bytes)
                    _values(blob(), watch, seen, vsync)        # a callee's effects
                    if version >= 7:
                        blob()
                if version >= 5 and flags & 2:
                    skip(24 * u32())
                calls += 1
                first = vsync if first is None else min(first, vsync)
                last = vsync if last is None else max(last, vsync)
        except EOFError:
            pass
    return {'path': path, 'address': address, 'game': gamenum, 'calls': calls, 'first': first, 'last': last,
            'seen': seen}


def timeline(obs):
    """[(vsync, value)] -> runs of one value: [[value, first vsync, last vsync, observations]]"""
    runs = []
    for vsync, value in sorted(obs):
        if runs and runs[-1][0] == value:
            runs[-1][2] = vsync
            runs[-1][3] += 1
        else:
            runs.append([value, vsync, vsync, 1])
    return runs


def scan_folder(folder, game, pool):
    files = sorted(os.path.join(folder, n) for n in os.listdir(folder) if n.endswith('.fnrec'))
    results = list(pool.map(scan_file, [(p, game) for p in files], chunksize=4))
    obs = {}
    functions = {}
    errors = 0
    for r in results:
        if 'error' in r:
            errors += 1
            continue
        functions['%08x' % r['address']] = [r['calls'], r['first'], r['last']]
        for name, vsync, value in r['seen']:
            obs.setdefault(name, []).append((vsync, value))
    lasts = [v[2] for v in functions.values() if v[2] is not None]
    return {'folder': folder, 'game': game, 'files': len(files), 'errors': errors,
            'last_vsync': max(lasts) if lasts else None,
            'state': {name: timeline(o) for name, o in obs.items()}, 'functions': functions}


class Folder:
    """A scanned folder's state, asked by a moment's test (route_moments.tsv): each helper answers True or False and
    notes what it found (the evidence column)."""

    def __init__(self, scan):
        self.scan, self.notes = scan, []
        self.state = scan['state']

    def note(self, ok, text):
        self.notes.append(('' if ok else 'not ') + text)
        return ok

    def runs(self, name, value):
        return [(a, b) for v, a, b, _ in self.state.get(name, []) if v == value]

    def seen(self, name, value, label=None):
        """The word took the value."""
        r = self.runs(name, value)
        return self.note(bool(r), '%s %s%s' % (label or name, value, ' at %d' % r[0][0] if r else ''))

    def held(self, name, value, vsyncs, label=None):
        """The word kept the value at least VSYNCS vsyncs in all (from its first to its last run)."""
        r = self.runs(name, value)
        span = (r[-1][1] - r[0][0]) if r else 0
        return self.note(span >= vsyncs, '%s %s for %d vsyncs (%d-%d)' % (label or name, value, span, r[0][0] if r else 0,
                                                                         r[-1][1] if r else 0) + (' of %d' % vsyncs))

    def most(self, name, least, label=None):
        """The word's largest value is at least LEAST."""
        top = max([v for v, _, _, _ in self.state.get(name, []) if v < 0x80000000] or [0])
        return self.note(top >= least, '%s up to %d (%d wanted)' % (label or name, top, least))

    def last(self, vsync):
        """The folder's records reach that vsync (the route's end)."""
        got = self.scan['last_vsync'] or 0
        return self.note(got >= vsync, 'records to vsync %d (%d wanted)' % (got, vsync))

    def fn(self, address, label=None):
        """The function was recorded (a call of it kept)."""
        f = self.scan['functions'].get('%08x' % address)
        ok = bool(f and f[0])
        return self.note(ok, '%s recorded%s' % (label or hex(address), ' at %d' % f[1] if ok else ''))

    def seq(self, name, values, label=None):
        """The word took the values in that order (each at or after the one before)."""
        at, got = -1, []
        for want in values:
            r = [a for v, a, b, _ in self.state.get(name, []) if v == want and b >= at]
            if not r:
                return self.note(False, '%s %s in order (%s, then no %s)' % (
                    label or name, ','.join(map(str, values)), ' '.join('%d@%d' % g for g in got) or 'none', want))
            at = max(r[0], at)
            got.append((want, at))
        return self.note(True, '%s %s' % (label or name, ' '.join('%d@%d' % g for g in got)))

    def new_after(self, vsync, least):
        """At least LEAST functions ran for the first time at or after VSYNC (a shutdown, a new part of the game)."""
        n = sum(1 for f in self.scan['functions'].values() if f[1] is not None and f[1] >= vsync)
        return self.note(n >= least, '%d functions first run from vsync %d (%d wanted)' % (n, vsync, least))

    def files(self, least=1):
        n = self.scan['files']
        return self.note(n >= least, '%d files' % n)

    def nonzero(self, name, label=None):
        r = [(v, a) for v, a, _, _ in self.state.get(name, []) if v]
        return self.note(bool(r), '%s set%s' % (label or name, ' at %d' % r[0][1] if r else ''))

    def distinct(self, name, least, label=None):
        n = len({v for v, _, _, _ in self.state.get(name, [])})
        return self.note(n >= least, '%d %s values (%d wanted)' % (n, label or name, least))


def judge(scan_path, moments_path, game, folders=None, prefix=''):
    """[(route, moment, verdict, evidence)] for the moments of GAME whose folder is in the scan. PREFIX: the folders
    are re-recordings named PREFIX + the route (routes4's r4-): each is judged by its route's moment."""
    scans = json.load(open(scan_path))
    if prefix:
        scans = {k[len(prefix):]: v for k, v in scans.items() if k.startswith(prefix)}
    out = []
    for line in open(moments_path):
        if not line.strip() or line.startswith('#'):
            continue
        g, route, moment, test = line.rstrip('\n').split('\t')[:4]
        if g != game or (folders and route not in folders) or (prefix and route not in scans):
            continue
        if route not in scans:
            out.append((route, moment, 'absent', 'no folder'))
            continue
        f = Folder(scans[route])
        if test == 'targeted' or scans[route]['files'] < FULL_LIST:   # (a short function list: the state words are not in its records)
            out.append((route, moment, 'targeted', '%d files of its list, records to vsync %s' % (
                scans[route]['files'], scans[route]['last_vsync'])))
            continue
        helpers = {k: getattr(f, k) for k in ('seen', 'held', 'most', 'last', 'fn', 'files', 'nonzero', 'distinct', 'seq', 'new_after')}
        try:
            ok = eval(test, {'__builtins__': {}}, helpers)
        except Exception as e:  # (a test this scan cannot answer)
            out.append((route, moment, 'error', '%s: %s' % (test, e)))
            continue
        out.append((route, moment, 'yes' if ok else 'no', '; '.join(f.notes)))
    judged = {r[0] for r in out}
    for route in sorted(scans):   # (a folder with no moment line: a short list's, by its size)
        if route in judged or (folders and route not in folders):
            continue
        r = scans[route]
        out.append((route, '(its targets)', 'targeted' if r['files'] < FULL_LIST else 'unjudged',
                    '%d files of its list, records to vsync %s' % (r['files'], r['last_vsync'])))
    return out


def show(path, routes):
    data = json.load(open(path))
    for name, r in sorted(data.items()):
        if routes and name not in routes:
            continue
        print('%s: %d files, last vsync %s' % (name, r['files'], r['last_vsync']))
        for var, runs in sorted(r['state'].items()):
            text = ' '.join('%s@%d-%d' % (hex(v) if v > 9 else v, a, b) for v, a, b, _ in runs[:40])
            print('  %-14s %s%s' % (var, text, ' ...' if len(runs) > 40 else ''))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('scan')
    s.add_argument('--game', required=True, choices=sorted(STATE))
    s.add_argument('--jobs', type=int, default=4)
    s.add_argument('--out')
    s.add_argument('folders', nargs='+')
    v = sub.add_parser('show')
    v.add_argument('file')
    v.add_argument('routes', nargs='*')
    j = sub.add_parser('judge')
    j.add_argument('--game', required=True, choices=sorted(STATE))
    j.add_argument('--moments', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'route_moments.tsv'))
    j.add_argument('--prefix', default='', help='folders named PREFIX + route (re-recordings)')
    j.add_argument('file')
    j.add_argument('routes', nargs='*')
    a = ap.parse_args()
    if a.cmd == 'show':
        show(a.file, set(a.routes))
        return 0
    if a.cmd == 'judge':
        for route, moment, verdict, evidence in judge(a.file, a.moments, a.game, set(a.routes), a.prefix):
            print('%s\t%s\t%s\t%s' % (route, verdict, moment, evidence))
        return 0
    out = {}
    if a.out and os.path.exists(a.out):
        out = json.load(open(a.out))
    with ProcessPoolExecutor(a.jobs) as pool:
        for folder in a.folders:
            name = os.path.basename(os.path.normpath(folder))
            out[name] = scan_folder(folder, a.game, pool)
            r = out[name]
            print('%s: %d files, last vsync %s, %s' % (name, r['files'], r['last_vsync'],
                                                     ' '.join('%s:%d' % (k, len(v)) for k, v in sorted(r['state'].items()))),
                  flush=True)
            if a.out:
                tmp = a.out + '.tmp'
                with open(tmp, 'w') as f:
                    json.dump(out, f)
                os.replace(tmp, a.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
