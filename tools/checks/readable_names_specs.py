#!/usr/bin/env python3
"""readable_names_specs.py REC.json SPEC...: struct specs into rows (docs/READABLE.md, "Names"): appends to the layout table and the names table.
Spec format:
  struct QNAME [size=0x..] [conf=medium] [classes=1,2] [delta=0] [skip=fnA:1,fnB:r,fnC:var] [base=QBASE] [name=var] [pname=param] ev=text to the end of line
  0xOFF field [conf] [*Q | Q] [# note]
  gtype ADDRESS | rtype ADDRESS-OR-FUNCTION
A class's parameters, results, globals and locals at that delta take ptype / rtype / gtype / vtype rows
(medium at least: a binding is firmer than the field names read from it).
"""
import json, os, re, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
import readable_names as rn
rec = json.load(open(sys.argv[1]))
C = {c['id']: c for c in rec['classes']}
fname = {int(k): v for k, v in rec['fname'].items()}
byname = {v: k for k, v in fname.items()}
layout = []; table = []
structs = []
cur = None
for path in sys.argv[2:]:
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line.strip() or line.lstrip().startswith('//'): continue
        m = re.match(r'^struct\s+([\w:]+)\s*(.*)$', line)
        if m:
            kv = m.group(2)
            ev = ''
            if ' ev=' in ' ' + kv:
                kv, ev = re.split(r'(?:^| )ev=', kv, 1)
            o = dict(x.split('=', 1) for x in kv.split())
            cur = dict(q=m.group(1), opts=o, ev=ev.strip(), fields=[], path=os.path.basename(path))
            structs.append(cur)
            continue
        m = re.match(r'^(gtype|rtype)\s+(\w+)\s*$', line)
        if m:
            a = m.group(2)
            cur.setdefault('xrows', []).append((m.group(1), a if a.startswith('0x') else hex(byname[a])))
            continue
        m = re.match(r'^\s*(0x[0-9A-Fa-f]+)\s+([a-z]\w*)(?:\s+(high|medium|low))?(?:\s+(\*?[A-Za-z_][\w:]*\*?))?(?:\s*#\s*(.*))?$', line)
        if not m:
            sys.exit('bad line: %s' % line)
        cur['fields'].append(m.groups())
existing_layout = set()
for l in open(rn.LAYOUT, encoding='utf-8'):
    c = l.rstrip('\n').split('\t')
    if len(c) > 2 and not l.startswith('#'):
        existing_layout.add((c[0], c[1].lower() if c[1].startswith('0x') else c[1], c[2] if c[1] in ('name', 'type', 'base') else ''))
rows = rn.read_table()
have_t = {(r[0], r[1]) for r in rows}
newl, newt = [], []
sizes = {}
ROLE = set(rn.ROLE_NAMES) | {'callback', 'flags', 'limit', 'index', 'enabled'}
pkeep = {r[1]: (r[2], r[4]) for r in rows if r[0] == 'param'}
drop = set(); added = set()
for s in structs:
    q = s['q']; o = s['opts']
    conf = o.get('conf', 'medium')
    bconf = 'high' if conf == 'high' else 'medium'   # (a binding is firmer than the field names read from it; low rows of these kinds are not applied)
    ev = 'names8: ' + (s['ev'] or 'read from its uses')
    if 'base' in o and (q, 'base', o['base']) not in existing_layout:
        newl.append([q, 'base', o['base'], 'high' if conf == 'high' else 'medium', ev])
    if 'size' in o and (q, 'size', '') not in existing_layout:
        newl.append([q, 'size', o['size'], conf, ev])
    if 'name' in o and (q, 'name', o['name']) not in existing_layout:
        newl.append([q, 'name', o['name'], conf, ev])
    seen = set()
    for off, f, c, ty, note in s['fields']:
        k = (q, off.lower(), '')
        if int(off, 16) in seen:
            sys.exit('%s: two rows at %s' % (q, off))
        seen.add(int(off, 16))
        if k in existing_layout or (rn.struct_fields().get(q, {}).get(int(off, 16), (None, ''))[1] in ('member', 'proven') and rn.struct_fields()[q][int(off, 16)][0] == f):
            sys.stderr.write('skip existing %s %s\n' % (q, off))
        else:
            newl.append([q, '0x%X' % int(off, 16), f, c or conf, ev + (': ' + note if note else '')])
        if ty:
            t = ty[1:] + '*' if ty.startswith('*') else ty
            if (q, 'type', '%s=%s' % (f, t)) not in existing_layout:
                newl.append([q, 'type', '%s=%s' % (f, t), c or conf, ev])
    for kind, a in s.get('xrows', ()):
        k = (kind, '0x%08X' % int(a, 16))
        if k not in have_t:
            have_t.add(k); newt.append([kind, k[1], q, bconf, ev])
    skip = set(o.get('skip', '').split(',')) - {''}
    delta = int(o.get('delta', '0'), 0)
    for cid in (int(x) for x in o.get('classes', '').split(',') if x):
        c = C[cid]
        for a, i, d in c['params']:
            if d == delta and a in fname and '%s:%d' % (fname[a], i) not in skip:
                k = ('ptype', '0x%08X:%d' % (a, i))
                if k not in have_t:
                    have_t.add(k); newt.append(['ptype', k[1], q, bconf, ev])
                k = ('param', '0x%08X:%d' % (a, i))
                pn = o.get('pname')
                if pn:
                    old = pkeep.get(k[1])
                    if old is None or old[1].startswith(('inferred:', 'callers:')) or old[0] in ROLE:
                        if old is not None:
                            drop.add(k)
                        if k not in added:
                            added.add(k); newt.append(['param', k[1], pn, bconf, ev])
        for a, d in c['rets']:
            if d == delta and a in fname and '%s:r' % fname[a] not in skip:
                k = ('rtype', '0x%08X' % a)
                if k not in have_t:
                    have_t.add(k); newt.append(['rtype', k[1], q, bconf, ev])
        direct = set()
        for ks in c['offs'].values():
            for v in ks.values():
                for x in v:
                    direct.update((x[0], m) for m in re.findall(r'[A-Za-z_]\w*', x[1]))
        for a, v, d in c.get('vars', []):
            if d == delta and a in fname and (a, v) in direct and '%s:%s' % (fname[a], v) not in skip and not v.startswith(('param_', 'arg', 'farg')):
                k = ('vtype', '0x%08X:%s' % (a, v))
                if k not in have_t:
                    have_t.add(k); newt.append(['vtype', k[1], q, bconf, 'names8: recovered with its struct'])
        for a, d in c['globs']:
            if d == delta and isinstance(a, int):
                k = ('gtype', '0x%08X' % a)
                if k not in have_t:
                    have_t.add(k); newt.append(['gtype', k[1], q, bconf, ev])
print(len(newl), 'layout rows', len(newt), 'table rows')
if os.environ.get('DRY'):
    for r in newl[:10] + newt[:10]: print(r)
    sys.exit()
with open(rn.LAYOUT, 'a', encoding='utf-8') as f:
    for r in newl: f.write('\t'.join(r) + '\n')
hdr = [l.rstrip('\n') for l in open(rn.TABLE, encoding='utf-8') if l.startswith('#')]
rows = [r for r in rows if (r[0], r[1]) not in drop]
body = sorted(rows + [tuple(r) for r in newt], key=lambda r: (r[0] != 'function', r[0], r[1]))
with open(rn.TABLE, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(hdr) + '\n')
    for r in body: f.write('\t'.join(r) + '\n')
