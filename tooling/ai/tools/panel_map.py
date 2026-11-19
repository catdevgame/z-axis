#!/usr/bin/env python3
"""The Thor bottom panel's level files, at build time only: a top-down map of
each level made from its geometry, with the world-to-map transform, and the
level's goals from its script. The running game converts nothing: it reads
what this writes (runtime/include/collection/panel_file.h has the format). Never commit what
this writes: it is your disc's data.

    python3 -I panel_map.py build --disc <disc folder or .iso> --out <dir> [--level MOVIE ...] [--png <dir>]
                                  [--zoo <folder of ZOO.*>] [--estate <folder of TUT.*>] [--xbox <the app's xbox folder>]
                                                    one <NAME>.pmap per level, UI.pmap; with --xbox each level's
                                                    goals as the Xbox release's script has them too (the career
                                                    scripts setting runs it): the panel takes the loaded one
    python3 -I panel_map.py build --game dm2 --disc <DM2 disc folder or .iso> --out <dir> [--level WOODWARD ...]
                                  [--png <dir>] [--dm2-xbox <dm2_xbox.py's folder>]
                                                    Dave Mirra BMX 2: one <FOLDER>.pmap per level (the map, its
                                                    name and its ProQuest goals), UI.pmap (its fonts and tricks),
                                                    Venice and Greenville too with --dm2-xbox
    python3 -I panel_map.py build --game bmx --disc <BMX XXX disc folder or .iso> --out <dir> [--level CHL_DAM ...]
                                  [--png <dir>]
                                                    BMX XXX: one <FOLDER>.pmap per level on the disc (the map, its
                                                    name and its challenges from its OBJCTV.TXT), UI.pmap (its
                                                    fonts and its trick list)

Python 3.8+, standard library only (the rasteriser is span-filling, so a level
takes seconds without numpy).
"""
import argparse
import array
import collections
import os
import re
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ai_level as al  # noqa: E402
import dm2_level as dl  # noqa: E402
import panel_goals as pg  # noqa: E402

# The career's levels (folder: display name): the game's own folder table (0x24BC30).
LEVELS = dict(al.LEVELS)

FILE_VERSION = 3    # panel_file.h kFileVersion
PIXELS_PER_UNIT = 3.0
MAX_SIDE = 2400
MARGIN = 12.0       # world units around the skateable area
CELL = 8.0          # world units, for finding the level proper
BACKGROUND = (10, 13, 19)

FACE_COLLIDES = al.FACE_COLLIDES


def say(text):
    print("[panelmap] " + text, flush=True)


# ---- textures: only their mean colours ---------------------------------------

def bmp_mean(data):
    """The mean colour (r, g, b, alpha share) of a TBMP / TZMP picture (an 8-
    or 4-bit BMP; a TZMP's palette has alpha, a TBMP's leaves it 0)."""
    if data[:2] != b"BM":
        return None
    off = struct.unpack_from("<I", data, 10)[0]
    head = struct.unpack_from("<I", data, 14)[0]
    width, height, _, bpp = struct.unpack_from("<iiHH", data, 18)
    height = abs(height)
    colours = struct.unpack_from("<I", data, 46)[0] or (1 << bpp)
    if bpp not in (4, 8):
        return None
    palette = data[14 + head:14 + head + colours * 4]
    counts = collections.Counter(data[off:off + (width * bpp // 8 + 3 & ~3) * height])
    if bpp == 4:
        pairs = collections.Counter()
        for value, n in counts.items():
            pairs[value >> 4] += n
            pairs[value & 15] += n
        counts = pairs
    has_alpha = any(palette[i * 4 + 3] for i in range(len(palette) // 4))
    total = weight = r = g = b = 0
    for value, n in counts.items():
        if value * 4 + 3 >= len(palette):
            continue
        bb, gg, rr, aa = palette[value * 4:value * 4 + 4]
        a = aa if has_alpha else 128
        if rr > 200 and bb > 200 and gg < 70:
            a = 0                                 # the art's colour key
        total += n
        weight += n * a
        r += n * a * rr
        g += n * a * gg
        b += n * a * bb
    if not weight:
        return (128, 128, 128, 0.0)
    return (r / weight, g / weight, b / weight, weight / (total * 128.0))


def texture_means(txr):
    means = {}
    for chunk in al.parse(txr)[0].kids:
        if chunk.tag != b"TXRC":
            continue
        ident = None
        for k in chunk.kids:
            if k.tag == b"TXRH":
                ident = struct.unpack_from("<I", k.data, 0)[0]
                if b"INVIS" in k.data[0x14:].split(b"\0")[0].upper():
                    means[ident] = (0, 0, 0, 0.0)  # collision walls: nothing to see
            elif k.tag in (b"TBMP", b"TZMP") and ident is not None and ident not in means:
                mean = bmp_mean(k.data)
                if mean:
                    means[ident] = mean
    return means


# ---- geometry ---------------------------------------------------------------

class Surface:
    __slots__ = ("verts", "faces", "colours", "albedo")


def read_objects(geo, means):
    objects = {}
    for obj in al.parse(geo)[0].kids:
        if obj.tag != b"TGEO":
            continue
        head = next(k for k in obj.kids if k.tag == b"TGHD")
        ident = struct.unpack_from("<I", head.data, 0x18)[0]
        parts = [k for k in obj.kids if k.tag == b"TGPT"]
        if not parts:
            continue
        # the most detailed part with collision faces (they share it, or the last part has them)
        best = None
        for part in parts:
            streams = {k.tag: k for k in part.kids}
            nv, nf = struct.unpack_from("<HH", streams[b"TGPH"].data, 4)
            faces = [struct.unpack_from("<4H", streams[b"TGFP"].data, 8 * i) for i in range(nf)]
            if any(f[0] & FACE_COLLIDES for f in faces):
                best = (part, streams, nv, faces)
                break
        if best is None:
            continue
        part, streams, nv, faces = best
        s = Surface()
        s.verts = [struct.unpack_from("<3f", streams[b"TGVP"].data, 12 * i) for i in range(nv)]
        s.faces = [f for f in faces if max(f[1:]) < nv]
        s.colours = None
        if b"TGVC" in streams:
            s.colours = streams[b"TGVC"].data
        elif b"TIVC" in streams:
            s.colours = al.expand(streams[b"TIVC"], nv, 4)
        table = next((k for k in part.kids if k.tag == b"TMTB"), None) or next((k for k in obj.kids if k.tag == b"TMTB"), None)
        s.albedo = []
        for mat in (table.kids if table else ()):
            if mat.tag != b"MATI":
                continue
            layers = struct.unpack_from("<I", mat.data, 0x2C)[0]
            texture = struct.unpack_from("<I", mat.data, 0x40)[0] if layers else 0
            s.albedo.append(means.get(texture, (128, 128, 128, 1.0)))
        objects[ident] = s
    return objects


def is_hidden_instance(name):
    """Left off the map: a roof hides what the level is about (the Movie Lot's
    studio, the Airfield's hangars; ceilings face down and go with the back
    faces), a shadow or a light's cone is no surface."""
    lower = name.lower()
    return any(word in lower for word in ("roof", "skylight", "ceiling", "shadow", "spotlight"))


def world_triangles(geo, ins, means):
    """(y top, a, b, c, normal, colour, water) for every collidable face seen
    from above (colour: one for the face, or its three corners'), and the invisible walls [(a, b, c)]: they draw nothing but
    bound the level."""
    objects = read_objects(geo, means)
    instances, place = al.instance_placer(ins)
    tris = []
    barriers = []
    for d in instances:
        flags = struct.unpack_from("<I", d, 0)[0]
        if flags & (al.INSTANCE_COLLIDES | al.INSTANCE_NO_COLLISION) != al.INSTANCE_COLLIDES:
            continue                       # sky planes, fog cards and decals: what the skater never touches
        s = objects.get(struct.unpack_from("<I", d, 0x40)[0])
        if s is None or is_hidden_instance(d[0x50:].split(b"\0")[0].decode("latin1")):
            continue
        world = [place(d, v) for v in s.verts]
        col = s.colours
        lower = d[0x50:].split(b"\0")[0].decode("latin1").lower()
        water = "water" in lower or "deathplane" in lower   # the sea, and the planes that kill under it
        for f in s.faces:
            if not f[0] & FACE_COLLIDES:
                continue
            i0, i1, i2 = f[1:]
            a, b, c = world[i0], world[i1], world[i2]
            ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
            vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
            nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
            length = (nx * nx + ny * ny + nz * nz) ** 0.5
            if length == 0:
                continue
            material = f[0] & 0xFF
            al_r, al_g, al_b, alpha = s.albedo[material] if material < len(s.albedo) else (128, 128, 128, 1)
            if alpha == 0.0 and abs(ny) < 0.5 * length:
                barriers.append((a, b, c))
            if ny > 0:                     # +y is down: this face looks down
                continue
            if alpha < 0.55:               # shadows and other see-through cards
                continue
            # each corner's own colour (the material's, lit by the corner's
            # vertex colour): the picture blends them across the face as the
            # game draws it, so a face's lighting does not stop at its edges
            if col:
                corners = tuple((al_r * col[i * 4] / 128.0, al_g * col[i * 4 + 1] / 128.0, al_b * col[i * 4 + 2] / 128.0)
                                for i in (i0, i1, i2))
            else:
                corners = ((al_r, al_g, al_b),) * 3
            mean = tuple(sum(v[k] for v in corners) / 3 for k in range(3))
            if not water and max(mean) < 14:   # unlit cards (shadows, the backs of things)
                continue
            colour = WATER_COLOUR if water else corners
            tris.append((min(a[1], b[1], c[1]), a, b, c, (nx / length, ny / length, nz / length), colour, water))
    return tris, barriers


def bounds_of(tris, seeds):
    """The box of the level proper: the connected cells (CELL units) of faces
    that hold the skater's start points; far scenery and stray props fall outside."""
    cells = set()
    for t in tris:
        if t[6]:
            continue                       # the sea goes on past the level
        area = abs((t[2][0] - t[1][0]) * (t[3][2] - t[1][2]) - (t[2][2] - t[1][2]) * (t[3][0] - t[1][0]))
        xs = t[1][0], t[2][0], t[3][0]
        zs = t[1][2], t[2][2], t[3][2]
        cx0, cx1 = int(min(xs) // CELL), int(max(xs) // CELL)
        cz0, cz1 = int(min(zs) // CELL), int(max(zs) // CELL)
        if area < 2 and (cx1 - cx0) + (cz1 - cz0) == 0:
            continue
        if (cx1 - cx0 + 1) * (cz1 - cz0 + 1) > 6400:   # a far panorama's sheet
            continue
        for cx in range(cx0, cx1 + 1):
            for cz in range(cz0, cz1 + 1):
                cells.add((cx, cz))
    start = [(int(p[0] // CELL), int(p[1] // CELL)) for p in seeds]
    todo = [c for c in start if c in cells] or [max(cells, key=lambda c: sum((c[0] + i, c[1] + j) in cells for i in range(-4, 5) for j in range(-4, 5)))]
    seen = set(todo)
    while todo:
        cx, cz = todo.pop()
        for i in (-1, 0, 1):
            for j in (-1, 0, 1):
                c = (cx + i, cz + j)
                if c in cells and c not in seen:
                    seen.add(c)
                    todo.append(c)
    xs = [c[0] for c in seen]
    zs = [c[1] for c in seen]
    return min(xs) * CELL - MARGIN, (max(xs) + 1) * CELL + MARGIN, min(zs) * CELL - MARGIN, (max(zs) + 1) * CELL + MARGIN


# ---- the picture -------------------------------------------------------------

def albedo(colour):
    """A face's own colour as the picture keeps it before the light: bright
    vertex-lit roofs roll off instead of clipping."""
    out = []
    for v in colour:
        out.append(max(0, min(255, int(v if v < 165 else 165 + (v - 165) * 60 / (60 + v - 165)))))
    return bytes(out)


class Frame:
    """World to picture: top down with +x right and +z up, or, for a level
    wider than tall, a quarter turn clockwise so its long side lies along the
    tall pane (panel_view.h MapTransform)."""

    def __init__(self, box, ppu):
        self.x0, self.x1, self.z0, self.z1 = box
        self.ppu = ppu
        self.turned = self.x1 - self.x0 > self.z1 - self.z0
        across, down = (self.z1 - self.z0, self.x1 - self.x0) if self.turned else (self.x1 - self.x0, self.z1 - self.z0)
        self.width = int(across * ppu) + 1
        self.height = int(down * ppu) + 1

    def point(self, x, z):
        if self.turned:
            return (z - self.z0) * self.ppu, (x - self.x0) * self.ppu
        return (x - self.x0) * self.ppu, (self.z1 - z) * self.ppu

    def world_left(self):
        return self.x0

    def world_top(self):
        return self.z0 if self.turned else self.z1


WATER_CODE = 255
PLAY_CHECK = None    # a list: build_level leaves each level's walk here (tools only)

# The height buffer's codes: 0 no ground, else 2 + HEIGHT_STEPS over the
# level's span (the ground's height as a 16-bit number, the lowest first).
# The callers get the same heights as a byte a pixel (0 no ground, WATER_CODE
# water, else 2 .. 253): reachable, fill_gaps and outline read those.
HEIGHT_STEPS = 60000
HEIGHT_NONE, HEIGHT_FIRST = 0, 2
KIND_GROUND, KIND_WATER = 0, 1
LIGHT_FLAT = 0.62       # the light of the lowest flat ground; the highest flat ground gets LIGHT_FLAT + LIGHT_RISE
LIGHT_RISE = 0.38
LIGHT_SLOPE = 0.32      # how much a slope towards the upper left brightens (away from it darkens), at 45 degrees
ONE_TO_FF = bytes([0, 255] + [0] * 254)
MUL255 = bytes((a * b + 127) // 255 for a in range(256) for b in range(256))


def ramp(c0, c1, count):
    """`count` codes from c0 to c1 (integers), the last one c1."""
    if count < 2 or c0 == c1:
        return [c0] * count
    run = count - 1
    return [c0 + (c1 - c0) * k // run for k in range(count)]


def face_spans(points, width, height):
    """(y, x0, x1, at0, at1) for each picture row a face covers: its span
    there and the face's values at the span's two ends. points: (x, y,
    value, ...) for the three corners, the values (a height code, a colour)
    blended along the edges."""
    (ax, ay, *av), (bx, by, *bv), (cx, cy, *cv) = sorted(points, key=lambda p: p[1])
    y0 = max(0, int(ay))
    y1 = min(height - 1, int(cy))
    for y in range(y0, y1 + 1):
        yy = min(cy, max(ay, y + 0.5))
        t_long = (yy - ay) / (cy - ay + 1e-9)
        xa = ax + (cx - ax) * t_long
        va = [p + (q - p) * t_long for p, q in zip(av, cv)]
        if yy < by:
            t_short = (yy - ay) / (by - ay + 1e-9)
            xb = ax + (bx - ax) * t_short
            vb = [p + (q - p) * t_short for p, q in zip(av, bv)]
        else:
            t_short = (yy - by) / (cy - by + 1e-9)
            xb = bx + (cx - bx) * t_short
            vb = [p + (q - p) * t_short for p, q in zip(bv, cv)]
        if xa > xb:
            xa, xb, va, vb = xb, xa, vb, va
        i = max(0, int(xa))
        j = min(width, int(xb) + 1)
        if j > i:
            yield y, i, j, va, vb


def light_of(code, dx, dy, kind, slope_gain):
    """A pixel's light (a byte): flat ground by its height, a slope towards the
    upper left brighter and away from it darker, as a printed relief map."""
    if code == HEIGHT_NONE:
        return 255
    if kind == KIND_WATER:
        return 255
    light = LIGHT_FLAT + LIGHT_RISE * (code - HEIGHT_FIRST) / HEIGHT_STEPS
    slope = -(dx + dy) * slope_gain
    if slope > 1.0:
        slope = 1.0
    elif slope < -1.0:
        slope = -1.0
    light *= 1.0 + LIGHT_SLOPE * slope
    return max(24, min(255, int(light * 255)))


def rasterise(tris, frame):
    """The picture and its heights: a height buffer, where the highest surface
    at each pixel wins whatever order the faces come in (walls project to
    nothing, undersides lose to what is above them), each pixel painted with
    the colour of the face that holds it, then shaded by its height and its
    slope (a relief map: streets flat, roofs flat and lighter, ramps and
    banks lit on the side that faces the upper left). (rows, heights as
    bytes, codes per unit for those bytes, the top height)."""
    from operator import eq, sub
    width, height = frame.width, frame.height
    rows = [bytearray(bytes(BACKGROUND) * width) for _ in range(height)]
    tops = [array.array("H", bytes(width * 2)) for _ in range(height)]
    kinds = [bytearray(width) for _ in range(height)]
    ys = sorted(t[0] for t in tris if not t[6])
    ymin, ymax = ys[len(ys) // 200], ys[-len(ys) // 200 - 1]
    span = max(1e-3, ymax - ymin)
    per_unit = HEIGHT_STEPS / span
    faces = []
    for top, a, b, c, normal, colour, water in tris:
        if abs(normal[1]) < 0.1:
            continue                       # a wall: nothing of it is seen from above
        points = []
        for p in (a, b, c):
            x, y = frame.point(p[0], p[2])
            points.append((x, y, HEIGHT_FIRST + max(0, min(HEIGHT_STEPS, int((ymax - p[1]) * per_unit)))))
        if water:
            paint = WATER_COLOUR
        elif isinstance(colour[0], tuple):
            paint = [albedo(v) for v in colour]
            if paint[0] == paint[1] == paint[2]:
                paint = paint[0]
        else:
            paint = albedo(colour)
        faces.append((points, paint, KIND_WATER if water else KIND_GROUND))
    # the top surface at each pixel
    for points, _, _ in faces:
        for y, i, j, c0, c1 in face_spans(points, width, height):
            row = tops[y]
            row[i:j] = array.array("H", map(max, row[i:j], ramp(int(c0[0]), int(c1[0]), j - i)))
    # each pixel's colour and kind: the face that holds the top
    for points, paint, kind in faces:
        blend = isinstance(paint, list)
        spans = face_spans([(x, y, h) + (tuple(paint[k]) if blend else ()) for k, (x, y, h) in enumerate(points)], width, height)
        for y, i, j, c0, c1 in spans:
            n = j - i
            wins = bytes(map(eq, ramp(int(c0[0]), int(c1[0]), n), tops[y][i:j]))
            if blend:
                rgb_row = bytearray(n * 3)
                for k in range(3):
                    rgb_row[k::3] = bytes(ramp(int(c0[k + 1] + 0.5), int(c1[k + 1] + 0.5), n))
                rgb_row = bytes(rgb_row)
            else:
                rgb_row = paint * n
            if 0 not in wins:
                rows[y][i * 3:j * 3] = rgb_row
                kinds[y][i:j] = bytes([kind]) * n
                continue
            if 1 not in wins:
                continue
            mask = wins.translate(ONE_TO_FF)
            mask3 = bytearray(n * 3)
            mask3[0::3] = mask3[1::3] = mask3[2::3] = mask
            m = int.from_bytes(mask3, "big")
            old = int.from_bytes(rows[y][i * 3:j * 3], "big")
            rows[y][i * 3:j * 3] = ((old & ~m) | (int.from_bytes(rgb_row, "big") & m)).to_bytes(n * 3, "big")
            if kind != KIND_GROUND:
                m = int.from_bytes(mask, "big")
                old = int.from_bytes(kinds[y][i:j], "big")
                kinds[y][i:j] = ((old & ~m) | (int.from_bytes(bytes([kind]) * n, "big") & m)).to_bytes(n, "big")
    # the light: the height, and the slope from the height buffer itself
    slope_gain = frame.ppu / per_unit / 2.0  # codes over two pixels to units a unit: a 45 degree slope is 1
    heights = []
    to_byte = lambda code: 0 if code == HEIGHT_NONE else HEIGHT_FIRST + (code - HEIGHT_FIRST) * 251 // HEIGHT_STEPS
    for y in range(height):
        row = tops[y]
        above = tops[y - 1] if y > 0 else row
        below = tops[y + 1] if y + 1 < height else row
        # the slope over two pixels each way: a facet's edge is softened, a wall's step stays a step
        dx = [0] + list(map(sub, row[2:], row[:-2])) + [0]
        dy = list(map(sub, below, above))
        light = bytes(map(light_of, row, dx, dy, kinds[y], [slope_gain] * width))
        light3 = bytearray(width * 3)
        light3[0::3] = light3[1::3] = light3[2::3] = light
        rows[y] = bytearray(map(MUL255.__getitem__, map(int.__or__, map((256).__mul__, rows[y]), light3)))
        codes = bytearray(map(to_byte, row))
        water = kinds[y]
        if 1 in water:
            codes = bytearray(WATER_CODE if k == KIND_WATER else c for c, k in zip(codes, water))
        heights.append(codes)
    return rows, heights, 253.0 / span, ymax


# ---- in play or not ----------------------------------------------------------

CELL_UNITS = 1.0     # the walk's grid
STEP_UNITS = 3.5     # what the skater rolls or hops up
SEED_UNITS = 6.0     # how far over its surface a thing can float
OUT_OF_PLAY = bytes(14 + v * 40 // 100 for v in range(256))


def reachable(frame, heights, per_unit, ymax, seeds, barriers):
    """Walks the height map from `seeds` (world x, z, y or None): the cells the
    skater can roll to (ramps climb, a wall does not). A seed with a height
    counts only on the surface at that height: a rooftop under a prop hung
    high over the courtyard is not in play. (grid, seen, cells across, cells
    down, step in pixels)."""
    step = max(1, int(CELL_UNITS * frame.ppu))
    across = frame.width // step
    down = frame.height // step
    grid = [bytes(heights[y * step][x * step] for x in range(across)) for y in range(down)]
    limit = max(1, int(STEP_UNITS * per_unit))
    seen = [bytearray(across) for _ in range(down)]
    # the invisible walls stop the walk where they stand
    blocked = [bytearray(across) for _ in range(down)]
    for tri in barriers:
        for p, q in ((tri[0], tri[1]), (tri[1], tri[2]), (tri[2], tri[0])):
            ax, ay = frame.point(p[0], p[2])
            bx, by = frame.point(q[0], q[2])
            steps = int(max(abs(bx - ax), abs(by - ay)) / step) + 1
            for k in range(steps + 1):
                cx = int(ax + (bx - ax) * k / steps) // step
                cy = int(ay + (by - ay) * k / steps) // step
                if 0 <= cx < across and 0 <= cy < down:
                    blocked[cy][cx] = 1
    todo = []
    near = int(SEED_UNITS * per_unit)
    for x, z, y in seeds:
        mx, my = frame.point(x, z)
        cx, cy = int(mx) // step, int(my) // step
        wanted = None if y is None else 2 + (ymax - y) * per_unit
        # a seed sits on what is near it (a prop hangs over its surface)
        for j in range(-2, 3):
            for i in range(-2, 3):
                xx, yy = cx + i, cy + j
                if 0 <= xx < across and 0 <= yy < down and 0 < grid[yy][xx] < WATER_CODE and not seen[yy][xx]:
                    if wanted is None or abs(grid[yy][xx] - wanted) <= near:
                        seen[yy][xx] = 1
                        todo.append((xx, yy))
    while todo:
        x, y = todo.pop()
        here = grid[y][x]
        for xx, yy in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= xx < across and 0 <= yy < down and not seen[yy][xx]:
                there = grid[yy][xx]
                if 0 < there < WATER_CODE and abs(there - here) <= limit and not blocked[yy][xx]:
                    seen[yy][xx] = 1
                    todo.append((xx, yy))
    return grid, seen, across, down, step


def mute_out_of_play(rows, grid, seen, across, down, step):
    """What the skater cannot reach steps back: the buildings the level's
    streets run between, the ground behind them. The share of ground kept."""
    kept = total = 0
    for y in range(down):
        line = grid[y]
        reached = seen[y]
        x = 0
        while x < across:
            if line[x] == 0 or line[x] == WATER_CODE or reached[x]:
                kept += reached[x]
                total += 1 if line[x] and line[x] != WATER_CODE else 0
                x += 1
                continue
            end = x
            while end < across and line[end] and line[end] != WATER_CODE and not reached[end]:
                end += 1
            total += end - x
            for row in rows[y * step:(y + 1) * step]:
                row[x * step * 3:end * step * 3] = row[x * step * 3:end * step * 3].translate(OUT_OF_PLAY)
            x = end
    return kept / max(1, total)


WATER_COLOUR = (44, 86, 128)
WALL_COLOUR = bytes((52, 56, 66))
WALL_HEIGHT = 240
GAP_UNITS = 12.0    # world units: what a building's wall can be thick


def fill_gaps(width, height, rows, heights, ppu):
    """Walls have no top to draw: a level's thick walls are empty ground on
    the map, between what the skater touches on either side (the Movie Lot's
    studio wall is six units of nothing). A short empty run with ground on
    both ends, across or down, is such a wall: drawn as one. The count."""
    most = int(GAP_UNITS * ppu)
    empty = re.compile(b"\0+")
    marks = []
    for y in range(height):
        for m in empty.finditer(heights[y]):
            if 0 < m.start() and m.end() < width and m.end() - m.start() <= most:
                marks.append((y, m.start(), m.end()))
    flat = b"".join(bytes(h) for h in heights)
    for x in range(width):
        for m in empty.finditer(flat[x::width]):
            if 0 < m.start() and m.end() < height and m.end() - m.start() <= most:
                for y in range(m.start(), m.end()):
                    marks.append((y, x, x + 1))
    code = bytes([WALL_HEIGHT])
    filled = 0
    for y, x0, x1 in marks:
        rows[y][x0 * 3:x1 * 3] = WALL_COLOUR * (x1 - x0)
        heights[y][x0:x1] = code * (x1 - x0)
        filled += x1 - x0
    return filled


def outline(width, height, rows, heights, ppu):
    """Height steps as ink: the eye reads walls, kerbs and ledges off them."""
    for y in range(height - 1):
        row = rows[y]
        h0 = heights[y]
        h1 = heights[y + 1]
        for x in range(width - 1):
            a = h0[x]
            d = abs(a - h0[x + 1])
            e = abs(a - h1[x])
            if e > d:
                d = e
            if d > 8:
                k = 150 if d > 24 else 200
                i = x * 3
                row[i] = row[i] * k >> 8
                row[i + 1] = row[i + 1] * k >> 8
                row[i + 2] = row[i + 2] * k >> 8


def draw_rails(rows, frame, rails):
    width, height = frame.width, frame.height
    for a, b, name, kind in rails:
        ax, ay = frame.point(a[0], a[2])
        bx, by = frame.point(b[0], b[2])
        steps = int(max(abs(bx - ax), abs(by - ay))) + 1
        for i in range(steps + 1):
            x = int(ax + (bx - ax) * i / steps)
            y = int(ay + (by - ay) * i / steps)
            if 0 <= x < width and 0 <= y < height:
                row = rows[y]
                row[x * 3] = row[x * 3] + 90 >> 1
                row[x * 3 + 1] = row[x * 3 + 1] + 200 >> 1
                row[x * 3 + 2] = row[x * 3 + 2] + 240 >> 1


def spawn_points(dat):
    """(x, z) of the level's spawn points (DataPoint: class +4, position +0x10)."""
    out = []
    for c in al.walk(al.parse(dat)):
        if c.tag == b"SDPT" and len(c.data) >= 0x2C:
            kind = struct.unpack_from("<H", c.data, 4)[0]
            x, _, z = struct.unpack_from("<3f", c.data, 0x10)
            if kind == 0x46BE:
                out.append((x, z, None))
    return out


def write_png(path, width, height, rows):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    raw = b"".join(b"\0" + bytes(r) for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


# ---- the file ----------------------------------------------------------------

def rle565(rows, width):
    """Rows of RGB bytes as runs of RGB565 {u16 run, u16 colour}."""
    out = bytearray()
    for row in rows:
        colours = [((row[i] & 0xF8) << 8) | ((row[i + 1] & 0xFC) << 3) | (row[i + 2] >> 3) for i in range(0, width * 3, 3)]
        x = 0
        while x < width:
            c = colours[x]
            run = x + 1
            while run < width and colours[run] == c and run - x < 0xFFFF:
                run += 1
            out += struct.pack("<HH", run - x, c)
            x = run
    return bytes(out)


def chunk(tag, data):
    return tag + struct.pack("<I", len(data)) + data + b"\0" * (-len(data) % 4)


def object_places(ins, dat):
    """{name hash: (x, z)} of the instances and data points by name, their
    names where known and their heights."""
    places = {}
    heights = {}
    instances, place = al.instance_placer(ins)
    names = {}
    for d in instances:
        name = d[0x50:].split(b"\0")[0].decode("latin1")
        key = al.name_hash(name)
        names[key] = name
        x, y, z = place(d, (0.0, 0.0, 0.0))
        places.setdefault(key, (x, z))
        heights.setdefault(key, y)
    for c in al.walk(al.parse(dat)):
        if c.tag == b"SDPT" and len(c.data) >= 0x2C:
            key = struct.unpack_from("<I", c.data, 8)[0]
            x, y, z = struct.unpack_from("<3f", c.data, 0x10)
            places.setdefault(key, (x, z))
            heights.setdefault(key, y)
    return places, names, heights


def play_seeds(places, names, heights):
    """Where the level puts things for the skater (x, z, y): what lies on a
    ledge nothing rolls up to is in play too. Terrain's origins say nothing,
    and the script's holders sit out of the way."""
    out = []
    for key, (x, z) in places.items():
        name = names.get(key, "").lower()
        if ("prop" in name or "script" in name or "npc" in name) and "helper" not in name and "terrain" not in name:
            out.append((x, z, heights[key]))
    return out


def script_key(script, goals):
    """SCRP: which script a goal set is for, as the game has it loaded: its
    variables' bytes (ScriptVm::variablesBytes) and each objective's name and
    description (Objective +0x18, +0x1C: offsets in the script's strings).
    The PS2 disc's script and the Xbox release's (the career scripts
    setting) differ in one or the other wherever their goals do."""
    return chunk(b"SCRP", struct.pack("<II", len(script.variables) * 4, len(goals)) +
                 b"".join(struct.pack("<II", g["title_at"], g["text_at"]) for g in goals))


def goal_chunks(script, places, names):
    """SCRP, GOAL, TARG, GIVE and TEXT for one level script."""
    goals = pg.analyse(script)
    text = bytearray(b"\0")

    def string(s):
        at = len(text)
        text.extend(s.encode("latin1") + b"\0")
        return at

    goal_data = b""
    target_data = b""
    target_count = 0
    nan = float("nan")
    stats = collections.Counter()
    for g in goals:
        first = target_count
        anchor = (nan, nan)
        for hash_, done in g["targets"].items():
            if "helper" in names.get(hash_, "").lower() or hash_ not in places:
                continue            # an out-of-the-way script holder, not a place
            word, value = done if done else (-1, 0)
            target_data += struct.pack("<IffiII", hash_, places[hash_][0], places[hash_][1], word, value, TARGET_PLACE)
            target_count += 1
        owner = g["owner"]
        if target_count == first and owner in places and "helper" not in names.get(owner, "").lower():
            anchor = places[owner]
        word, target = g["counter"] if g["counter"] else (-1, 0)
        score_kind, score_target = g["score"] if g["score"] else (0, 0)
        goal_data += struct.pack("<IIIiiIIBBBBffBBHiIiI", string(g["title"]), string(g["text"]), owner, word, target,
                                 first, target_count - first, g["type"], g["tier"], g["locked"], g["points"],
                                 anchor[0], anchor[1], score_kind, 0, 0, score_target, 0, -1, 0)
        stats["goals"] += 1
        stats["placed"] += 1 if target_count > first or anchor[0] == anchor[0] else 0
        stats["counted"] += 1 if g["counter"] or g["score"] else 0
    data = script_key(script, goals) + chunk(b"GOAL", struct.pack("<I", len(goals)) + goal_data)
    data += chunk(b"TARG", struct.pack("<I", target_count) + target_data)
    givers = pg.bubbles(script)
    data += chunk(b"GIVE", struct.pack("<I", len(givers)) + b"".join(
        struct.pack("<IffiI", npc, places.get(npc, (0.0, 0.0))[0], places.get(npc, (0.0, 0.0))[1], word, state)
        for npc, word, state in givers))
    data += chunk(b"TEXT", bytes(text))
    return data, stats, target_count


class FolderLevel:
    """A level that is not on the disc: <folder>/<stem>.<EXT>, as packaged."""

    def __init__(self, folder, stem):
        self.folder, self.stem = folder, stem

    def read(self, ext):
        path = os.path.join(self.folder, "%s.%s" % (self.stem, ext))
        if not os.path.isfile(path):
            return None
        with open(path, "rb") as f:
            return f.read()


def write_level(out, level, title, frame, rows, extra=b""):
    """<level>.pmap: the name, the picture with its transform, and the goals' chunks (extra)."""
    file_data = b"PMAP" + struct.pack("<I", FILE_VERSION)
    file_data += chunk(b"NAME", title.encode("latin1") + b"\0")
    file_data += chunk(b"MAPI", struct.pack("<IIfffI", frame.width, frame.height, frame.world_left(), frame.world_top(),
                                            frame.ppu, 1 if frame.turned else 0) + rle565(rows, frame.width))
    file_data += extra
    with open(os.path.join(out, level + ".pmap"), "wb") as f:
        f.write(file_data)
    return len(file_data)


def build_level(read, level, title, out, png, xbox_script=None):
    """`level` the file's name, `title` the name the panel shows; with
    `xbox_script` (the Xbox release's .BIN of the level, which the career
    scripts setting runs) a second goal set for it."""
    t0 = time.time()
    geo, ins, txr = read("GEO"), read("INS"), read("TXR")
    tris, barriers = world_triangles(geo, ins, texture_means(txr))
    box = bounds_of(tris, spawn_points(read("DAT")))
    ppu = min(PIXELS_PER_UNIT, MAX_SIDE / max(box[1] - box[0], box[3] - box[2]))
    frame = Frame(box, ppu)
    width, height = frame.width, frame.height
    rows, heights, per_unit, ymax = rasterise(tris, frame)
    places, names, heights_of = object_places(ins, read("DAT"))
    seeds = spawn_points(read("DAT")) + play_seeds(places, names, heights_of)
    grid, seen, across, down, step = reachable(frame, heights, per_unit, ymax, seeds, barriers)
    in_play = mute_out_of_play(rows, grid, seen, across, down, step)
    if PLAY_CHECK is not None:
        PLAY_CHECK.append((level, frame, seen, step))
    fill_gaps(width, height, rows, heights, ppu)
    outline(width, height, rows, heights, ppu)
    rails = al.grind_edges(geo, ins)
    draw_rails(rows, frame, rails)
    say("%s: %d faces, box x %.0f .. %.0f z %.0f .. %.0f, %dx%d%s, %.0f%% in play, %.1f s" % (
        level, len(tris), box[0], box[1], box[2], box[3], width, height, " turned" if frame.turned else "",
        in_play * 100, time.time() - t0))
    if png:
        os.makedirs(png, exist_ok=True)
        write_png(os.path.join(png, level + ".png"), width, height, rows)
    stats, targets = collections.Counter(), 0
    script = read("BIN")
    goals = b""
    if script:
        goals, stats, targets = goal_chunks(pg.Script(script), places, names)
    if script and xbox_script and xbox_script != script:
        more, xstats, xtargets = goal_chunks(pg.Script(xbox_script), places, names)
        goals += more
        say("%s: the Xbox script's goals too: %d counted, %d targets" % (level, xstats["counted"], xtargets))
    size = write_level(out, level, title, frame, rows, goals)
    say("%s: file %.1f MB, %d goals, %d placed, %d counted, %d targets, %.1f s" % (
        level, size / 1e6, stats["goals"], stats["placed"], stats["counted"], targets, time.time() - t0))


# ---- Dave Mirra BMX 2's levels ------------------------------------------------

# The level select's folders by menu value (Root.CurrentLevel), and their
# names in the string table (KSTR_LEVELNAMEMENU_<value>).
DM2_LEVELS = ("WOODWARD", "TRAINYAR", "SWAMPYTR", "COMMERCI", "GALLOON", "CLOVERLE", "DEVILSPE", "AIRPORT")
# DM2's map has no textures to average: its ridden ground in one colour,
# shaded by slope and height as AI's.
DM2_GROUND = (150, 146, 138)


def dm2_strings(data):
    """DM2's string table (STRING.EUS): {name_hash(key): text}, a count, then (hash, offset) pairs."""
    count = struct.unpack_from("<I", data, 4)[0]
    out = {}
    for i in range(count):
        key, at = struct.unpack_from("<II", data, 8 + 8 * i)
        end = data.find(b"\0", at)
        if 0 <= at < len(data) and end > at:
            out[key] = data[at:end].decode("latin1")
    return out


def dm2_triangles(geo, ins):
    """(y top, a, b, c, normal, colour, water) of every face DM2's collision
    tree lists, facing up (dm2_level.collision_triangles: what the bike rides)."""
    tris = []
    for group in dl.collision_triangles(geo, ins):
        for a, b, c in group[4]:
            ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
            vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
            nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
            length = (nx * nx + ny * ny + nz * nz) ** 0.5
            if length == 0:
                continue
            if ny > 0:                     # +y is down: the face's upward normal
                nx, ny, nz = -nx, -ny, -nz
            tris.append((min(a[1], b[1], c[1]), a, b, c, (nx / length, ny / length, nz / length), DM2_GROUND, False))
    return tris


def build_dm2_level(read, level, title, out, png, goals=b"", goal_count=0, capitals=True):
    """A DM2 level's map: its collision faces from above, the level proper
    the densest stretch of them (DM2 keeps no start points in its DAT), walls
    filled and outlined as AI's; and its goals' chunks. BMX XXX's levels are
    the same format (build_bmx): their titles keep their case (capitals off)."""
    t0 = time.time()
    tris = dm2_triangles(read("GEO"), read("INS"))
    box = bounds_of(tris, [])
    ppu = min(PIXELS_PER_UNIT, MAX_SIDE / max(box[1] - box[0], box[3] - box[2]))
    frame = Frame(box, ppu)
    rows, heights, _, _ = rasterise(tris, frame)
    fill_gaps(frame.width, frame.height, rows, heights, ppu)
    outline(frame.width, frame.height, rows, heights, ppu)
    if png:
        os.makedirs(png, exist_ok=True)
        write_png(os.path.join(png, level + ".png"), frame.width, frame.height, rows)
    # In capitals, as DM2's menus show level names (its fonts have no small letters).
    size = write_level(out, level, title.upper() if capitals else title, frame, rows, goals)
    say("%s (%s): %d faces, box x %.0f .. %.0f z %.0f .. %.0f, %dx%d%s, %d goals, file %.1f MB, %.1f s" % (
        level, title, len(tris), box[0], box[1], box[2], box[3], frame.width, frame.height,
        " turned" if frame.turned else "", goal_count, size / 1e6, time.time() - t0))


# DM2's ProQuest goals (game/dm2/game/goals.h GoalDefinition: eight game
# levels of five challenges of four goals, 0x160 bytes each; +0 the objective
# count, +4 the respect it earns, +0x20 four objectives of 0x50: a kind, four
# words), the game level of each level menu entry (g_levelMenuGameLevels),
# and the challenges' names in the goals' text keys
# ("KSTRSP_GOAL_LEV%02d_%s_%d"; the rookie challenge's one goal
# "KSTRSP_GOAL_LEV%02d_ROOKIE") and on the objectives screen.
DM2_GOALS, DM2_GOAL_BYTES, DM2_GAME_LEVEL_BYTES = 0x00233900, 0x160, 0x1B80
DM2_MENU_GAME_LEVELS = 0x0025A1C8
DM2_CHALLENGE_KEYS = ("ROOKIE", "EASY", "HARD", "XTRA", "EXPERT")
DM2_CHALLENGE_TITLES = ("ROOKIE", "AMATEUR", "PRO", "HARDCORE", "EXPERT")  # KSTRSP_OBJCTV_<title>


def dm2_goal_type(kinds, words):
    """The panel's look of a goal (GoalRecord type) by its objectives' kinds
    (goals.h ObjectiveKind): a record of a run's points, grinds, transfers
    and jumps off objects, else other."""
    if kinds and kinds[0] == 22 and words[0][0] == 1:
        return 1
    if any(k in (8, 9, 18) for k in kinds):
        return 5
    if any(14 <= k <= 21 or k in (11, 12) for k in kinds):
        return 7
    return 0


# Where an objective's names are (game/dm2/game/goals.cpp findObjectiveTargets),
# by its kind: the words holding them, and what each names (a tagged face, a
# tagged edge, an object). Kinds 0, 4, 5 and 22 are anywhere.
DM2_OBJECTIVE_NAMES = {1: (3,), 2: (3,), 3: (3,), 6: (1,), 7: (1,), 8: (0,), 9: (0,), 10: (0,), 11: (0,), 12: (0,),
                       13: (0,), 23: (0,), 14: (0, 1), 15: (0, 1), 16: (0, 1), 17: (0, 1), 18: (0, 1), 19: (0, 1),
                       20: (0, 1), 21: (0, 1)}
DM2_OBJECTIVE_TARGETS = {1: ("face",), 6: ("face",), 10: ("face",), 14: ("face", "face"), 15: ("face", "face"),
                         16: ("face", "face"), 17: ("face", "face"), 3: ("edge",), 8: ("edge",), 18: ("edge", "edge"),
                         2: ("object",), 7: ("object",), 9: ("object",), 11: ("object",), 12: ("object",),
                         13: ("object",), 23: ("object",), 19: ("object", "object"), 20: ("face", "edge"),
                         21: ("face", "object")}
# The panel's script words past the twenty goals' counts: an objective met
# (dave_mirra_panel.cpp), four a goal.
DM2_OBJECTIVE_MET_WORDS = 20
# The targets an objective keeps of each name (goals.h kObjectiveTargetsMost);
# an edge gives two points (its ends).
DM2_OBJECTIVE_TARGETS_MOST = 128
# Targets this close (world units, on the map) are one place: the faces of one
# ramp, the ends of one rail's edges.
DM2_PLACE_REACH = 12.0


class Dm2ElfGoals:
    """A level's goal definitions in the executable (DM2_GOALS): words by
    offset in the level's 0x1B80 bytes, a name field's string."""

    def __init__(self, elf, game_level):
        self.elf, self.base = elf, DM2_GOALS + game_level * DM2_GAME_LEVEL_BYTES

    def word(self, offset):
        return self.elf.word(self.base + offset)

    def name(self, offset):
        pointer = self.word(offset)
        return self.elf.text(pointer).decode("latin1") if pointer else None


class Dm2CareerGoals:
    """An Xbox level's goal definitions as dm2_xbox_career.py writes them
    (CAREER.BIN's goals block: the same 0x1B80 bytes, its names as
    relocations to strings)."""

    def __init__(self, body, relocations, strings):
        self.body = body
        self.names = {offset: strings[index] for offset, index, _ in relocations}

    def word(self, offset):
        return struct.unpack_from("<I", self.body, offset)[0]

    def name(self, offset):
        return self.names.get(offset)


def dm2_objective_places(goals, at, kind, places):
    """(first name's hash, [(x, z)]) of an objective whose definition is at
    `at` in `goals`: the targets the game finds for it as its challenge
    starts (its names' faces, edges or objects), joined into places."""
    points, first = [], 0
    for i, word in enumerate(DM2_OBJECTIVE_NAMES.get(kind, ())):
        name = goals.name(at + 4 + 4 * word)
        if not name:
            continue
        key = dl.name_hash(name)
        first = first or key
        found = DM2_OBJECTIVE_TARGETS[kind][i]
        # An objective keeps the first 128 of a name's targets (findObjectiveTargets' lists), in the level's order.
        points += places.get(key, {}).get(found, [])[:DM2_OBJECTIVE_TARGETS_MOST * (2 if found == "edge" else 1)]
    return first, join_places(points)


def join_places(points):
    """World points (x, y, z) joined into places (x, z): points within
    DM2_PLACE_REACH of one of a group's are its, each group its mean."""
    groups = []
    for p in points:
        near = [g for g in groups if any((p[0] - q[0]) ** 2 + (p[2] - q[2]) ** 2 <= DM2_PLACE_REACH ** 2 for q in g)]
        joined = [p]
        for g in near:
            joined += g
            groups.remove(g)
        groups.append(joined)
    return [(sum(q[0] for q in g) / len(g), sum(q[2] for q in g) / len(g)) for g in groups]


def dm2_goal_chunks(goals, names, game_level, places=None):
    """GOAL, TARG, GIVE and TEXT of a DM2 level: twenty goals, challenge by
    challenge (as dave_mirra_panel.cpp reads their states), a slot with no
    goal hidden (tier 3, never done); a goal of several objectives counts
    them (script word = its slot: the objectives met), the rookie
    challenge's run's points against the rider's score. Its title is the
    goal's text, its text that again with its challenge and respect. Its
    targets are its objectives' places (`places`: dm2_level.goal_places of
    the level), each done as its objective is met. `goals`: Dm2ElfGoals or
    Dm2CareerGoals; `game_level` the number in the texts' keys."""
    text = bytearray(b"\0")
    targets = b""
    target_count = 0

    def string(s):
        at = len(text)
        text.extend(s.encode("latin1") + b"\0")
        return at

    nan = float("nan")
    data = b""
    shown = placed = 0
    for c, key in enumerate(DM2_CHALLENGE_KEYS):
        challenge = names.get(dl.name_hash("KSTRSP_OBJCTV_" + DM2_CHALLENGE_TITLES[c]), key.lower() + " challenges:")
        challenge = challenge.rstrip(":").rstrip("s").capitalize()
        for g in range(4):
            slot = c * 4 + g
            at = slot * DM2_GOAL_BYTES
            count = goals.word(at) & 0xFF
            respect = goals.word(at + 4)
            goal_key = "KSTRSP_GOAL_LEV%02d_%s" % (game_level, key) if c == 0 else "KSTRSP_GOAL_LEV%02d_%s_%d" % (game_level, key, g)
            words = names.get(dl.name_hash(goal_key)) if count else None
            if not words or not 1 <= count <= 4:
                data += struct.pack("<IIIiiIIBBBBffBBHiIiI", 0, 0, 0, -1, 0, 0, 0, 0, 3, 0, 0, nan, nan, 0, 0, 0, 0, 0, -1, 0)
                continue
            kinds = [goals.word(at + 0x20 + o * 0x50) for o in range(count)]
            objective_words = [[goals.word(at + 0x24 + o * 0x50 + 4 * i) for i in range(4)] for o in range(count)]
            first = target_count
            for o in range(count):
                name, spots = dm2_objective_places(goals, at + 0x20 + o * 0x50, kinds[o], places or {})
                for x, z in spots:
                    targets += struct.pack("<IffiII", name, x, z, DM2_OBJECTIVE_MET_WORDS + slot * 4 + o, 1, TARGET_PLACE)
                    target_count += 1
            placed += 1 if target_count > first else 0
            # In capitals, as DM2's menus and their fonts have them.
            title = " ".join(words.replace("\r\n", " ").replace("\n", " ").split()).upper()
            score_kind, score_target = (1, objective_words[0][1] + 1) if kinds[0] == 22 and objective_words[0][0] == 1 else (0, 0)
            data += struct.pack("<IIIiiIIBBBBffBBHiIiI", string(title), string(("%s (%s, %d respect)" % (title, challenge, respect)).upper()),
                                0, slot if count > 1 else -1, count, first, target_count - first,
                                dm2_goal_type(kinds, objective_words), 0, 0, 0, nan, nan, score_kind, 0, 0, score_target, 0, -1, 0)
            shown += 1
    out = chunk(b"GOAL", struct.pack("<I", 20) + data)
    out += chunk(b"TARG", struct.pack("<I", target_count) + targets)
    out += chunk(b"GIVE", struct.pack("<I", 0)) + chunk(b"TEXT", bytes(text))
    return out, shown, placed, target_count


# DM2's moves (game/dm2/overlay/screen_trick_list.cpp): 0x48 tricks, then 0x56
# modifiers (a modifier's move number is its number + 0x48), each a name and
# its buttons in ButtonLib's glyphs ("CGE 2": A-H the stick's directions from
# up clockwise, ? any, then the button), their kinds (u16; 8: a move the
# characters start without, a trick of it a signature move); and the trick
# list's categories by trick number, in its order, as the panel's six
# families (their titles shortened to fit its buttons).
DM2_TRICK_NAMES, DM2_MODIFIER_NAMES = 0x00245D38, 0x00245E58
DM2_TRICK_BUTTONS, DM2_MODIFIER_BUTTONS = 0x00245FD8, 0x002460F8
DM2_MOVE_KINDS = 0x00245520
DM2_TRICKS, DM2_MODIFIERS, DM2_FIRST_MODIFIER = 0x48, 0x56, 0x48
DM2_MOVE_OFF_AT_FIRST = 0x8
DM2_FAMILIES = ("AIR", "MODIFIERS", "STALLS", "GRINDS", "OTHERS", "SIGNATURE")
DM2_CATEGORIES = ((0, 0x00, 0x32), (2, 0x3C, 0x42), (3, 0x33, 0x3B), (4, 0x43, 0x47))  # family, first, last trick
DM2_DIRECTIONS = "ABCDEFGH"
# ButtonLib's directions as the panel's trick table asks for them (panel.cpp
# kDirections: AI's fnt_ps2buttons characters for up, then clockwise).
DM2_DIRECTION_GLYPHS = {ord(d): ord(g) for d, g in zip(DM2_DIRECTIONS, ",]>'.;<[")}


def dm2_trick_record(family, buttons, name):
    """A TRIK row of a move's buttons text: its directions (1 up, then clockwise, 9 any) and its button."""
    text = buttons.decode("latin1").strip()
    stick, _, press = text.partition(" ")
    if stick.isdigit() and not press:
        stick, press = "", stick
    inputs = bytes(DM2_DIRECTIONS.index(d) + 1 if d in DM2_DIRECTIONS else 9 for d in stick)[:4]
    return trick_record(family, 0, inputs, ord(press[0]) if press else 0, name)


def dm2_trick_chunks(elf):
    """TRIK, MOVE and FAMS: the trick list as the pause menu's has it."""
    half = lambda address: (elf.word(address & ~3) >> (8 * (address & 2))) & 0xFFFF
    kind = lambda move: half(DM2_MOVE_KINDS + 2 * move)
    rows, moves = [], []
    def add(family, move, names, buttons, index, while_able=0):
        rows.append(dm2_trick_record(family, elf.text(elf.word(buttons + 4 * index)), elf.text(elf.word(names + 4 * index)).upper()))
        moves.append(struct.pack("<BBH", move, while_able, 0))
    for family, first, last in DM2_CATEGORIES[:1]:
        for trick in range(first, last + 1):
            if not kind(trick) & DM2_MOVE_OFF_AT_FIRST:
                add(family, trick, DM2_TRICK_NAMES, DM2_TRICK_BUTTONS, trick)
    for modifier in range(DM2_MODIFIERS):
        if not kind(modifier + DM2_FIRST_MODIFIER) & DM2_MOVE_OFF_AT_FIRST:
            add(1, modifier + DM2_FIRST_MODIFIER, DM2_MODIFIER_NAMES, DM2_MODIFIER_BUTTONS, modifier)
    for family, first, last in DM2_CATEGORIES[1:]:
        for trick in range(first, last + 1):
            if not kind(trick) & DM2_MOVE_OFF_AT_FIRST:
                add(family, trick, DM2_TRICK_NAMES, DM2_TRICK_BUTTONS, trick)
    for trick in range(DM2_TRICKS):
        if kind(trick) & DM2_MOVE_OFF_AT_FIRST:
            add(5, trick, DM2_TRICK_NAMES, DM2_TRICK_BUTTONS, trick, 1)
    out = chunk(b"TRIK", struct.pack("<I", len(rows)) + b"".join(rows))
    out += chunk(b"MOVE", struct.pack("<I", len(moves)) + b"".join(moves))
    out += chunk(b"FAMS", struct.pack("<I", len(DM2_FAMILIES)) + b"".join(n.encode().ljust(16, b"\0") for n in DM2_FAMILIES))
    return out, len(rows)


# The panel's three fonts (panel.cpp: body, title, numbers) from DM2's own UI
# library: its menus' small text, its overlay's headings and its scores; and
# ButtonLib's PlayStation font for the trick table's directions and buttons.
DM2_UI_FONTS = {"mnu_small": "fnt_14pt", "ovr_sm3": "fnt_e_12pt", "ovr_sm": "fnt_ovr_score", "ps2_btn": "fnt_ps2buttons"}


def build_dm2_ui(disc, elf, out):
    """UI.pmap for DM2: its fonts under the names the panel asks for, and its
    trick list (no icons: DM2's are not read yet)."""
    tricks, rows = dm2_trick_chunks(elf)
    data = b"PMAP" + struct.pack("<I", FILE_VERSION) + tricks
    found = set()
    for entry in zdir(disc.read("ASSETS/UILIB.ZAL")):
        if entry[:4] != b"ULIB":
            continue
        for u in al.parse(entry)[0].kids:
            if u.tag != b"UAST":
                continue
            hast = next(k for k in u.kids if k.tag == b"HAST").data
            name = hast[128:160].split(b"\0")[0].decode("latin1")
            if name == "button":    # the three ButtonLib fonts, by their source file
                name = re.split(r"[\\/]", hast[:128].split(b"\0")[0].decode("latin1"))[-1].rsplit(".", 1)[0]
            for k in u.kids:
                if k.tag == b"UFNT" and name in DM2_UI_FONTS and name not in found:
                    found.add(name)
                    hfnt = next(c for c in k.kids if c.tag == b"HFNT").data
                    line, _, glyphs, pages = struct.unpack_from("<BBHH", hfnt, 0)
                    body = struct.pack("<III", line, glyphs, pages)
                    for t in (c for c in k.kids if c.tag == b"TXRC"):
                        w, h, rgba = bmp_rgba(picture_of(t))
                        body += struct.pack("<II", w, h) + rgba
                    glyph_records = bytearray(hfnt[6 + 40 * pages:6 + 40 * pages + 12 * glyphs])
                    if name == "ps2_btn":
                        for g in range(glyphs):
                            glyph_records[12 * g] = DM2_DIRECTION_GLYPHS.get(glyph_records[12 * g], glyph_records[12 * g])
                    body += bytes(glyph_records)
                    data += chunk(b"FONT", DM2_UI_FONTS[name].encode("latin1").ljust(32, b"\0") + body)
    with open(os.path.join(out, "UI.pmap"), "wb") as f:
        f.write(data)
    missing = [n for n in DM2_UI_FONTS if n not in found]
    say("UI: %.1f MB, %d fonts, %d tricks%s" % (len(data) / 1e6, len(found), rows,
                                              ", missing " + " ".join(missing) if missing else ""))


def build_dm2(a):
    disc = al.Disc(a.disc)
    os.makedirs(a.out, exist_ok=True)
    elf = Elf(disc.read("SLUS_201.59"))
    build_dm2_ui(disc, elf, a.out)
    names = dm2_strings(disc.read("ASSETS/STRING.EUS"))
    for value, level in enumerate(DM2_LEVELS):
        if a.level and level not in a.level:
            continue
        title = names.get(dl.name_hash("KSTR_LEVELNAMEMENU_%d" % value), level.title())
        game_level = elf.word(DM2_MENU_GAME_LEVELS + value & ~3) >> (8 * ((DM2_MENU_GAME_LEVELS + value) & 3)) & 0xFF
        read = lambda ext, level=level: disc.read("ASSETS/LEVELS/%s/%s.%s" % (level, level, ext))
        goals, goal_count, placed, targets = dm2_goal_chunks(Dm2ElfGoals(elf, game_level), names, game_level,
                                                             dl.goal_places(read("GEO"), read("INS")))
        say("%s: %d goals, %d placed, %d targets" % (level, goal_count, placed, targets))
        build_dm2_level(read, level, title, a.out, a.png, goals, goal_count)
    if a.dm2_xbox:
        import dm2_xbox  # noqa: E402  (its levels' folders and names)
        careers = dm2_xbox_careers(a.dm2_xbox)
        for folder, (_, title) in dm2_xbox.LEVELS.items():
            if os.path.isdir(os.path.join(a.dm2_xbox, folder)) and (not a.level or folder in a.level):
                read = FolderLevel(os.path.join(a.dm2_xbox, folder), folder).read
                goals, goal_count = b"", 0
                if folder in careers:
                    table, texts = careers[folder]
                    goals, goal_count, placed, targets = dm2_goal_chunks(
                        table, {**names, **texts}, DM2_XBOX_SLOT, dl.goal_places(read("GEO"), read("INS")))
                    say("%s: %d goals, %d placed, %d targets" % (folder, goal_count, placed, targets))
                build_dm2_level(read, folder, title, a.out, a.png, goals, goal_count)


# The Xbox levels run in Woodward Camp's slot (game level 2): their goals'
# texts are under that level's keys (dave_mirra_xcareer.cpp).
DM2_XBOX_SLOT = 2


def dm2_xbox_careers(folder):
    """{level folder: (Dm2CareerGoals, {key hash: English text})} of the Xbox
    levels' career (<folder>/CAREER.BIN, dm2_xbox_career.py's); {} without it."""
    path = os.path.join(folder, "CAREER.BIN")
    if not os.path.isfile(path):
        return {}
    import dm2_xbox_career as dc  # noqa: E402
    with open(path, "rb") as f:
        levels, strings, texts, _ = dc.read(f.read())
    out = {}
    for name, level, blocks in levels:
        table = next((Dm2CareerGoals(body, rel, strings) for kind, body, rel in blocks if kind == dc.GOALS), None)
        if table:
            out[name] = (table, {dl.name_hash(key): text.decode("latin1")
                                 for lang, at, key, text in texts if lang == 0 and at == level and text})
    return out


# ---- BMX XXX's levels ---------------------------------------------------------

# BMX XXX's levels are DM2's format (ASSETS/LEVELS/<FOLDER>/<FOLDER>.GEO, .INS,
# .DAT; dm2_level.py reads them, most of their parts without face normals),
# with the level's challenges in its own OBJCTV.TXT (the engine's objectives
# script: game/zge/scene/objectives.h, game/bmx/objectives). The level table
# in its executable (game/bmx/profile/players.h
# LevelInfo, g_levels: 0x10 bytes a row, +0 the two-letter code, +4 the
# folder, +8 the title) lists the disc's eight levels first, in the level
# select's order; the panel's capture (bmx_xxx_panel.cpp) reads the same
# table's folder for the level being ridden, so the files are named by folder.
BMX_ELF = "SLUS_204.15"
BMX_LEVELS, BMX_LEVEL_BYTES, BMX_LEVEL_ROWS = 0x00258A98, 0x10, 8
# Its string tables (ASSETS/STRING.ZAL, a ZDIR archive of tables in DM2's
# STRING.EUS format; game/bmx/ui/strings.cpp): the common one, and one a
# level by the level's code, which the overlay loads with the level
# (overlay/screens.cpp openOverlay); American English.
BMX_COMMON_STRINGS, BMX_LEVEL_STRINGS, BMX_LANGUAGE = "string.common.%s", "string.level.%s.%s", "eus"
# A goal's texts' keys, after its objective's STRINGID (overlay/
# screen_objectives.cpp): the name (the detail screen's title), the
# description (the list's line once the goal was activated) and what
# activates it (the line while it can be).
BMX_GOAL_TEXT_KEYS = ("%s_NAME", "%s_DESC", "%s_ACTIVATE")


def zdir_named(data):
    """{name hash: entry} of a ZDIR archive: its GRID chunk holds the entries'
    name hashes, in the order of zdir's entries."""
    assert data[:4] == b"ZDIR" and data[8:12] == b"DIDF"
    count = struct.unpack_from("<I", data, 0x10)[0]
    hashes = [struct.unpack_from("<I", data, 0x18 + 8 + 4 * i)[0] for i in range(count)]
    return dict(zip(hashes, zdir(data)))


def bmx_objectives(text):
    """A level's OBJCTV.TXT read: its objectives in the file's order, each
    {"name", "type", "persistent", "child", "stringid", "params"}, params the
    PARAMETERS block as {KEY: "value", BLOCK: {...}} (a SET's children are
    its OBJECTIVE<n> values). The file is lines of KEY "value" between
    <BLOCK>_BEGIN and <BLOCK>_END, with C and C++ comments."""
    source = re.sub(rb"/\*.*?\*/", b" ", text, flags=re.S)
    source = re.sub(rb"//[^\n]*", b" ", source)
    tokens = re.findall(rb'"[^"]*"|\S+', source)
    root = {}
    stack = [root]
    key = None
    for token in tokens:
        word = token.decode("latin1")
        if word.startswith('"'):
            if key is not None:
                stack[-1][key] = word[1:-1]
                key = None
        elif word.endswith("_BEGIN"):
            block = {}
            name = word[:-6]
            # a file holds many OBJECTIVE blocks; a PARAMETERS block holds one of each kind
            stack[-1].setdefault(name, []).append(block) if name == "OBJECTIVE" else stack[-1].__setitem__(name, block)
            stack.append(block)
            key = None
        elif word.endswith("_END"):
            if len(stack) > 1:
                stack.pop()
            key = None
        else:
            key = word
    out = []
    for block in root.get("OBJECTIVE", []):
        out.append({"name": block.get("NAME", ""), "type": block.get("TYPE", "").upper(),
                    "persistent": block.get("PERSISTENT", "NO").upper() == "YES",
                    # The engine's default: an objective is a set's child unless its
                    # block says IS_CHILD "NO" (the 20 goals of a challenge level say
                    # so; a set's members and the helpers say nothing).
                    "child": block.get("IS_CHILD", "YES").upper() != "NO",
                    "stringid": block.get("STRINGID", ""), "params": block.get("PARAMETERS", {})})
    return out


# What an objective's parameters name in the level (game/bmx/objectives, the
# types' readers): a transfer's faces and edges, a grind's edges, a collect's
# pickups (instances), a proximity's face or entity; a set's children lend it
# their places. Keys are numbered (ITEM1, EDGE2: the digits are dropped).
# TargetRecord.role (panel.h TargetRole): what the skater does at a target.
TARGET_PICKUP, TARGET_DROP_OFF, TARGET_PLACE = 0, 1, 2
BMX_PLACE_KEYS = {("TRANSFER", "FROM_FACE"): "face", ("TRANSFER", "TO_FACE"): "face",
                  ("TRANSFER", "FROM_EDGE"): "edge", ("TRANSFER", "TO_EDGE"): "edge",
                  ("GRIND", "EDGE"): "edge", ("COLLECT", "ITEM"): "object"}
# The panel's look of a goal (GoalRecord type) by its objective's type.
BMX_GOAL_TYPES = {"TRANSFER": 7, "GRIND": 5, "POINTS": 1}
# A goal's objective type whose places are its children's.
BMX_SET_DEPTH_MOST = 8


def bmx_target_role(kind, name):
    """A target's role from the level's own naming: a face or an edge is a
    place to ride, an object named as a checkpoint ("checkpoint_can01",
    "CheckPoint_junkie01") is where to bring things, any other a pickup."""
    if kind != "object":
        return TARGET_PLACE
    return TARGET_DROP_OFF if "checkpoint" in name.lower() or "chkpt" in name.lower() else TARGET_PICKUP


def bmx_objective_places(objective, by_name, places, depth=0):
    """(first name's hash, [(role, name hash, world point)]) an objective's
    parameters name, through a set's children."""
    first, points = 0, []
    for block, body in objective["params"].items():
        if not isinstance(body, dict):
            continue
        for key, value in body.items():
            if not isinstance(value, str):
                continue
            base = key.rstrip("0123456789")
            if block == "SET" and base == "OBJECTIVE":
                child = by_name.get(value)
                if child and depth < BMX_SET_DEPTH_MOST:
                    child_first, child_points = bmx_objective_places(child, by_name, places, depth + 1)
                    first = first or child_first
                    points += child_points
                continue
            kind = BMX_PLACE_KEYS.get((block, base))
            if block == "PROXIMITY" and base == "NAME":
                kind = "face" if body.get("TYPE", "").lower() == "face" else "object"
            if kind:
                key_hash = dl.name_hash(value)
                first = first or key_hash
                points += [(bmx_target_role(kind, value), key_hash, p) for p in places.get(key_hash, {}).get(kind, [])]
    return first, points


def bmx_goal_type(objective, by_name):
    """A goal's look: its type's, a set's its first child's, a record of a
    run's score a points goal."""
    kind = objective["type"]
    if kind == "SET":
        children = [by_name.get(v) for k, v in objective["params"].get("SET", {}).items() if k.startswith("OBJECTIVE")]
        return bmx_goal_type(children[0], by_name) if children and children[0] else 0
    if kind == "RECORD":
        return 1 if objective["params"].get("RECORD", {}).get("RECORDTYPE", "") == "SCORE_RUN" else 0
    return BMX_GOAL_TYPES.get(kind, 0)


# The level's people (its DAT's SDPT records of this class; the goal giver's
# record carries its goal's name: playbot/bmxgoals.py givers).
BMX_PERSON_CLASS = 0x0422B9CE


def bmx_givers(dat, goal_names):
    """[(goal name, x, z)]: the people who hold a goal's name, where the
    level puts them (the ones the rider rides up to and talks to)."""
    out = []
    if not dat:
        return out
    for c in al.walk(al.parse(dat)):
        if c.tag != b"SDPT" or len(c.data) < 0x2C or struct.unpack_from("<I", c.data, 4)[0] != BMX_PERSON_CLASS:
            continue
        x, _, z = struct.unpack_from("<3f", c.data, 0x10)
        words = [w.decode("latin1") for w in re.findall(rb"[ -~]{4,}", c.data)]
        named = [g for w in words for g in goal_names if w.endswith(g)]
        if named:
            out.append((max(named, key=len), x, z))
    return out


def bmx_listed(objectives):
    """The objectives the game's CHALLENGES list shows (overlay/
    screen_objectives.cpp addObjectiveLine: every objective that is not a
    set's child, in the order the engine keeps them, the file's), in that
    order: a child is every objective but the ones whose block says
    IS_CHILD "NO" (the engine's default; checked against the engine's pool in
    the Bronx: its 20 records without the IS_CHILD flag are the 20 blocks with
    the line, and a SET named only by messages, Rent_a_Bitch, is a child)."""
    return [o for o in objectives if not o["child"]]


def bmx_goal_chunks(objectives, texts, places, givers=()):
    """GOAL, TARG, GIVE and TEXT of a BMX XXX level: the objectives its
    CHALLENGES list shows (bmx_listed), in the file's order, which is the
    order the engine's objective pool keeps them; bmx_xxx_panel.cpp walks
    that pool in the same order, leaving out the children as the list does,
    and reads each one's state, count and target (script words 2n and
    2n + 1 of goal n: the record's counterWord and targetWord). A goal's
    title is its "_NAME" text, its text its "_DESC" (else "_ACTIVATE") text,
    its activate text its "_ACTIVATE", its status its "_STATUS" when that is
    a count of a target ("Gaps found: %d of %d"), from the level's and the
    common string tables; its targets are the places its parameters name (a
    set's its children's) with their roles, for the goals the capture has no
    live targets of; its givers (GIVE, by the goal's name hash as the
    record's owner, shown while the goal is ready) are the level's people
    who carry its name (`givers`: bmx_givers)."""
    text = bytearray(b"\0")
    targets = b""
    target_count = 0
    counted = lambda t: t is not None and re.findall(r"%[^%]", t) == ["%d", "%d"]

    def string(s):
        at = len(text)
        text.extend(s.encode("latin1", "replace") + b"\0")
        return at

    def named(stringid, suffix):
        return texts.get(dl.name_hash(suffix % stringid)) if stringid else None

    by_name = {o["name"]: o for o in objectives}
    nan = float("nan")
    data = b""
    shown = placed = 0
    for objective in bmx_listed(objectives):
        slot = shown
        name_key, desc_key, activate_key = BMX_GOAL_TEXT_KEYS
        title = named(objective["stringid"], name_key) or objective["name"]
        body = named(objective["stringid"], desc_key) or named(objective["stringid"], activate_key) or title
        clean = lambda s: " ".join(s.replace("\\n", " ").replace("\r\n", " ").replace("\n", " ").split())
        status = named(objective["stringid"], "%s_STATUS")
        activate = named(objective["stringid"], activate_key)
        name, spots = bmx_objective_places(objective, by_name, places)
        first = target_count
        # (each named thing once: a transfer's faces or a grind's edge ends join into one place)
        for role, key in sorted({(r, k) for r, k, _ in spots}):
            for x, z in join_places([p for r, k, p in spots if r == role and k == key]):
                targets += struct.pack("<IffiII", key, x, z, -1, 0, role)
                target_count += 1
        placed += 1 if target_count > first else 0
        goal_name = dl.name_hash(objective["name"])
        data += struct.pack("<IIIiiIIBBBBffBBHiIiI", string(clean(title)), string(clean(body)), goal_name, 2 * slot, 0,
                            first, target_count - first, bmx_goal_type(objective, by_name), 0, 0, 0, nan, nan, 0, 0, 0, 0,
                            string(clean(status)) if counted(status) else 0, 2 * slot + 1, string(clean(activate)) if activate else 0)
        shown += 1
    # A giver's goal: the listed one of that name, else the listed one whose name begins with it.
    listed_names = [o["name"] for o in bmx_listed(objectives)]
    give = b""
    give_count = 0
    for goal, x, z in givers:
        matches = [n for n in listed_names if n == goal] or [n for n in listed_names if n.startswith(goal)]
        if matches:
            give += struct.pack("<IffiI", dl.name_hash(matches[0]), x, z, -1, 0)
            give_count += 1
    out = chunk(b"GOAL", struct.pack("<I", shown) + data)
    out += chunk(b"TARG", struct.pack("<I", target_count) + targets)
    out += chunk(b"GIVE", struct.pack("<I", give_count) + give) + chunk(b"TEXT", bytes(text))
    return out, shown, placed, target_count, give_count


# BMX XXX's moves (game/bmx/ui/trick_list.cpp): 100 tricks, then 0x56
# modifiers, each a name and its buttons in ButtonLib's glyphs (DM2's "G 2");
# the saved game's movesLanded has a bit a trick, the modifiers' from bit 100
# (the panel's move numbers are those bits: bmx_xxx_panel.cpp). The trick
# list's categories list the tricks by number, leaving out those whose
# flags (game/trick_types.h g_trickFlags, a word a trick) say so; its
# SIGNATURE MOVES category lists every trick again, so the panel's signature
# family lists the tricks the four categories leave out, each move once.
BMX_TRICK_NAMES, BMX_MODIFIER_NAMES = 0x00282960, 0x00282AF0
BMX_TRICK_BUTTONS, BMX_MODIFIER_BUTTONS = 0x00282C70, 0x00282E00
BMX_TRICK_FLAGS = 0x00281550
BMX_TRICKS, BMX_MODIFIERS, BMX_FIRST_MODIFIER_BIT = 100, 0x56, 100
BMX_TRICK_NOT_LISTED = 0x8
BMX_FAMILIES = ("AIR", "MODIFIERS", "STALLS", "GRINDS", "OTHERS", "SIGNATURE")
BMX_CATEGORIES = ((0, 0x00, 0x32), (2, 0x3C, 0x42), (3, 0x33, 0x3B), (4, 0x43, 0x63))  # family, first, last trick


def bmx_trick_chunks(elf):
    """TRIK, MOVE and FAMS: the trick list as the pause menu's has it, the
    names in the game's own case (its font has small letters)."""
    rows, moves = [], []
    flags = lambda trick: elf.word(BMX_TRICK_FLAGS + 4 * trick)

    def add(family, move, names, buttons, index):
        rows.append(dm2_trick_record(family, elf.text(elf.word(buttons + 4 * index)), elf.text(elf.word(names + 4 * index))))
        moves.append(struct.pack("<BBH", move, 0, 0))
    for family, first, last in BMX_CATEGORIES[:1]:
        for trick in range(first, last + 1):
            if not flags(trick) & BMX_TRICK_NOT_LISTED:
                add(family, trick, BMX_TRICK_NAMES, BMX_TRICK_BUTTONS, trick)
    for modifier in range(BMX_MODIFIERS):
        add(1, modifier + BMX_FIRST_MODIFIER_BIT, BMX_MODIFIER_NAMES, BMX_MODIFIER_BUTTONS, modifier)
    for family, first, last in BMX_CATEGORIES[1:]:
        for trick in range(first, last + 1):
            if not flags(trick) & BMX_TRICK_NOT_LISTED:
                add(family, trick, BMX_TRICK_NAMES, BMX_TRICK_BUTTONS, trick)
    for trick in range(BMX_TRICKS):
        if flags(trick) & BMX_TRICK_NOT_LISTED:
            add(5, trick, BMX_TRICK_NAMES, BMX_TRICK_BUTTONS, trick)
    out = chunk(b"TRIK", struct.pack("<I", len(rows)) + b"".join(rows))
    out += chunk(b"MOVE", struct.pack("<I", len(moves)) + b"".join(moves))
    out += chunk(b"FAMS", struct.pack("<I", len(BMX_FAMILIES)) + b"".join(n.encode().ljust(16, b"\0") for n in BMX_FAMILIES))
    return out, len(rows)


# The panel's fonts from BMX XXX's UI library (UILIB.ZAL): its menus' typeface
# (font_normal, 35 pixels a line, with small letters) under the panel's three
# names, halved to the panel's size (AI's and DM2's fonts are 15 to 23 a
# line), and the ButtonLib PlayStation font (ps2_btn, DM2's) for the trick
# table's directions and buttons.
BMX_UI_FONTS = {"font_normal": ("fnt_14pt", "fnt_e_12pt", "fnt_ovr_score"), "ps2_btn": ("fnt_ps2buttons",)}
BMX_FONT_HALVED = {"font_normal"}


def halve_rgba(width, height, rgba):
    """An RGBA picture at half size: each pixel the mean of four."""
    w, h = width // 2, height // 2
    out = bytearray(w * h * 4)
    for y in range(h):
        top = rgba[(2 * y) * width * 4:(2 * y + 1) * width * 4]
        bottom = rgba[(2 * y + 1) * width * 4:(2 * y + 2) * width * 4]
        for x in range(w):
            i, o = x * 8, (y * w + x) * 4
            for c in range(4):
                out[o + c] = (top[i + c] + top[i + 4 + c] + bottom[i + c] + bottom[i + 4 + c] + 2) >> 2
    return w, h, bytes(out)


def halve_glyphs(records):
    """Glyph records (character, page, _, x, y, width, height) at half size."""
    out = bytearray()
    for g in range(len(records) // 12):
        character, page, extra, x, y, width, height = struct.unpack_from("<BBHHHHH", records, 12 * g)
        x2, y2 = x // 2, y // 2
        out += struct.pack("<BBHHHHH", character, page, extra, x2, y2,
                           max(1, (x + width + 1) // 2 - x2), max(1, (y + height + 1) // 2 - y2))
    return bytes(out)


def build_bmx_ui(disc, elf, out):
    """UI.pmap for BMX XXX: its fonts under the names the panel asks for, and
    its trick list (no icons)."""
    tricks, rows = bmx_trick_chunks(elf)
    data = b"PMAP" + struct.pack("<I", FILE_VERSION) + tricks
    found = set()
    for entry in zdir(disc.read("ASSETS/UILIB.ZAL")):
        if entry[:4] != b"ULIB":
            continue
        for u in al.parse(entry)[0].kids:
            if u.tag != b"UAST":
                continue
            hast = next(k for k in u.kids if k.tag == b"HAST").data
            name = hast[128:160].split(b"\0")[0].decode("latin1")
            if name == "button":    # the ButtonLib fonts, by their source file
                name = re.split(r"[\\/]", hast[:128].split(b"\0")[0].decode("latin1"))[-1].rsplit(".", 1)[0]
            for k in u.kids:
                if k.tag == b"UFNT" and name in BMX_UI_FONTS and name not in found:
                    found.add(name)
                    hfnt = next(c for c in k.kids if c.tag == b"HFNT").data
                    line, _, glyphs, pages = struct.unpack_from("<BBHH", hfnt, 0)
                    halved = name in BMX_FONT_HALVED
                    body = struct.pack("<III", (line + 1) // 2 if halved else line, glyphs, pages)
                    for t in (c for c in k.kids if c.tag == b"TXRC"):
                        w, h, rgba = bmp_rgba(picture_of(t))
                        if halved:
                            w, h, rgba = halve_rgba(w, h, rgba)
                        body += struct.pack("<II", w, h) + rgba
                    glyph_records = bytearray(hfnt[6 + 40 * pages:6 + 40 * pages + 12 * glyphs])
                    if name == "ps2_btn":
                        for g in range(glyphs):
                            glyph_records[12 * g] = DM2_DIRECTION_GLYPHS.get(glyph_records[12 * g], glyph_records[12 * g])
                    if halved:
                        glyph_records = halve_glyphs(glyph_records)
                    body += bytes(glyph_records)
                    for panel_name in BMX_UI_FONTS[name]:
                        data += chunk(b"FONT", panel_name.encode("latin1").ljust(32, b"\0") + body)
    with open(os.path.join(out, "UI.pmap"), "wb") as f:
        f.write(data)
    missing = [n for n in BMX_UI_FONTS if n not in found]
    say("UI: %.1f MB, %d fonts, %d tricks%s" % (len(data) / 1e6, len(found), rows,
                                              ", missing " + " ".join(missing) if missing else ""))


def build_bmx(a):
    disc = al.Disc(a.disc)
    os.makedirs(a.out, exist_ok=True)
    elf = Elf(disc.read(BMX_ELF))
    build_bmx_ui(disc, elf, a.out)
    tables = zdir_named(disc.read("ASSETS/STRING.ZAL"))
    common = dm2_strings(tables[dl.name_hash(BMX_COMMON_STRINGS % BMX_LANGUAGE)])
    for row in range(BMX_LEVEL_ROWS):
        at = BMX_LEVELS + row * BMX_LEVEL_BYTES
        code, folder, title = (elf.text(elf.word(at + o)).decode("latin1") for o in (0, 4, 8))
        if a.level and folder not in a.level:
            continue
        read = lambda ext, folder=folder: disc.read("ASSETS/LEVELS/%s/%s.%s" % (folder, folder, ext))
        if not read("GEO"):
            say("%s (%s): not on the disc" % (folder, title))
            continue
        level_table = tables.get(dl.name_hash(BMX_LEVEL_STRINGS % (code, BMX_LANGUAGE)))
        texts = {**common, **(dm2_strings(level_table) if level_table else {})}
        objectives = bmx_objectives(disc.read("ASSETS/LEVELS/%s/OBJCTV.TXT" % folder) or b"")
        givers = bmx_givers(read("DAT"), [o["name"].split(".")[0] for o in objectives if o["name"]])
        goals, goal_count, placed, targets, give_count = bmx_goal_chunks(objectives, texts, dl.goal_places(read("GEO"), read("INS")), givers)
        say("%s: %d objectives, %d goals, %d placed, %d targets, %d givers" % (folder, len(objectives), goal_count, placed, targets, give_count))
        build_dm2_level(read, folder, title, a.out, a.png, goals, goal_count, capitals=False)


# ---- the game's fonts and icons ---------------------------------------------

UI_FONTS = ("fnt_ovr_list", "fnt_ovr_obj", "fnt_ovr_score", "fnt_ovr_trick", "fnt_14pt", "fnt_e_12pt", "fnt_ps2buttons")
UI_ICONS = ("AttJump", "AttSpeed", "AttSpin", "AttGrind", "AttManual", "AttFakie", "AttWallride",
            "attrib_circ_full", "attrib_circ_empty", "level_lock", "arrow_up", "arrow_dn", "arrow_lf", "arrow_rt",
            "head_taig", "head_franky", "head_jaren", "head_matt", "head_shane", "head_sven", "head_chris",
            "window_top_lf", "Window_Top_mid", "window_top_rt", "window_side_lf", "window_side_mid",
            "window_btm_lf", "window_btm_mid", "scratch_score", "card",
            "KeyAirfield", "KeyBoardwalk", "KeyCannery", "KeyCivic", "KeyIndustrial", "KeyMovie", "KeyMuseum", "KeyZoo")
for extra in (b"ULIB", b"UAST", b"UMTL", b"UFNT"):
    al.CONTAINERS.add(extra)


def bmp_rgba(data):
    """(width, height, RGBA rows top down) of a game picture: a 4- or 8-bit BMP
    whose palette has PS2 alpha (0 .. 128; all 0 for an opaque TBMP)."""
    off = struct.unpack_from("<I", data, 10)[0]
    head = struct.unpack_from("<I", data, 14)[0]
    width, height, _, bpp = struct.unpack_from("<iiHH", data, 18)
    colours = struct.unpack_from("<I", data, 46)[0] or (1 << bpp)
    palette = data[14 + head:14 + head + colours * 4]
    opaque = not any(palette[i * 4 + 3] for i in range(colours))
    table = []
    for i in range(1 << bpp):
        b, g, r, a = palette[i * 4:i * 4 + 4] if i < colours else (0, 0, 0, 0)
        a = 255 if opaque else min(255, a * 2)
        if r > 200 and b > 200 and g < 70:
            a = 0
        table.append(bytes((r, g, b, a)))
    stride = (width * bpp + 31) // 32 * 4
    rows = []
    for y in range(abs(height)):
        at = off + (abs(height) - 1 - y if height > 0 else y) * stride
        line = data[at:at + stride]
        if bpp == 8:
            rows.append(b"".join(table[v] for v in line[:width]))
        else:
            px = []
            for x in range(width):
                v = line[x >> 1]
                px.append(table[v >> 4 if x & 1 == 0 else v & 15])
            rows.append(b"".join(px))
    return width, abs(height), b"".join(rows)


def zdir(data):
    assert data[:4] == b"ZDIR" and data[8:12] == b"DIDF"
    count = struct.unpack_from("<I", data, 0x10)[0]
    lengths = 0x18 + 8 + 4 * count
    offsets = lengths + 8 + 4 * count
    for i in range(count):
        w = struct.unpack_from("<I", data, lengths + 8 + 4 * i)[0]
        o = struct.unpack_from("<I", data, offsets + 8 + 4 * i)[0]
        yield data[o:o + (w & 0x0FFFFFFF)]


def picture_of(asset):
    for k in asset.kids:
        if k.tag in (b"TBMP", b"TZMP"):
            return k.data
    return None


class Elf:
    """Enough of the game's executable to read its data by EE address."""

    def __init__(self, data):
        self.data = data
        count = struct.unpack_from("<H", data, 44)[0]
        table = struct.unpack_from("<I", data, 28)[0]
        self.segments = []
        for i in range(count):
            kind, offset, vaddr, _, size = struct.unpack_from("<5I", data, table + i * 32)
            if kind == 1:
                self.segments.append((vaddr, offset, size))

    def word(self, address):
        for vaddr, offset, size in self.segments:
            if vaddr <= address < vaddr + size:
                return struct.unpack_from("<I", self.data, offset + address - vaddr)[0]
        raise ValueError("no segment holds 0x%x" % address)

    def text(self, address):
        for vaddr, offset, size in self.segments:
            if vaddr <= address < vaddr + size:
                at = offset + address - vaddr
                return self.data[at:self.data.index(b"\0", at)]
        raise ValueError("no segment holds 0x%x" % address)


# The pause menu's special trick list (inlinePausePopulateSpecialTrickList): ten rows.
SPECIAL_BITS, SPECIAL_NAMES, SPECIAL_BUTTONS = 0x00267BB0, 0x00267C50, 0x00267C78
SPECIAL_TRICKS = 10


def special_tricks(elf):
    out = b""
    for i in range(SPECIAL_TRICKS):
        bit = elf.word(SPECIAL_BITS + i * 4)
        name = elf.text(elf.word(SPECIAL_NAMES + i * 4))
        buttons = elf.text(elf.word(SPECIAL_BUTTONS + i * 4))
        out += struct.pack("<I", bit) + name[:47].ljust(48, b"\0") + buttons[:15].ljust(16, b"\0")
    return chunk(b"SPEC", struct.pack("<I", SPECIAL_TRICKS) + out)


class StringTable:
    """ASSETS/STRING.EUS: u32 text bytes, u32 count, {u32 name hash, u32 offset}[count]."""

    def __init__(self, data):
        self.texts = {}
        for i in range(struct.unpack_from("<I", data, 4)[0]):
            key, offset = struct.unpack_from("<II", data, 8 + i * 8)
            self.texts[key] = data[offset:data.index(b"\0", offset)]

    def text(self, key):
        return self.texts.get(al.name_hash(key), key.encode("latin1"))


# The trick tables: {create, unlock bit, pattern[2], mask[2]} with the recent
# stick directions newest first (tricks.cpp TrickButtonEntry): the air
# table's, the grind table's (the trick button 0x245E50) and the stall's. A
# table names a trick only by the function that makes it: these are those
# functions (air_tricks.cpp, trick_objects.cpp, stall_trick.cpp) by the
# string table's keys of the tricks' texts.
AIR_TABLE, AIR_ENTRIES = 0x00245928, 55
GRIND_TABLE, GRIND_ENTRIES = 0x00245E50, 25
STALL_TABLE, STALL_ENTRIES = 0x002460A8, 4
MANUAL_TABLE, MANUAL_ENTRIES = 0x00246108, 2
BUTTON_AIR, BUTTON_GRIND = ord("x"), ord("y")
FAMILY_GRAB, FAMILY_FLIP, FAMILY_COMBO, FAMILY_GRIND, FAMILY_OTHER, FAMILY_SPECIAL = range(6)

GRAB_KEYS = ["Rocket", "LongDick", "StaleAir", "LookBackJudo", "Abstract", "ParallelThat", "Method", "LuiKang"]
# (Two flips share the text "Flatspin"; the rows say which way round, in the HUD's words for a spin.)
FLIP_KEYS = ["FrontFlip", "HotFlip", "Flatspin", "BioFlip", "BackFlip", "MistyFlip", "Flatspin", "McTwistFlip"]
FLIP_SUFFIX = {2: b" CW", 6: b" CCW"}
SPECIAL_AIR_KEYS = ["SupermanLateFlip", "DoubleBackflip", "AbstractDoubleBackflip", "Bio", "Corkscrew", "DoubleFlatspin", "OneFourFourZero"]
GRIND_KEYS = {0x001F0768: "SoulGrind", 0x001F07F0: "BackslideGrind", 0x001F0870: "MiszouGrind", 0x001F08F8: "FishBrainGrind",
              0x001F0980: "RoyaleGrind", 0x001F0A08: "MakioGrind", 0x001F0A90: "PornstarGrind", 0x001F0B18: "FastslideGrind",
              0x001F0BA0: "TorqueSoulGrind", 0x001F0C28: "SoyaleGrind", 0x001F0CB0: "XGrind", 0x001F0D38: "AcidSoulGrind",
              0x001F0DC0: "FreestyleFastslideGrind", 0x001F0E48: "FreestyleBackslideGrind", 0x001F0ED0: "FreestyleFishbrainGrind",
              0x001F0F58: "FreestyleMakioGrind", 0x001F0FD8: "WonderGrind", 0x001F1060: "SweatStanceGrind", 0x001F10E8: "MistrialGrind",
              0x001F1170: "UnityGrind", 0x001F11F8: "SamFogertyGrindCombo", 0x001F1280: "FrankieMoralesGrindCombo",
              0x001F1308: "MattSalernoGrindCombo", 0x001F1390: "DoubleGrind", 0x001F1410: "HandGrind"}
STALL_KEYS = {0x001EACB8: "AbstractHandPlant", 0x001EAD18: "RocketHandPlant", 0x001EAD78: "MethodHandPlant", 0x001EADD8: "StaleHandPlant"}
MANUAL_KEYS = {0x001E84C0: "SingleHeelManual", 0x001E8590: "SingleToeManual"}


def air_trick_name(create, strings):
    """(family, text) of an air table's creator."""
    if 0x001EEF38 <= create < 0x001EF2B8:
        return FAMILY_GRAB, strings.text(GRAB_KEYS[(create - 0x001EEF38) // 0x70])
    if 0x001EF2B8 <= create < 0x001EF578:
        flip = (create - 0x001EF2B8) // 0x58
        return FAMILY_FLIP, strings.text(FLIP_KEYS[flip]) + FLIP_SUFFIX.get(flip, b"")
    if 0x001EF578 <= create < 0x001EF7E0:
        return FAMILY_SPECIAL, strings.text(SPECIAL_AIR_KEYS[(create - 0x001EF578) // 0x58])
    index = (create - 0x001EF7E0) // 0x78
    grab = ("Rocket", "StaleAir", "Abstract", "Method")[index // 8]
    flip = index % 8
    return FAMILY_COMBO, strings.text(grab) + b" " + strings.text(FLIP_KEYS[flip]) + FLIP_SUFFIX.get(flip, b"")


def trick_inputs(elf, entry):
    """The directions of a table entry, oldest first (the masks are made at start-up)."""
    return struct.pack("<I", elf.word(entry + 8)).rstrip(b"\0")[::-1]


def trick_record(family, unlock, inputs, button, name):
    return struct.pack("<BBBB4s", family, unlock, len(inputs), button, inputs.ljust(4, b"\0")) + name[:39].ljust(40, b"\0")


def trick_list(elf, strings):
    records = []
    for i in range(AIR_ENTRIES):
        entry = AIR_TABLE + i * 0x18
        family, name = air_trick_name(elf.word(entry), strings)
        records.append(trick_record(family, elf.word(entry + 4), trick_inputs(elf, entry), BUTTON_AIR, name))
    for i in range(GRIND_ENTRIES):
        entry = GRIND_TABLE + i * 0x18
        unlock = elf.word(entry + 4)
        family = FAMILY_SPECIAL if unlock else FAMILY_GRIND
        records.append(trick_record(family, unlock, trick_inputs(elf, entry), BUTTON_GRIND, strings.text(GRIND_KEYS[elf.word(entry)])))
    for i in range(STALL_ENTRIES):
        entry = STALL_TABLE + i * 0x18
        records.append(trick_record(FAMILY_OTHER, 0, trick_inputs(elf, entry), BUTTON_GRIND, strings.text(STALL_KEYS[elf.word(entry)])))
    for i in range(MANUAL_ENTRIES):
        entry = MANUAL_TABLE + i * 0x18
        records.append(trick_record(FAMILY_OTHER, 0, trick_inputs(elf, entry), 0, strings.text(MANUAL_KEYS[elf.word(entry)])))
    return chunk(b"TRIK", struct.pack("<I", len(records)) + b"".join(records))


def build_ui(disc, out):
    data = b"PMAP" + struct.pack("<I", FILE_VERSION)
    elf = Elf(disc.read("SLUS_203.27"))
    data += special_tricks(elf)
    data += trick_list(elf, StringTable(disc.read("ASSETS/STRING.EUS")))
    found = set()
    for entry in zdir(disc.read("ASSETS/UILIB.ZAL")):
        if entry[:4] != b"ULIB":
            continue
        for u in al.parse(entry)[0].kids:
            if u.tag != b"UAST":
                continue
            name = next(k for k in u.kids if k.tag == b"HAST").data[128:160].split(b"\0")[0].decode("latin1")
            label = name.encode("latin1")[:31].ljust(32, b"\0")
            for k in u.kids:
                if k.tag == b"UFNT" and name in UI_FONTS and name not in found:
                    found.add(name)
                    hfnt = next(c for c in k.kids if c.tag == b"HFNT").data
                    line, _, glyphs, pages = struct.unpack_from("<BBHH", hfnt, 0)
                    body = struct.pack("<III", line, glyphs, pages)
                    for t in (c for c in k.kids if c.tag == b"TXRC"):
                        w, h, rgba = bmp_rgba(picture_of(t))
                        body += struct.pack("<II", w, h) + rgba
                    body += hfnt[6 + 40 * pages:6 + 40 * pages + 12 * glyphs]
                    data += chunk(b"FONT", label + body)
                elif k.tag == b"UMTL" and name in UI_ICONS and name not in found:
                    found.add(name)
                    t = next(c for c in k.kids if c.tag == b"TXRC")
                    w, h, rgba = bmp_rgba(picture_of(t))
                    data += chunk(b"ICON", label + struct.pack("<II", w, h) + rgba)
    with open(os.path.join(out, "UI.pmap"), "wb") as f:
        f.write(data)
    missing = [n for n in UI_FONTS + UI_ICONS if n not in found]
    say("UI: %.1f MB, %d assets%s" % (len(data) / 1e6, len(found), ", missing " + " ".join(missing) if missing else ""))


def xbox_script(folder, level):
    """The Xbox release's script of a level (aggressive_inline_careerscripts.cpp
    serves it in place of the PS2 disc's): levels/<L>/<L>.bin under the
    folder, its assets/ or media/assets/, in any case; None without one."""
    if not folder:
        return None
    for base in (folder, os.path.join(folder, "assets"), os.path.join(folder, "media", "assets")):
        at = base
        for part in ("levels", level, level + ".bin"):
            if not os.path.isdir(at):
                at = None
                break
            match = [n for n in os.listdir(at) if n.lower() == part.lower()]
            at = os.path.join(at, match[0]) if match else None
            if at is None:
                break
        if at and os.path.isfile(at):
            with open(at, "rb") as f:
                return f.read()
    return None


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--disc", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--level", action="append")
    b.add_argument("--png")
    b.add_argument("--zoo")
    b.add_argument("--estate")
    b.add_argument("--game", choices=("ai", "dm2", "bmx"), default="ai")
    b.add_argument("--dm2-xbox")
    b.add_argument("--xbox", help="the Xbox release's files (the app's xbox folder): its level scripts' goals too")
    a = ap.parse_args(argv)
    if a.cmd == "build" and a.game == "dm2":
        build_dm2(a)
    elif a.cmd == "build" and a.game == "bmx":
        build_bmx(a)
    elif a.cmd == "build":
        disc = al.Disc(a.disc)
        os.makedirs(a.out, exist_ok=True)
        build_ui(disc, a.out)
        for level in a.level or LEVELS:
            build_level(lambda ext, level=level: disc.read("ASSETS/LEVELS/%s/%s.%s" % (level, level, ext)),
                        level, LEVELS[level], a.out, a.png, xbox_script(a.xbox, level))
        if a.zoo:
            build_level(FolderLevel(a.zoo, "ZOO").read, "ZOO", "Zoo", a.out, a.png)
        if a.estate:
            build_level(FolderLevel(a.estate, "TUT").read, "ESTATE", "Estate", a.out, a.png)


if __name__ == "__main__":
    main(sys.argv[1:])
