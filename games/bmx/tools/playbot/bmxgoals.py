#!/usr/bin/env python3
"""bmxgoals.py LEVEL: BMX XXX's goals of a level (its OBJCTV.TXT) with where their things are (its INS instances).

An objective is OBJECTIVE_BEGIN .. OBJECTIVE_END: NAME, TYPE (COLLECT, SET, TALK, ...), ACTIVE, and its PARAMETERS
(COLLECT: ITEMn, the instances to ride through; SET: OBJECTIVEn, its children). Instances are the INS file's TGIH
records: name at +0x50 (32 bytes), position at +0x18 relative to the parent (id +0x48, parent +0x44, rotation +0x08).
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', 'tools'))   # (tools/: zpaths.py)
import zpaths  # noqa: E402  (tools/paths.env; the playbot's folders, each game's and the shared one)
zpaths.use('playbot', 'aggressive-playbot', 'dm2-playbot', 'bmx-playbot', 'modding', 'checks', 'aggressive', 'dm2', 'bmx')
TOOLS = os.environ.get('DM2_LEVEL_TOOLS', zpaths.TOOL_DIRS['modding'])   # (dm2_level.py)
sys.path.insert(0, TOOLS)
DISC = os.environ.get('BMX_DISC', os.path.join(zpaths.BMX_DATA, 'disc'))


def objectives(text):
    """[{name, type, active, items, children, string, prerequisites, timeout, params}] in file order."""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    out = []
    for body in re.findall(r'OBJECTIVE_BEGIN(.*?)OBJECTIVE_END', text, flags=re.S):
        keys = re.findall(r'^\s*([A-Z_0-9]+)\s+"([^"]*)"', body, flags=re.M)
        top = dict()
        for k, v in keys:
            top.setdefault(k, v)
        params = body.split('PARAMETERS_BEGIN', 1)[1] if 'PARAMETERS_BEGIN' in body else ''
        pk = re.findall(r'^\s*([A-Z_]+)(\d+)\s+"([^"]*)"', params, flags=re.M)
        pre = body.split('PREREQUISITE_BEGIN', 1)[1].split('PREREQUISITE_END', 1)[0] if 'PREREQUISITE_BEGIN' in body else ''
        out.append(dict(name=top.get('NAME'), type=top.get('TYPE'), active=top.get('ACTIVE'), string=top.get('STRINGID'),
                        prerequisites=re.findall(r'NAME\s+"([^"]*)"', pre), timeout=top.get('TIMEOUT'),
                        items=[v for k, n, v in pk if k == 'ITEM'], children=[v for k, n, v in pk if k == 'OBJECTIVE'],
                        params={k: v for k, v in re.findall(r'^\s*([A-Z_]+)\s+"([^"]*)"', params, flags=re.M)}))
    return out


def instances(ins):
    """{name: world position} of every named instance."""
    import dm2_level as dl
    recs = [c.data for c in dl.walk(dl.parse(ins)) if c.tag == b'TGIH']
    by_id = {struct.unpack_from('<I', d, 0x48)[0]: d for d in recs}
    out = {}
    for d in recs:
        name = d[0x50:0x70].split(b'\0')[0].decode('latin-1')
        v, e = (0.0, 0.0, 0.0), d
        for _ in range(16):
            v = tuple(a + b for a, b in zip(dl.quat_rotate(struct.unpack_from('<4f', e, 8), v), struct.unpack_from('<3f', e, 0x18)))
            e = by_id.get(struct.unpack_from('<I', e, 0x44)[0])
            if e is None:
                break
        if name:
            out[name] = v
    return out


# The level's data points (the .DAT's SDPT records: a class word at +4, the position at +0x10). The people's records
# (class 0x0422B9CE, the model's name in them, at the same place as their animation record 0x0E482A04): a goal giver's
# also holds its goal's name, the first part of its objectives' names in OBJCTV.TXT (DAM: Dynamite, Drain, Patch,
# Canal, Brownout, Potty; NPCDeliver's people are the deliveries').
PERSON_CLASS = 0x0422B9CE


def data_points(dat):
    """[(class, position, record)] of the SDPT records."""
    out, at = [], 8
    while at + 8 <= len(dat):
        tag, size = dat[at:at + 4], struct.unpack_from('<I', dat, at + 4)[0]
        if tag == b'SDPT':
            r = dat[at + 8:at + 8 + size]
            out.append((struct.unpack_from('<I', r, 4)[0], struct.unpack_from('<3f', r, 16), r))
        at += 8 + size
    return out


def givers(level, objs=None):
    """[{goal, model, pos}]: the people who hold a goal's name."""
    base = os.path.join(DISC, 'ASSETS', 'LEVELS', level)
    if objs is None:
        objs = objectives(open(os.path.join(base, 'OBJCTV.TXT'), encoding='latin-1').read())
    goals = {o['name'].split('.')[0] for o in objs if o['name']}
    out = []
    for cls, pos, r in data_points(open(os.path.join(base, level + '.DAT'), 'rb').read()):
        if cls != PERSON_CLASS:
            continue
        models = re.findall(rb'dm[A-Z]{4,6}[a-z]?', r)
        # (a name follows a length byte, which can read as a letter: "5NPCDeliver")
        named = [g for w in re.findall(rb'[ -~]{4,}', r) for g in goals if w.decode('latin-1').endswith(g)]
        if named:
            out.append(dict(goal=max(named, key=len), model=models[0].decode() if models else None, pos=pos))
    return out


def collect_items(level, objs=None):
    """{goal: [world positions]}: what a giver's COLLECT objectives ask the rider through (their ITEMn instances),
    the pickups first and the checkpoint (an objective named ...Chkpt) last, as OBJCTV.TXT names them."""
    if objs is None:
        objs, where = level_goals(level)
    else:
        where = None
    out = {}
    for o in objs:
        if o['type'] != 'COLLECT' or not o['name']:
            continue
        goal = o['name'].split('.')[0]
        pts = [w for w in o.get('where', []) if w is not None]
        if pts:
            out.setdefault(goal, []).append((1 if 'chkpt' in o['name'].lower() else 0, o['name'], pts))
    return {g: [p for _, _, ps in sorted(v) for p in ps] for g, v in out.items()}


def level_goals(level):
    base = os.path.join(DISC, 'ASSETS', 'LEVELS', level)
    objs = objectives(open(os.path.join(base, 'OBJCTV.TXT'), encoding='latin-1').read())
    where = instances(open(os.path.join(base, level + '.INS'), 'rb').read())
    lower = {k.lower(): v for k, v in where.items()}
    for o in objs:
        o['where'] = [lower.get(i.lower()) for i in o['items']]
    return objs, where


if __name__ == '__main__':
    objs, where = level_goals(sys.argv[1])
    kinds = {}
    for o in objs:
        kinds[o['type']] = kinds.get(o['type'], 0) + 1
    print(sys.argv[1], len(objs), 'objectives', kinds, len(where), 'named instances')
    by_name = {o['name']: o for o in objs}
    for g in givers(sys.argv[1], objs):
        o = by_name.get(g['goal'], {})
        print(' giver %-12s %-8s (%.0f,%.0f,%.0f) %s %s' % (g['goal'], g['model'], *g['pos'], o.get('type'),
              'after ' + ','.join(o['prerequisites']) if o.get('prerequisites') else ''))
    for o in objs:
        if o['type'] != 'SET':
            print(' %-28s %-8s %-3s %s' % (o['name'], o['type'], o['active'], ' '.join('%s@%s' % (i, 'none' if w is None else '(%.0f,%.0f,%.0f)' % w)
                                                                                 for i, w in zip(o['items'], o['where'])) or o['params']))
