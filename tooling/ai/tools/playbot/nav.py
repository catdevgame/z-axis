#!/usr/bin/env python3
"""A level as the player sees it: walkable cells, rails, poles, pipes and data points.

Built once per level from the disc's own level files by the parsers of tooling/ai/tools/ai_level.py
(the same records the game reads) and kept as a pickle in the scratch folder, never in the repository:
it is derived from the disc. +y is down in this game.

    nav.py build [LEVEL ...]     the eight levels by default
    nav.py info LEVEL
"""
import collections
import heapq
import math
import os
import pickle
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
import zpaths  # noqa: E402  (tooling/paths.env)
import zguard  # noqa: E402
# (the map makers' first line, as core.py's for the players: not from or into the main checkout; tools36)
zguard.refuse(os.path.basename(sys.argv[0] or 'playbot') or 'playbot',
              [os.environ.get(k) for k in ('PLAYAI', 'PLAYDM2', 'BMXCORPUS', 'PLAYCOLL')])
MAIN = os.environ.get('AI_MAIN', zpaths.AI_WORKSPACE)
ZAXIS = zpaths.ZAXIS_MAIN
SCRATCH = os.environ.get('PLAYAI', os.path.join(zpaths.ZAXIS_SCRATCH, 'playai'))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))   # (ai_level.py, dm2_level.py: tooling/ai/tools)
DISC = os.path.join(MAIN, 'recomp', 'disc-root')

CELL = 2.0
STEP = 1.3         # a height a skater rolls over between cells (curbs, ramps)
HEADROOM = 2.2     # clear height over a floor
FLOOR_NY = 0.6     # a face this flat is rolled on
LEVELS = ('TUT', 'MOVIE', 'CIVIC', 'INDUST', 'BRDWLK', 'CANNERY', 'HANGER', 'MUSEUM')  # by level select entry
NPC_CLASS, SPAWN_CLASS = 0x5343, 0x46BE


def cell_of(x, z):
    return int(math.floor(x / CELL)), int(math.floor(z / CELL))


def centre(i, k):
    return (i + 0.5) * CELL, (k + 0.5) * CELL


def wall_segment(a, b, c, nx, nz):
    """A tall straight wall face as (x0, z0, x1, z1, top, bottom, normal x, normal z), or None."""
    ends = max(((p, q) for p in (a, b, c) for q in (a, b, c)), key=lambda e: math.hypot(e[0][0] - e[1][0], e[0][2] - e[1][2]))
    length = math.hypot(ends[0][0] - ends[1][0], ends[0][2] - ends[1][2])
    top, bottom = min(a[1], b[1], c[1]), max(a[1], b[1], c[1])
    if length < 7.0 or bottom - top < 3.0:
        return None
    size = math.hypot(nx, nz)
    return ends[0][0], ends[0][2], ends[1][0], ends[1][2], top, bottom, nx / size, nz / size


def rasterize(tris, floors, walls, segments):
    """Each triangle's cells: a floor's height at the cell centre, a wall's height range; tall walls as segments."""
    for a, b, c in tris:
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        size = math.sqrt(nx * nx + ny * ny + nz * nz)
        if size < 1e-9:
            continue
        flat = abs(ny) / size
        x0, x1 = min(a[0], b[0], c[0]), max(a[0], b[0], c[0])
        z0, z1 = min(a[2], b[2], c[2]), max(a[2], b[2], c[2])
        i0, k0 = cell_of(x0, z0)
        i1, k1 = cell_of(x1, z1)
        if (i1 - i0 + 1) * (k1 - k0 + 1) > 40000:
            continue
        if flat >= FLOOR_NY:
            d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
            if abs(d) < 1e-9:
                continue
            for i in range(i0, i1 + 1):
                for k in range(k0, k1 + 1):
                    x, z = centre(i, k)
                    l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d
                    l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d
                    l3 = 1 - l1 - l2
                    if min(l1, l2, l3) >= -0.05:
                        floors[(i, k)].append((l1 * a[1] + l2 * b[1] + l3 * c[1], flat))
        else:
            if flat < 0.15:
                seg = wall_segment(a, b, c, nx, nz)
                if seg:
                    segments.append(seg)
            r = CELL * 0.5
            for i in range(cell_of(x0 - r, 0)[0], cell_of(x1 + r, 0)[0] + 1):
                for k in range(cell_of(0, z0 - r)[1], cell_of(0, z1 + r)[1] + 1):
                    x, z = centre(i, k)
                    part = clip((a, b, c), x - r, x + r, z - r, z + r)
                    if part:      # the height the face has here, not all of it: a gate's sloped arch leaves its opening open
                        ys = [p[1] for p in part]
                        walls[(i, k)].append((min(ys), max(ys)))


def clip(points, x0, x1, z0, z1):
    """A polygon cut to a box in x and z."""
    for axis, limit, keep in ((0, x0, 1), (0, x1, -1), (2, z0, 1), (2, z1, -1)):
        out = []
        for n, p in enumerate(points):
            q = points[n - 1]
            dp, dq = (p[axis] - limit) * keep, (q[axis] - limit) * keep
            if (dp >= 0) != (dq >= 0):
                t = dq / (dq - dp)
                out.append(tuple(q[m] + (p[m] - q[m]) * t for m in range(3)))
            if dp >= 0:
                out.append(p)
        points = out
        if not points:
            return points
    return points


def layers(floors, walls):
    """Cells' distinct floors, without those a wall stands on."""
    nodes = {}
    for key, hs in floors.items():
        hs.sort()
        merged = []
        for y, flat in hs:
            if merged and abs(y - merged[-1][0]) < 0.8:
                continue
            merged.append((y, flat))
        kept = []
        for n, (y, flat) in enumerate(merged):
            above = merged[n - 1][0] if n else -1e9
            if y - above < HEADROOM:
                continue                 # no room under the floor above it
            if any(top < y - 0.5 and bot > y - HEADROOM for top, bot in walls.get(key, ())):
                continue
            kept.append((y, flat))
        if kept:
            nodes[key] = kept
    return nodes


NEAR = ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1))


DROP = 14.0        # a ledge a skater rolls off and lands from, one way (+y is down)


def links(nodes, key, layer):
    i, k = key
    y = nodes[key][layer][0]
    for di, dk in NEAR:
        other = nodes.get((i + di, k + dk))
        if not other:
            continue
        near, below = None, None
        for n, (oy, _) in enumerate(other):
            if abs(oy - y) <= STEP:
                near = n
                break
            if STEP < oy - y <= DROP and below is None:
                below = n               # the first floor under the ledge
        if near is not None:
            yield (i + di, k + dk), near, math.hypot(di, dk) * CELL
        elif below is not None:
            yield (i + di, k + dk), below, math.hypot(di, dk) * CELL + 6.0


def components(nodes):
    """The cells' parts, joined whichever way a link runs: {(key, layer): part}, sizes. A part says a place can be
    reached at all; a path may still have no way back up a ledge."""
    parent = {}

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for key, ls in nodes.items():
        for layer in range(len(ls)):
            parent[(key, layer)] = (key, layer)
    for key, ls in nodes.items():
        for layer in range(len(ls)):
            for ok, ol, _ in links(nodes, key, layer):
                ra, rb = find((key, layer)), find((ok, ol))
                if ra != rb:
                    parent[ra] = rb
    number, part, sizes = {}, {}, []
    for node in parent:
        root = find(node)
        if root not in number:
            number[root] = len(sizes)
            sizes.append(0)
        part[node] = number[root]
        sizes[number[root]] += 1
    return part, sizes


def weld(edges):
    """Grind edges into rails: chains of edges sharing ends, each with its points."""
    key = lambda p: tuple(round(v * 4) for v in p)
    ends = collections.defaultdict(list)
    for n, (a, b, name, kind) in enumerate(edges):
        ends[key(a)].append(n)
        ends[key(b)].append(n)
    used, rails = set(), []
    for n, e in enumerate(edges):
        if n in used:
            continue
        used.add(n)
        chain = [e[0], e[1]]
        for side in (1, 0):
            while True:
                tip = chain[-1] if side else chain[0]
                nxt = None
                for m in ends[key(tip)]:
                    if m in used or edges[m][3] != e[3]:
                        continue
                    a, b = edges[m][0], edges[m][1]
                    other = b if key(a) == key(tip) else a
                    u = (tip[0] - (chain[-2] if side else chain[1])[0], tip[2] - (chain[-2] if side else chain[1])[2])
                    v = (other[0] - tip[0], other[2] - tip[2])
                    lu, lv = math.hypot(*u), math.hypot(*v)
                    if lu > 1e-6 and lv > 1e-6 and (u[0] * v[0] + u[1] * v[1]) / (lu * lv) > 0.7:
                        nxt = (m, other)
                        break
                if nxt is None:
                    break
                used.add(nxt[0])
                if side:
                    chain.append(nxt[1])
                else:
                    chain.insert(0, nxt[1])
        length = sum(math.dist(chain[i], chain[i + 1]) for i in range(len(chain) - 1))
        rails.append(dict(points=chain, length=length, kind=e[3], name=e[2]))
    return rails


def build(level):
    import ai_level as al
    disc = al.Disc(DISC)
    files = al.read_level(disc, level)
    groups = al.collision_triangles(files['GEO'], files['INS'])
    floors, walls = collections.defaultdict(list), collections.defaultdict(list)
    segments = []
    tris = 0
    for g in groups:
        rasterize(g[4], floors, walls, segments)
        tris += len(g[4])
    nodes = layers(floors, walls)
    part, sizes = components(nodes)
    rails = weld(al.grind_edges(files['GEO'], files['INS']))
    points = []
    for c in al.parse(files['DAT'])[0].kids:
        if c.tag == b'SDPT' and len(c.data) >= 0x1C:
            kind, name = struct.unpack_from('<II', c.data, 4)
            pos = struct.unpack_from('<3f', c.data, 0x10)
            points.append(dict(cls=kind & 0xFFFF, kind=kind, name=name, pos=pos, data=c.data[0x2C:0x60].hex()))
    out = dict(level=level, nodes=nodes, rails=rails, points=points, walls=segments)
    path = os.path.join(SCRATCH, 'level', level + '.nav')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        pickle.dump(out, f, protocol=4)
    print('%s: %d triangles, %d cells, %d parts (largest %d), %d rails, %d points, %d walls' % (
        level, tris, len(nodes), len(sizes), max(sizes or [0]), len(rails), len(points), len(segments)), flush=True)


class Level:
    """One level's plan: path finding over cells, rails with ways onto them."""

    def __init__(self, level):
        with open(os.path.join(SCRATCH, 'level', level + '.nav'), 'rb') as f:
            d = pickle.load(f)
        self.name = level
        self.nodes, self.rails, self.points, self.walls = d['nodes'], d['rails'], d['points'], d.get('walls', [])
        self.part, self.sizes = components(self.nodes)
        self.around = {}
        self.extra = {}
        self.join_parts()

    def strong(self):
        """The cells' strongly connected groups: {node: group}, sizes. Ledges run one way, so a part may have no way back."""
        order, seen = [], set()
        back = {}
        nodes = [(key, n) for key, ls in self.nodes.items() for n in range(len(ls))]
        ways = {}
        for node in nodes:
            ways[node] = [(k, l) for k, l, _ in links(self.nodes, node[0], node[1])]
            for t in ways[node]:
                back.setdefault(t, []).append(node)
        for node in nodes:
            if node in seen:
                continue
            seen.add(node)
            stack = [(node, iter(ways[node]))]
            while stack:
                cur, it = stack[-1]
                for t in it:
                    if t not in seen:
                        seen.add(t)
                        stack.append((t, iter(ways[t])))
                        break
                else:
                    order.append(cur)
                    stack.pop()
        group, sizes = {}, []
        for node in reversed(order):
            if node in group:
                continue
            group[node] = len(sizes)
            sizes.append(1)
            stack = [node]
            while stack:
                for t in back.get(stack.pop(), ()):
                    if t not in group:
                        group[t] = group[node]
                        sizes[-1] += 1
                        stack.append(t)
        return group, sizes

    def join_parts(self):
        """Large groups of cells one missing cell apart at one height: a gate or door in the collision that stands open
        in play (Movie Lot's studio gates). The cells' links get a way across, and the parts become one."""
        group, gsize = self.strong()
        for (key, layer), p in group.items():
            if gsize[p] < 200:
                continue
            y = self.nodes[key][layer][0]
            for di, dk in NEAR:
                if any(abs(h[0] - y) < 1.5 for h in self.nodes.get((key[0] + di, key[1] + dk), ())):
                    continue
                far = (key[0] + di * 2, key[1] + dk * 2)
                for n, (oy, _) in enumerate(self.nodes.get(far, ())):
                    q = group[(far, n)]
                    if abs(oy - y) < 0.6 and q != p and gsize[q] >= 200:
                        self.extra.setdefault((key, layer), []).append((far, n, 2 * CELL * math.hypot(di, dk) + 8.0))
        if not self.extra:
            return
        parent = list(range(len(self.sizes)))

        def find(a):
            while parent[a] != a:
                parent[a] = parent[parent[a]]
                a = parent[a]
            return a

        for a, ways in self.extra.items():
            for b, m, _ in ways:
                parent[find(self.part[a])] = find(self.part[(b, m)])
        sizes = [0] * len(self.sizes)
        for node, p in self.part.items():
            self.part[node] = find(p)
            sizes[self.part[node]] += 1
        self.sizes = sizes

    def nearest(self, pos, radius=6):
        """The cell and layer under pos."""
        ci, ck = cell_of(pos[0], pos[2])
        best, cost = None, 1e18
        for di in range(-radius, radius + 1):
            for dk in range(-radius, radius + 1):
                key = (ci + di, ck + dk)
                for n, (y, _) in enumerate(self.nodes.get(key, ())):
                    d = di * di + dk * dk + (y - pos[1]) ** 2
                    if d < cost:
                        best, cost = (key, n), d
        return best

    def path(self, start, goal, limit=60000):
        """World points from start to goal over the cells, or None."""
        a, b = self.nearest(start), self.nearest(goal)
        if not a or not b or self.part.get(a) != self.part.get(b):
            return None
        open_, came, best = [(0.0, 0, a)], {}, {a: 0.0}
        n = 0
        h = lambda key: CELL * math.hypot(key[0][0] - b[0][0], key[0][1] - b[0][1])
        while open_ and n < limit:
            _, _, cur = heapq.heappop(open_)
            if cur == b:
                out = []
                while cur in came:
                    out.append(cur)
                    cur = came[cur]
                out.reverse()
                return [self.world(c) for c in out]
            for ok, ol, d in list(links(self.nodes, cur[0], cur[1])) + self.extra.get(cur, []):
                key = (ok, ol)
                # keep off walls and ledges: a cell with few neighbours costs more
                near = self.around.get(key)
                if near is None:
                    near = self.around[key] = sum(1 for _ in links(self.nodes, ok, ol))
                c = best[cur] + d * (1.0 if near >= 8 else 2.5)
                if c < best.get(key, 1e18):
                    best[key] = c
                    came[key] = cur
                    n += 1
                    heapq.heappush(open_, (c + h(key), n, key))
        return None

    def world(self, node):
        (i, k), layer = node
        x, z = centre(i, k)
        return x, self.nodes[(i, k)][layer][0], z

    def main_part(self, pos):
        node = self.nearest(pos)
        return self.part.get(node) if node else None


def main(argv):
    if argv and argv[0] == 'build':
        for level in argv[1:] or LEVELS:
            build(level)
    elif argv and argv[0] == 'info':
        lv = Level(argv[1])
        print(len(lv.nodes), 'cells', len(lv.rails), 'rails; longest',
              [(round(r['length']), r['kind'], r['name']) for r in sorted(lv.rails, key=lambda r: -r['length'])[:8]])
        print(collections.Counter(p['cls'] for p in lv.points).most_common(8))
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
