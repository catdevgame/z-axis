#!/usr/bin/env python3
"""dead_code.py [OUT.tsv] [--table]: sort each game's dead-flagged functions (AI's, DM2's, BMX XXX's) into the
three groups of docs/DEAD-CODE.md and write the tables (default $ZAXIS_SCRATCH/dead-code/dead-code.tsv for AI and
dead-code-dm2-flags.tsv, dead-code-bmx-flags.tsv beside it: they hold strings from the games' ELFs, so they stay
out of the repo), printing the counts; --table also prints docs/DEAD-CODE.md's "The three groups" table, a row per
game and group.

Groups: sdk (Sony's libraries, layer sce), engine (NgEngine/ZGE, layer zge) and game
(the game's own code: layer aggressive, dm2 or bmx). Input: the work database
($ZAXIS_SCRATCH/workdb/work.duckdb, tools/workdb/workdb.py build). The flags are its
notes (func_notes of each game's PS2 build, topic cut), one a function, in one form:

    Flagged KIND in the sweep manifest (LAYER layer, STATUS): what KIND means. Reason column: WHY.

KIND is dead, port-dead, script-dead or data-dead, LAYER the sweep's (sce, zge, aggressive); the
reason is there when the manifest's row had one. They are the sweep manifests' flags column
(the manifests are gone since the sweep was emptied): knowledge1's notes and leftovers1's for
the rows that left the manifests after it, each row's last revision. A function's latest such
note wins. Besides the flags, the database gives counterparts in the other eight builds,
modules, names and the strings each function uses (modules and counterpart names for AI's; a DM2 or BMX XXX
function's counterparts through its AI PS2 pair).
Run with the venv that has duckdb: $ZAXIS_SCRATCH/venv/bin/python -I tools/dead_code.py
"""
import collections, os, re, sys

import duckdb

HOME = os.path.expanduser('~')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))    # (tools/)
sys.path.insert(0, ROOT)
import zpaths  # noqa: E402  (tools/paths.env: the scratch folder)
DB = os.environ.get('WORKDB', zpaths.ZAXIS_SCRATCH + '/workdb/work.duckdb')
ARGS = [a for a in sys.argv[1:] if a != '--table']
OUT = ARGS[0] if ARGS else zpaths.ZAXIS_SCRATCH + '/dead-code/dead-code.tsv'
os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)
GROUP = {'sce': 'sdk', 'zge': 'engine', 'aggressive': 'game', 'dm2': 'game', 'bmx': 'game'}
GAMES = (('ai', 'ai_ps2', 'Aggressive Inline'), ('dm2', 'dm2_ps2', 'Dave Mirra BMX 2'), ('bmx', 'bmx_ps2', 'BMX XXX'))
RELEASES = ('ai_ps2', 'ai_xbox', 'ai_gc', 'dm2_ps2', 'dm2_xbox', 'dm2_gc', 'bmx_ps2', 'bmx_xbox', 'bmx_gc')
FLAG = re.compile(r'Flagged (dead|port-dead|script-dead|data-dead) in the sweep manifest '
                  r'\((sce|zge|aggressive|dm2|bmx) layer, (\w+)\): .*?(?: Reason column: (.*?)\.(?: \(The manifest of .*)?)?$')

db = duckdb.connect(DB, read_only=True)
OTHER = {'ai': ('dm2', 'bmx'), 'dm2': ('ai', 'bmx'), 'bmx': ('ai', 'dm2')}
NAMES = {'sdk': 'Sony SDK and libraries', 'engine': 'Engine (NgEngine / ZGE)', 'game': 'Game content'}
LAYER = {'sdk': 'sce', 'engine': 'zge', 'game': None}
OWN = {'ai': 'aggressive', 'dm2': 'dm2', 'bmx': 'bmx'}
SHORT = {'ai': 'AI', 'dm2': 'DM2', 'bmx': 'BMX XXX'}
# AI PS2 address <-> each release's high or medium counterpart
pairs = collections.defaultdict(set)      # ai_addr -> {build}
to_ai = {}                                # (build, addr) -> ai_addr
for a, b, x in db.execute("select ai_addr, build, addr from counterparts where confidence in ('high', 'medium')").fetchall():
    pairs[a].add(b)
    to_ai[(b, x)] = a


def game_rows(game, build):
    flagged = {}
    for a, text in db.execute("select addr, text from func_notes where build = ? and topic = 'cut'"
                              " and text like 'Flagged % in the sweep manifest (%' order by ts", [build]).fetchall():
        m = FLAG.match(text)
        if not m:
            raise SystemExit('dead_code.py: a flag note not in the form: %s 0x%06X %s' % (build, a, text[:120]))
        flagged[a] = {'addr': a, 'kind': m.group(1), 'layer': m.group(2), 'status': m.group(3), 'reason': m.group(4) or ''}
    best = dict(db.execute("select addr, name from best_names where build = ?", [build]).fetchall())
    rows = []
    for a, r in flagged.items():
        r['name'] = best.get(a) or 'FUN_%08x' % a
        rows.append(r)
    want = {r['addr'] for r in rows}
    ai_of = {a: (a if game == 'ai' else to_ai.get((build, a))) for a in want}
    where = {a: sorted(b for b in set(pairs.get(ai_of[a], ())) | ({'ai_ps2'} if game != 'ai' and ai_of[a] else set())
                       if b != build) for a in want}
    module, hint = {}, {}
    if game == 'ai':
        for a, m in db.execute("select func, module from modules where build = 'ai_ps2' and module is not null").fetchall():
            if a in want:
                module[a] = m
        for a, m in db.execute("select ai_addr, name from ai_names where kind = 'module' order by confidence").fetchall():
            if a in want and a not in module:
                module[a] = m
        # The rest by link order: a function between two functions of the same module (from the module
        # map or a counterpart's source file) is taken to be in it, marked with a leading '~'.
        known = dict(db.execute("select func, module from modules where build = 'ai_ps2' and module is not null").fetchall())
        for a, m in db.execute("select ai_addr, name from ai_names where kind = 'module'").fetchall():
            known.setdefault(a, m)
        order = sorted(a for (a,) in db.execute("select addr from functions where build = 'ai_ps2'").fetchall())
        for i, a in enumerate(order):
            if a not in want or a in module:
                continue
            before = next((known[b] for b in reversed(order[:i]) if b in known), None)
            after = next((known[b] for b in order[i + 1:] if b in known), None)
            if before and before == after:
                module[a] = '~' + before
        for a, n, b in db.execute("select ai_addr, name, build from ai_names where kind = 'function' and build <> 'ai_ps2'"
                                  " order by case confidence when 'high' then 0 when 'medium' then 1 else 2 end").fetchall():
            if a in want and a not in hint:
                hint[a] = '%s (%s)' % (n, b)
    strings = collections.defaultdict(list)
    for a, t in db.execute("select func, text from func_strings where build = ?", [build]).fetchall():
        if a in want and len(strings[a]) < 3:
            strings[a].append(t.replace('\t', ' ').replace('\n', ' ')[:60])
    size = {a: b for a, b in db.execute("select addr, bytes from functions where build = ?", [build]).fetchall() if a in want}
    return rows, where, module, hint, strings, size


count = collections.Counter()      # (game, group, kind, shared) -> functions
nbytes = collections.Counter()     # (game, group) -> bytes
total_rows = 0
for game, build, _ in GAMES:
    rows, where, module, hint, strings, size = game_rows(game, build)
    out = OUT if game == 'ai' else (OUT[:-4] if OUT.endswith('.tsv') else OUT) + '-%s-flags.tsv' % game
    with open(out, 'w') as f:
        f.write('# tools/readable/dead_code.py: %s\'s dead-flagged functions by group (docs/DEAD-CODE.md). Columns:\n'
                '# address, group, kind (flag), bytes, name, shared (other releases with a high or medium counterpart),\n'
                '# module, counterpart name, strings.\n' % SHORT[game])
        for r in sorted(rows, key=lambda r: (GROUP[r['layer']], r['addr'])):
            a = r['addr']
            g = GROUP[r['layer']]
            rel = [b for b in RELEASES if b in where[a]]
            other = any(b.startswith(OTHER[game]) for b in rel)
            share = 'other game' if other else ('other release' if rel else 'only here')
            count[(game, g, r['kind'], share)] += 1
            nbytes[(game, g)] += size.get(a, 0)
            f.write('\t'.join(['0x%06X' % a, g, r['kind'], str(size.get(a, 0)), r['name'], ','.join(rel) or '-',
                               module.get(a) or '-', hint.get(a) or '-', ' | '.join(s for s in strings[a] if s) or '-']) + '\n')
    total_rows += len(rows)
    print('wrote', out, len(rows), 'rows')
for k in sorted(count):
    print('%-4s %-7s %-12s %-14s %5d' % (k + (count[k],)))
print('bytes:', {'%s %s' % k: v for k, v in sorted(nbytes.items())})

if '--table' in sys.argv[1:]:
    # docs/DEAD-CODE.md's "The three groups" table, a row per game and group.
    print('| Game | Group | Layer | Functions | Bytes | Also in another game |')
    print('|---|---|---|---|---|---|')
    for game, _, title in GAMES:
        for g in ('sdk', 'engine', 'game'):
            kinds = collections.Counter()
            for (gm, gg, kind, share), n in count.items():
                if gm == game and gg == g:
                    kinds[kind] += n
            total = sum(kinds.values())
            if not total:
                continue
            split = ', '.join('%d %s' % (kinds[k], k) for k in ('dead', 'port-dead', 'data-dead', 'script-dead') if kinds[k])
            other = sum(n for (gm, gg, kind, share), n in count.items() if gm == game and gg == g and share == 'other game')
            print('| %s | %s | %s | %s (%s) | %d KB | %d |' % (title, NAMES[g], LAYER[g] or OWN[game], format(total, ','),
                                                              split, round(nbytes[(game, g)] / 1000.0), other))
