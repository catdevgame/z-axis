#!/usr/bin/env python3
"""DM2's levels as the player sees them: rideable cells, rails, ramps, walls, from the disc's level files through the
parsers of tools/modding/dm2_level.py (the records the game reads). Kept as pickles in the scratch folder, never in
the repository: derived from the disc. y grows down.

    dm2map.py build [LEVEL ...]   the eight levels by default
    dm2map.py info LEVEL
"""
import collections
import math
import os
import pickle
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', 'tools'))   # (tools/: zpaths.py)
import zpaths  # noqa: E402  (tools/paths.env; the playbot's folders, each game's and the shared one)
zpaths.use('playbot', 'aggressive-playbot', 'dm2-playbot', 'bmx-playbot', 'modding', 'checks', 'aggressive', 'dm2', 'bmx')
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # nav.py, until it lands next to this
import nav  # noqa: E402

MAIN = os.environ.get('AI_MAIN', zpaths.AI_WORKSPACE)
ZAXIS = zpaths.ZAXIS_MAIN
SCRATCH = os.environ.get('PLAYDM2', os.path.join(zpaths.ZAXIS_SCRATCH, 'playdm2'))
DISC = os.environ.get('DM2_DISC', os.path.join(zpaths.DM2_WORKSPACE, 'recomp', 'disc-root'))
# By the level select's order (the menu value).
LEVELS = ('WOODWARD', 'TRAINYAR', 'SWAMPYTR', 'COMMERCI', 'GALLOON', 'CLOVERLE', 'DEVILSPE', 'AIRPORT')


DROP = 6.0        # a ledge ridden off
RIDE_UP = 0.75    # a face flatter than this is pedalled up (ny)
CURB = 0.3        # a bike rolls up this much between level floors; more only on a slope


def wall_between(walls, key, low, high):
    """A steep face in this cell that stands in the way between two heights (y grows down)."""
    for y0, y1 in walls.get(key, ()):
        if y1 - y0 > 0.4 and y0 < high - CURB and y1 > low - 0.8:
            return True
    return False


def adjacency(nodes, walls):
    """Which floors a bike rides between: level floors at a curb's height, slopes within nav.STEP, no wall in the way."""
    adj = {}
    for key, floors_here in nodes.items():
        i, k = key
        for layer, (y, flat) in enumerate(floors_here):
            out = []
            for di, dk in nav.NEAR:
                okey = (i + di, k + dk)
                reached = False
                for n, (oy, oflat) in enumerate(nodes.get(okey, ())):
                    dy = abs(oy - y)
                    level = flat > 0.97 and oflat > 0.97
                    if oy < y - CURB and min(flat, oflat) < RIDE_UP:
                        continue                # too steep to pedal up from slow: down only
                    if dy > (CURB if level else nav.STEP):
                        continue
                    if dy > CURB * 0.5 or level:
                        a, b = min(y, oy), max(y, oy)
                        if wall_between(walls, key, a, b) and wall_between(walls, okey, a, b):
                            continue
                    out.append((okey, n, math.hypot(di, dk) * nav.CELL))
                    reached = True
                    break
                if not reached:          # off a ledge: the first floor under it
                    for n, (oy, oflat) in enumerate(nodes.get(okey, ())):
                        if nav.STEP < oy - y <= DROP:
                            out.append((okey, n, math.hypot(di, dk) * nav.CELL + 6.0))
                            break
            adj[(key, layer)] = out
    return adj


def links_of(adj):
    def links(nodes, key, layer):
        return adj.get((key, layer), ())
    return links


def buffer(data):
    """A TGFE or TGFT chunk: an index per edge or face (0xFFFF none), the records."""
    import dm2_level as dl
    desc, count, indices, records = dl.tag_buffer(data)
    return struct.unpack_from('<%dH' % count, indices), records


def objects_of(geo):
    """{object id: (vertices, faces [(corners, normal, two sided, tag record or None)], edge tags)}."""
    import dm2_level as dl
    objects = {}
    for obj in dl.parse(geo)[0].kids:
        if obj.tag != b'TGEO':
            continue
        kids = obj.kids
        head = next(k for k in kids if k.tag == b'TGHD')
        parts = [k for k in kids if k.tag == b'TGPT']
        trees = [k for k in kids if k.tag == b'TGCI']
        if not parts or not trees:
            continue
        streams = {k.tag: k.data for k in parts[-1].kids}
        for k in kids:
            streams.setdefault(k.tag, k.data)
        nv, nf = struct.unpack_from('<HH', streams[b'TGPH'], 4)
        verts = [struct.unpack_from('<3f', streams[b'TGVP'], 12 * i) for i in range(nv)]
        faces = [struct.unpack_from('<4H', streams[b'TGFP'], 8 * i) for i in range(nf)]
        normals = [struct.unpack_from('<3f', streams[b'TGFN'], 12 * i) for i in range(nf)]
        listed = dl.tree_faces(trees[0].data)
        fi, fr = buffer(streams[b'TGFT']) if b'TGFT' in streams else ((), ())
        ei, er = buffer(streams[b'TGFE']) if b'TGFE' in streams else ((), ())
        kept = []
        for i, f in enumerate(faces):
            if i not in listed:
                continue
            tag = fr[fi[i]] if i < len(fi) and fi[i] != 0xFFFF else None
            edges = []
            for side in range(3):
                n = i * 3 + side
                if n < len(ei) and ei[n] != 0xFFFF:
                    edges.append((side, er[ei[n]]))
            kept.append((f[1:], normals[i], f[0] & 0x4000, tag, edges))
        objects[struct.unpack_from('<I', head.data, 0x18)[0]] = (verts, kept)
    return objects


def build(level):
    import dm2_level as dl
    disc = dl.Disc(DISC)
    geo = disc.read('ASSETS/LEVELS/%s/%s.GEO' % (level, level))
    ins = disc.read('ASSETS/LEVELS/%s/%s.INS' % (level, level))
    objects = objects_of(geo)
    instances = [c.data for c in dl.walk(dl.parse(ins)) if c.tag == b'TGIH']
    by_id = {struct.unpack_from('<I', d, 0x48)[0]: d for d in instances}

    def place(d, v):
        for _ in range(16):
            v = tuple(a + b for a, b in zip(dl.quat_rotate(struct.unpack_from('<4f', d, 8), v), struct.unpack_from('<3f', d, 0x18)))
            d = by_id.get(struct.unpack_from('<I', d, 0x44)[0])
            if d is None:
                break
        return v

    def turn(d, n):
        for _ in range(16):
            n = dl.quat_rotate(struct.unpack_from('<4f', d, 8), n)
            d = by_id.get(struct.unpack_from('<I', d, 0x44)[0])
            if d is None:
                break
        return n

    floors, walls = collections.defaultdict(list), collections.defaultdict(list)
    segments = []
    edges, ramps, wall_faces, kinds = [], [], [], collections.Counter()
    tris = 0
    for d in instances:
        if struct.unpack_from('<I', d, 0)[0] & 0x12 != 0x2:
            continue
        verts, faces = objects.get(struct.unpack_from('<I', d, 0x40)[0], ((), ()))
        world = [place(d, v) for v in verts]
        up = []
        for corners, normal, two_sided, tag, tagged in faces:
            n = turn(d, normal)
            t = tuple(world[c] for c in corners)
            if two_sided or n[1] < 0.0:
                up.append(t)
            kind = tag[8] if tag and len(tag) > 8 else 0
            strength = tag[9] if tag and len(tag) > 9 else 0
            kinds[kind] += 1
            if kind in (1, 2, 3) and n[1] < -0.05:
                c = tuple(sum(p[i] for p in t) / 3 for i in range(3))
                ramps.append(dict(kind=kind, strength=strength, centre=c, normal=n, tag=struct.unpack_from('<I', tag, 0)[0], top=min(p[1] for p in t)))
            if abs(n[1]) < 0.25:
                c = tuple(sum(p[i] for p in t) / 3 for i in range(3))
                wall_faces.append((c, n))
            for side, record in tagged:
                if len(record) > 5 and record[5] & 1:
                    a, b = t[side], t[(side + 1) % 3]
                    edges.append((a, b, struct.unpack_from('<I', record, 0)[0], record[6]))
        nav.rasterize(up, floors, walls, segments)
        tris += len(up)
    nodes = nav.layers(floors, walls)
    adj = adjacency(nodes, walls)
    nav.links = links_of(adj)
    part, sizes = nav.components(nodes)
    rails = nav.weld(edges)
    out = dict(level=level, adj=adj, walls=segments, nodes=nodes, part=part, sizes=sizes, rails=rails, ramps=ramps, wall_faces=wall_faces, points=[])
    os.makedirs(os.path.join(SCRATCH, 'level'), exist_ok=True)
    path = os.path.join(SCRATCH, 'level', level + '.nav')
    with open(path + '.new', 'wb') as f:
        pickle.dump(out, f, protocol=4)
    os.replace(path + '.new', path)
    long_rails = [r for r in rails if r['length'] > 8]
    print('%s: %d triangles, %d cells, %d parts (largest %d), %d rail edges, %d rails (%d over 8 units, longest %.0f), %d ramp faces, kinds %s' % (
        level, tris, len(nodes), len(sizes), max(sizes or [0]), len(edges), len(rails), len(long_rails),
        max([r['length'] for r in rails] or [0]), len(ramps), dict(kinds)), flush=True)


class Level(nav.Level):
    def __init__(self, level):
        nav.SCRATCH = SCRATCH
        with open(os.path.join(SCRATCH, 'level', level + '.nav'), 'rb') as f:
            d = pickle.load(f)
        nav.links = links_of(d['adj'])     # before nav.Level finds the parts
        super().__init__(level)
        self.ramps, self.wall_faces = d['ramps'], d['wall_faces']


if __name__ == '__main__':
    a = sys.argv[1:]
    if a and a[0] == 'build':
        for level in a[1:] or LEVELS:
            build(level)
    elif a and a[0] == 'info':
        lv = Level(a[1])
        print(lv.name, len(lv.nodes), 'cells', len(lv.rails), 'rails', len(lv.ramps), 'ramp faces')
