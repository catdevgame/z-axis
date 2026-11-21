#!/usr/bin/env python3
"""The work database's function notes as a public export: workdb.py's `card` and `notes` in a checkout that has no
database (a public clone: the database is built from the discs and stays on the workshop's machines).

    python3 -I tools/workdb/workdb.py card NAME|ADDR [--build ai_ps2|dm2_ps2|bmx_ps2] [--json]
    python3 -I tools/workdb/workdb.py notes [TOPIC|NAME|ADDR] [--build B] [--json]

(workdb.py answers from here when the duckdb module or the database is missing, or with WORKDB_PUBLIC=1.)

The export, workdb_notes.jsonl beside this file, is text only: one JSON line per note, with the game (ai, dm2, bmx),
the key of the function the database puts the note in (its name in that game's recomp/ghidra-functions.csv, the
public function map, whose rows but the entry_ labels are the database's functions, bounds and all; a note in no
function is kept under the readable name of its exact address, its own game's or AI's the bind tables pair it with,
`at` saying the symbol's kind, and left out when no public table names that address), the name the database gives
it, the AI counterparts' keys for a DM2 or BMX XXX function (the first is the one its card turns into; each one's
card shows its notes), the topic and the text. It has no addresses of its own: a card finds a function's address,
size and names in the public tables (each game's ghidra-functions.csv, tools/readable/readable_symbols.tsv, the bind
tables in tools/readable/bind), so the export stays the notes' words. `workdb.py export-notes` writes it from the
database (the workshop's step), held to public_scan.py's rules: the notes' sources are left out, a private or
workshop token in a text becomes "[workshop file]" and the like, and an address none of the public tables has
(another release's, an unnamed datum) becomes "[address]"; the file is scanned whole before it is written.
"""
import bisect
import collections
import csv
import json
import os
import re
import sys
import textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLING = os.path.dirname(HERE)     # (tools/)
ROOT = os.path.dirname(TOOLING)
EXPORT = os.environ.get('WORKDB_NOTES', os.path.join(HERE, 'workdb_notes.jsonl'))
GAMES = ('ai', 'dm2', 'bmx')
BUILD_OF = {'ai': 'ai_ps2', 'dm2': 'dm2_ps2', 'bmx': 'bmx_ps2'}
GAME_OF = {v: k for k, v in BUILD_OF.items()}
LABEL = {'ai': 'AI PS2', 'dm2': 'DM2 PS2', 'bmx': 'BMX PS2'}
TOPICS = ('purpose', 'cut', 'bug-kept', 'trigger', 'route', 'layout', 'evidence', 'caveat', 'history')
CARD_NOTES = 12


def h(a):
    return '0x%08X' % a


def parse_addr(s):
    s = str(s).strip()
    if s.lower().startswith('fun_'):
        s = s[4:]
    return int(s, 16)


# ------------------------------------------------------------------ the public tables

class Tables:
    """Each PS2 game's public function map and names: recomp/ghidra-functions.csv (name, start, end),
    tools/readable/readable_symbols.tsv (the readable names of functions, data and points) and the bind tables
    (DM2's and BMX XXX's functions against AI's)."""

    def __init__(self, root=ROOT):
        self.root = root
        self.funcs = {}       # game -> sorted [(start, end, name)]
        self.starts = {}      # game -> [start]
        self.ghidra = {}      # game -> {start: name}
        self.by_ghidra = {}   # game -> {name: [start]}
        self.readable = {g: collections.defaultdict(list) for g in GAMES}     # game -> {addr: [(kind, name)]}
        self.by_readable = {g: collections.defaultdict(list) for g in GAMES}  # game -> {name: [addr]} (functions)
        self.pairs = collections.defaultdict(list)   # (game, addr) -> [(other game, addr, confidence, cls)]
        self.data_pairs = {}                          # (game, addr) -> AI's addr: the bind tables' data rows
        self.by_symbol = {g: collections.defaultdict(list) for g in GAMES}    # game -> {name: [addr]} (every kind)
        for g in GAMES:
            rows = []
            p = os.path.join(root, 'games', {'ai': 'aggressive'}.get(g, g), 'workspace', 'recomp', 'ghidra-functions.csv')
            if os.path.exists(p):
                with open(p, newline='') as f:
                    for r in csv.DictReader(f):
                        # (the database's functions: the map without its entry_ rows, the labels inside a function,
                        # so an address is in the function the database puts it in)
                        a, e = int(r['Start'], 16), int(r['End'], 16)
                        if not r['Name'].startswith('entry_') and e > a:
                            rows.append((a, e, r['Name']))
            rows.sort()
            self.funcs[g] = rows
            self.starts[g] = [r[0] for r in rows]
            self.ghidra[g] = {s: n for s, _, n in rows}
            by = collections.defaultdict(list)
            for s, _, n in rows:
                by[n].append(s)
            self.by_ghidra[g] = by
        p = os.path.join(root, 'tools', 'readable', 'readable_symbols.tsv')
        if os.path.exists(p):
            for line in open(p, encoding='utf-8'):
                if line.startswith('#') or not line.strip():
                    continue
                c = line.rstrip('\n').split('\t')
                if len(c) < 3:
                    continue
                g = (c[4] if len(c) > 4 else '') or 'ai'
                if g not in self.readable:
                    continue
                try:
                    a = int(c[2], 16)
                except ValueError:
                    continue
                self.readable[g][a].append((c[0], c[1]))
                self.by_symbol[g][c[1]].append(a)
                if c[0] == 'function':
                    self.by_readable[g][c[1]].append(a)
        for g in ('dm2', 'bmx'):
            p = os.path.join(root, 'tools', 'readable', 'bind', g + '.tsv')
            if not os.path.exists(p):
                continue
            for line in open(p, encoding='utf-8'):
                c = line.rstrip('\n').split('\t')
                if line.startswith('#') or len(c) < 6 or c[0] not in ('entry', 'body', 'data'):
                    continue
                ai, other = int(c[1], 16), int(c[3], 16)
                if c[0] == 'data':
                    self.data_pairs[(g, other)] = ai
                    continue
                self.pairs[('ai', ai)].append((g, other, c[4], c[5]))
                self.pairs[(g, other)].append(('ai', ai, c[4], c[5]))

    def function(self, game, a):
        """(start, end, ghidra name) of game's function containing address a, or None."""
        i = bisect.bisect_right(self.starts[game], a) - 1
        if i >= 0:
            s, e, n = self.funcs[game][i]
            if s <= a < e:
                return s, e, n
        return None

    def key(self, game, start):
        """The export's key of game's function at start: its ghidra-functions.csv name (with @START when two
        functions share it); None when the map has no function there."""
        n = self.ghidra[game].get(start)
        if n is None:
            return None
        return n if len(self.by_ghidra[game][n]) == 1 else '%s@%08X' % (n, start)

    def symbol(self, game, a):
        """(kind, name) of the readable symbol at exactly address a in game (a function's first, then a datum's,
        then a point's), else, for DM2's and BMX XXX's, of AI's symbol the bind tables pair a with; or None."""
        srcs = [(game, a)]
        if game != 'ai':
            srcs.append(('ai', self.data_pairs.get((game, a))))
            srcs += [('ai', oa) for og, oa, _, _ in self.pairs.get((game, a), ()) if og == 'ai']
        for src in srcs:
            if src[1] is None:
                continue
            got = self.readable[src[0]].get(src[1], ())
            for kind in ('function', 'data', 'point'):
                for k, n in got:
                    if k == kind:
                        return kind, n
        return None

    def symbol_addr(self, game, name):
        """game's address of the readable symbol NAME (its own row, else the bind tables' pair of AI's), or None."""
        own = self.by_symbol[game].get(name)
        if own:
            return own[0]
        if game != 'ai':
            for ai in self.by_symbol['ai'].get(name, ()):
                for (g, a), x in self.data_pairs.items():
                    if g == game and x == ai:
                        return a
                for og, oa, _, _ in self.pairs.get(('ai', ai), ()):
                    if og == game:
                        return oa
        return None

    def names(self, game, start):
        out = [n for k, n in self.readable[game].get(start, ()) if k == 'function']
        g = self.ghidra[game].get(start)
        if g and g not in out:
            out.append(g)
        return out

    def known(self, a):
        """Whether a public table has address a: a function's range or a readable symbol, in any of the games."""
        return any(a in self.readable[g] or self.function(g, a) for g in GAMES)

    def lookup(self, name, games=GAMES):
        """[(game, start)] of the functions with this readable or ghidra name, AI's first."""
        out = []
        for g in games:
            for a in self.by_readable[g].get(name, ()):
                f = self.function(g, a)
                if f:
                    out.append((g, f[0]))
            for a in self.by_ghidra[g].get(name, ()):
                out.append((g, a))
            m = re.match(r'(.+)@([0-9A-Fa-f]{8})$', name)
            if m and int(m.group(2), 16) in self.by_ghidra[g].get(m.group(1), ()):
                out.append((g, int(m.group(2), 16)))
        seen, res = set(), []
        for x in out:
            if x not in seen:
                seen.add(x)
                res.append(x)
        return res


# ------------------------------------------------------------------ the export, read

class Export:
    def __init__(self, path=EXPORT):
        if not os.path.exists(path):
            sys.exit('workdb: no database and no public notes export (%s)' % path)
        self.meta = {}
        self.notes = []
        for line in open(path, encoding='utf-8'):
            if not line.strip():
                continue
            r = json.loads(line)
            if 'export' in r:
                self.meta = r
            else:
                self.notes.append(r)
        self.of = collections.defaultdict(list)        # (game, key) -> notes
        self.by_name = collections.defaultdict(list)   # name -> [(game, key)]
        self.cp = collections.defaultdict(list)        # AI key -> [(game, key)] of DM2's and BMX XXX's with notes
        for n in self.notes:
            k = (n['game'], n['key'])
            if not self.of[k]:
                for nm in {n['key'], n.get('name') or n['key']}:
                    self.by_name[nm].append(k)
                for ai in n.get('ai') or ():
                    self.cp[ai].append(k)
            self.of[k].append(n)
        self.symbols = collections.defaultdict(list)   # name -> [game] of the notes at a named address in no function
        for g, k in self.of:
            if self.of[(g, k)][0].get('at'):
                self.symbols[k].append(g)


def resolve(T, E, key, build=None):
    """(game, start, key) of the function a card is about, as workdb.py card_key: an address (AI's, BUILD's with
    --build), a name (AI's first; another game's own function when only it has the name); a DM2 or BMX XXX
    function with an AI counterpart is AI's card, which shows its notes."""
    games = (GAME_OF[build],) if build else GAMES
    found = None
    try:
        a = parse_addr(key)
    except ValueError:
        a = None
    if a is not None:
        f = T.function(games[0], a)
        if f:
            found = (games[0], f[0])
        else:
            sym = T.symbol(games[0], a)
            if sym and games[0] in E.symbols.get(sym[1], ()):
                return games[0], None, sym[1]
    if found is None and key in E.symbols:
        # a note at a named address in no function (a global, a function the map has only as entry_ labels)
        for g in games:
            if g in E.symbols[key]:
                return g, None, key
    if found is None:
        cands = T.lookup(key, games)
        if not cands:
            for g, k in E.by_name.get(key, ()):
                if g in games:
                    s = _start_of(T, g, k)
                    if s is not None:
                        cands.append((g, s))
        if cands:
            found = sorted(cands, key=lambda x: GAMES.index(x[0]))[0]
    if found is None:
        # a function the export has and the public tables no longer name (renamed since): by its key alone
        for g, k in E.by_name.get(key, ()):
            if g in games:
                return g, None, k
        return None
    g, s = found
    if g != 'ai':
        ai = _ai_of(T, E, g, s)
        if ai is not None and T.key('ai', ai):
            return 'ai', ai, T.key('ai', ai)
    return g, s, T.key(g, s)


def _start_of(T, g, k):
    m = re.match(r'(.+)@([0-9A-Fa-f]{8})$', k)
    if m:
        return int(m.group(2), 16)
    s = T.by_ghidra[g].get(k) or T.by_readable[g].get(k)
    return s[0] if s else None


def _ai_of(T, E, g, s):
    """The AI counterpart's start: the database's (the export's `ai`) for a function with notes, else the bind
    tables'."""
    own = E.of.get((g, T.key(g, s)), ())
    for n in own:
        if n.get('ai'):
            return _start_of(T, 'ai', n['ai'][0])
    if own:
        return None
    for og, oa, _, _ in T.pairs.get((g, s), ()):
        if og == 'ai':
            return oa
    return None


def _order(notes):
    return sorted(notes, key=lambda n: TOPICS.index(n['topic']) if n['topic'] in TOPICS else len(TOPICS))


def card_data(T, E, key, build=None):
    r = resolve(T, E, key, build)
    if r is None:
        return None
    g, s, k = r
    d = dict(game=g, build=BUILD_OF[g], key=k, addr=s, end=None, names=[], counterparts=[], notes=[],
             exported=E.meta.get('exported'), database_built=E.meta.get('database_built'))
    if s is not None:
        f = T.function(g, s)
        d['end'] = f[1] if f else None
        d['names'] = T.names(g, s)
        for og, oa, conf, cls in T.pairs.get((g, s), ()):
            d['counterparts'].append(dict(build=BUILD_OF[og], addr=oa, key=T.key(og, oa),
                                          names=T.names(og, oa), confidence=conf, cls=cls))
    own = E.of.get((g, k), [])
    d['name'] = (own[0].get('name') if own else None) or (d['names'][0] if d['names'] else k)
    notes = [dict(n, build=BUILD_OF[n['game']]) for n in _order(own)]
    if own and own[0].get('at'):
        # a named address in no function: its symbol, and the other games' notes under the same name
        d['symbol'] = own[0]['at']
        d['addr'] = T.symbol_addr(g, k)
        for og in GAMES:
            if og != g and og in E.symbols.get(k, ()):
                notes += [dict(n, build=BUILD_OF[n['game']]) for n in _order(E.of[(og, k)])]
    elif g == 'ai':
        for og, ok in sorted(E.cp.get(k, ()), key=lambda x: (GAMES.index(x[0]), x[1])):
            notes += [dict(n, build=BUILD_OF[n['game']]) for n in _order(E.of[(og, ok)])]
    d['notes'] = notes
    return d


def fmt_card(d, prefix='// ', width=100):
    out = []
    W = width - len(prefix)

    def line(s, indent=''):
        for t in textwrap.wrap(s, W, subsequent_indent=indent + '  ', break_long_words=True,
                               break_on_hyphens=False) or ['']:
            out.append(prefix + t)

    g = d['game']
    line('workdb card: %s %s %s (public notes export of %s, database built %s; tools/workdb.py card)' % (
        LABEL[g], h(d['addr']) if d['addr'] is not None else '(not in the public tables)', d['name'],
        d['exported'] or '?', d['database_built'] or '?'))
    if d.get('symbol'):
        line('a readable symbol (%s) in no function of the public map: its notes, by name' % d['symbol'])
    if d['addr'] is not None and d['end'] is not None:
        line('%d bytes (%s to %s, recomp/ghidra-functions.csv)' % (d['end'] - d['addr'], h(d['addr']), h(d['end'])))
    if d['names']:
        line('names: ' + ', '.join(d['names']), '  ')
    if d['counterparts']:
        line('counterparts (bind tables): ' + '; '.join(
            '%s %s %s (%s %s)' % (LABEL[GAME_OF[c['build']]], h(c['addr']), (c['names'] or [c['key'] or '(no name)'])[0],
                                  c['confidence'], c['cls']) for c in d['counterparts']), '  ')
    if d['notes']:
        line('notes (%d; workdb.py notes %s for all):' % (len(d['notes']), d['key'] if d['addr'] is None or
                                                            d.get('symbol') else
                                                            h(d['addr']) + ('' if g == 'ai' else ' --build ' + d['build'])))
        for n in d['notes'][:CARD_NOTES]:
            who = '' if n['game'] == g else '%s %s: ' % (LABEL[n['game']], n.get('name') or n['key'])
            t = n['text'] if len(n['text']) <= 500 else n['text'][:497] + '...'
            line('  [%s] %s%s' % (n['topic'], who, t), '    ')
        if len(d['notes']) > CARD_NOTES:
            line('  ... %d more' % (len(d['notes']) - CARD_NOTES))
    else:
        line('notes: none in the export')
    line('(the public export: the notes and the public tables. Callers, callees, strings, the other releases\' '
         'matches, recordings and coverage are in the full database, which is built from the discs: workdb.py build)')
    return '\n'.join(out)


def _opts(argv):
    argv = list(argv)
    o = dict(json='--json' in argv, prefix='// ', width=100, build=None)
    if o['json']:
        argv.remove('--json')
    for flag, conv in (('--prefix', str), ('--width', int), ('--build', str)):
        if flag in argv:
            i = argv.index(flag)
            o[flag[2:]] = conv(argv[i + 1])
            del argv[i:i + 2]
    if o['build'] and o['build'] not in GAME_OF:
        sys.exit('workdb: --build is a PS2 build: ai_ps2, dm2_ps2 or bmx_ps2')
    return o, argv


def cmd_card(argv):
    o, argv = _opts(argv)
    if not argv:
        sys.exit('usage: workdb.py card ADDR|NAME ... [--build ai_ps2|dm2_ps2|bmx_ps2] [--json] [--prefix "// "] '
                 '[--width 100]')
    T, E = Tables(), Export()
    res, rc = [], 0
    for k in argv:
        d = card_data(T, E, k, o['build'])
        if d is None:
            print('workdb: no %s function %s' % (LABEL[GAME_OF[o['build']]] if o['build'] else
                                                 'AI, DM2 or BMX PS2', k), file=sys.stderr)
            rc = 1
        elif o['json']:
            res.append(d)
        else:
            print(fmt_card(d, o['prefix'], o['width']))
    if o['json']:
        print(json.dumps(res if len(res) != 1 else res[0], indent=1))
    sys.exit(rc)


def cmd_notes(argv):
    o, argv = _opts(argv)
    E = Export()
    games = (GAME_OF[o['build']],) if o['build'] else GAMES
    notes = [n for n in E.notes if n['game'] in games]
    if not argv:
        c = collections.Counter(n['topic'] for n in notes)
        fns = collections.defaultdict(set)
        for n in notes:
            fns[n['topic']].add((n['game'], n['key']))
        print('%-8s  %5s  %9s' % ('topic', 'notes', 'functions'))
        for t in TOPICS:
            print('%-8s  %5d  %9d' % (t, c[t], len(fns[t])))
        return
    key = ' '.join(argv)
    if key in TOPICS:
        rows = [n for n in notes if n['topic'] == key]
    else:
        r = resolve(Tables(), E, key, o['build'])
        if r is None:
            sys.exit('workdb: %r is neither a topic (%s) nor a known function' % (key, ', '.join(TOPICS)))
        rows = _order(E.of.get((r[0], r[2]), []))
    if o['json']:
        print(json.dumps(rows, indent=1))
        return
    last = None
    for n in rows:
        if (n['game'], n['key']) != last:
            print('%s %s%s' % (LABEL[n['game']], n['key'], ' (%s)' % n['name'] if n.get('name') else ''))
            last = (n['game'], n['key'])
        print('  [%s] %s' % (n['topic'], n['text']))
    print('%d notes' % len(rows))


# ------------------------------------------------------------------ the export, written

PLACEHOLDER = (('personal.home', '[home folder]'), ('private.scratch', '[workshop file]'),
               ('private.host', '[workshop machine]'), ('private.device', '[test device]'),
               ('workshop.', '[workshop file]'), ('', '[private]'))
TOKEN_EDGE = set('()[]{},;"\'`')
HEX = re.compile(r'(?<![\w+-])0x([0-9A-Fa-f]{5,8})\b')
OTHER_RELEASE = re.compile(r'(?i)\bxbox\b|\.xbe\b|\bxbe\b|\bgc\b|gamecube|\.dol\b|\bdol\b')
PS2_RELEASE = re.compile(r'\bPS2\b|\bDM2\b|\bBMX\b|\bAI\b|\bSLUS')
SENTENCE = re.compile(r'[.;]\s|\n')


def _placeholder(rid):
    for prefix, text in PLACEHOLDER:
        if rid.startswith(prefix):
            return text
    return '[private]'


def redact_tokens(text, rules):
    """Each whitespace-delimited token a content rule matches becomes its placeholder; (text, count)."""
    n = 0
    for rid, rx in rules:
        for _ in range(200):
            m = rx.search(text)
            if not m:
                break
            s, e = m.start(), m.end()
            while s > 0 and not text[s - 1].isspace() and text[s - 1] not in TOKEN_EDGE:
                s -= 1
            while e < len(text) and not text[e].isspace() and text[e] not in TOKEN_EDGE:
                e += 1
            while e > s and text[e - 1] in '.:':    # (a sentence's full stop or colon stays)
                e -= 1
            text = text[:s] + _placeholder(rid) + text[e:]
            n += 1
    return text, n


def redact_addresses(text, tables):
    """An address none of the public tables has becomes [address]: one in another release's context (the last
    release named before it in its sentence is the Xbox's or the GameCube's), a GameCube-range code address, or a PS2
    image address no public table names; (text, count)."""
    out, last, n = [], 0, 0
    for m in HEX.finditer(text):
        v = int(m.group(1), 16)
        start = 0
        for sm in SENTENCE.finditer(text, 0, m.start()):
            start = sm.end()
        before = text[start:m.start()]
        other = [x.end() for x in OTHER_RELEASE.finditer(before)]
        ps2 = [x.end() for x in PS2_RELEASE.finditer(before)]
        drop = bool(other) and (not ps2 or other[-1] > ps2[-1])
        if not drop and 0x80000000 <= v < 0x81800000:
            drop = v & 3 == 0 and v & 0xFFFF != 0
        if not drop and 0x100000 <= v < 0x400000:
            drop = not tables.known(v)
        if drop:
            out.append(text[last:m.start()] + '[address]')
            last = m.end()
            n += 1
    out.append(text[last:])
    return ''.join(out), n


def write_export(rows, meta, rules, allow, out=EXPORT, rel=None):
    """rows: dicts with build, addr, func_addr (the start of the database's function it is in), topic, text, name,
    ai_addrs (a DM2 or BMX XXX function's AI counterparts, the one its card turns into first), in note order.
    rules: public_scan's rule list (with the workshop's private rules); allow: its allow list. Writes OUT (a
    temporary file renamed over it) when the whole file scans clean; returns a stats dict."""
    sys.path.insert(0, os.path.join(TOOLING, 'checks'))
    import public_scan as ps
    T = Tables()
    content = [(rid, re.compile(rx, re.M)) for rid, scope, rx, _ in rules
               if scope == 'content' and rx and rid.split('.')[0] in ('personal', 'secret', 'private', 'workshop')]
    stats = collections.Counter()
    lines = []
    for r in rows:
        g = GAME_OF.get(r['build'])
        if g is None:
            stats['other build'] += 1
            continue
        text, a = redact_addresses(r['text'].strip(), T)
        text, t = redact_tokens(text, content)
        stats['addresses redacted'] += a
        stats['tokens redacted'] += t
        s = ps.Scanner(rules, [])
        s.scan_text(text.encode('utf-8'), None, 'note')
        if s.findings:
            stats['dropped (still a finding)'] += 1
            continue
        # the function the database puts the note in (func_addr: its start, or the start of the function its
        # address is inside), by that function's public name; a note with neither is left out
        k = T.key(g, r['func_addr']) if r.get('func_addr') is not None else None
        at = None
        if k is None:
            # in no function: by the readable name of its exact address (its game's, or AI's the bind tables pair)
            sym = T.symbol(g, r['addr'])
            if sym is None:
                stats['left out (in no function, and no public table names its address)'] += 1
                continue
            at, k = sym
            stats['kept by a symbol name (in no function)'] += 1
        rec = collections.OrderedDict([('game', g), ('key', k)])
        if at:
            rec['at'] = at
        if r.get('name') and r['name'] != k:
            rec['name'] = r['name']
        ai = [T.key('ai', x) for x in r.get('ai_addrs') or () if g != 'ai' and not at]
        if ai and all(ai):
            rec['ai'] = ai
        elif ai:
            stats['left out (an AI counterpart the map has no function at)'] += 1
            continue
        rec['topic'] = r['topic']
        rec['text'] = text
        lines.append(json.dumps(rec, ensure_ascii=False))
        stats['notes'] += 1
    head = collections.OrderedDict([('export', 'workdb function notes'), ('exported', meta.get('exported')),
                                    ('database_built', meta.get('database_built')), ('notes', stats['notes']),
                                    ('about', 'tools/workdb_notes.py: the work database\'s notes, text only; '
                                              'workdb.py card and notes read it when the database is not there')])
    data = ('\n'.join([json.dumps(head, ensure_ascii=False)] + lines) + '\n').encode('utf-8')
    rel = rel or os.path.relpath(out, ROOT)
    s = ps.Scanner(rules, allow)
    s.scan_path(rel, rel)
    s.scan_text(data, rel, rel)
    if s.findings:
        for rid, found in sorted(s.findings.items()):
            print('workdb: export-notes: %s: %s (%d)' % (rid, s.why.get(rid, ''), len(found)), file=sys.stderr)
        sys.exit('workdb: export-notes: the export fails public_scan: not written')
    hits = sum(1 for ln in data.split(b'\n') if ps.ADDRESS.search(ln))
    stats['lines naming an address'] = hits
    stats['lines'] = data.count(b'\n')
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, out)
    stats['bytes'] = len(data)
    return stats
