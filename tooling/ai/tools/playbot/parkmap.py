#!/usr/bin/env python3
"""A park's own geometry as the playbot's map, for the three games' editor parks and a level folder: the collidable
faces and grind edges of every piece as the game placed it, the ground's cells, and from them the features a rider
plans to (rails, walls, ramps), each with a run-up checked clear on the map.

    parkmap.py dm2 CARD [SLOT]             DM2: the card's park (CARD/BASLUS-20159BMX2PARK/BMX2PARK.DAT, slot SLOT,
                                           0 the first) through loadPark's order, each object and rail from the
                                           theme's GEO, placed as loadPlacedObject and placeRail place them
    parkmap.py folder FOLDER [SLOT]        AI: a level folder (FOLDER/ASSETS/LEVELS/SLOT/SLOT.*, modlevel.py's)
    parkmap.py info MAP                    a map's features (MAP: a .parkmap file)
    parkmap.py picture MAP OUT.png [RIDE]  the map from above: ground by height, rails, walls, ramps, the run-ups, and a
                                           ride's track (RIDE: a .track.json the park players write)

AI's and BMX XXX's parks are read from the game while they are ridden (from_editor): the park editor's core stays
while a park is played (EditCore::toolSet 0), its cells' anchors hold the towers and the pieces, each piece's world
matrix and PieceDef::objectHash (the model in ASSETS/EDITOR/OBJECTS/OBJECTS.GEO). DM2 frees its editor's records
once the park is built (enterParkLevel), so its map comes from the card. Maps are kept as pickles beside the ride
(derived from the disc and the save, never in the repository). +y is down in all three games.

The map is the one nav.py makes for a disc level (nav.rasterize, nav.layers, nav.weld), so a level folder's map
(modlevel.py) and a park's are the same thing; the features and the tour are what a park adds.
"""
import collections
import heapq
import json
import math
import os
import pickle
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..'))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..'))
import nav  # noqa: E402
import zpaths  # noqa: E402

FACE_COLLIDES, FACE_TWO_SIDED = 0x2000, 0x4000
RAIL_SHORTEST = 1.5        # a welded rail shorter than this is a corner, not a feature
WALL_SHORTEST = 4.0        # a wall ride wants this much wall
SLOPE = (0.3, 0.97)        # a face this steep (|ny| of the unit normal) is a ramp's
MODEL_KEEP = ('Editobj_', 'EditObj_')


# ---- models: a GEO's objects by name hash ---------------------------------------------------------------------------

class Model:
    """One object of a GEO in its own space: vertices, collidable faces (i0, i1, i2, two-sided), grind edges
    (a, b, kind)."""

    def __init__(self, name, verts, faces, edges):
        self.name, self.verts, self.faces, self.edges = name, verts, faces, edges


def _head(obj):
    head = next((k for k in obj.kids if k.tag == b'TGHD'), None)
    if head is None or len(head.data) < 0x40:
        return None, None
    name = head.data[0x1C:0x3C].split(b'\0')[0].decode('latin1')
    return name, struct.unpack_from('<I', head.data, 0x3C)[0]


def models_ai(geo):
    """AI's GEO (ZGE3): collidable faces are the last part's with 0x2000 (ai_level.collision_triangles), grind edges
    each part's TGFE records with bit 0 of +9, kind +8 (ai_level.grind_edges)."""
    import ai_level as al
    out = {}
    for obj in al.parse(geo)[0].kids:
        if obj.tag != b'TGEO':
            continue
        name, h = _head(obj)
        parts = [k for k in obj.kids if k.tag == b'TGPT']
        if name is None or not parts:
            continue
        streams = {k.tag: k.data for k in parts[-1].kids if k.kids is None}
        nv, nf = struct.unpack_from('<HH', streams[b'TGPH'], 4)
        verts = [struct.unpack_from('<3f', streams[b'TGVP'], 12 * i) for i in range(nv)]
        faces = [struct.unpack_from('<4H', streams[b'TGFP'], 8 * i) for i in range(nf)]
        kept = [(f[1], f[2], f[3], bool(f[0] & FACE_TWO_SIDED)) for f in faces if f[0] & FACE_COLLIDES and max(f[1:]) < nv]
        edges = []
        for part in parts:
            s = {k.tag: k.data for k in part.kids if k.kids is None}
            if b'TGFE' not in s:
                continue
            pv, pf = struct.unpack_from('<HH', s[b'TGPH'], 4)
            pverts = [struct.unpack_from('<3f', s[b'TGVP'], 12 * i) for i in range(pv)]
            pfaces = [struct.unpack_from('<4H', s[b'TGFP'], 8 * i) for i in range(pf)]
            desc, count, indices, records = al.tag_buffer(s[b'TGFE'])
            for e, r in enumerate(struct.unpack_from('<%dH' % count, indices)):
                if r == 0xFFFF or not records[r][9] & 1 or e // 3 >= pf:
                    continue
                f = pfaces[e // 3]
                i0, i1 = f[1 + e % 3], f[1 + (e + 1) % 3]
                if max(i0, i1) < pv:
                    edges.append((pverts[i0], pverts[i1], records[r][8]))
        out[h] = Model(name, verts, kept, edges)
    return out


def models_dm2(geo):
    """DM2's GEO (ZGE2): the faces its collision tree (TGCI) lists, grind edges the TGFE records with bit 0 of +5,
    kind +6 (dm2map.objects_of)."""
    import dm2_level as dl
    import dm2map
    by_id = dm2map.objects_of(geo)
    out = {}
    for obj in dl.parse(geo)[0].kids:
        if obj.tag != b'TGEO':
            continue
        name, h = _head(obj)
        head = next(k for k in obj.kids if k.tag == b'TGHD')
        got = by_id.get(struct.unpack_from('<I', head.data, 0x18)[0])
        if name is None or got is None:
            continue
        verts, faces = got
        kept, edges = [], []
        for corners, normal, two_sided, tag, tagged in faces:
            kept.append((corners[0], corners[1], corners[2], bool(two_sided)))
            for side, record in tagged:
                if len(record) > 6 and record[5] & 1:
                    edges.append((verts[corners[side]], verts[corners[(side + 1) % 3]], record[6]))
        out[h] = Model(name, verts, kept, edges)
    return out


def models_bmx(geo):
    """BMX XXX's GEO (ZGE4): dm2_level's reader of its trees and normals (most parts have no TGFN), TGFE as DM2's."""
    import dm2_level as dl
    import dm2map
    out = {}
    for obj in dl.parse(geo)[0].kids:
        if obj.tag != b'TGEO':
            continue
        name, h = _head(obj)
        parts = [k for k in obj.kids if k.tag == b'TGPT']
        trees = [k for k in obj.kids if k.tag == b'TGCI']
        if name is None or not parts or not trees:
            continue
        streams = {k.tag: k.data for k in parts[-1].kids if k.kids is None}
        for k in obj.kids:
            if k.kids is None:
                streams.setdefault(k.tag, k.data)
        nv, nf = struct.unpack_from('<HH', streams[b'TGPH'], 4)
        verts = [struct.unpack_from('<3f', streams[b'TGVP'], 12 * i) for i in range(nv)]
        faces = [struct.unpack_from('<4H', streams[b'TGFP'], 8 * i) for i in range(nf)]
        listed = dl.tree_faces(trees[0].data, offset_leaves=True)
        kept = [(f[1], f[2], f[3], bool(f[0] & FACE_TWO_SIDED)) for i, f in enumerate(faces) if i in listed and max(f[1:]) < nv]
        edges = []
        if b'TGFE' in streams:
            ei, er = dm2map.buffer(streams[b'TGFE'])
            for n, r in enumerate(ei):
                if r == 0xFFFF or n // 3 >= nf:
                    continue
                rec = er[r]
                f = faces[n // 3]
                if len(rec) > 6 and rec[5] & 1 and max(f[1:]) < nv:
                    edges.append((verts[f[1 + n % 3]], verts[f[1 + (n + 1) % 3]], rec[6]))
        out[h] = Model(name, verts, kept, edges)
    return out


# ---- geometry: faces and edges in the world -------------------------------------------------------------------------

def place(m, v):
    """A point through a ZGE matrix (rows X, Y, Z, T: world = x X + y Y + z Z + T)."""
    return (v[0] * m[0] + v[1] * m[4] + v[2] * m[8] + m[12],
            v[0] * m[1] + v[1] * m[5] + v[2] * m[9] + m[13],
            v[0] * m[2] + v[1] * m[6] + v[2] * m[10] + m[14])


def heading_matrix(turns, at):
    """matrixRotateY of an identity by `turns` quarter turns (x' = x cos + z sin, z' = z cos - x sin, rows), at `at`."""
    a = turns * math.pi / 2
    c, s = round(math.cos(a), 9), round(math.sin(a), 9)
    return (c, 0.0, -s, 0.0, 0.0, 1.0, 0.0, 0.0, s, 0.0, c, 0.0, at[0], at[1], at[2], 1.0)


def normal(t):
    a, b, c = t
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    n = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
    size = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2)
    return (n[0] / size, n[1] / size, n[2] / size) if size > 1e-9 else None


class Geometry:
    """The park's collidable faces and grind edges in the world, each with the piece it came from."""

    def __init__(self):
        self.faces = []          # (triangle, two-sided, piece)
        self.edges = []          # (a, b, piece name, kind)
        self.pieces = []         # dict(name, at, turns or None, model name)

    def add(self, model, matrix, label=None):
        piece = len(self.pieces)
        self.pieces.append(dict(name=label or model.name, model=model.name, at=tuple(round(v, 3) for v in matrix[12:15])))
        world = [place(matrix, v) for v in model.verts]
        for i0, i1, i2, two in model.faces:
            self.faces.append(((world[i0], world[i1], world[i2]), two, piece))
        for a, b, kind in model.edges:
            self.edges.append((place(matrix, a), place(matrix, b), label or model.name, kind))
        return piece

    def add_ground(self, x0, x1, z0, z1, y, label='ground'):
        """A flat rectangle of ground (two faces seen from above: +y is down, so the winding is x then z)."""
        piece = len(self.pieces)
        self.pieces.append(dict(name=label, model=None, at=((x0 + x1) / 2, y, (z0 + z1) / 2)))
        a, b, c, d = (x0, y, z0), (x1, y, z0), (x1, y, z1), (x0, y, z1)
        self.faces.append(((a, b, c), True, piece))
        self.faces.append(((a, c, d), True, piece))
        return piece

    def add_side(self, p, q, top, bottom, label='ground side'):
        """A vertical face from p to q (x, z) between two heights."""
        piece = len(self.pieces)
        self.pieces.append(dict(name=label, model=None, at=((p[0] + q[0]) / 2, top, (p[1] + q[1]) / 2)))
        a, b, c, d = (p[0], top, p[1]), (q[0], top, q[1]), (q[0], bottom, q[1]), (p[0], bottom, p[1])
        self.faces.append(((a, b, c), True, piece))
        self.faces.append(((a, c, d), True, piece))
        return piece


def ground_cells(geo, cells, size, centre):
    """Square cells of ground (cells: {(a, b): y}, centre(a, b) -> (x, z)) and the side faces between cells of
    different heights, down to the lower one (an edge of the grid down DROP)."""
    for (a, b), y in cells.items():
        x, z = centre(a, b)
        h = size / 2
        geo.add_ground(x - h, x + h, z - h, z + h, y)
        for da, db in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            other = cells.get((a + da, b + db))
            low = y + nav.DROP if other is None else other
            if low <= y + 0.05:
                continue
            ox, oz = centre(a + da, b + db)
            mx, mz = (x + ox) / 2, (z + oz) / 2
            dx, dz = (ox - x) / size, (oz - z) / size
            p = (mx - dz * h, mz + dx * h)
            q = (mx + dz * h, mz - dx * h)
            geo.add_side(p, q, y, low)


# ---- the map ----------------------------------------------------------------------------------------------------------

def build(geo, name, units=1.0, extra=None):
    """The navigation map (nav.py's: cells of nav.CELL with their floors, rails, walls) and the park's features."""
    floors, walls, segments = collections.defaultdict(list), collections.defaultdict(list), []
    up, steep = [], []
    for t, two, piece in geo.faces:
        n = normal(t)
        if n is None:
            continue
        if two or n[1] < 0:
            up.append(t)
        if abs(n[1]) < nav.FLOOR_NY:
            steep.append(t)
    nav.rasterize(up, floors, walls, segments)
    nav.rasterize(steep, collections.defaultdict(list), walls, [])
    nodes = nav.layers(floors, walls)
    rails = dedupe(nav.weld(geo.edges))
    out = dict(level=name, nodes=nodes, rails=rails, points=[], walls=segments, pieces=geo.pieces,
               features=features(geo, rails, segments, units))
    out.update(extra or {})
    return out


def dedupe(rails):
    """nav.weld's chains, each once: a model's grind edge is often listed by both faces that share it, so a chain
    and its reverse come out twice."""
    seen, out = set(), []
    for r in rails:
        key = tuple(sorted(tuple(round(v, 1) for v in p) for p in r['points']))
        if key in seen:
            continue
        seen.add(key)
        out.append(r)
    return out


def straight_runs(pts, bend=0.94):
    """A chain cut where it bends (the cosine between pieces under `bend`): [(first, last)] points of each run."""
    runs, first = [], 0
    for i in range(1, len(pts) - 1):
        u = (pts[i][0] - pts[i - 1][0], pts[i][2] - pts[i - 1][2])
        v = (pts[i + 1][0] - pts[i][0], pts[i + 1][2] - pts[i][2])
        lu, lv = math.hypot(*u), math.hypot(*v)
        if lu > 1e-6 and lv > 1e-6 and (u[0] * v[0] + u[1] * v[1]) / (lu * lv) < bend:
            runs.append((first, i))
            first = i
    runs.append((first, len(pts) - 1))
    return [(pts[a], pts[b]) for a, b in runs if math.hypot(pts[b][0] - pts[a][0], pts[b][2] - pts[a][2]) >= RAIL_SHORTEST]


def wall_planes(geo, units=1.0):
    """The wall pieces' tall flat sides (pieces named for a wall: the games' own piece names, AI's Center Wall, DM2's
    ShortWall and TallWall rails, BMX XXX's walls), each plane's faces joined: (a, b, top, bottom, normal, name)."""
    planes = collections.defaultdict(list)
    for t, two, piece in geo.faces:
        p = geo.pieces[piece]
        if p['model'] is None or 'wall' not in p['name'].lower():
            continue
        n = normal(t)
        if n is None or abs(n[1]) > 0.15:
            continue
        h = math.hypot(n[0], n[2])
        nx, nz = n[0] / h, n[2] / h
        off = nx * t[0][0] + nz * t[0][2]
        planes[(round(nx, 1), round(nz, 1), round(off * 2) / 2)].append((t, nx, nz, p['name']))
    out = []
    for key, faces in planes.items():
        nx, nz = faces[0][1], faces[0][2]
        tx, tz = -nz, nx
        off = sum(nx * t[0][0] + nz * t[0][2] for t, *_ in faces) / len(faces)
        spans = sorted((min(tx * v[0] + tz * v[2] for v in t), max(tx * v[0] + tz * v[2] for v in t),
                        min(v[1] for v in t), max(v[1] for v in t), name) for t, _, _, name in faces)
        runs = []
        for lo, hi, top, bottom, name in spans:      # faces that touch along the plane are one wall
            if runs and lo <= runs[-1][1] + 0.3:
                r = runs[-1]
                runs[-1] = (r[0], max(r[1], hi), min(r[2], top), max(r[3], bottom), r[4])
            else:
                runs.append((lo, hi, top, bottom, name))
        for lo, hi, top, bottom, name in runs:
            if hi - lo < WALL_SHORTEST * units or bottom - top < 1.0 * units:
                continue
            a = (nx * off + tx * lo, nz * off + tz * lo)
            b = (nx * off + tx * hi, nz * off + tz * hi)
            out.append((a, b, top, bottom, (nx, nz), name))
    return out


def features(geo, rails, segments, units=1.0):
    """What a rider goes to: each rail (a welded chain RAIL_SHORTEST long or more), each wall piece's tall side, each
    ramp (a piece's sloped faces: their middle, the way they look, the top)."""
    out = []
    for n, r in enumerate(rails):
        if r['length'] < RAIL_SHORTEST * units:
            continue
        pts = r['points']
        out.append(dict(kind='rail', id='rail %d' % n, name=r['name'], points=pts, length=r['length'],
                        y=min(p[1] for p in pts), runs=straight_runs(pts)))
    for n, (a, b, top, bottom, nrm, name) in enumerate(wall_planes(geo, units)):
        out.append(dict(kind='wall', id='wall %d' % n, name=name, a=a, b=b, top=top, bottom=bottom, normal=nrm))
    by_piece = collections.defaultdict(list)
    for t, two, piece in geo.faces:
        n = normal(t)
        if n is None or geo.pieces[piece]['model'] is None:
            continue
        if n[1] < 0 and SLOPE[0] < -n[1] < SLOPE[1]:
            by_piece[piece].append((t, n))
    for piece, faces in by_piece.items():
        area = 0.0
        sx = sz = cx = cy = cz = 0.0
        top = 1e9
        for t, n in faces:
            a, b, c = t
            w = 0.5 * math.sqrt(sum(v * v for v in (
                (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]),
                (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]),
                (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]))))
            area += w
            sx += n[0] * w
            sz += n[2] * w
            cx += w * (a[0] + b[0] + c[0]) / 3
            cy += w * (a[1] + b[1] + c[1]) / 3
            cz += w * (a[2] + b[2] + c[2]) / 3
            top = min(top, a[1], b[1], c[1])
        size = math.hypot(sx, sz)
        if area < 2.0 * units * units or size < 0.2 * area:
            continue                # a bowl's or a spine's faces look every way: no one way up
        p = geo.pieces[piece]
        out.append(dict(kind='ramp', id='ramp %d' % piece, name=p['name'], centre=(cx / area, cy / area, cz / area),
                        facing=(sx / size, sz / size), top=top, area=area,
                        drop_in='roll' in p['name'].lower()))
    return out


def save(m, path):
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    with open(path + '.new', 'wb') as f:
        pickle.dump(m, f, protocol=4)
    os.replace(path + '.new', path)


def load(path):
    with open(path, 'rb') as f:
        return pickle.load(f)


CURB = 0.35            # a step ridden up between two flat floors
CLIMB = 2.5            # between floors on a slope (a ramp's cells, 2 m apart, rise this much at most)


def park_links(nodes, key, layer):
    """nav.links for a park: up a curb, up and down a slope (a ramp is ridden up), off a ledge one way."""
    i, k = key
    y, flat = nodes[key][layer]
    for di, dk in nav.NEAR:
        other = nodes.get((i + di, k + dk))
        if not other:
            continue
        near, below = None, None
        for n, (oy, oflat) in enumerate(other):
            dy = abs(oy - y)
            if dy <= CURB or (dy <= CLIMB and min(flat, oflat) < 0.97):
                near = n
                break
            if CURB < oy - y <= nav.DROP and below is None:
                below = n
        if near is not None:
            yield (i + di, k + dk), near, math.hypot(di, dk) * nav.CELL
        elif below is not None:
            yield (i + di, k + dk), below, math.hypot(di, dk) * nav.CELL + 6.0


class Plan(nav.Level):
    """A map as nav.Level plans over it (paths over the cells: park_links), with its features and their run-ups; an
    entry whose run-up starts where the rider cannot get back from (the strongly joined cells around `home`) is
    left out."""

    def __init__(self, m, run_up=14.0, clear=1.2, home=None):
        nav.links = park_links
        self.name = m['level']
        self.nodes, self.rails, self.points, self.walls = m['nodes'], m['rails'], m.get('points', []), m.get('walls', [])
        self.part, self.sizes = nav.components(self.nodes)
        self.around, self.extra = {}, {}
        self.map = m
        self.features = m['features']
        self.run_up, self.clear = run_up, clear
        self.rail_lines = [r['points'] for r in self.rails]
        self.rail_names = {id(r['points']): r['name'] for r in self.rails}
        self.group, gsize = self.strong()
        if home is None and m.get('spawn'):
            home = m['spawn']
        node = self.nearest(home) if home else None
        self.home = self.group.get(node) if node else max(range(len(gsize)), key=lambda g: gsize[g])
        for f in self.features:
            f['entries'] = [e for e in self.entries(f) if self.reachable(e['start'])]

    def reachable(self, p):
        node = self.nearest(p, radius=2)
        return node is not None and self.group.get(node) == self.home

    def floors_at(self, x, z):
        """The floors at (x, z): heights, top first (+y down: smallest first)."""
        return [y for y, _ in self.nodes.get(nav.cell_of(x, z), ())]

    def clear_length(self, start, d, y, most, mine=None):
        """How far a run-up from `start` along -d (towards the feature) stays on floor at the feature's height
        (within nav.STEP) with no other rail within `clear` of its line, and no wall cell across it; metres."""
        step = nav.CELL * 0.5
        n = 0
        while n * step < most:
            s = n * step
            px, pz = start[0] - d[0] * s, start[2] - d[1] * s
            for side in (-self.clear, 0.0, self.clear):
                qx, qz = px - d[1] * side, pz + d[0] * side
                if mine and min(seg_dist((qx, qz), (u[0], u[2]), (v[0], v[2])) for u, v in zip(mine, mine[1:])) < nav.CELL * 1.5:
                    continue            # the rail's own cells: its collision stands in them (nav.layers drops them)
                hs = self.floors_at(qx, qz)
                if not any(abs(h - y) <= nav.STEP for h in hs) or any(y - 3.0 < h < y - nav.STEP for h in hs):
                    return max(0.0, s - step)   # no floor at the run-up's height, or a piece over it (under a ramp)
            for line in self.rail_lines:
                if line is mine:
                    continue
                if mine and self.rail_names.get(id(line)) == self.rail_names.get(id(mine)) and parallel(line, mine):
                    continue            # the same piece's other edge beside this one (a box's or a wall's top)
                if s > 2.0 and any(seg_dist((px, pz), (u[0], u[2]), (v[0], v[2])) < self.clear for u, v in zip(line, line[1:])):
                    return max(0.0, s - step)
            n += 1
        return most

    def entries(self, f):
        """The ways into a feature, best first: dict(on, dir, start, clear): the point it is met at, the way along it,
        where the run-up starts."""
        out = []
        if f['kind'] == 'rail':
            for u, v in f['runs']:
                for a, b in ((u, v), (v, u)):
                    d = (b[0] - a[0], b[2] - a[2])
                    size = math.hypot(*d)
                    d = (d[0] / size, d[1] / size)
                    # the ground beside the rail where it is met: under the run-up's last metres, below its top
                    ground = None
                    for back in (2.0, 3.0, 4.0):
                        ground = self.ground_below((a[0] - d[0] * back, a[1] + 0.2, a[2] - d[1] * back))
                        if ground is not None:
                            break
                    if ground is None:
                        continue
                    c = self.clear_length(a, d, ground, self.run_up + 4.0, mine=f['points'])
                    out.append(dict(on=a, end=b, dir=(d[0], 0.0, d[1]), ground=ground, clear=c, length=size,
                                    rise=ground - min(a[1], b[1]), start=(a[0] - d[0] * c, ground, a[2] - d[1] * c)))
        elif f['kind'] == 'wall':
            (x0, z0), (x1, z1) = f['a'], f['b']
            nx, nz = f['normal']
            for a, b in (((x0, z0), (x1, z1)), ((x1, z1), (x0, z0))):
                d = (b[0] - a[0], b[1] - a[1])
                size = math.hypot(*d)
                d = (d[0] / size, d[1] / size)
                for side in (1,):              # the side the face looks to (each side of a wall is its own plane)
                    mid = ((a[0] + b[0]) / 2 + nx * side * 0.5, (a[1] + b[1]) / 2 + nz * side * 0.5)
                    ground = self.ground_below((mid[0] + nx * side * 2.0, f['bottom'] - 0.5, mid[1] + nz * side * 2.0))
                    if ground is None or abs(ground - f['bottom']) > 1.5:
                        continue
                    # in at about 30 degrees from the wall's side
                    w = (d[0] * 0.87 - nx * side * 0.5, d[1] * 0.87 - nz * side * 0.5)
                    c = self.clear_length((mid[0], ground, mid[1]), w, ground, self.run_up,
                                          mine=[(x0, ground, z0), (x1, ground, z1)])
                    out.append(dict(on=(mid[0], ground, mid[1]), dir=(w[0], 0.0, w[1]), ground=ground, clear=c,
                                    side=(nx * side, nz * side), along=(d[0], 0.0, d[1]),
                                    start=(mid[0] - w[0] * c, ground, mid[1] - w[1] * c)))
        elif f['kind'] == 'ramp':
            cx, cy, cz = f['centre']
            fx, fz = f['facing']
            if f.get('drop_in'):
                # a roll-in is ridden down from its deck: in along the way its face looks
                top = (cx - fx * 3.0, f['top'], cz - fz * 3.0)
                ground = self.ground_below((top[0], top[1] - 0.5, top[2]))
                if ground is not None:
                    d = (fx, fz)
                    c = self.clear_length(top, d, ground, self.run_up, mine=None)
                    out.append(dict(on=top, dir=(d[0], 0.0, d[1]), ground=ground, clear=c, through=(cx + fx * 8.0, cy, cz + fz * 8.0),
                                    start=(top[0] - d[0] * c, ground, top[2] - d[1] * c)))
            else:
                ground = self.ground_below((cx + fx * 4.0, cy, cz + fz * 4.0))
                if ground is not None:
                    d = (-fx, -fz)                  # up the ramp: against the way its face looks
                    foot = (cx + fx * 3.0, ground, cz + fz * 3.0)
                    c = self.clear_length(foot, d, ground, self.run_up, mine=None)
                    out.append(dict(on=foot, dir=(d[0], 0.0, d[1]), ground=ground, clear=c, through=(cx - fx * 3.0, cy, cz - fz * 3.0),
                                    start=(foot[0] - d[0] * c, ground, foot[2] - d[1] * c)))
        out.sort(key=lambda e: -e['clear'])
        return out

    def ground_below(self, p):
        """The floor under a point (the first at or below it, +y down), None if none."""
        hs = [h for h in self.floors_at(p[0], p[2]) if h >= p[1] - 0.3]
        return min(hs) if hs else None


def parallel(a, b):
    """Two chains run the same way (or opposite ways): their ends' directions within about 10 degrees."""
    u = (a[-1][0] - a[0][0], a[-1][2] - a[0][2])
    v = (b[-1][0] - b[0][0], b[-1][2] - b[0][2])
    lu, lv = math.hypot(*u), math.hypot(*v)
    return lu > 1e-6 and lv > 1e-6 and abs(u[0] * v[0] + u[1] * v[1]) / (lu * lv) > 0.985


def seg_dist(p, a, b):
    ax, az = b[0] - a[0], b[1] - a[1]
    size = ax * ax + az * az
    t = 0.0 if size < 1e-12 else max(0.0, min(1.0, ((p[0] - a[0]) * ax + (p[1] - a[1]) * az) / size))
    return math.hypot(p[0] - a[0] - ax * t, p[1] - a[1] - az * t)


class Tour:
    """The features in turn: the nearest one not yet ridden this lap (by the path's length), those ridden least
    first; a feature that fails twice in a lap waits for the next."""

    def __init__(self, plan, kinds=('rail', 'wall', 'ramp'), rng=None, cost=None, least=4.0, rise=None):
        self.plan = plan
        self.cost = cost if cost is not None else dict(ramp=25.0)      # metres added: rails and walls before ramps
        for f in plan.features:
            # a run-up at least LEAST long; a rail no higher over its ground than the rider's hop or ollie reaches
            f['entries'] = [e for e in f['entries'] if e['clear'] >= least and (rise is None or e.get('rise', 0.0) <= rise)]
        self.todo = [f for f in plan.features if f['kind'] in kinds and f['entries']]
        self.ridden = collections.Counter()
        self.failed = collections.Counter()
        self.lap = 1
        self.done_this_lap = set()
        self.rng = rng

    def given_up(self, f):
        """Missed three times and never ridden: left for the rest of the ride."""
        return not self.ridden[f['id']] and sum(n for (lap, i), n in self.failed.items() if i == f['id']) >= 3

    def next(self, pos):
        self.todo = [f for f in self.todo if not self.given_up(f)]
        left = [f for f in self.todo if f['id'] not in self.done_this_lap and self.failed[(self.lap, f['id'])] < 2]
        if not left:
            self.lap += 1
            self.done_this_lap = set()
            left = list(self.todo)
        if not left:
            return None
        least = min(self.ridden[f['id']] for f in left)
        pool = [f for f in left if self.ridden[f['id']] <= least + 1]
        return min(pool, key=lambda f: math.hypot(f['entries'][0]['start'][0] - pos[0], f['entries'][0]['start'][2] - pos[2])
                   + 15.0 * self.ridden[f['id']] + self.cost.get(f['kind'], 0.0))

    def result(self, f, ok):
        if ok:
            self.ridden[f['id']] += 1
            self.done_this_lap.add(f['id'])
        else:
            self.failed[(self.lap, f['id'])] += 1
            if self.failed[(self.lap, f['id'])] >= 2:
                self.done_this_lap.add(f['id'])


class Reached:
    """Which features a ride reached, by the game's own state each tick: a grind within REACH of a rail, a wall ride
    beside a wall, an air begun on a ramp's piece; and the track (positions every 10 ticks)."""
    REACH = 1.5

    def __init__(self, plan):
        self.plan = plan
        self.hits = collections.Counter()
        self.track = []
        self.was = None
        self.n = 0

    def nearest(self, pos, kind):
        best, bd = None, 1e9
        for f in self.plan.features:
            if f['kind'] != kind:
                continue
            if kind == 'rail':
                d = min(seg_dist((pos[0], pos[2]), (u[0], u[2]), (v[0], v[2])) + abs(u[1] - pos[1]) * 0.5
                        for u, v in zip(f['points'], f['points'][1:]))
            elif kind == 'wall':
                d = seg_dist((pos[0], pos[2]), f['a'], f['b'])
            else:
                d = math.hypot(pos[0] - f['centre'][0], pos[2] - f['centre'][2]) - math.sqrt(f['area']) * 0.5
            if d < bd:
                best, bd = f, d
        return best, bd

    def tick(self, pos, state):
        """state: 'grind', 'wall', 'air' or None, this tick."""
        self.n += 1
        if self.n % 10 == 0:
            self.track.append((round(pos[0], 2), round(pos[2], 2), state or ''))
        if state and state != self.was:
            kind = dict(grind='rail', wall='wall', air='ramp')[state]
            f, d = self.nearest(pos, kind)
            if f is not None and d < (self.REACH if kind != 'ramp' else 4.0):
                self.hits[f['id']] += 1
        self.was = state

    def summary(self):
        kinds = collections.Counter(f['kind'] for f in self.plan.features)
        got = collections.Counter(f['kind'] for f in self.plan.features if self.hits[f['id']])
        named = {}
        for f in self.plan.features:
            if self.hits[f['id']]:
                key = '%s %s' % (f['kind'], f['name'])
                named[key] = named.get(key, 0) + self.hits[f['id']]
        return dict(features={k: '%d of %d' % (got[k], kinds[k]) for k in sorted(kinds)}, reached=named)


# ---- DM2: the card's park ---------------------------------------------------------------------------------------------

DM2_DISC = os.environ.get('DM2_DISC', os.path.join(zpaths.DM2_WORKSPACE, 'recomp', 'disc-root'))
DM2_PARK_FILE = 'BASLUS-20159BMX2PARK/BMX2PARK.DAT'
DM2_LIBRARY_HEAD, DM2_PARK_BYTES = 0x1130, 0x2565      # the library's names, then 50 parks of kParkFileBytes
DM2_CELL = 5.0


def dm2_text(path):
    import dm2_level as dl
    return dl.Disc(DM2_DISC).read(path).decode('latin1').replace('\r', '')


def dm2_themes():
    t = dm2_text('ASSETS/EDITOR/THEMES.TXT')
    out = []
    for block in re.findall(r'THEME\s*\{(.*?)\n\}', t, re.S):
        out.append(dict(name=re.search(r'NAME\s+"([^"]*)"', block).group(1), file=re.search(r'FILE\s+"([^"]*)"', block).group(1),
                        width=int(re.search(r'WIDTH\s+(\d+)', block).group(1)), height=int(re.search(r'HEIGHT\s+(\d+)', block).group(1))))
    return out


def dm2_objects():
    """OBJECT.TXT: the objects by catalog index (file order: dm2park.catalog), their model and rail group; the rail
    objects' models per type and the groups' eight slots (object, quarter turns, drop)."""
    t = dm2_text('ASSETS/EDITOR/OBJECT.TXT')
    rails = t.split('RAILS', 1)[1]
    types = re.findall(r'TYPE\s+"([^"]*)"', rails.split('OBJECTS', 1)[0])
    robj = []
    for name, body in re.findall(r'NAME\s+"([^"]*)"\s*\{(.*?)\}', rails.split('OBJECTS', 1)[1].split('GROUPS', 1)[0], re.S):
        robj.append((name, re.findall(r'"([^"]*)"', body)))
    groups = []
    for line in rails.split('GROUPS', 1)[1].split('\n'):
        m = re.match(r'\s*NAME\s+"([^"]*)"(.*)', line)
        if not m:
            continue
        rest = re.findall(r'"([^"]*)"\s+(\d+)\s+(\d+)', m.group(2))
        groups.append((m.group(1), [(n, int(q), int(d)) for n, q, d in rest]))
    objs = []
    for o in re.findall(r'OBJECT \{(.*?)\}', t.split('\nOBJECTS', 1)[1], re.S):
        g = re.search(r'RAIL\s+"([^"]*)"', o)
        objs.append(dict(model=re.search(r'HASH "([^"]*)"', o).group(1), group=g.group(1) if g else None))
    return dict(types=types, rail_objects=robj, groups=groups, objects=objs)


def dm2_park_stream(card, slot=0):
    with open(os.path.join(card, DM2_PARK_FILE), 'rb') as f:
        data = f.read()
    at = DM2_LIBRARY_HEAD + slot * DM2_PARK_BYTES
    return data[at:at + DM2_PARK_BYTES]


def dm2_read_park(stream):
    """loadPark's order: version (4), theme (1), the start (column, row, turns, placed), then every cell by row: its
    level, whether its object starts here (then object, unused, rotation, column, row), its rails (whether any, then
    group, slots and a type and slot for each set bit)."""
    themes = dm2_themes()
    version = struct.unpack_from('<I', stream, 0)[0]
    theme = stream[4]
    th = themes[theme]
    start = tuple(stream[5:9])
    at = 9
    cells, objects, rails = {}, [], []
    for row in range(th['height']):
        for col in range(th['width']):
            level = struct.unpack_from('<b', stream, at)[0]
            has = stream[at + 1]
            at += 2
            cells[(row, col)] = level
            if has:
                o, unused, rot, ocol, orow = stream[at:at + 5]
                at += 5
                objects.append(dict(object=o, rotation=rot, column=ocol, row=orow))
            has_rails = stream[at]
            at += 1
            if has_rails:
                group, slots = stream[at], stream[at + 1]
                at += 2
                for i in range(8):
                    if slots >> i & 1:
                        typ, slot = stream[at], stream[at + 1]
                        at += 2
                        rails.append(dict(row=row, column=col, group=group, type=typ, slot=slot))
    return dict(version=version, theme=theme, theme_name=th['name'], file=th['file'], rows=th['height'],
                columns=th['width'], start=dict(column=start[0], row=start[1], turns=start[2], placed=start[3]),
                cells=cells, objects=objects, rails=rails, read=at)


DM2_SLOT_BY_ROTATION = None


def dm2_slot_by_rotation(run_mem=None):
    """railSlotByRotation [turns][slot] (rail_tool.cpp kRailSlotByRotation): the table is in the ELF's data."""
    global DM2_SLOT_BY_ROTATION
    if DM2_SLOT_BY_ROTATION is None:
        import dm2park
        at = dm2park.symbols('dm2').get('railSlotByRotation')
        elf = open(os.path.join(DM2_DISC, 'SLUS_201.59'), 'rb').read()
        DM2_SLOT_BY_ROTATION = elf_bytes(elf, at, 32)
    return DM2_SLOT_BY_ROTATION


def elf_bytes(elf, address, n):
    """n bytes at a virtual address of an ELF32's loaded segments."""
    phoff, = struct.unpack_from('<I', elf, 0x1C)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x2A)
    for i in range(phnum):
        p_type, off, vaddr, paddr, filesz = struct.unpack_from('<5I', elf, phoff + i * phentsize)
        if p_type == 1 and vaddr <= address < vaddr + filesz:
            return elf[off + address - vaddr:off + address - vaddr + n]
    raise KeyError('%#x is in no loaded segment' % address)


def dm2_map(card, slot=0, name=None):
    import dm2_level as dl
    park = dm2_read_park(dm2_park_stream(card, slot))
    defs = dm2_objects()
    disc = dl.Disc(DM2_DISC)
    folder = 'ASSETS/EDITOR/%s/%s' % (park['file'].upper(), park['file'].upper())
    geo_bytes = disc.read(folder + '.GEO')
    models = models_dm2(geo_bytes)
    rows, cols = park['rows'], park['columns']

    def cell_world(col, row):
        # cellWorldPosition: x = column x 5 - (columns - 1) x 2.5, z = row x 5 - (rows - 1) x 2.5, y = -level
        return (col * DM2_CELL - (cols - 1) * DM2_CELL / 2, -park['cells'][(row, col)], row * DM2_CELL - (rows - 1) * DM2_CELL / 2)

    geo = Geometry()
    missing = collections.Counter()
    for o in park['objects']:
        d = defs['objects'][o['object']]
        m = models.get(dl.name_hash(d['model']))
        if m is None:
            missing[d['model']] += 1
            continue
        geo.add(m, heading_matrix(o['rotation'], cell_world(o['column'], o['row'])),
                label=re.sub('(?i)^editobj_', '', d['model']))
    table = dm2_slot_by_rotation()
    group_index = {}
    for o in park['objects']:
        group_index[(o['row'], o['column'])] = o
    names = [g[0] for g in defs['groups']]
    robj = {n.lower(): models_ for n, models_ in defs['rail_objects']}
    for r in park['rails']:
        turns = 0
        owner = None
        # a rail on an object's cell hangs on the object (placeRail: the object's turns and rail group)
        for o in park['objects']:
            if o['row'] == r['row'] and o['column'] == r['column']:
                owner = o
        if owner is not None:
            turns = owner['rotation']
        turned = table[turns * 8 + r['slot']]
        gname, slots = defs['groups'][r['group']]
        oname, quarter, drop = slots[turned]
        mname = robj.get(oname.lower(), [''] * 4)[r['type']] if oname.lower() != 'none' else ''
        m = models.get(dl.name_hash(mname)) if mname else None
        if m is None:
            missing['rail %s/%s' % (oname, r['type'])] += 1
            continue
        at = list(cell_world(r['column'], r['row']))
        at[1] -= drop
        geo.add(m, heading_matrix(quarter + turns, at), label='%s %s' % (defs['types'][r['type']].replace('railtype_', '').replace('_name', ''), oname))
    ground = {}
    for (row, col), level in park['cells'].items():
        if level > 0:
            ground[(col, row)] = -float(level)
    ground_cells(geo, ground, DM2_CELL, lambda c, r: (c * DM2_CELL - (cols - 1) * DM2_CELL / 2, r * DM2_CELL - (rows - 1) * DM2_CELL / 2))
    s = park['start']
    spawn = cell_world(s['column'], s['row'])
    m = build(geo, name or 'dm2-park', extra=dict(game='dm2', spawn=spawn, park=dict(
        theme=park['theme_name'], rows=rows, columns=cols, objects=len(park['objects']), rails=len(park['rails']),
        missing=dict(missing), version=park['version'], read=park['read'])))
    return m


# ---- AI and BMX XXX: a saved park (Park NNN.sav) ------------------------------------------------------------------------

ZGE3_PARK_END = 0xAA       # loadEditPark's last byte (kParkEndMark)


def zge3_disc(game):
    if game == 'ai':
        return os.path.join(zpaths.AI_WORKSPACE, 'recomp', 'disc-root')
    return os.path.join(zpaths.PATHS.get('BMX_DATA', os.path.join(zpaths.ZAXIS_SCRATCH, 'zbind1', 'bmx')), 'disc')


def zge3_file(game, *parts):
    """A file of the game's disc, the name's case as the disc has it."""
    at = zge3_disc(game)
    for part in parts:
        names = {n.lower(): n for n in os.listdir(at)}
        at = os.path.join(at, names.get(part.lower(), part))
    return at


def zge3_text(game, *parts):
    with open(zge3_file(game, *parts), 'rb') as f:
        return f.read().decode('latin1').replace('\r', '')


def zge3_pieces(game):
    """OBJECT.OBJ's ObjectDefs in file order: the catalog's index (PieceDef::index), each Name, MaxObjectName, ClassName."""
    t = zge3_text(game, 'ASSETS', 'EDITOR', 'OBJECT.OBJ')
    out = []
    for block in re.findall(r'ObjectDefBegin\s*\{(.*?)\n\s*\}\s*\n\s*ObjectDefEnd', t, re.S):
        name = re.search(r'Name\s+"([^"]*)"', block)
        model = re.search(r'MaxObjectName\s+"([^"]*)"', block)
        cls = re.search(r'ClassName\s+(\S+)', block)
        out.append(dict(name=name.group(1) if name else '', model=model.group(1) if model else '', cls=cls.group(1) if cls else ''))
    return out


def zge3_theme(game, index):
    """The project file's (INLINE.PEP, BMX3.PEP) theme by index: its name and map (MapDimension x, z, y;
    MapSectorSize)."""
    pep = 'INLINE.PEP' if game == 'ai' else 'BMX3.PEP'
    t = zge3_text(game, 'ASSETS', 'EDITOR', pep)
    themes = re.findall(r'ThemeDefBegin\s*\{.*?Name\s+"([^"]*)".*?MapFileName\s+"([^"]*)"', t, re.S)
    name, path = themes[index]
    m = zge3_text(game, *path.split('\\'))
    dims = [int(v) for v in re.search(r'MapDimension\s*\{([^}]*)\}', m).group(1).split()]
    size = [float(v) for v in re.search(r'MapSectorSize\s*\{([^}]*)\}', m).group(1).split()]
    centre = [float(v) for v in re.search(r'MapGridCenter\s*\{([^}]*)\}', m).group(1).split()]
    return dict(name=name, map=path, width=dims[0], depth=dims[1], height=dims[2], cell=size[0], centre=centre)


def zge3_read_park(data):
    """A Park NNN.sav: two words (a check and the stream's size), then loadParkData's stream: the theme (4), the author
    and name (0x20 each), the edit view (18 bytes: three floats and three angles), then every cell (forEachParkCell:
    y, z, x) through its tower (EditTowerObject: its height, a float, then a word of the anchors that hold a piece;
    each such anchor: its quarter turns, a signed byte, then the piece: its catalog index (u16), its own data (none
    for a plain EditObject) and its word of held anchors), then the end mark."""
    size = struct.unpack_from('<I', data, 4)[0]
    at = 8
    theme = struct.unpack_from('<i', data, at)[0]
    at += 4 + 0x20 + 0x20 + 18
    return theme, at, size


def zge3_park_map(game, save_path, name=None):
    with open(save_path, 'rb') as f:
        data = f.read()
    theme_index, at, size = zge3_read_park(data)
    th = zge3_theme(game, theme_index)
    defs = zge3_pieces(game)
    geo_bytes = open(zge3_file(game, 'ASSETS', 'EDITOR', 'OBJECTS', 'OBJECTS.GEO'), 'rb').read()
    import ai_level as al
    models = (models_ai if game == 'ai' else models_bmx)(geo_bytes)
    W, D, cell = th['width'], th['depth'], th['cell']

    def centre(x, z):
        # the selection box's cell centre (custom0, aipark.W and bikepark.BMX): x right, the grid's z towards -z
        return ((x - (W - 1) / 2.0) * cell + th['centre'][0], -(z - (D - 1) / 2.0) * cell + th['centre'][2])

    geo = Geometry()
    towers, placed, missing, spawn = {}, [], collections.Counter(), None
    for c in range(W * D * max(1, th['height'])):
        x, z = c % W, (c // W) % D
        height, hold = struct.unpack_from('<fI', data, at)
        at += 8
        towers[(x, z)] = height
        for bit in range(32):
            if not hold >> bit & 1:
                continue
            turns = struct.unpack_from('<b', data, at)[0] % 4
            index = struct.unpack_from('<H', data, at + 1)[0]
            inner = struct.unpack_from('<I', data, at + 3)[0]
            at += 7
            if inner:
                raise ValueError('a piece holding pieces (cell %d, %d): not read yet' % (x, z))
            d = defs[index] if index < len(defs) else dict(name='?%d' % index, model='', cls='')
            cx, cz = centre(x, z)
            placed.append(dict(x=x, z=z, turns=turns, index=index, name=d['name']))
            if d['name'].lower() == 'startobject':
                spawn = (cx, height, cz)
                continue
            m = models.get(al.name_hash(d['model'])) if d['model'] else None
            if m is None:
                missing[d['name']] += 1
                continue
            geo.add(m, heading_matrix(turns, (cx, height, cz)), label=d['name'])
    if data[at] != ZGE3_PARK_END:
        raise ValueError('the park stream ends at %#x with %#x, not the end mark' % (at, data[at]))
    ground_cells(geo, towers, cell, centre)
    return build(geo, name or '%s-park' % game, extra=dict(game=game, spawn=spawn, park=dict(
        theme=th['name'], width=W, depth=D, cell=cell, pieces=len(placed), missing=dict(missing),
        raised=sum(1 for h in towers.values() if h))))


# ---- a level folder (modlevel.py's) -----------------------------------------------------------------------------------

def folder_map(folder, slot='TUT'):
    """A level folder's collision and grind edges (ai_level's readers), as a park's geometry."""
    import ai_level as al
    disc = al.Disc(folder)
    files = al.read_level(disc, slot)
    geo = Geometry()
    for g in al.collision_triangles(files['GEO'], files['INS']):
        piece = len(geo.pieces)
        geo.pieces.append(dict(name=g[5], model=g[5], at=((g[0] + g[1]) / 2, 0.0, (g[2] + g[3]) / 2)))
        for t in g[4]:
            geo.faces.append((t, False, piece))
    geo.edges = list(al.grind_edges(files['GEO'], files['INS']))
    points = []
    for c in al.parse(files['DAT'])[0].kids:
        if c.tag == b'SDPT' and len(c.data) >= 0x1C:
            kind, nm = struct.unpack_from('<II', c.data, 4)
            pos = struct.unpack_from('<3f', c.data, 0x10)
            points.append(dict(cls=kind & 0xFFFF, kind=kind, name=nm, pos=pos, data=c.data[0x2C:0x60].hex()))
    m = build(geo, slot, extra=dict(game='ai'))
    m['points'] = points
    return m


# ---- looking at a map -------------------------------------------------------------------------------------------------

def info(m):
    lines = ['%s (%s): %d cells, %d rails, %d walls, %d pieces' % (m['level'], m.get('game'), len(m['nodes']), len(m['rails']),
                                                                    len(m['walls']), len(m['pieces']))]
    if m.get('park'):
        lines.append('park: %s' % json.dumps(m['park']))
    plan = Plan(m)
    for f in plan.features:
        e = f['entries'][0] if f['entries'] else None
        where = ''
        if f['kind'] == 'rail':
            a, b = f['points'][0], f['points'][-1]
            where = '(%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f), %.1f m' % (a[0], a[1], a[2], b[0], b[1], b[2], f['length'])
        elif f['kind'] == 'wall':
            where = '(%.1f, %.1f) to (%.1f, %.1f), %.1f to %.1f' % (f['a'] + f['b'] + (f['top'], f['bottom']))
        else:
            where = 'at (%.1f, %.1f, %.1f) facing (%.2f, %.2f), %.0f m2' % (f['centre'] + f['facing'] + (f['area'],))
        lines.append('  %-9s %-28s %s; run-up %s' % (f['id'], f['name'][:28], where,
                                                       '%.1f m clear' % e['clear'] if e else 'none'))
    return '\n'.join(lines)


def picture(m, out, track=None, scale=8):
    """The map from above as a PNG: floors by height (light high), rails red, walls blue, ramps' run-ups green, the
    track yellow (grinds orange, airs white)."""
    import zlib
    xs = [k[0] for k in m['nodes']]
    zs = [k[1] for k in m['nodes']]
    i0, i1, k0, k1 = min(xs) - 2, max(xs) + 2, min(zs) - 2, max(zs) + 2
    w, h = (i1 - i0 + 1) * scale, (k1 - k0 + 1) * scale
    img = bytearray(b'\x18\x18\x18' * (w * h))
    ys = [l[0][0] for l in m['nodes'].values()]
    lo, hi = min(ys), max(ys)

    def put(px, py, c):
        if 0 <= px < w and 0 <= py < h:
            img[(py * w + px) * 3:(py * w + px) * 3 + 3] = bytes(c)

    def to_px(x, z):
        return int((x / nav.CELL - i0) * scale), int((z / nav.CELL - k0) * scale)

    for (i, k), layers in m['nodes'].items():
        y = layers[0][0]
        t = 0.0 if hi == lo else (hi - y) / (hi - lo)
        c = (int(60 + 150 * t), int(60 + 150 * t), int(70 + 150 * t))
        for dx in range(scale):
            for dz in range(scale):
                put((i - i0) * scale + dx, (k - k0) * scale + dz, c)

    def line(a, b, c, width=1):
        (ax, ay), (bx, by) = to_px(*a), to_px(*b)
        n = max(abs(bx - ax), abs(by - ay), 1)
        for s in range(n + 1):
            px, py = ax + (bx - ax) * s // n, ay + (by - ay) * s // n
            for ox in range(-(width // 2), width - width // 2):
                for oy in range(-(width // 2), width - width // 2):
                    put(px + ox, py + oy, c)

    plan = Plan(m)
    for f in plan.features:
        for e in f['entries'][:1]:
            line((e['start'][0], e['start'][2]), (e['on'][0], e['on'][2]), (60, 200, 60))
    for x0, z0, x1, z1, top, bottom, nx, nz in m['walls']:
        line((x0, z0), (x1, z1), (80, 120, 255), 2)
    for r in m['rails']:
        for u, v in zip(r['points'], r['points'][1:]):
            line((u[0], u[2]), (v[0], v[2]), (230, 40, 40), 2)
    if track:
        colours = {'': (240, 220, 40), 'grind': (255, 140, 0), 'air': (255, 255, 255), 'wall': (80, 255, 255)}
        for x, z, s in track:
            px, py = to_px(x, z)
            for ox in range(max(2, scale // 5)):
                for oy in range(max(2, scale // 5)):
                    put(px + ox, py + oy, colours.get(s, (240, 220, 40)))
    raw = b''.join(b'\0' + bytes(img[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
    with open(out, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
    return out


def main(argv):
    if len(argv) >= 2 and argv[0] == 'dm2':
        m = dm2_map(argv[1], int(argv[2]) if len(argv) > 2 else 0)
        import zpaths   # (tooling/paths.env: the scratch folder; tooling is on sys.path above)
        path = os.path.join(os.environ.get('PLAYDM2', os.path.join(zpaths.ZAXIS_SCRATCH, 'playdm2')), 'level', 'park.parkmap')
        save(m, path)
        print(info(m))
        print('map:', path)
    elif len(argv) >= 2 and argv[0] == 'folder':
        m = folder_map(argv[1], argv[2] if len(argv) > 2 else 'TUT')
        path = os.path.join(nav.SCRATCH, 'level', m['level'] + '.parkmap')
        save(m, path)
        print(info(m))
        print('map:', path)
    elif len(argv) >= 2 and argv[0] == 'info':
        print(info(load(argv[1])))
    elif len(argv) >= 3 and argv[0] == 'picture':
        track = json.load(open(argv[3]))['track'] if len(argv) > 3 else None
        print(picture(load(argv[1]), argv[2], track, scale=int(os.environ.get('SCALE', '8'))))
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
