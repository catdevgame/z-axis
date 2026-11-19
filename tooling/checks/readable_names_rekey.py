#!/usr/bin/env python3
"""readable_names_rekey.py RENAMES.jsonl: the table's vtype rows keyed by the register-shaped name each local has in a
draft before any naming (docs/READABLE.md, "Names"). RENAMES.jsonl is what a draft made with NAMES_DUMP_RENAMES=FILE
writes: {final name: first name} per function. A row whose key is a final name takes the first name, so it outlives
later changes of the naming rules; a row whose key no longer exists is said and dropped with --drop."""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
import readable_names as rn
maps = {}
for line in open(sys.argv[1], encoding='utf-8'):
    r = json.loads(line)
    maps[r['address']] = r['names']
rows = rn.read_table()
done = stale = 0
hdr = [l.rstrip('\n') for l in open(rn.TABLE, encoding='utf-8') if l.startswith('#')]
out = []
seen = set()
for r in rows:
    if r[0] == 'vtype':
        a, v = r[1].split(':', 1)
        m = maps.get(int(a, 16))
        if m is not None and v in m:
            r = (r[0], '%s:%s' % (a, m[v])) + tuple(r[2:])
            done += 1
        elif m is not None and v not in m.values():
            stale += 1
        if r[1] in seen:
            continue
        seen.add(r[1])
    out.append(r)
print('%d rows rekeyed, %d keys neither a final nor a first name' % (done, stale))
with open(rn.TABLE, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(hdr) + '\n')
    for r in out:
        f.write('\t'.join(r) + '\n')
