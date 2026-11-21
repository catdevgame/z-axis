#!/usr/bin/env python3
"""Dave Mirra Freestyle BMX 2's level files read: the chunk trees of a
level's GEO, INS and DAT (ZGE2), its collision trees and collidable faces in
world space, the ZDIR archives and BMPs of its UI library, from the disc
(folder or .iso). A library for the tools that make and look at DM2's
levels (dm2_xbox.py and dm2_xbox_career.py, the playbot's DM2 and BMX XXX
maps, qpride.py).

    python3 -I dm2_level.py info <file>...        chunk summary of level files
    python3 -I dm2_level.py selftest              the readers and helpers on hand-made records

Objects past the collision tree's range (split values are s16 hundredths,
+-327.67; DM2's exporter wrapped larger ones) can be recentred, their
instances moved to match (recentre_object; dm2_xbox.py does it for the Xbox
levels).

Python 3.8+, standard library only.
"""
import argparse
import os
import struct
import sys


CONTAINERS = {b"FORM", b"TGEO", b"TGPT", b"TMTB", b"TGIN", b"TXRC", b"FSCN", b"TERR", b"ZPVS", b"ZDYN"}


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


# ---- records ------------------------------------------------------------------

DM2_MATI = 0x9C  # a material's (MATI) size


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


# The collision kd-tree's split values are s16 hundredths (+-327.67); DM2's
# exporter wrapped larger ones (Cloverleaf's canal end hill, local z -463 to
# -414: DM2's tree walk missed it). Such objects can be recentred.
SPLIT_LIMIT = 327.67


def object_head(obj):
    head = next(k for k in obj.kids if k.tag == b"TGHD")
    return head, struct.unpack_from("<I", head.data, 0x18)[0]


def unwrap_splits(blob, lo, hi, offset):
    """A DM2 TGCI tree (DM2 leaves) with every split value moved by -offset
    (x, y, z), the wrapped ones first put back into the object's bounds."""
    out = bytearray(blob)

    def visit(at):
        kind = blob[at]
        if kind == 0:
            return
        axis = kind - 1
        split, first, second = struct.unpack_from("<hHH", blob, at + 2)
        value = split / 100.0
        while value < lo[axis] - 1.0:
            value += 655.36
        while value > hi[axis] + 1.0:
            value -= 655.36
        moved = int(round((value - offset[axis]) * 100.0))
        if not -32768 <= moved <= 32767:
            raise ValueError("split %.2f out of range after recentring" % (value - offset[axis]))
        struct.pack_into("<h", out, at + 2, moved)
        visit(first * 4)
        visit(second * 4)

    visit(0)
    return bytes(out)


def recentre_object(obj):
    """Moves an object whose local bounds pass the tree's range to around
    its own centre: positions (TGVP of every part), bounds (TGHD) and the
    tree's splits. Returns the offset (its instances move by it), or None."""
    head, _ = object_head(obj)
    bounds = struct.unpack_from("<6f", head.data, 0)
    if max(abs(v) for v in bounds) <= SPLIT_LIMIT:
        return None
    lo, hi = bounds[:3], bounds[3:]
    offset = tuple(float(round((a + b) / 2.0)) for a, b in zip(lo, hi))
    head.data = struct.pack("<6f", *(v - offset[i % 3] for i, v in enumerate(bounds))) + head.data[0x18:]
    for part in obj.kids:
        if part.tag != b"TGPT":
            continue
        for k in part.kids:
            if k.tag == b"TGVP":
                n = len(k.data) // 12
                v = struct.unpack("<%df" % (3 * n), k.data)
                k.data = struct.pack("<%df" % (3 * n), *(v[i] - offset[i % 3] for i in range(3 * n)))
    for k in obj.kids:
        if k.tag == b"TGCI":
            part, size = struct.unpack_from("<2I", k.data, 0)
            k.data = k.data[:8] + unwrap_splits(k.data[8:8 + size], lo, hi, offset) + k.data[8 + size:]
    return offset


SPAWN = 0x005846BE      # "Spawn"
PLAYER_ONE = name_hash("playerone")
assert SPAWN == name_hash("Spawn") and PLAYER_ONE == 0x06DA4725


# ---- the disc -----------------------------------------------------------------

class Iso:
    """Enough ISO9660 to read files by path (the DM2 disc image)."""

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


# ---- archives and pictures ------------------------------------------------------

def zal_entries(data):
    """The entries of a ZDIR archive (.ZAL), as bytes."""
    if data[:4] != b"ZDIR" or data[8:12] != b"DIDF":
        raise ValueError("not a ZDIR archive")
    count = struct.unpack_from("<I", data, 0x10)[0]
    lens = struct.unpack_from("<%dI" % count, data, 0x20 + 4 * count)
    offs = struct.unpack_from("<%dI" % count, data, 0x28 + 8 * count)
    return [data[o:o + (n & 0x0FFFFFFF)] for o, n in zip(offs, lens)]


def decode_bmp(data, opaque=False):
    """A 4- or 8-bit Windows BMP: (width, height, rows of (r, g, b, a)), top row
    first. DM2's palettes leave alpha 0 (opaque=True reads them as 255)."""
    if data[:2] != b"BM":
        raise ValueError("not a BMP")
    pixels, head = struct.unpack_from("<I", data, 10)[0], struct.unpack_from("<I", data, 14)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp not in (4, 8):
        raise ValueError("%d-bit BMP" % bpp)
    colours = struct.unpack_from("<I", data, 46)[0] or 1 << bpp
    pal = [(data[14 + head + 4 * i + 2], data[14 + head + 4 * i + 1], data[14 + head + 4 * i],
            255 if opaque else data[14 + head + 4 * i + 3]) for i in range(colours)]
    stride = (w * bpp + 31) // 32 * 4
    rows = []
    for y in range(abs(h)):
        at = pixels + (abs(h) - 1 - y if h > 0 else y) * stride
        row = data[at:at + stride]
        if bpp == 8:
            rows.append([pal[row[x]] for x in range(w)])
        else:
            rows.append([pal[(row[x >> 1] >> 4) if x % 2 == 0 else row[x >> 1] & 15] for x in range(w)])
    return w, abs(h), rows


def median_cut(counts, boxes):
    """A palette of at most `boxes` colours for {rgba: count} and each
    colour's index (median cut by the widest channel; ties ordered by the
    whole colour, so every platform makes the same palette)."""
    work = [sorted(counts)]
    while len(work) < boxes:
        best, best_score, best_ch = None, 0, 0
        for i, box in enumerate(work):
            if len(box) < 2:
                continue
            ranges = [max(c[k] for c in box) - min(c[k] for c in box) for k in range(4)]
            ch = max(range(4), key=lambda k: (ranges[k], -k))
            score = ranges[ch] * sum(counts[c] for c in box)
            if score > best_score:
                best, best_score, best_ch = i, score, ch
        if best is None:
            break
        box = sorted(work[best], key=lambda c: (c[best_ch], c))
        total, run, cut = sum(counts[c] for c in box), 0, 1
        for k, c in enumerate(box[:-1]):
            run += counts[c]
            if run * 2 >= total:
                cut = k + 1
                break
        work[best:best + 1] = [box[:cut], box[cut:]]
    palette, index = [], {}
    for box in work:
        n = sum(counts[c] for c in box)
        palette.append(tuple(int(sum(c[k] * counts[c] for c in box) / n + 0.5) for k in range(4)))
        for c in box:
            index[c] = len(palette) - 1
    return palette, index


def bmp8(rows, palette):
    """An 8-bit Windows BMP (bottom-up, BGRA palette of 256) of index rows."""
    w, h = len(rows[0]), len(rows)
    pal = b"".join(struct.pack("<4B", b, g, r, a) for r, g, b, a in palette) + b"\0" * (4 * (256 - len(palette)))
    head = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 8, 0, (w + -w % 4) * h, 0, 0, 256, 0)
    pixels = b"".join(bytes(row) + b"\0" * (-w % 4) for row in reversed(rows))
    return struct.pack("<2sIHHI", b"BM", 14 + 40 + 1024 + len(pixels), 0, 0, 14 + 40 + 1024) + head + pal + pixels


# ---- commands -------------------------------------------------------------------

def cmd_info(args):
    for path in args.files:
        with open(path, "rb") as f:
            tree = parse(f.read())
        from collections import Counter
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


# ---- the collision surface -----------------------------------------------------

def quat_rotate(q, v):
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return (v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx))


def tree_faces(data, offset_leaves=None):
    """The faces a collision tree (a TGCI record: a word, the tree's size,
    the tree) lists in its leaves. A node is a kind word (0: a leaf) and,
    for a branch, its two children as offsets in words. DM2's leaves keep
    their count at +2 and the indices inline from +4, and the record's first
    word is small; BMX XXX's (ZGE4) leaves are 8 bytes, the count at +2 and
    at +4 the offset, in halfwords from the tree's start, of the indices in
    the list after the nodes, and the record's first word is a name hash.
    (Read as DM2's, a BMX XXX tree lists a quarter of the faces and numbers
    past the part: the Bronx map's missing triangles.)
    offset_leaves: the game's form, which a caller that knows the game gives
    (BMX XXX: True); None guesses it from the first word, which BMX XXX's
    GameCube-only test levels (TST_GRND, TST_FLAT, TST_VERT, CMP_SCRT) hold
    small (2, 4, 6 ...: not a hash), so the guess reads them as DM2's."""
    first, size = struct.unpack_from("<II", data, 0)
    blob = data[8:8 + size]
    if offset_leaves is None:
        offset_leaves = first > 0xFFFF
    faces = set()
    stack = [0]
    seen = set()
    while stack:
        at = stack.pop()
        if at in seen or at + 8 > len(blob):
            continue
        seen.add(at)
        if blob[at] == 0:
            count = struct.unpack_from("<H", blob, at + 2)[0]
            start = 2 * struct.unpack_from("<I", blob, at + 4)[0] if offset_leaves else at + 4
            if start + 2 * count <= len(blob):
                faces.update(struct.unpack_from("<%dH" % count, blob, start))
        else:
            stack += [4 * c for c in struct.unpack_from("<2H", blob, at + 4)]
    return faces


def face_normals(streams, verts, faces):
    """A part's face normals: its TGFN stream, or, where the part has none
    (most of BMX XXX's parts), the winding's cross product (unnormalised;
    only its side is read). Where BMX XXX's parts carry both, every face's
    winding agrees with its TGFN (CHL_MULY: 4,655 of 4,655)."""
    if b"TGFN" in streams:
        return [struct.unpack_from("<3f", streams[b"TGFN"], 12 * i) for i in range(len(faces))]
    out = []
    for f in faces:
        if max(f[1:]) >= len(verts):
            out.append((0.0, 0.0, 0.0))
            continue
        a, b, c = (verts[i] for i in f[1:])
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        out.append((uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx))
    return out


def collision_triangles(geo, ins):
    """Every instance's collidable faces in world space, grouped per
    instance with its x/z bounds: the faces DM2's collision tree (TGCI)
    lists, which are what DM2's tree walk (FUN_00119888) tests. They are the
    faces with flag 0x2000, AI's walk's own test (FUN_00123110); faces with
    0x4000 and not 0x2000 (tree crowns, 0x5300) are in no DM2 tree. Seen
    from above (the probe's segment runs down), a face counts if its normal
    (TGFN) points up or it is two-sided (0x4000; both walks test
    "in front, or 0x4000"): the panorama's inward faces (0x33xx) do not.
    Instances without flag 0x2 (Airport's conveyor belts, 0x21) or with
    flag 0x10 (the panoramas, 0x33 where the rest is 0x23) are left out, as
    both games' collision worlds leave them (both games' TGIH readers keep
    bits 0-4 as they are; ai_level.py collision_triangles). BMX XXX's levels
    are the same format, but most of their parts carry no TGFN: their faces'
    normals come from the winding (face_normals)."""
    objects = {}
    for obj in parse(geo)[0].kids:
        if obj.tag != b"TGEO":
            continue
        kids = obj.kids
        head = next(k for k in kids if k.tag == b"TGHD")
        parts = [k for k in kids if k.tag == b"TGPT"]
        trees = [k for k in kids if k.tag == b"TGCI"]
        if not parts or not trees:
            continue
        streams = {k.tag: k.data for k in parts[-1].kids}
        nv, nf = struct.unpack_from("<HH", streams[b"TGPH"], 4)
        verts = [struct.unpack_from("<3f", streams[b"TGVP"], 12 * i) for i in range(nv)]
        faces = [struct.unpack_from("<4H", streams[b"TGFP"], 8 * i) for i in range(nf)]
        normals = face_normals(streams, verts, faces)
        listed = tree_faces(trees[0].data)
        # (A face past the part's vertices, listed in a tree of BMX XXX's CHL_MNCO, is left out.)
        objects[struct.unpack_from("<I", head.data, 0x18)[0]] = (
            verts, [(f[1:], normals[i], f[0] & 0x4000) for i, f in enumerate(faces) if i in listed and max(f[1:]) < nv])
    # An instance with a parent (+0x44, the parent's id at +0x48; the decals)
    # is placed in its parent's space (both games' TGIH readers attach it).
    instances = [c.data for c in walk(parse(ins)) if c.tag == b"TGIH"]
    by_id = {struct.unpack_from("<I", d, 0x48)[0]: d for d in instances}

    def place(d, v):
        for _ in range(16):
            v = tuple(a + b for a, b in zip(quat_rotate(struct.unpack_from("<4f", d, 8), v), struct.unpack_from("<3f", d, 0x18)))
            d = by_id.get(struct.unpack_from("<I", d, 0x44)[0])
            if d is None:
                break
        return v

    def turn(d, n):
        for _ in range(16):
            n = quat_rotate(struct.unpack_from("<4f", d, 8), n)
            d = by_id.get(struct.unpack_from("<I", d, 0x44)[0])
            if d is None:
                break
        return n

    groups = []
    for d in instances:
        if struct.unpack_from("<I", d, 0)[0] & 0x12 != 0x2:
            continue
        verts, faces = objects.get(struct.unpack_from("<I", d, 0x40)[0], ((), ()))
        world = [place(d, v) for v in verts]
        tris = [(world[a], world[b], world[c2]) for (a, b, c2), n, two_sided in faces
                if two_sided or turn(d, n)[1] < 0.0]
        if tris:
            xs = [v[0] for t in tris for v in t]
            zs = [v[2] for t in tris for v in t]
            groups.append((min(xs), max(xs), min(zs), max(zs), tris))
    return groups


def goal_places(geo, ins):
    """What DM2's goals find in a level by name (games/dm2/game/goals.cpp
    findObjectiveTargets): {name hash: {"face": [centre], "edge": [end],
    "object": [position]}} in world space. A face's or an edge's name is the
    tag word (+0) of its record in its part's TGFT (one index per face) or
    TGFE (one per face edge: face * 3 + k, from the face's vertex k to k + 1)
    (collectTaggedFaces, collectTaggedEdges; every node with tags, drawn or
    not); an object's its instance's (TGIH +0x4C: DM2's instances keep their
    name 4 bytes before AI's), at its origin (findObjects, objectPosition).
    Each edge gives both ends, as fitObjectiveBox grows the box by them."""
    meshes = {}
    for obj in parse(geo)[0].kids:
        if obj.tag != b"TGEO":
            continue
        head = next(k for k in obj.kids if k.tag == b"TGHD")
        tagged = []
        for part in (k for k in obj.kids if k.tag == b"TGPT"):
            streams = {k.tag: k.data for k in part.kids}
            if b"TGFT" not in streams and b"TGFE" not in streams:
                continue
            nv, nf = struct.unpack_from("<HH", streams[b"TGPH"], 4)
            verts = [struct.unpack_from("<3f", streams[b"TGVP"], 12 * i) for i in range(nv)]
            faces = [struct.unpack_from("<4H", streams[b"TGFP"], 8 * i) for i in range(nf)]
            for stream, kind in ((b"TGFT", "face"), (b"TGFE", "edge")):
                if stream not in streams:
                    continue
                _, count, indices, records = tag_buffer(streams[stream])
                for e, r in enumerate(struct.unpack_from("<%dH" % count, indices)):
                    if r == 0xFFFF or r >= len(records) or len(records[r]) < 4:
                        continue
                    word = struct.unpack_from("<I", records[r], 0)[0]
                    if not word:
                        continue
                    if kind == "face" and e < nf and max(faces[e][1:]) < nv:
                        tagged.append((word, kind, [verts[i] for i in faces[e][1:]]))
                    elif kind == "edge" and e // 3 < nf:
                        face = faces[e // 3]
                        i0, i1 = face[1 + e % 3], face[1 + (e + 1) % 3]
                        if max(i0, i1) < nv:
                            tagged.append((word, kind, [verts[i0], verts[i1]]))
        meshes[struct.unpack_from("<I", head.data, 0x18)[0]] = tagged
    instances = [c.data for c in walk(parse(ins)) if c.tag == b"TGIH"]
    by_id = {struct.unpack_from("<I", d, 0x48)[0]: d for d in instances}

    def place(d, v):
        for _ in range(16):
            v = tuple(a + b for a, b in zip(quat_rotate(struct.unpack_from("<4f", d, 8), v), struct.unpack_from("<3f", d, 0x18)))
            d = by_id.get(struct.unpack_from("<I", d, 0x44)[0])
            if d is None:
                break
        return v

    out = {}
    for d in instances:
        # (BMX XXX's instances keep the name at +0x50, as AI's: +0x4C is a zero word there)
        name = (d[0x4C:].split(b"\0")[0] or d[0x50:].split(b"\0")[0]).decode("latin1")
        if name:
            out.setdefault(name_hash(name), {}).setdefault("object", []).append(place(d, (0.0, 0.0, 0.0)))
        for word, kind, points in meshes.get(struct.unpack_from("<I", d, 0x40)[0], ()):
            world = [place(d, p) for p in points]
            if kind == "face":
                world = [tuple(sum(p[i] for p in world) / len(world) for i in range(3))]
            out.setdefault(word, {}).setdefault(kind, []).extend(world)
    return out


def topmost(groups, x, z):
    """The topmost collidable surface (smallest y: +y is down) over (x, z)."""
    best = None
    for x0, x1, z0, z1, tris in groups:
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


def cmd_selftest(args):
    """The readers and helpers on hand-made records (no disc needed)."""
    from collections import Counter
    assert name_hash("playerone") == 0x06DA4725
    # A tag buffer: two 8-byte edge records behind three indices.
    edge = struct.pack("<2I", (2 << 14) | (8 << 4) | 1, 3) + struct.pack("<3H", 0, 1, 0xFFFF) + b"\0" * 10
    edge += bytes(range(8)) + bytes(range(8, 16))
    _, count, _, records = tag_buffer(edge)
    assert count == 3 and records == [bytes(range(8)), bytes(range(8, 16))]
    # A split wrapped past s16 (z -440.00 stored as 215.36) back in the
    # bounds, then moved by the recentring offset.
    wrapped = struct.pack("<BBhHH", 3, 0, 21536, 2, 4) + struct.pack("<BBH", 0, 0, 0) + b"\0" * 4 + struct.pack("<BBH", 0, 0, 0)
    fixed = unwrap_splits(wrapped, (-10.0, -10.0, -463.0), (10.0, 10.0, -414.0), (0.0, 0.0, -438.0))
    assert struct.unpack_from("<h", fixed, 2)[0] == -200 and fixed[4:] == wrapped[4:]
    # A recentred object: its part's positions and bounds move by the offset.
    head = struct.pack("<6f", -10.0, -1.0, -463.0, 10.0, 1.0, -414.0) + struct.pack("<I", 7) + b"\0" * 36
    part = Chunk(b"TGPT", kids=[Chunk(b"TGVP", struct.pack("<3f", 1.0, 0.0, -440.0))])
    obj = Chunk(b"TGEO", kids=[Chunk(b"TGHD", head), part, Chunk(b"TGCI", struct.pack("<2I", 7, len(wrapped)) + wrapped)])
    obj = parse(serialize([obj]))[0]
    assert recentre_object(obj) == (0.0, 0.0, -438.0) and object_head(obj)[1] == 7
    kids = {k.tag: k for k in obj.kids}
    assert struct.unpack_from("<3f", kids[b"TGPT"].kids[0].data) == (1.0, 0.0, -2.0)
    assert struct.unpack_from("<6f", kids[b"TGHD"].data) == (-10.0, -1.0, -25.0, 10.0, 1.0, 24.0)
    # BMPs: an 8-bit one written and read back.
    assert decode_bmp(bmp8([[0, 1], [1, 0]], [(1, 2, 3, 4), (5, 6, 7, 8)]))[2] == [[(1, 2, 3, 4), (5, 6, 7, 8)], [(5, 6, 7, 8), (1, 2, 3, 4)]]
    palette, index = median_cut(Counter({(0, 0, 0, 255): 3, (255, 255, 255, 255): 1}), 2)
    assert len(palette) == 2 and index[(0, 0, 0, 255)] != index[(255, 255, 255, 255)]
    print("dm2_level selftest: ok")


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("selftest")
    t.set_defaults(fn=cmd_selftest)
    i = sub.add_parser("info")
    i.add_argument("files", nargs="+")
    i.set_defaults(fn=cmd_info)
    args = p.parse_args(argv)
    args.fn(args)


if __name__ == "__main__":
    main(sys.argv[1:])
