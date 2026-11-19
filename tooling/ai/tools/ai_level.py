#!/usr/bin/env python3
"""Aggressive Inline's level files read: the chunk trees of a level's GEO,
INS and DAT (ZGE3), its collidable faces and grindable edges in world space,
from the disc (folder or .iso). A library for the tools that look at AI's
levels (panel_map.py, panel_goals.py, the playbot's navigation map).

    python3 -I ai_level.py info <file>...         chunk summary of level files
    python3 -I ai_level.py selftest               the readers on hand-made records
    python3 -I ai_level.py ground --disc <AI disc> --points <file> --out <file> [--level MOVIE] [--step 5]
                                                  AI's own collidable surface under a grid, for
                                                  the runtime's PS2X_ESTATE_PROBE=<points>:<out>
    python3 -I ai_level.py edges --disc <AI disc> --out <file> [--level MOVIE]
                                                  AI's grindable edges (world space), for the
                                                  runtime's PS2X_ESTATE_GRINDS=<file>:<out>
    python3 -I ai_level.py compare <ground out> <probe out>
                                                  the level files' ground against what the
                                                  game's query found there, point by point

Python 3.8+, standard library only.
"""
import argparse
import os
import struct
import sys

LEVELS = {
    # AI folder: display name (AI's free skate order)
    "MOVIE": "Movie Lot",
    "CIVIC": "Civic Center",
    "INDUST": "Industrial",
    "BRDWLK": "Boardwalk",
    "CANNERY": "Cannery",
    "HANGER": "Airfield",
    "MUSEUM": "Museum",
    "TUT": "Tutorial",
}

CONTAINERS = {b"FORM", b"TGEO", b"TGPT", b"TMTB", b"TGIN", b"TXRC", b"FSCN", b"TERR", b"ZPVS", b"ZDYN",
              b"TIVC", b"TIVN", b"TIVU", b"TIFN"}


# ---- chunk trees -------------------------------------------------------------

class Chunk:
    __slots__ = ("tag", "data", "kids")

    def __init__(self, tag, data=b"", kids=None):
        self.tag = tag
        self.data = data
        self.kids = kids

    def size(self):
        if self.kids is None:
            return len(self.data)
        return sum(8 + k.size() for k in self.kids)

    def write(self, out):
        out += self.tag
        out += struct.pack("<I", self.size())
        if self.kids is None:
            out += self.data
        else:
            for k in self.kids:
                k.write(out)


def parse(data, start=0, end=None):
    end = len(data) if end is None else end
    chunks = []
    at = start
    while at + 8 <= end:
        tag = data[at:at + 4]
        size = struct.unpack_from("<I", data, at + 4)[0]
        body = at + 8
        if body + size > end:
            raise ValueError("chunk %r at 0x%x runs past its parent" % (tag, at))
        if tag in CONTAINERS:
            chunks.append(Chunk(tag, kids=parse(data, body, body + size)))
        else:
            chunks.append(Chunk(tag, data[body:body + size]))
        at = body + size
    if at != end:
        raise ValueError("%d stray bytes at 0x%x" % (end - at, at))
    return chunks


def serialize(chunks):
    out = bytearray()
    for c in chunks:
        c.write(out)
    return bytes(out)


def walk(chunks):
    for c in chunks:
        yield c
        if c.kids is not None:
            yield from walk(c.kids)


def name_hash(name):
    """The engine's name hash (AI 0x148888, DM2 0x133F18): case-insensitive ELF hash."""
    h = 0
    for ch in name.encode("latin1"):
        c = ch - 32 if 97 <= ch <= 122 else ch
        h = ((h << 4) + c) & 0xFFFFFFFF
        high = h & 0xF0000000
        if high:
            h ^= high >> 24
        h &= ~high & 0xFFFFFFFF
    return h


# ---- indexed streams and tag buffers ------------------------------------------

def indexed(chunk):
    """An indexed stream's table and its indices (TIDA, TII8 | TII6)."""
    table, indices = None, None
    for k in chunk.kids:
        if k.tag == b"TIDA":
            table = k.data
        elif k.tag == b"TII8":
            indices = list(k.data)
        elif k.tag == b"TII6":
            indices = list(struct.unpack("<%dH" % (len(k.data) // 2), k.data[:len(k.data) // 2 * 2]))
    if table is None or indices is None:
        raise ValueError("%r without its table or indices" % chunk.tag)
    return table, indices


def expand(chunk, count, stride):
    """An indexed stream as the plain one: element indices[i] of the table,
    for each of the part's count vertices (faces for TIFN)."""
    table, indices = indexed(chunk)
    if len(indices) < count:
        raise ValueError("%r: %d indices for %d elements" % (chunk.tag, len(indices), count))
    out = bytearray(count * stride)
    for i in range(count):
        at = indices[i] * stride
        if at + stride > len(table):
            raise ValueError("%r: index %d past its table of %d bytes" % (chunk.tag, indices[i], len(table)))
        out[i * stride:(i + 1) * stride] = table[at:at + stride]
    return bytes(out)


FACE_COLLIDES = 0x2000


def tag_buffer(data):
    """A TGFE / TGFT chunk: a descriptor (bits 0-3 the index kind, 1 = u16;
    bits 4-13 the record size; bits 14-31 the record count), the index
    count, the indices (one per face edge / face, 0xFFFF = none) padded to
    16 bytes, then the records (AI FUN_00135658, DM2 FUN_00126368)."""
    desc, count = struct.unpack_from("<2I", data, 0)
    if desc & 0xF != 1:
        raise ValueError("tag buffer of index kind %d" % (desc & 0xF))
    size, records = (desc >> 4) & 0x3FF, desc >> 14
    index_bytes = (count * 2 + 15) & ~15
    body = data[8 + index_bytes:]
    if len(body) != size * records:
        raise ValueError("tag buffer of %d bytes for %d records of %d" % (len(body), records, size))
    return desc, count, data[8:8 + index_bytes], [body[i * size:(i + 1) * size] for i in range(records)]


# ---- the disc -----------------------------------------------------------------

class Iso:
    """Enough ISO9660 to read files by path."""

    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(16 * 2048)
        pvd = self.f.read(2048)
        if pvd[1:6] != b"CD001":
            raise ValueError("%s is not an ISO9660 image" % path)
        self.root = self.record(pvd, 156)

    @staticmethod
    def record(buf, at):
        length = buf[at]
        extent, size = struct.unpack_from("<I", buf, at + 2)[0], struct.unpack_from("<I", buf, at + 10)[0]
        flags = buf[at + 25]
        nlen = buf[at + 32]
        name = buf[at + 33:at + 33 + nlen].decode("latin1").split(";")[0].rstrip(".")
        return length, extent, size, flags, name

    def entries(self, extent, size):
        self.f.seek(extent * 2048)
        buf = self.f.read(size)
        at = 0
        while at < len(buf):
            if buf[at] == 0:
                at = (at // 2048 + 1) * 2048
                continue
            rec = self.record(buf, at)
            at += rec[0]
            yield rec

    def read(self, path):
        _, extent, size, flags, _ = self.root
        for part in path.upper().split("/"):
            for rec in self.entries(extent, size):
                if rec[4].upper() == part:
                    _, extent, size, flags, _ = rec
                    break
            else:
                return None
        self.f.seek(extent * 2048)
        return self.f.read(size)


class Disc:
    def __init__(self, path):
        self.iso = Iso(path) if os.path.isfile(path) else None
        self.root = path

    def read(self, path):
        if self.iso:
            return self.iso.read(path)
        at = self.root
        for part in path.split("/"):
            names = {n.upper(): n for n in os.listdir(at)} if os.path.isdir(at) else {}
            if part.upper() not in names:
                return None
            at = os.path.join(at, names[part.upper()])
        with open(at, "rb") as f:
            return f.read()


def read_level(disc, level):
    files = {}
    for ext in ("ZIT", "GEO", "INS", "TXR", "DAT"):
        data = disc.read("ASSETS/LEVELS/%s/%s.%s" % (level, level, ext))
        if data is None:
            raise ValueError("ASSETS/LEVELS/%s/%s.%s is not on the disc" % (level, level, ext))
        files[ext] = data
    return files


# ---- commands -------------------------------------------------------------------

def cmd_info(args):
    from collections import Counter
    for path in args.files:
        with open(path, "rb") as f:
            tree = parse(f.read())
        counts = Counter()
        sizes = {}
        for c in walk(tree):
            counts[c.tag] += 1
            if c.kids is None:
                sizes.setdefault(c.tag, set()).add(len(c.data))
        print(path)
        for tag, n in sorted(counts.items()):
            s = sorted(sizes.get(tag, ()))
            print("  %s %6d %s" % (tag.decode("latin1"), n, ",".join("%x" % x for x in s[:4]) + ("..." if len(s) > 4 else "")))


# ---- the collision surface and the grind edges ------------------------------

def quat_rotate(q, v):
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return (v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx))


def instance_placer(ins):
    """(instances' TGIH data, place(d, v) -> world point, turn(d, n) ->
    world direction). An instance with a parent (+0x44, the parent's id; its
    own at +0x48; decals, statue parts, animated pieces) is placed in its
    parent's space (both games' TGIH readers attach it; dm2_level.py's
    reference does the same)."""
    instances = [c.data for c in walk(parse(ins)) if c.tag == b"TGIH"]
    by_id = {struct.unpack_from("<I", d, 0x48)[0]: d for d in instances}

    def place(d, v):
        for _ in range(16):
            v = tuple(a + b for a, b in zip(quat_rotate(struct.unpack_from("<4f", d, 8), v), struct.unpack_from("<3f", d, 0x18)))
            d = by_id.get(struct.unpack_from("<I", d, 0x44)[0]) if struct.unpack_from("<I", d, 0x44)[0] else None
            if d is None:
                break
        return v
    return instances, place


INSTANCE_COLLIDES = 0x2
INSTANCE_NO_COLLISION = 0x10   # with 0x2 on panoramas (dm2_level.py's reference has the same rule)
FACE_TWO_SIDED = 0x4000


def collision_triangles(geo, ins, every_instance=False):
    """Every collidable instance's collidable faces in world space, grouped
    per instance with its x/z bounds. An instance collides when its TGIH
    flags (+0) have bit 1: both games copy that word into the scene node
    (AI FUN_0012F940 to node bit 1, tested by its object test FUN_001250C8;
    DM2 FUN_00121E00 to node bit 33, tested by FUN_0011AE90); the Movie Lot
    has 529 instances without it (roofs, a road's decal layer, props), and
    one with it and 0x10 (panorama_01, which neither game collides with). The
    faces are those of the object's last part with 0x2000 (the only ones in
    either game's TGCI leaves; AI's query takes an object without a tree
    too, FUN_00124BF0). Positions are plain TGVP in both games. A face
    without 0x4000 (two-sided) is hit from its front only: for a ray going
    down, when (b - a) x (c - a) points up (-y)."""
    objects = {}
    for obj in parse(geo)[0].kids:
        if obj.tag != b"TGEO":
            continue
        kids = obj.kids
        head = next(k for k in kids if k.tag == b"TGHD")
        parts = [k for k in kids if k.tag == b"TGPT"]
        if not parts:
            continue
        streams = {k.tag: k.data for k in parts[-1].kids if k.kids is None}
        nv, nf = struct.unpack_from("<HH", streams[b"TGPH"], 4)
        verts = [struct.unpack_from("<3f", streams[b"TGVP"], 12 * i) for i in range(nv)]
        faces = [struct.unpack_from("<4H", streams[b"TGFP"], 8 * i) for i in range(nf)]
        objects[struct.unpack_from("<I", head.data, 0x18)[0]] = (
            verts, [f for f in faces if f[0] & FACE_COLLIDES and max(f[1:]) < nv])
    groups = []
    instances, place = instance_placer(ins)
    for d in instances:
        if not every_instance and struct.unpack_from("<I", d, 0)[0] & (INSTANCE_COLLIDES | INSTANCE_NO_COLLISION) != INSTANCE_COLLIDES:
            continue
        verts, faces = objects.get(struct.unpack_from("<I", d, 0x40)[0], ((), ()))
        world = [place(d, v) for v in verts]
        tris = []
        for flags, i0, i1, i2 in faces:
            a, b, c2 = world[i0], world[i1], world[i2]
            up = (b[2] - a[2]) * (c2[0] - a[0]) - (b[0] - a[0]) * (c2[2] - a[2])
            if flags & FACE_TWO_SIDED or up < 0:
                tris.append((a, b, c2))
        if tris:
            xs = [v[0] for t in tris for v in t]
            zs = [v[2] for t in tris for v in t]
            groups.append((min(xs), max(xs), min(zs), max(zs), tris, d[0x50:].split(b"\0")[0].decode("latin1")))
    return groups


def grind_edges(geo, ins):
    """Every grindable edge of the collidable instances in world space:
    [(a, b, instance name, AI kind)]. An edge is face * 3 + k of the
    object's collision part (the part with the TGFE), from the face's
    vertex k to k + 1 (DM2 FUN_0017A080, AI the same); grindable when its
    record's flags (AI +9) have bit 0."""
    objects = {}
    for obj in parse(geo)[0].kids:
        if obj.tag != b"TGEO":
            continue
        head = next(k for k in obj.kids if k.tag == b"TGHD")
        for part in (k for k in obj.kids if k.tag == b"TGPT"):
            streams = {k.tag: k for k in part.kids}
            if b"TGFE" not in streams:
                continue
            nv, nf = struct.unpack_from("<HH", streams[b"TGPH"].data, 4)
            verts = [struct.unpack_from("<3f", streams[b"TGVP"].data, 12 * i) for i in range(nv)]
            faces = [struct.unpack_from("<4H", streams[b"TGFP"].data, 8 * i) for i in range(nf)]
            desc, count, indices, records = tag_buffer(streams[b"TGFE"].data)
            edges = []
            for e, r in enumerate(struct.unpack_from("<%dH" % count, indices)):
                if r == 0xFFFF or not records[r][9] & 1 or e // 3 >= nf:
                    continue
                face = faces[e // 3]
                i0, i1 = face[1 + e % 3], face[1 + (e + 1) % 3]
                if max(i0, i1) < nv:
                    edges.append((verts[i0], verts[i1], records[r][8]))
            objects[struct.unpack_from("<I", head.data, 0x18)[0]] = edges
    out = []
    instances, place = instance_placer(ins)
    for d in instances:
        if struct.unpack_from("<I", d, 0)[0] & (INSTANCE_COLLIDES | INSTANCE_NO_COLLISION) != INSTANCE_COLLIDES:
            continue
        name = d[0x50:].split(b"\0")[0].decode("latin1")
        for a, b, kind in objects.get(struct.unpack_from("<I", d, 0x40)[0], ()):
            out.append((place(d, a), place(d, b), name, kind))
    return out


def say(text):
    print("[ai_level] " + text, flush=True)


def topmost(groups, x, z):
    """The topmost collidable surface (smallest y: +y is down) over (x, z)."""
    best = None
    for x0, x1, z0, z1, tris, _ in groups:
        if not (x0 <= x <= x1 and z0 <= z <= z1):
            continue
        for a, b, c in tris:
            d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
            if abs(d) < 1e-9:
                continue
            l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d
            l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d
            l3 = 1 - l1 - l2
            if min(l1, l2, l3) >= -1e-6:
                y = l1 * a[1] + l2 * b[1] + l3 * c[1]
                best = y if best is None or y < best else best
    return best


def cmd_ground(args):
    """A grid over the level (--points) and AI's own ground under each point
    from its files (--out): the reference for what the runtime's
    PS2X_ESTATE_PROBE writes."""
    disc = Disc(args.disc)
    files = read_level(disc, args.level.upper())
    groups = collision_triangles(files["GEO"], files["INS"])
    # The grid over every instance's collidable faces (the same points
    # whichever instances collide).
    every = collision_triangles(files["GEO"], files["INS"], every_instance=True)
    x0, x1 = min(g[0] for g in every), max(g[1] for g in every)
    z0, z1 = min(g[2] for g in every), max(g[3] for g in every)
    points, out = [], []
    x = x0 + args.step / 2
    while x < x1:
        z = z0 + args.step / 2
        while z < z1:
            points.append("%.3f %.3f\n" % (x, z))
            y = topmost(groups, x, z)
            out.append("%.3f %.3f %s\n" % (x, z, "none" if y is None else "%.4f" % y))
            z += args.step
        x += args.step
    with open(args.points, "w") as f:
        f.writelines(points)
    with open(args.out, "w") as f:
        f.writelines(out)
    say("%d points every %g over x %.0f..%.0f, z %.0f..%.0f" % (len(points), args.step, x0, x1, z0, z1))


def cmd_edges(args):
    """AI's grindable edges of a level (grind_edges), one per line:
    "ax ay az bx by bz kind name", for the runtime's PS2X_ESTATE_GRINDS (the
    game's rail search at each edge's middle). Edges shared by two faces are
    listed once."""
    disc = Disc(args.disc)
    files = read_level(disc, args.level.upper())
    seen, lines = set(), []
    for a, b, name, kind in grind_edges(files["GEO"], files["INS"]):
        key = tuple(sorted((tuple(round(v, 3) for v in a), tuple(round(v, 3) for v in b))))
        if key in seen:
            continue
        seen.add(key)
        lines.append("%.4f %.4f %.4f %.4f %.4f %.4f %d %s\n" % (a + b + (kind, name)))
    with open(args.out, "w") as f:
        f.writelines(lines)
    say("%s: %d grindable edges" % (args.level.upper(), len(lines)))


def cmd_compare(args):
    """The files' ground (cmd_ground) against the game's (PS2X_ESTATE_PROBE), point by point."""
    from collections import Counter
    counts = Counter()
    with open(args.files) as a, open(args.game) as b:
        for ref, got in zip(a, b):
            ref, got = ref.split(), got.split()
            if ref[2] == "none" or got[2] == "none":
                counts["files %s, game %s" % ("none" if ref[2] == "none" else "ground",
                                              "none" if got[2] == "none" else "ground")] += 1
                continue
            d = abs(float(got[2]) - float(ref[2]))
            counts["same height (0.01)" if d <= 0.01 else "within 0.1" if d <= 0.1 else "different"] += 1
    total = sum(counts.values())
    for key, n in sorted(counts.items()):
        print("%-24s %6d  %6.2f%%" % (key, n, 100.0 * n / total))
    ground = total - counts["files none, game none"]
    same = counts["same height (0.01)"] + counts["within 0.1"]
    if ground:
        print("ground in either: %d points, the game's within 0.1 of the files': %d (%.1f%%)" % (ground, same, 100.0 * same / ground))

def cmd_selftest(args):
    """The readers on hand-made records (no disc needed)."""
    assert name_hash("PlayerOne") == 0x06DA4725
    # a child instance is placed in its parent's space
    def tgih(x, parent, me):
        return struct.pack("<II4f3f", 0x23, 0, 0, 0, 0, 1, x, 0, 0).ljust(0x44, b"\0") + struct.pack("<II", parent, me) + b"\0" * 0x24
    ins = serialize([Chunk(b"FORM", kids=[Chunk(b"TGIN", kids=[Chunk(b"TGIH", tgih(10, 0, 5)), Chunk(b"TGIH", tgih(1, 5, 6))])])])
    instances, place = instance_placer(ins)
    assert place(instances[1], (0.5, 0, 0)) == (11.5, 0, 0) and place(instances[0], (0, 0, 0)) == (10, 0, 0)
    edges = struct.pack("<2I", (2 << 14) | (24 << 4) | 1, 3) + struct.pack("<3H", 0, 1, 0xFFFF) + b"\0" * 10
    edges += b"\1" * 24 + b"\2" * 24
    desc, count, indices, records = tag_buffer(edges)
    assert count == 3 and len(indices) == 16 and records == [b"\1" * 24, b"\2" * 24]
    print("ai_level selftest: ok")


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("ground")
    g.add_argument("--disc", required=True)
    g.add_argument("--level", default="MOVIE")
    g.add_argument("--step", type=float, default=5.0)
    g.add_argument("--points", required=True)
    g.add_argument("--out", required=True)
    g.set_defaults(fn=cmd_ground)
    e = sub.add_parser("edges")
    e.add_argument("--disc", required=True)
    e.add_argument("--level", default="MOVIE")
    e.add_argument("--out", required=True)
    e.set_defaults(fn=cmd_edges)
    m = sub.add_parser("compare")
    m.add_argument("files")
    m.add_argument("game")
    m.set_defaults(fn=cmd_compare)
    t = sub.add_parser("selftest")
    t.set_defaults(fn=cmd_selftest)
    i = sub.add_parser("info")
    i.add_argument("files", nargs="+")
    i.set_defaults(fn=cmd_info)
    args = p.parse_args(argv)
    try:
        args.fn(args)
    except ValueError as e:
        raise SystemExit("ai_level.py: %s" % e)


if __name__ == "__main__":
    main(sys.argv[1:])
