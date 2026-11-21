#!/usr/bin/env python3
"""dead_code.py [OUT.tsv]: sort the sweep's dead-flagged functions into the three groups of
docs/DEAD-CODE.md and write the table (default $ZAXIS_SCRATCH/dead-code/dead-code.tsv: it
holds strings from the game's ELF, so it stays out of the repo), printing the counts.

Groups: sdk (Sony's libraries, layer sce), engine (NgEngine/ZGE, layer zge) and game
(Aggressive Inline's own code, layer aggressive). Inputs: the sweep's manifests (the
flags column: dead, port-dead, script-dead, data-dead) and the work database
($ZAXIS_SCRATCH/workdb/work.duckdb, tooling/dm2/tools/workdb.py build): counterparts
in the other eight builds, modules, names and the strings each function uses.
Run with the venv that has duckdb: $ZAXIS_SCRATCH/venv/bin/python -I tools/dead_code.py
"""
import collections, os, sys

import duckdb

HOME = os.path.expanduser('~')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(ROOT), 'checks'))
import zlayout  # noqa: E402  (the tree's folders, old or new)
sys.path.insert(0, os.path.dirname(ROOT))
import zpaths  # noqa: E402  (tooling/paths.env: the scratch folder)
GAME_SRC = os.environ.get('DEAD_CODE_GAME_SRC', zlayout.path(os.path.dirname(os.path.dirname(ROOT)), 'runtime', 'src', 'game'))
DB = os.environ.get('WORKDB', zpaths.ZAXIS_SCRATCH + '/workdb/work.duckdb')
OUT = sys.argv[1] if len(sys.argv) > 1 else zpaths.ZAXIS_SCRATCH + '/dead-code/dead-code.tsv'
os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)
GROUP = {'sce': 'sdk', 'zge': 'engine', 'aggressive': 'game'}
FLAGS = ('data-dead', 'script-dead', 'port-dead', 'dead')  # first match is the row's kind
RELEASES = ('ai_xbox', 'ai_gc', 'dm2_ps2', 'dm2_xbox', 'dm2_gc', 'bmx_ps2', 'bmx_xbox', 'bmx_gc')

rows = []
for layer in GROUP:
    for line in open(os.path.join(GAME_SRC, layer, 'sweep', 'manifest.txt')):
        if line.startswith('#') or not line.strip():
            continue
        c = line.rstrip('\n').split('\t')
        flags = c[5].split(',')
        kind = next((f for f in FLAGS if f in flags), None)
        if kind:
            rows.append({'addr': int(c[0], 16), 'layer': layer, 'kind': kind, 'status': c[1],
                         'name': c[6], 'reason': c[9] if len(c) > 9 else ''})

db = duckdb.connect(DB, read_only=True)
want = {r['addr'] for r in rows}
where = collections.defaultdict(set)
for a, b in db.execute("select ai_addr, build from counterparts where confidence in ('high', 'medium')").fetchall():
    if a in want:
        where[a].add(b)
module = {}
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
hint = {}
for a, n, b in db.execute("select ai_addr, name, build from ai_names where kind = 'function' and build <> 'ai_ps2'"
                          " order by case confidence when 'high' then 0 when 'medium' then 1 else 2 end").fetchall():
    if a in want and a not in hint:
        hint[a] = '%s (%s)' % (n, b)
strings = collections.defaultdict(list)
for a, t in db.execute("select func, text from func_strings where build = 'ai_ps2'").fetchall():
    if a in want and len(strings[a]) < 3:
        strings[a].append(t.replace('\t', ' ').replace('\n', ' ')[:60])
size = {a: b for a, b in db.execute("select addr, bytes from functions where build = 'ai_ps2'").fetchall() if a in want}

count = collections.Counter()
nbytes = collections.Counter()
with open(OUT, 'w') as f:
    f.write('# tools/dead_code.py: the sweep\'s dead-flagged functions by group (docs/DEAD-CODE.md). Columns:\n'
            '# address, group, kind (flag), bytes, name, shared (other releases with a high or medium counterpart),\n'
            '# module, counterpart name, strings.\n')
    for r in sorted(rows, key=lambda r: (GROUP[r['layer']], r['addr'])):
        a = r['addr']
        g = GROUP[r['layer']]
        rel = [b for b in RELEASES if b in where[a]]
        other = any(b.startswith(('dm2', 'bmx')) for b in rel)
        share = 'DM2/BMX' if other else ('AI Xbox/GC' if rel else 'AI PS2 only')
        count[(g, r['kind'], share)] += 1
        nbytes[g] += size.get(a, 0)
        f.write('\t'.join(['0x%06X' % a, g, r['kind'], str(size.get(a, 0)), r['name'], ','.join(rel) or '-',
                           module.get(a) or '-', hint.get(a) or '-', ' | '.join(s for s in strings[a] if s) or '-']) + '\n')
for k in sorted(count):
    print('%-7s %-12s %-12s %5d' % (k + (count[k],)))
print('bytes:', dict(nbytes))
print('wrote', OUT, len(rows), 'rows')
