#!/usr/bin/env python3
"""Dave Mirra Freestyle BMX 2's level files written: the ZGE2 chunks of a
level's GEO, INS, DAT, ZIT and TXR as typed records (ai_level_write.py's, with
DM2's forms where ZGE2 differs), and the round trips that prove them on the
stock levels.

    python3 -I dm2_level_write.py roundtrip --disc <DM2 disc> [--xbox-levels DIR] [--level L ...] [--verbose]
                                       every level folder's GEO, INS, DAT, ZIT and TXR read, written, read back
                                       and compared, chunk by chunk and as whole files; --xbox-levels adds
                                       Venice and Greenville as dm2_xbox.py wrote them (DIR/<LEVEL>/)
    python3 -I dm2_level_write.py rewrite --disc <DM2 disc> --level L --out <folder>
                                         [--rebuild-trees] [--clean]
                                       the level written through the writers into <folder>/ASSETS/LEVELS/L/
                                       (EDITOR/L for a park theme: a Mods folder's layout); --rebuild-trees builds every collision tree
                                       here (over the faces the stock tree lists), --clean writes 0 in the
                                       bytes the game never reads
    python3 -I dm2_level_write.py selftest   a small level made from records, written, read back

What ZGE2 has that ZGE3 (ai_level_write.py) has not, each from DM2's own
loaders and readers:
- TGCI: the leaves keep their faces inline (zge/collision/collision.h
  InlineLeafTree: {0, a byte, u16 count, the u16 faces}, padded to 4), the
  nodes in preorder, the greater side first (InlineCollisionTree). Every
  DM2 tree lists exactly the part's faces with 0x2000.
- TGFE and TGFT: 8- and 12-byte records (zge_tags.py, shared with BMX XXX;
  games/dm2/rider/collision/tags.cpp reads them).
- TGIH: 0x6C bytes, no region word, the name at +0x4C (zge/scene/node.h
  InstanceChunk, loadPlacedInstance).
- SDPT: the point's index is a word (zge/scene/locators.h SdptChunk).
- PVSO: the older body without the flag word, the only one DM2 reads.
- No indexed streams (TIV*, TIFN): DM2 registers none (geometry_nodes.cpp);
  every part has its face normals plain (TGFN).
- MATI: 0x9C bytes (AI's 0xA0; kept as chunks, dm2_xbox.py's convert_mati).
- TGFM: no 0x4000 winding flag on any entry (Part.unwind).

Bytes the exporter left and the game never reads, kept in the records so
the stock files round trip exactly (--clean writes 0): a tree node's
second byte, the two bytes after an odd leaf's faces, TGFT bytes 10-11.
Every other padding byte in the stock files is zero, and the writers write
zero.

Python 3.8+, standard library only. Never commit what this writes from a
disc: it is the disc's data.
"""
import argparse
import dataclasses
import os
import struct
import sys
from dataclasses import dataclass
from typing import List

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ai_level as L  # noqa: E402  (chunk trees, the name hash; the disc)
import ai_level_write as W  # noqa: E402  (ZGE3's records, the base of these)
import dm2_level as D  # noqa: E402  (DM2's readers: collision triangles, tag buffers)
import zge_scene as Z  # noqa: E402  (the ZIT, the same in all three games)
import zge_tags as T  # noqa: E402  (the bike games' 8- and 12-byte tag records)

Chunk = L.Chunk


# ---- GEO ---------------------------------------------------------------------------

class Part(W.Part):
    """TGPT as ZGE2 has it: plain streams only, TGFE and TGFT of 8- and 12-byte records."""
    EDGE_TAG = T.EdgeTag8
    FACE_TAG = T.FaceTag12
    # The PS2 exporter's order (dm2_xbox.py PS2_PART_ORDER: every stock part).
    DEFAULT_ORDER = [b"TGPH", b"TGVP", b"TGVN", b"TGVC", b"TGVU", b"TGFP", b"TGFN", b"TGFM", b"TGFE", b"TGFT"]

    def write(self):
        for name, (plain, indexed, stride, per) in W.STREAMS.items():
            if isinstance(getattr(self, name), W.IndexedStream):
                raise ValueError("part %d: DM2 reads no indexed streams (%s): expand_streams first" % (self.id, indexed.decode()))
        if any(e & W.StripGroup.FLIPPED for g in self.groups for e in g.entries):
            raise ValueError("part %d: a strip entry with 0x4000: DM2's strips have no winding flag (unwound)" % self.id)
        return super().write()

    def unwind(self):
        """The strips' 0x4000 flags (ZGE3's winding mark) cleared: DM2's strips have none (0 in all 210,553 entries of
        Woodward and Warehouse, and every stock part writes without one), a triangle's winding is its parity after
        the strip's restart; with the flag left on, DM2 drew the vertex 16,384 past the index (spikes across the
        screen)."""
        for g in self.groups:
            g.entries = [e & ~W.StripGroup.FLIPPED for e in g.entries]


@dataclass
class InlineLeaf(W.FaceTreeLeaf):
    """A ZGE2 leaf: the faces inline after the count. `tail` is the two bytes after an odd count's faces,
    up to the next node (the exporter's leftovers; the walk never reads them)."""
    tail: bytes = b"\0\0"


@dataclass
class InlineCollisionTree(W.CollisionTree):
    """TGCI as ZGE2 has it: {u32 part id, u32 bytes, the tree}. The nodes are 8-byte splits (zge::FaceTreeSplit:
    the axis, a byte, the plane in hundredths, the greater and lesser child as word offsets) and inline
    leaves (zge::InlineLeafTree: 0, a byte, the count, the faces, padded to 4 bytes), in preorder, the
    greater side first. DM2's queries need a tree on every object that collides (none makes one)."""

    @staticmethod
    def read(data):
        part_id, size = struct.unpack_from("<2I", data, 0)
        blob = data[8:8 + size]
        if len(blob) != size or len(data) != (8 + size + 3) & ~3:
            raise ValueError("TGCI of %d bytes for a tree of %d" % (len(data) - 8, size))

        def node(at, depth):
            if depth > 64:
                raise ValueError("collision tree deeper than 64")
            kind, extra = blob[at], blob[at + 1]
            if kind == 0:
                count = struct.unpack_from("<H", blob, at + 2)[0]
                end = at + 4 + 2 * count
                return InlineLeaf(list(struct.unpack_from("<%dH" % count, blob, at + 4)), extra, bytes(blob[end:end + 2]) if count & 1 else b"")
            if kind > 3:
                raise ValueError("collision tree node of type %d" % kind)
            plane, greater, lesser = struct.unpack_from("<hHH", blob, at + 2)
            return W.FaceTreeSplit(kind, plane, node(greater * 4, depth + 1), node(lesser * 4, depth + 1), extra)

        tree = InlineCollisionTree(part_id, node(0, 0))
        if tree.write()[8:] != data[8:]:
            raise ValueError("TGCI not in the exporter's layout (preorder, greater side first)")
        return tree

    def write(self):
        out = bytearray()

        def emit(n):
            at = len(out)
            if isinstance(n, W.FaceTreeLeaf):
                out.extend(struct.pack("<BBH%dH" % len(n.faces), 0, n.leftover, len(n.faces), *n.faces))
                if len(n.faces) & 1:
                    out.extend((getattr(n, "tail", b"") or b"\0\0").ljust(2, b"\0")[:2])
                return
            out.extend(struct.pack("<BBhHH", n.axis, n.leftover, n.plane, 0, 0))
            emit(n.greater)
            lesser = len(out)
            emit(n.lesser)
            if lesser // 4 > 0xFFFF:
                raise ValueError("collision tree of %d bytes: past the u16 word offsets" % len(out))
            struct.pack_into("<HH", out, at + 4, (at + 8) // 4, lesser // 4)

        emit(self.root)
        return struct.pack("<2I", self.part_id, len(out)) + bytes(out)

    @staticmethod
    def build(part_id, positions, faces, extra=(), only=None, leaf_faces=W.CollisionTree.LEAF_FACES, depth=W.CollisionTree.DEPTH):
        """ai_level_write's builder (median splits in hundredths, leaves of 8, depth 11) in DM2's form. (DM2's
        exporter split otherwise: its trees reach depth 11 too but with larger leaves and many empty ones.)"""
        tree = W.CollisionTree.build(part_id, positions, faces, extra, only, leaf_faces, depth)
        if tree is None:
            return None

        def convert(n):
            if isinstance(n, W.FaceTreeLeaf):
                return InlineLeaf(n.faces)
            return W.FaceTreeSplit(n.axis, n.plane, convert(n.greater), convert(n.lesser))

        return InlineCollisionTree(part_id, convert(tree.root))

    def clean(self):
        """The bytes the walk never reads written 0."""
        for n in self.nodes():
            n.leftover = 0
            if isinstance(n, InlineLeaf):
                n.tail = b"\0\0"

    def nodes(self):
        out, stack = [], [self.root]
        while stack:
            n = stack.pop()
            out.append(n)
            if isinstance(n, W.FaceTreeSplit):
                stack += [n.lesser, n.greater]
        return out


class GeoObject(W.GeoObject):
    PART = Part
    TREE = InlineCollisionTree


class Geometry(W.Geometry):
    """A GEO file: FORM { TGEO ..., ZDYN ... } (ZDYN, the dynamics bodies, kept as chunks)."""
    OBJECT = GeoObject


# ---- INS ---------------------------------------------------------------------------

@dataclass
class Instance(W.Instance):
    """TGIH as ZGE2 has it (0x6C bytes; zge::InstanceChunk, loadPlacedInstance): AI's without the region word,
    the name at +0x4C. `flags` become the node's state word (bits 0-4 as they are, 5-10 moved up two): 0x2
    collides, 0x10 with it not collided with, 0x40 drawn (0x23 and 0x63 in the stock levels)."""
    SIZE = 0x6C

    @staticmethod
    def read(data, tags=None):
        if len(data) != Instance.SIZE:
            raise ValueError("TGIH of %d bytes (DM2's are 0x6C)" % len(data))
        flags, level, mset, pad = struct.unpack_from("<IBBH", data, 0)
        if pad:
            raise ValueError("TGIH with its padding set")
        name_hash, geo, parent, me = struct.unpack_from("<4I", data, 0x3C)
        return Instance(flags, struct.unpack_from("<4f", data, 8), struct.unpack_from("<3f", data, 0x18), geo, me,
                        W.c_string(data[0x4C:0x6C]), name_hash, parent, 0, level, mset,
                        struct.unpack_from("<3f", data, 0x24), struct.unpack_from("<3f", data, 0x30), tags)

    def write(self):
        if self.region:
            raise ValueError("instance %s: DM2's TGIH has no region word" % self.name)
        return struct.pack("<IBBH4f3f3f3f4I", self.flags, self.level, self.material_set, 0, *self.rotation, *self.position,
                           *self.world_min, *self.world_max, self.name_hash, self.geometry_id, self.parent,
                           self.instance_id) + W.c_field(self.name, 32)

    @staticmethod
    def make(name, geometry, position, rotation=(0.0, 0.0, 0.0, 1.0), instance_id=0, collides=True, tags=None):
        return Instance(**{f.name: getattr(W.Instance.make(name, geometry, position, rotation, instance_id, collides, tags), f.name)
                           for f in dataclasses.fields(W.Instance)})


class Instances(W.Instances):
    INSTANCE = Instance


# ---- DAT ---------------------------------------------------------------------------

@dataclass
class ScenePoint(W.ScenePoint):
    """SDPT as ZGE2 has it: the index (findScenePoint's third key) a word."""

    @staticmethod
    def read(data):
        header, kind, name, index = struct.unpack_from("<4I", data, 0)
        if header != ScenePoint.HEADER:
            raise ValueError("SDPT header of %d bytes" % header)
        return ScenePoint(kind, name, struct.unpack_from("<3f", data, 0x10), struct.unpack_from("<4f", data, 0x1C), index, data[header:])

    def write(self):
        return struct.pack("<4I3f4f", self.HEADER, self.kind, self.name, self.index, *self.position, *self.orientation) + self.tags

    @staticmethod
    def spawn(name, position, orientation=(0.0, 0.0, 0.0, 1.0), tags=b""):
        return ScenePoint(W.SPAWN, L.name_hash(name), position, orientation, tags=tags)


class SceneData(W.SceneData):
    """A DAT file: FORM { SDPT ..., ZLIT (empty), ZPVS { PVSO ... } } (ZPVS in the levels that have zones; DM2
    reads PVSO's older body: write ViewRegion(..., flag=None))."""
    POINT = ScenePoint

    def write(self):
        if self.regions and any(r.flag is not None for r in self.regions):
            raise ValueError("DM2 reads PVSO's older body only: a ViewRegion's flag must be None")
        return super().write()


# ---- ZIT ---------------------------------------------------------------------------

# A ZIT is the same form in all three games: zge_scene.SceneFile.
SceneFile = Z.SceneFile


# ---- TXR ---------------------------------------------------------------------------

@dataclass
class Texture:
    """A TXRC: its TXRH (52 bytes; zge::TextureHeaderChunk: the id, which is the name's hash in every stock
    texture, the size, two reserved halfwords the texture keeps and nothing reads (0 in every PS2 file; the
    Xbox's +0x0A is 1 on Venice's and Greenville's two reflection maps, dm2_xbox.py copies it), 8 zero bytes,
    the name) and its levels, each a TBMP: a Windows BMP of 4 or 8 bits a pixel (a few of 24) with a
    palette whose alpha is 0 (transparency is the colour key, magenta: dm2_xbox.py), the largest first, every
    level the chunk padded to 4 bytes. The BMPs are kept as their bytes (dm2_level.bmp8 makes one)."""
    id: int
    width: int
    height: int
    name: str
    levels: List[bytes]
    reserved08: int = 0
    reserved0a: int = 0

    @staticmethod
    def read(chunk):
        if chunk.tag != b"TXRC" or not chunk.kids or chunk.kids[0].tag != b"TXRH":
            raise ValueError("a texture is TXRC { TXRH, TBMP ... }")
        head = chunk.kids[0].data
        if len(head) != 0x34 or any(head[0x0C:0x14]):
            raise ValueError("TXRH of %d bytes or with its padding set" % len(head))
        tid, w, h, r08, r0a = struct.unpack_from("<IHHHH", head, 0)
        levels = []
        for k in chunk.kids[1:]:
            if k.tag != b"TBMP" or k.data[:2] != b"BM":
                raise ValueError("a %r level in a texture" % k.tag)
            size = struct.unpack_from("<I", k.data, 2)[0]
            if len(k.data) != (size + 3) & ~3 or any(k.data[size:]):
                raise ValueError("a TBMP of %d bytes for a BMP of %d" % (len(k.data), size))
            levels.append(k.data[:size])
        return Texture(tid, w, h, W.c_string(head[0x14:]), levels, r08, r0a)

    def chunk(self):
        head = struct.pack("<IHHHH", self.id, self.width, self.height, self.reserved08, self.reserved0a) + b"\0" * 8 + W.c_field(self.name, 32)
        return Chunk(b"TXRC", kids=[Chunk(b"TXRH", head)] + [Chunk(b"TBMP", W.pad_to(bmp, 4)) for bmp in self.levels])

    @staticmethod
    def make(name, rows, palette):
        """A one-level texture of palette indices (rows top first), its id the name's hash."""
        return Texture(L.name_hash(name), len(rows[0]), len(rows), name, [D.bmp8(rows, [(r, g, b, 0) for r, g, b, *_ in palette])])


@dataclass
class TextureLibrary:
    """A TXR file: FORM { TXRC ... }."""
    textures: List[Texture]

    @staticmethod
    def read(data):
        tree = L.parse(data)
        if len(tree) != 1 or tree[0].tag != b"FORM":
            raise ValueError("a TXR is one FORM")
        return TextureLibrary([Texture.read(c) for c in tree[0].kids])

    def write(self):
        return L.serialize([Chunk(b"FORM", kids=[t.chunk() for t in self.textures])])


# ---- the round trip -------------------------------------------------------------------

LEVEL_FOLDERS = ["LEVELS/AIRPORT", "LEVELS/CLOVERLE", "LEVELS/COMMERCI", "LEVELS/DEVILSPE", "LEVELS/GALLOON",
                 "LEVELS/SWAMPYTR", "LEVELS/TRAINYAR", "LEVELS/WOODWARD",
                 "EDITOR/LEAIRPOR", "EDITOR/LEBEACH", "EDITOR/LECOMMER", "EDITOR/LECOMP", "EDITOR/LEGALLON",
                 "EDITOR/LESWAMP", "EDITOR/LETRAIN", "EDITOR/LEWAREHO", "EDITOR/LEWOOD"]
XBOX_LEVELS = ["VENICE", "GREENVIL"]
EXTENSIONS = ("ZIT", "GEO", "INS", "DAT", "TXR")


class FolderFiles:
    """A level folder on disc, the DM2 disc (folder or .iso) or a folder of dm2_xbox.py's output."""

    def __init__(self, disc, folder):
        self.disc, self.folder = disc, folder
        self.name = folder.rsplit("/", 1)[-1]

    def read(self, name):
        return self.disc.read("ASSETS/%s/%s" % (self.folder, name)) if isinstance(self.disc, L.Disc) else self.read_file(name)

    def read_file(self, name):
        path = os.path.join(self.disc, self.folder, name)
        if not os.path.isfile(path):
            return None
        with open(path, "rb") as f:
            return f.read()

    def files(self):
        out = {}
        for ext in EXTENSIONS:
            data = self.read("%s.%s" % (self.name, ext))
            if data is None:
                raise ValueError("%s/%s.%s is missing" % (self.folder, self.name, ext))
            out[ext] = data
        return out


def roundtrip_geo(data, rt):
    tree = L.parse(data)
    geo = Geometry.read(data)
    for obj, rec in zip([k for k in tree[0].kids if k.tag == b"TGEO"], geo.objects):
        rt.chunk(b"TGHD", next(k.data for k in obj.kids if k.tag == b"TGHD"), rec.header.write())
        for k in obj.kids:
            if k.tag == b"TGCI":
                rt.chunk(b"TGCI", k.data, rec.collision.write())
            elif k.tag == b"TGLO":
                rt.chunk(b"TGLO", k.data, W.LevelOfDetail.read(k.data).write())
        for part, (prec, _, _) in zip([k for k in obj.kids if k.tag == b"TGPT"], rec.parts):
            chunks = prec.chunks()
            for k in part.kids:
                rt.chunk(k.tag, k.data, chunks[k.tag].data)
            rt.chunk(b"TGPT", L.serialize([part]), L.serialize([prec.write()]))
        rt.chunk(b"TGEO", L.serialize([obj]), L.serialize([rec.write()]))
    out = geo.write()
    rt.files["GEO"] = (data == out, W.first_difference(data, out), len(data), len(out))
    return geo


def roundtrip_ins(data, rt):
    tree = L.parse(data)
    ins = Instances.read(data)
    for g, rec in zip(tree[0].kids, ins.instances):
        rt.chunk(b"TGIH", g.kids[0].data, rec.write())
        rt.chunk(b"TGIN", L.serialize([g]), L.serialize([rec.chunk()]))
    out = ins.write()
    rt.files["INS"] = (data == out, W.first_difference(data, out), len(data), len(out))
    return ins


def roundtrip_dat(data, rt):
    tree = L.parse(data)
    dat = SceneData.read(data)
    points = iter(dat.points)
    for k in tree[0].kids:
        if k.tag == b"SDPT":
            rt.chunk(b"SDPT", k.data, next(points).write())
        elif k.tag == b"ZPVS":
            for p, r in zip(k.kids, dat.regions):
                rt.chunk(b"PVSO", p.data, r.write())
    out = dat.write()
    rt.files["DAT"] = (data == out, W.first_difference(data, out), len(data), len(out))
    return dat


def roundtrip_zit(data, rt):
    zit = SceneFile.read(data)
    out = zit.write()
    rt.chunk(b"TEHD", next(c.data for c in L.walk(L.parse(data)) if c.tag == b"TEHD"), next(c.data for c in L.walk(L.parse(out)) if c.tag == b"TEHD"))
    rt.files["ZIT"] = (data == out, W.first_difference(data, out), len(data), len(out))
    return zit


def roundtrip_txr(data, rt):
    tree = L.parse(data)
    txr = TextureLibrary.read(data)
    for c, t in zip(tree[0].kids, txr.textures):
        rt.chunk(b"TXRC", L.serialize([c]), L.serialize([t.chunk()]))
    out = txr.write()
    rt.files["TXR"] = (data == out, W.first_difference(data, out), len(data), len(out))
    return txr


def readers_agree(geo_a, ins_a, geo_b, ins_b):
    """DM2's readers (dm2_level.py's collision surface, dm2_xbox.py's grind edges and goal places) give the same
    from one pair of files as from the other."""
    import dm2_xbox
    a, b = D.collision_triangles(geo_a, ins_a), D.collision_triangles(geo_b, ins_b)
    if a != b:
        raise ValueError("the collision triangles read back differ")
    ea, eb = dm2_xbox.grind_edges(geo_a, ins_a), dm2_xbox.grind_edges(geo_b, ins_b)
    if ea != eb:
        raise ValueError("the grind edges read back differ")
    if D.goal_places(geo_a, ins_a) != D.goal_places(geo_b, ins_b):
        raise ValueError("the goal places read back differ")
    return len(a), sum(len(g[4]) for g in a), len(ea)


def level_sources(args, levels=None):
    disc = L.Disc(args.disc)
    want = {l.upper() for l in levels} if levels else None
    out = []
    for folder in LEVEL_FOLDERS:
        if want is None or folder.rsplit("/", 1)[-1] in want:
            out.append(FolderFiles(disc, folder))
    if args.xbox_levels:
        for name in XBOX_LEVELS:
            if want is None or name in want:
                out.append(FolderFiles(args.xbox_levels, name))
    return out


def cmd_roundtrip(args):
    identical = True
    totals = W.RoundTrip()
    for src in level_sources(args, args.level):
        files = src.files()
        rt = W.RoundTrip()
        geo = roundtrip_geo(files["GEO"], rt)
        ins = roundtrip_ins(files["INS"], rt)
        dat = roundtrip_dat(files["DAT"], rt)
        roundtrip_zit(files["ZIT"], rt)
        txr = roundtrip_txr(files["TXR"], rt)
        groups, tris, edges = readers_agree(files["GEO"], files["INS"], geo.write(), ins.write())
        parts = sum(len(o.parts) for o in geo.objects)
        trees = sum(1 for o in geo.objects if o.collision)
        print("%s: %d objects, %d parts, %d trees, %d instances, %d points, %s zones, %d textures; %d collidable instances "
              "(%d triangles), %d grind edges and the goal places read back the same" %
              (src.folder, len(geo.objects), parts, trees, len(ins.instances), len(dat.points),
               len(dat.regions) if dat.regions is not None else "no", len(txr.textures), groups, tris, edges))
        for ext in EXTENSIONS:
            same, at, n_in, n_out = rt.files[ext]
            identical &= same
            print("  %s: %s (%d bytes)" % (ext, "byte-identical" if same else "differs at 0x%x (%d written)" % (at, n_out), n_in))
        if args.verbose or rt.differ:
            print("\n".join(rt.report()))
        totals.same.update(rt.same)
        totals.differ.update(rt.differ)
    print("chunks over all folders:")
    print("\n".join(totals.report()))
    print("all files byte-identical" if identical else "some files differ")
    return 0 if identical else 1


def clean(geo):
    """The bytes the game never reads written 0 (the records' leftovers). Returns how many fields changed."""
    changed = 0
    for obj in geo.objects:
        if obj.collision is not None:
            before = obj.collision.write()
            obj.collision.clean()
            changed += obj.collision.write() != before
        for part, _, _ in obj.parts:
            if part.face_tags is not None:
                for r in part.face_tags.records:
                    changed += r.leftover != b"\0\0"
                    r.leftover = b"\0\0"
    return changed


def cmd_rewrite(args):
    src = next(iter(level_sources(args, [args.level])), None)
    if src is None:
        raise ValueError("%s is not a DM2 level folder (%s)" % (args.level, ", ".join(f.rsplit("/", 1)[-1] for f in LEVEL_FOLDERS)))
    files = src.files()
    geo, ins, dat, zit = Geometry.read(files["GEO"]), Instances.read(files["INS"]), SceneData.read(files["DAT"]), SceneFile.read(files["ZIT"])
    if args.rebuild_trees:
        rebuilt = 0
        for obj in geo.objects:
            if obj.collision is not None:
                part = obj.collision_part()
                tree = InlineCollisionTree.build(part.id, part.positions, part.faces, only=obj.collision.faces())
                if tree is not None:
                    obj.collision, rebuilt = tree, rebuilt + 1
        print("variant: %d collision trees built here over the stock trees' faces" % rebuilt)
    if args.clean:
        print("variant: %d records' unread bytes written 0" % clean(geo))
    written = {"GEO": geo.write(), "INS": ins.write(), "DAT": dat.write(), "ZIT": zit.write(),
               "TXR": TextureLibrary.read(files["TXR"]).write()}
    groups, tris, edges = readers_agree(files["GEO"], files["INS"], written["GEO"], written["INS"])
    print("%d collidable instances (%d triangles), %d grind edges and the goal places read back the same" % (groups, tris, edges))
    out_dir = os.path.join(args.out, "ASSETS", *src.folder.split("/"))
    os.makedirs(out_dir, exist_ok=True)
    for ext, data in written.items():
        with open(os.path.join(out_dir, "%s.%s" % (src.name, ext)), "wb") as f:
            f.write(data)
        print("%s.%s: %d bytes%s" % (src.name, ext, len(data), "" if data == files[ext] else " (not the disc's bytes)"))


# ---- a level from records ---------------------------------------------------------------

def make_test_level():
    """A floor of two triangles with a grindable edge and a quarter-pipe face tag, one instance, a PlayerOne spawn
    and one zone: the smallest level these writers make."""
    positions = [(-10.0, 0.0, -10.0), (10.0, 0.0, -10.0), (10.0, 0.0, 10.0), (-10.0, 0.0, 10.0)]
    faces = [W.Face.make(0, 0, 1, 2), W.Face.make(0, 0, 2, 3)]
    up = struct.pack("<3f", 0.0, -1.0, 0.0)
    part = Part(1, positions, faces, W.StripGroup.from_strip(0, [0, 1, 3, 2]), normals=up * 4, colours=b"\x80\x80\x80\x80" * 4,
                uvs=b"".join(struct.pack("<2f", u, v) for u, v in ((0, 0), (1, 0), (1, 1), (0, 1))), face_normals=up * 2,
                edge_tags=W.TagBuffer([T.EdgeTag8(L.name_hash("ledge"), flags=T.EdgeTag8.GRINDABLE)], [0] + [None] * 5),
                face_tags=W.TagBuffer([T.FaceTag12(0, surface_kind=1)], [0, None]), material_count=1)
    part.unwind()
    obj = GeoObject(W.GeoHeader((-10.0, 0.0, -10.0), (10.0, 0.0, 10.0), 1, "floor"), b"\0\0\x01\0",
                    [(part, W.LevelOfDetail(500.0, 1), Chunk(b"TMTB", kids=[]))], type_data=struct.pack("<I", L.name_hash("Terrain")),
                    collision=InlineCollisionTree.build(1, positions, faces))
    geo = Geometry([obj])
    ins = Instances([Instance.make("floor_01", obj, (0.0, 0.0, 0.0), tags=struct.pack("<I", L.name_hash("Terrain")))])
    ins.instances[0].flags = 0x23
    dat = SceneData([ScenePoint.spawn("PlayerOne", (0.0, -1.0, 0.0), tags=b"\0" * 4)],
                    [W.ViewRegion(L.name_hash("zone_a"), (-20.0, -20.0, -20.0), (20.0, 20.0, 20.0), [], None)])
    zit = SceneFile.make("TEST", (-50.0, -50.0, -50.0), (50.0, 50.0, 50.0))
    return geo, ins, dat, zit, TextureLibrary([Texture.make("floor", [[0, 1], [1, 0]], [(10, 20, 30), (200, 100, 50)])])


def cmd_selftest(args):
    geo, ins, dat, zit, txr = make_test_level()
    t = txr.write()
    assert TextureLibrary.read(t).write() == t and len(L.parse(t)[0].kids[0].kids[1].data) % 4 == 0
    assert D.decode_bmp(TextureLibrary.read(t).textures[0].levels[0])[2][0][1][:3] == (200, 100, 50)
    g, i, d, z = geo.write(), ins.write(), dat.write(), zit.write()
    g2, i2, d2, z2 = Geometry.read(g), Instances.read(i), SceneData.read(d), SceneFile.read(z)
    assert g2.write() == g and i2.write() == i and d2.write() == d and z2.write() == z
    tris = D.collision_triangles(g, i)
    assert len(tris) == 1 and len(tris[0][4]) == 2, tris
    assert D.topmost(tris, 0.0, 0.0) == 0.0
    import dm2_xbox
    assert dm2_xbox.grind_edges(g, i) == [((-10.0, 0.0, -10.0), (10.0, 0.0, -10.0), "floor_01", 0)], dm2_xbox.grind_edges(g, i)
    assert len(i2.instances[0].write()) == 0x6C and i2.instances[0].name == "floor_01"
    assert len(d2.points[0].write()) == 0x30 and d2.points[0].name == W.PLAYER_ONE and d2.regions[0].flag is None
    part = g2.objects[0].parts[0][0]
    assert isinstance(part.edge_tags.records[0], T.EdgeTag8) and part.face_tags.records[0].surface_kind == 1
    # An inline tree: a split over two leaves of odd counts keeps its tails, in preorder with the greater side first.
    tree = InlineCollisionTree(7, W.FaceTreeSplit(1, 150, InlineLeaf([4], 0, b"\x12\x34"), InlineLeaf([1, 2, 3], 9, b"\xab\xcd"), 5))
    raw = tree.write()
    assert raw[8:16] == struct.pack("<BBhHH", 1, 5, 150, 2, 4) and raw[16:24] == struct.pack("<BBHH", 0, 0, 1, 4) + b"\x12\x34"
    assert len(raw) == 8 + 8 + 8 + 12 and InlineCollisionTree.read(raw) == tree and D.tree_faces(raw) == {1, 2, 3, 4}
    tree.clean()
    assert tree.write()[16:24] == struct.pack("<BBHH", 0, 0, 1, 4) + b"\0\0"
    # Indexed streams are refused; expanded they write.
    part.colours = W.IndexedStream.make([b"\x80\x80\x80\x80"] * 4)
    try:
        part.write()
        raise AssertionError("an indexed stream was written for DM2")
    except ValueError:
        pass
    part.expand_streams()
    assert Geometry.read(g2.write()).write() == g
    print("dm2_level_write selftest: ok (%d + %d + %d + %d bytes)" % (len(g), len(i), len(d), len(z)))


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    for name, fn in (("roundtrip", cmd_roundtrip), ("rewrite", cmd_rewrite)):
        s = sub.add_parser(name)
        s.add_argument("--disc", required=True, help="the DM2 PS2 disc (folder or .iso)")
        s.add_argument("--xbox-levels", help="dm2_xbox.py convert's output folder (VENICE/, GREENVIL/)")
        s.set_defaults(fn=fn)
        if name == "roundtrip":
            s.add_argument("--level", action="append")
            s.add_argument("--verbose", action="store_true")
        else:
            s.add_argument("--level", required=True)
            s.add_argument("--out", required=True)
            s.add_argument("--rebuild-trees", action="store_true", help="collision trees built here, over the stock trees' faces")
            s.add_argument("--clean", action="store_true", help="the bytes the game never reads written 0")
    t = sub.add_parser("selftest")
    t.set_defaults(fn=cmd_selftest)
    args = p.parse_args(argv)
    try:
        code = args.fn(args)
    except ValueError as e:
        raise SystemExit("dm2_level_write.py: %s" % e)
    return code or 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
