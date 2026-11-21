#!/usr/bin/env python3
"""Aggressive Inline's level files written: the ZGE3 chunks of a level's GEO,
INS and DAT as typed records (read from the chunk trees ai_level.py parses,
written back as bytes the game loads), and the round trips that prove them
on the stock levels.

    python3 -I ai_level_write.py roundtrip --disc <AI disc> [--level L ...] [--verbose]
                                       every level's GEO, INS and DAT read, written, read back and
                                       compared, chunk by chunk and as whole files
    python3 -I ai_level_write.py rewrite --disc <AI disc> --level L --out <folder>
                                         [--rebuild-trees stock|all] [--plain-streams]
                                       the level written through the writers into
                                       <folder>/ASSETS/LEVELS/L/ (a Mods folder's layout), with the
                                       ZIT, TXR, scripts and MOTIONS.ZAL copied; the options write
                                       the same geometry in the writers' other encodings (collision
                                       trees built here, indexed streams written plain)
    python3 -I ai_level_write.py selftest   a small level made from records, written, read back

The layouts are the engine's loaders' (engine/zge/assets/geo.cpp,
scene/geometry_nodes.cpp, scene/scene_points.cpp, assets/chunk_reader.cpp,
collision/collision.h), which keep the vertex, face, strip, tag-buffer and
collision-tree chunks as they are on disc: those are byte-identical in a
round trip. Three fields the exporter wrote and the game never reads are
kept in the records so the stock files round trip exactly (EdgeTag.byte_0a,
FaceTag.leftover, the face tree nodes' leftover byte); every other padding
byte is zero in every stock file, and the writers write zero.

Python 3.8+, standard library only.
"""
import argparse
import os
import struct
import sys
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ai_level as L  # noqa: E402

Chunk = L.Chunk


# ---- small helpers ----------------------------------------------------------------

def pad_to(data, n):
    return data + b"\0" * (-len(data) % n)


def c_string(raw):
    """A fixed char[] field: the text before the NUL."""
    return raw.split(b"\0", 1)[0].decode("latin1")


def c_field(text, size):
    enc = text.encode("latin1")
    if len(enc) > size:
        raise ValueError("name %r longer than %d" % (text, size))
    return enc.ljust(size, b"\0")


# ---- GEO: objects ---------------------------------------------------------------

@dataclass
class GeoHeader:
    """TGHD (0x40 bytes; zge::GeoHeaderChunk): the object's bounds in its own space, its id, name and name hash.

    The name field keeps at most NAME_CHARS characters and its NUL; the hash is of the whole name, as the
    exporter wrote it (types1: the word is nameHash(name) on every stock TGHD of the three games' discs, and for
    the names cut at 31 characters the hash of the uncut name, e.g. PropDef_vert-truss_short_carplant). A header
    made with a name and no hash gets that: the hash of the name given, the field the name cut to NAME_CHARS. A
    header read keeps the file's name and word."""
    bounds_min: Tuple[float, float, float]
    bounds_max: Tuple[float, float, float]
    id: int                 # findGeometryById: what a TGIH's geometry_id names
    name: str               # the name field: at most NAME_CHARS characters once made
    name_hash: Optional[int] = None  # nameHash of the whole name (GeoObject::nameHash): what findWorldObjectByHash
                                     # and cloneDynamicsDefinition look up; None: the hash of `name`, made here

    NAME_CHARS = 31

    def __post_init__(self):
        if self.name_hash is None:
            self.name_hash = L.name_hash(self.name)
            self.name = self.name[:self.NAME_CHARS]

    @staticmethod
    def read(data):
        if len(data) != 0x40:
            raise ValueError("TGHD of %d bytes" % len(data))
        mn = struct.unpack_from("<3f", data, 0)
        mx = struct.unpack_from("<3f", data, 0xC)
        return GeoHeader(mn, mx, struct.unpack_from("<I", data, 0x18)[0], c_string(data[0x1C:0x3C]), struct.unpack_from("<I", data, 0x3C)[0])

    def write(self):
        return struct.pack("<3f3fI", *self.bounds_min, *self.bounds_max, self.id) + c_field(self.name, 32) + struct.pack("<I", self.name_hash)


@dataclass
class Face:
    """A TGFP record (8 bytes; zge::Face): flags (low byte the material, bits 8-11 the vertex count 3,
    0x2000 collides, 0x4000 two-sided) and three vertex indices."""
    flags: int
    vertices: Tuple[int, int, int]

    COLLIDES = 0x2000
    TWO_SIDED = 0x4000

    @staticmethod
    def read(data, at):
        f, a, b, c = struct.unpack_from("<4H", data, at)
        return Face(f, (a, b, c))

    def write(self):
        return struct.pack("<4H", self.flags, *self.vertices)

    @staticmethod
    def make(material, a, b, c, collides=True, two_sided=False):
        return Face((material & 0xFF) | 0x300 | (Face.COLLIDES if collides else 0) | (Face.TWO_SIDED if two_sided else 0), (a, b, c))

    @property
    def material(self):
        return self.flags & 0xFF


@dataclass
class StripGroup:
    """A TGFM group (zge::StripGroup): the material (submesh) it draws with, a mode byte, and its entries
    (a vertex index in bits 0-13, 0x4000 the triangle's winding reversed, 0x8000 no triangle ends here:
    a strip's (re)start). At most GROUP_ENTRIES entries: the VU1 buffer's (every stock group obeys it)."""
    submesh: int
    mode: int
    entries: List[int]

    FLIPPED = 0x4000
    NO_TRIANGLE = 0x8000
    GROUP_ENTRIES = 60

    @staticmethod
    def read_all(data):
        groups, at = [], 0
        while at + 4 <= len(data):
            head, count = struct.unpack_from("<HH", data, at)
            entries = list(struct.unpack_from("<%dH" % count, data, at + 4))
            groups.append(StripGroup(head >> 8, head & 0xFF, entries))
            at += (count * 2 + 7) & ~3
        if at != len(data):
            raise ValueError("TGFM with %d stray bytes" % (len(data) - at))
        return groups

    def write(self):
        if len(self.entries) > self.GROUP_ENTRIES:
            raise ValueError("a strip group of %d entries (at most %d)" % (len(self.entries), self.GROUP_ENTRIES))
        return pad_to(struct.pack("<HH%dH" % len(self.entries), (self.submesh << 8) | self.mode, len(self.entries), *self.entries), 4)

    @staticmethod
    def write_all(groups):
        return b"".join(g.write() for g in groups)

    @staticmethod
    def from_strip(submesh, strip, mode=0, max_entries=GROUP_ENTRIES):
        """One triangle strip (vertex indices, the first two drawing nothing) cut into groups, as the
        Zoo converter's restrip does (ps2/runtime/zaxis_xbox.cpp): odd entries flipped, and a
        group that continues a strip starts with the two vertices before it, drawing nothing."""
        entries = []
        for k, v in enumerate(strip):
            if k < 2 or strip[k - 2] == strip[k - 1] or strip[k - 1] == v or strip[k - 2] == v:
                entries.append(v | StripGroup.NO_TRIANGLE)
            else:
                entries.append(v | (StripGroup.FLIPPED if k & 1 else 0))
        groups, k = [], 0
        max_entries = max(max_entries, 4)
        while k < len(entries):
            group = []
            if k >= 2:
                group += [(entries[k - 2] & 0x3FFF) | StripGroup.NO_TRIANGLE, (entries[k - 1] & 0x3FFF) | StripGroup.NO_TRIANGLE]
            while k < len(entries) and len(group) < max_entries:
                group.append(entries[k])
                k += 1
            groups.append(StripGroup(submesh, mode, group))
        return groups


@dataclass
class TagBuffer:
    """A TGFE / TGFT chunk (loadBufferStreamChunk, zge::TagTable): a descriptor {bits 0-3 the index kind
    (1: u16), 4-13 a record's bytes, 14-31 the record count}, the element count, one index per element
    (None: no record) padded to 16 bytes, then the records."""
    records: list                      # EdgeTag or FaceTag
    indices: List[Optional[int]]       # per element (a face edge is 3 * face + k, k the edge from vertex k to k + 1; a face is itself)
    INDEX_U16 = 1

    @staticmethod
    def read(data, record_type):
        desc, count = struct.unpack_from("<2I", data, 0)
        kind, size, records = desc & 0xF, (desc >> 4) & 0x3FF, desc >> 14
        if kind != TagBuffer.INDEX_U16:
            raise ValueError("tag buffer of index kind %d" % kind)
        if size != record_type.SIZE:
            raise ValueError("%s records of %d bytes" % (record_type.__name__, size))
        index_bytes = (count * 2 + 15) & ~15
        raw = struct.unpack_from("<%dH" % count, data, 8)
        body = data[8 + index_bytes:]
        if len(body) != size * records:
            raise ValueError("tag buffer of %d bytes for %d records of %d" % (len(body), records, size))
        return TagBuffer([record_type.read(body, i * size) for i in range(records)], [None if r == 0xFFFF else r for r in raw])

    def write(self):
        size = self.records[0].SIZE if self.records else 0
        count = len(self.indices)
        desc = (len(self.records) << 14) | (size << 4) | self.INDEX_U16
        indices = pad_to(struct.pack("<%dH" % count, *(0xFFFF if i is None else i for i in self.indices)), 16)
        return struct.pack("<2I", desc, count) + indices + b"".join(r.write() for r in self.records)


@dataclass
class EdgeTag:
    """A TGFE record (24 bytes; zge::EdgeRecord): a face edge's grind data. `kind` 1 or 9 is a quarter
    pipe's lip (tied to kind-1 faces at load), 2 a spine's, 3 a vertical pole, 4 a horizontal pole, 5 and
    6 double rails; `flags` bit 0: a rail search of no kind takes this edge (grindable); `material` the
    grind sound and rumble (0, 2, 8 metal, 1 wood); the launch fields a pole's exit. `element` (+4) is 0
    on disc and written at load (the last face edge that picks the record); `byte_0a` is 3 in every
    stock record and read by nothing."""
    event_id: int
    kind: int
    flags: int
    material: int
    launch_min: float = 0.0
    launch_max: float = 0.0
    launch_speed: float = 0.0
    element: int = 0
    byte_0a: int = 3
    SIZE = 24
    GRINDABLE = 1

    @staticmethod
    def read(data, at):
        event, element, kind, flags, b0a, material, lmin, lmax, lspeed = struct.unpack_from("<2I4B3f", data, at)
        return EdgeTag(event, kind, flags, material, lmin, lmax, lspeed, element, b0a)

    def write(self):
        return struct.pack("<2I4B3f", self.event_id, self.element, self.kind, self.flags, self.byte_0a, self.material,
                           self.launch_min, self.launch_max, self.launch_speed)


@dataclass
class FaceTag:
    """A TGFT record (28 bytes; zge::FaceTagRecord): a face's surface kind (1 a quarter pipe, 2 a spine,
    3 a slope with `gravity`, 8 a wall ride, 13 roads, floors and columns) and the word its "Jump" and
    "Land" events carry. `nearest_edge` and `face` are 0 on disc and written at load (setupCollisionPart);
    `leftover` is the three bytes after the kind, which the game never reads (not always zero on disc)."""
    event_kind: int
    kind: int
    gravity: float = 0.0
    launch_cap: float = 0.0
    launch_push: float = 0.0
    nearest_edge: int = 0
    face: int = 0
    leftover: bytes = b"\0\0\0"
    SIZE = 28

    @staticmethod
    def read(data, at):
        event, edge, face, kind = struct.unpack_from("<3IB", data, at)
        gravity, cap, push = struct.unpack_from("<3f", data, at + 0x10)
        return FaceTag(event, kind, gravity, cap, push, edge, face, data[at + 0xD:at + 0x10])

    def write(self):
        return struct.pack("<3IB", self.event_kind, self.nearest_edge, self.face, self.kind) + self.leftover.ljust(3, b"\0")[:3] + \
            struct.pack("<3f", self.gravity, self.launch_cap, self.launch_push)


@dataclass
class FaceTreeSplit:
    """A split node (zge::FaceTreeSplit): the axis (1 x, 2 y, 3 z), the plane in hundredths (s16), and the
    children where the coordinate is greater and lesser. `leftover`: the node's second byte, which the
    game never reads (the exporter left varying values there)."""
    axis: int
    plane: int
    greater: "FaceTreeNode"
    lesser: "FaceTreeNode"
    leftover: int = 0


@dataclass
class FaceTreeLeaf:
    """A leaf (zge::FaceTreeLeaf): the faces (indices into the collision part's TGFP) it lists."""
    faces: List[int]
    leftover: int = 0


FaceTreeNode = object  # FaceTreeSplit | FaceTreeLeaf


@dataclass
class CollisionTree:
    """TGCI: {u32 part id, u32 bytes, the face tree}. The tree is the split nodes and leaves in preorder,
    8 bytes each, then every leaf's u16 face list in the same order (an empty leaf points at where the
    next list starts); the chunk is padded to 4 bytes. AI's query builds a leaf of all the part's faces
    for an object without a tree, so a level needs none; DM2 and BMX XXX need theirs."""
    part_id: int
    root: FaceTreeNode

    LEAF_FACES = 8
    DEPTH = 11

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
                count, offset = struct.unpack_from("<HI", blob, at + 2)
                return FaceTreeLeaf(list(struct.unpack_from("<%dH" % count, blob, offset * 2)), extra)
            if kind > 3:
                raise ValueError("collision tree node of type %d" % kind)
            plane, greater, lesser = struct.unpack_from("<hHH", blob, at + 2)
            return FaceTreeSplit(kind, plane, node(greater * 4, depth + 1), node(lesser * 4, depth + 1), extra)

        return CollisionTree(part_id, node(0, 0))

    def write(self):
        order = []

        def visit(n):
            order.append(n)
            if isinstance(n, FaceTreeSplit):
                visit(n.greater)
                visit(n.lesser)

        visit(self.root)
        index = {id(n): i for i, n in enumerate(order)}
        if len(order) * 2 > 0xFFFF:
            raise ValueError("collision tree of %d nodes: past the u16 word offsets" % len(order))
        nodes, lists, at = bytearray(), bytearray(), 8 * len(order)
        for n in order:
            if isinstance(n, FaceTreeLeaf):
                nodes += struct.pack("<BBHI", 0, n.leftover, len(n.faces), at // 2)
                lists += struct.pack("<%dH" % len(n.faces), *n.faces)
                at += 2 * len(n.faces)
            else:
                nodes += struct.pack("<BBhHH", n.axis, n.leftover, n.plane, 2 * index[id(n.greater)], 2 * index[id(n.lesser)])
        tree = bytes(nodes + lists)
        return pad_to(struct.pack("<2I", self.part_id, len(tree)) + tree, 4)

    def leaves(self):
        out, stack = [], [self.root]
        while stack:
            n = stack.pop()
            if isinstance(n, FaceTreeLeaf):
                out.append(n)
            else:
                stack += [n.lesser, n.greater]
        return out

    def faces(self):
        """The faces the tree lists: the only ones the game's queries reach on this object. The stock
        trees leave out some faces with 0x2000 (the Tutorial 200 in 17 trees, the Movie Lot 883 in 23):
        those are not solid in the game."""
        return {f for leaf in self.leaves() for f in leaf.faces}

    @staticmethod
    def build(part_id, positions, faces, extra=(), only=None, leaf_faces=LEAF_FACES, depth=DEPTH):
        """A tree for a part, as the games' exporter builds theirs (read from their levels): the faces with
        0x2000 (and `extra`; or exactly `only`), split at the median of their centres on the longest axis
        of their bounds, in hundredths; a face on both sides goes to both; leaves of at most `leaf_faces`
        or at `depth`. None when nothing collides."""
        def bounds(f):
            return [(min(positions[i][a] for i in f.vertices), max(positions[i][a] for i in f.vertices)) for a in range(3)]

        boxes = {i: bounds(f) for i, f in enumerate(faces)
                 if ((f.flags & Face.COLLIDES or i in extra) if only is None else i in only) and max(f.vertices) < len(positions)}
        if not boxes:
            return None

        def build(members, level):
            if len(members) > leaf_faces and level < depth:
                extent = [(min(boxes[f][a][0] for f in members), max(boxes[f][a][1] for f in members)) for a in range(3)]
                axis = max(range(3), key=lambda a: extent[a][1] - extent[a][0])
                centres = sorted((boxes[f][axis][0] + boxes[f][axis][1]) / 2 for f in members)
                plane = int(round(centres[len(centres) // 2] * 100))
                if -32768 <= plane <= 32767:
                    at = plane / 100.0
                    low = [f for f in members if boxes[f][axis][0] <= at]
                    high = [f for f in members if boxes[f][axis][1] >= at]
                    if len(low) < len(members) and len(high) < len(members):
                        return FaceTreeSplit(axis + 1, plane, build(high, level + 1), build(low, level + 1))
            return FaceTreeLeaf(sorted(members))

        return CollisionTree(part_id, build(sorted(boxes), 0))


@dataclass
class IndexedStream:
    """TIVC / TIVN / TIVU / TIFN: a table of distinct elements (TIDA) and one index per vertex (face for
    TIFN) into it (TII8 when every index fits a byte, else TII6), the indices padded to 4 bytes. The game
    reads the plain form of the same stream (TGVC ...) the same way."""
    table: List[bytes]
    indices: List[int]
    wide: bool = False

    @staticmethod
    def from_chunk(chunk, stride, count):
        table, raw, wide = None, None, False
        for k in chunk.kids:
            if k.tag == b"TIDA":
                if len(k.data) % stride:
                    raise ValueError("%r table of %d bytes, stride %d" % (chunk.tag, len(k.data), stride))
                table = [k.data[i:i + stride] for i in range(0, len(k.data), stride)]
            elif k.tag in (b"TII8", b"TII6"):
                wide, raw = k.tag == b"TII6", k.data
        if table is None or raw is None:
            raise ValueError("%r without its table or indices" % chunk.tag)
        width = 2 if wide else 1
        if len(raw) != (count * width + 3) & ~3:
            raise ValueError("%r: %d index bytes for %d elements" % (chunk.tag, len(raw), count))
        return IndexedStream(table, list(struct.unpack_from("<%d%s" % (count, "H" if wide else "B"), raw, 0)), wide)

    def write(self, tag):
        raw = pad_to(struct.pack("<%d%s" % (len(self.indices), "H" if self.wide else "B"), *self.indices), 4)
        return Chunk(tag, kids=[Chunk(b"TIDA", b"".join(self.table)), Chunk(b"TII6" if self.wide else b"TII8", raw)])

    def expand(self):
        """The plain stream: one element per index."""
        return b"".join(self.table[i] for i in self.indices)

    @staticmethod
    def make(elements):
        """An indexed stream of the elements (bytes each), equal ones shared."""
        table, where, indices = [], {}, []
        for e in elements:
            if e not in where:
                where[e] = len(table)
                table.append(e)
            indices.append(where[e])
        return IndexedStream(table, indices, len(table) > 256)


# A vertex or face stream: plain bytes (TGV*, TGFN) or indexed (TIV*, TIFN).
Stream = object  # bytes | IndexedStream

# attribute: (plain tag, indexed tag, element bytes, per vertex or per face)
STREAMS = {"normals": (b"TGVN", b"TIVN", 12, "v"), "colours": (b"TGVC", b"TIVC", 4, "v"), "uvs": (b"TGVU", b"TIVU", 8, "v"),
           "face_normals": (b"TGFN", b"TIFN", 12, "f")}


@dataclass
class Part:
    """TGPT: a mesh. The TGPH header's counts come from the streams; its id, flags, reserved byte, bone
    and material counts are kept. Positions are float3 (TGVP), normals float3, colours RGBA bytes (0x80
    full), UVs float2; faces 8-byte records; strip groups the VU1's; face normals float3 per face; the
    materials a TMTB of MATI chunks (kept as chunks: the Zoo converter writes those)."""
    id: int
    positions: List[Tuple[float, float, float]]
    faces: List[Face]
    groups: List[StripGroup]
    normals: Optional[Stream] = None
    colours: Optional[Stream] = None
    uvs: Optional[Stream] = None
    face_normals: Optional[Stream] = None
    edge_tags: Optional[TagBuffer] = None
    face_tags: Optional[TagBuffer] = None
    flags: int = 0            # TGPH +0: bits 0-2 the render class, 0x3800 taken by the part
    reserved: int = 0         # TGPH +0x0A, copied to the part and never read (0 in every stock part)
    bone_count: int = 0
    material_count: int = 0   # submeshes (the TMTB's MATI count)
    other: List[Chunk] = field(default_factory=list)   # chunks this writer does not type (TGPB, TGVI, TGVT)
    order: List[bytes] = field(default_factory=list)   # the file's chunk order, kept for the round trip

    # The tag records this engine's TGFE and TGFT hold (a game's subclass names its own).
    EDGE_TAG = EdgeTag
    FACE_TAG = FaceTag

    @classmethod
    def read(cls, part):
        kids = part.kids
        head = next((k for k in kids if k.tag == b"TGPH"), None)
        if head is None or len(head.data) != 0x14:
            raise ValueError("TGPT without a 20-byte TGPH")
        flags, w02, nv, nf, ng, reserved, p0b, nb, p0d, nm, p0f, pid = struct.unpack_from("<5H6BI", head.data, 0)
        if w02 or p0b or p0d or p0f:
            raise ValueError("TGPH with its padding set (%x %x %x %x)" % (w02, p0b, p0d, p0f))
        self = cls(pid, [], [], [], flags=flags, reserved=reserved, bone_count=nb, material_count=nm)
        for k in kids:
            self.order.append(k.tag)
            if k.tag == b"TGPH":
                continue
            if k.tag == b"TGVP":
                if len(k.data) != 12 * nv:
                    raise ValueError("TGVP of %d bytes for %d vertices" % (len(k.data), nv))
                self.positions = [struct.unpack_from("<3f", k.data, 12 * i) for i in range(nv)]
            elif k.tag == b"TGFP":
                if len(k.data) != 8 * nf:
                    raise ValueError("TGFP of %d bytes for %d faces" % (len(k.data), nf))
                self.faces = [Face.read(k.data, 8 * i) for i in range(nf)]
            elif k.tag == b"TGFM":
                self.groups = StripGroup.read_all(k.data)
                if len(self.groups) != ng:
                    raise ValueError("TGPH counts %d strip groups, TGFM has %d" % (ng, len(self.groups)))
            elif k.tag == b"TGFE":
                self.edge_tags = TagBuffer.read(k.data, cls.EDGE_TAG)
            elif k.tag == b"TGFT":
                self.face_tags = TagBuffer.read(k.data, cls.FACE_TAG)
            else:
                for name, (plain, indexed, stride, per) in STREAMS.items():
                    count = nv if per == "v" else nf
                    if k.tag == plain:
                        if len(k.data) != stride * count:
                            raise ValueError("%r of %d bytes for %d elements" % (plain, len(k.data), count))
                        setattr(self, name, k.data)
                        break
                    if k.tag == indexed:
                        setattr(self, name, IndexedStream.from_chunk(k, stride, count))
                        break
                else:
                    self.other.append(k)
        return self

    def header(self):
        return struct.pack("<5H6BI", self.flags, 0, len(self.positions), len(self.faces), len(self.groups),
                           self.reserved, 0, self.bone_count, 0, self.material_count, 0, self.id)

    def chunks(self):
        """The part's chunks by tag."""
        out = {b"TGPH": Chunk(b"TGPH", self.header()),
               b"TGVP": Chunk(b"TGVP", b"".join(struct.pack("<3f", *p) for p in self.positions)),
               b"TGFP": Chunk(b"TGFP", b"".join(f.write() for f in self.faces)),
               b"TGFM": Chunk(b"TGFM", StripGroup.write_all(self.groups))}
        for name, (plain, indexed, stride, per) in STREAMS.items():
            s = getattr(self, name)
            if isinstance(s, IndexedStream):
                out[indexed] = s.write(indexed)
            elif s is not None:
                out[plain] = Chunk(plain, s)
        if self.edge_tags is not None:
            out[b"TGFE"] = Chunk(b"TGFE", self.edge_tags.write())
        if self.face_tags is not None:
            out[b"TGFT"] = Chunk(b"TGFT", self.face_tags.write())
        for k in self.other:
            out[k.tag] = k
        return out

    # The exporter's order when a part is made from scratch.
    DEFAULT_ORDER = [b"TGPH", b"TGVP", b"TIVN", b"TGVN", b"TIVC", b"TGVC", b"TIVU", b"TGVU", b"TGFP", b"TIFN", b"TGFN", b"TGFM",
                     b"TGFE", b"TGFT"]

    def write(self):
        chunks = self.chunks()
        order = [t for t in (self.order or self.DEFAULT_ORDER) if t in chunks]
        order += [t for t in chunks if t not in order]
        return Chunk(b"TGPT", kids=[chunks[t] for t in order])

    def expand_streams(self):
        """Every indexed stream written plain (TIVC -> TGVC ...): the same data in the other encoding."""
        for name, (plain, indexed, stride, per) in STREAMS.items():
            s = getattr(self, name)
            if isinstance(s, IndexedStream):
                setattr(self, name, s.expand())
                self.order = [plain if t == indexed else t for t in self.order]


@dataclass
class LevelOfDetail:
    """TGLO (12 bytes; readTgloChunk): the part drawn out to `distance`. The middle word is 0 on disc and unread."""
    distance: float
    part_id: int
    word_04: int = 0

    @staticmethod
    def read(data):
        d, w, p = struct.unpack("<fII", data)
        return LevelOfDetail(d, p, w)

    def write(self):
        return struct.pack("<fII", self.distance, self.word_04, self.part_id)


@dataclass
class GeoObject:
    """TGEO: a geometry object: its header, tag table (TGTD: the object's class hash, then the class's
    data, kept as bytes), frame (TGFR: 4 bytes of flags), parts each followed by its TGLO (a drawn level
    of detail) and material table, and the collision tree."""
    header: GeoHeader
    frame: bytes                                  # TGFR
    parts: List[Tuple[Part, Optional[LevelOfDetail], Optional[Chunk]]]  # (part, its TGLO, its TMTB)
    type_data: Optional[bytes] = None             # TGTD
    collision: Optional[CollisionTree] = None     # TGCI
    tmtb_first: bool = False                      # the file's order after a part: TMTB before TGLO (no stock object)

    # The part and collision tree records (a game's subclass names its own).
    PART = Part
    TREE = CollisionTree

    @classmethod
    def read(cls, obj):
        kids = obj.kids
        head = next((k for k in kids if k.tag == b"TGHD"), None)
        if head is None:
            raise ValueError("TGEO without TGHD")
        self = cls(GeoHeader.read(head.data), b"", [])
        i = 0
        while i < len(kids):
            k = kids[i]
            if k.tag == b"TGHD":
                pass
            elif k.tag == b"TGTD":
                self.type_data = k.data
            elif k.tag == b"TGFR":
                self.frame = k.data
            elif k.tag == b"TGCI":
                self.collision = cls.TREE.read(k.data)
            elif k.tag == b"TGPT":
                part, table, lod = cls.PART.read(k), None, None
                while i + 1 < len(kids) and kids[i + 1].tag in (b"TMTB", b"TGLO"):
                    i += 1
                    if kids[i].tag == b"TMTB":
                        table = kids[i]
                        if lod is None:
                            self.tmtb_first = True
                    else:
                        lod = LevelOfDetail.read(kids[i].data)
                self.parts.append((part, lod, table))
            else:
                raise ValueError("a %r chunk in a TGEO" % k.tag)
            i += 1
        if self.tmtb_first and any(lod is not None for _, lod, _ in self.parts):
            self.tmtb_first = False
        return self

    def write(self):
        kids = [Chunk(b"TGHD", self.header.write())]
        if self.type_data is not None:
            kids.append(Chunk(b"TGTD", self.type_data))
        kids.append(Chunk(b"TGFR", self.frame))
        for part, lod, table in self.parts:
            kids.append(part.write())
            after = [Chunk(b"TGLO", lod.write())] if lod is not None else []
            if table is not None:
                after.append(table)
            kids += after
        if self.collision is not None:
            kids.append(Chunk(b"TGCI", self.collision.write()))
        return Chunk(b"TGEO", kids=kids)

    def collision_part(self):
        """The part the collision tree names, else the last part (AI's query without a tree)."""
        if self.collision is not None:
            for part, _, _ in self.parts:
                if part.id == self.collision.part_id:
                    return part
        return self.parts[-1][0] if self.parts else None


@dataclass
class Geometry:
    """A GEO file: FORM { TGEO ..., ZDYN ... }. The dynamics bodies (ZDYN: DDEF, DBDY) are kept as chunks."""
    objects: List[GeoObject]
    other: List[Tuple[int, Chunk]] = field(default_factory=list)   # (position among the FORM's kids, chunk)

    OBJECT = GeoObject   # (a game's subclass names its own)

    @classmethod
    def read(cls, data):
        tree = L.parse(data)
        if len(tree) != 1 or tree[0].tag != b"FORM":
            raise ValueError("a GEO is one FORM")
        self = cls([])
        for i, k in enumerate(tree[0].kids):
            if k.tag == b"TGEO":
                self.objects.append(cls.OBJECT.read(k))
            else:
                self.other.append((i, k))
        return self

    def write(self):
        kids = [o.write() for o in self.objects]
        for i, k in sorted(self.other):
            kids.insert(min(i, len(kids)), k)
        return L.serialize([Chunk(b"FORM", kids=kids)])


# ---- INS: instances ----------------------------------------------------------------

@dataclass
class Instance:
    """TGIH (0x70 bytes; zge::InstanceChunk, loadGeometryInstance): a geometry object placed in the world
    (or under its parent instance). `flags`: bits 0-7 the node's (bit 1 collides; 0x10 with it: not
    collided with), bits 8-9 its bits 15-16, bit 10 mirrored, bit 11 keeps its region. The two boxes at
    +0x24 are the instance's bounds in the world (the exporter's; the game reads neither)."""
    flags: int
    rotation: Tuple[float, float, float, float]   # a quaternion (x, y, z, w)
    position: Tuple[float, float, float]
    geometry_id: int
    instance_id: int
    name: str
    name_hash: int = 0
    parent: int = 0
    region: int = 0           # a PVSO's mesh hash, 0: by position
    level: int = 0            # the level-of-detail table (setLodTable)
    material_set: int = 0
    world_min: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    world_max: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    tags: Optional[bytes] = None   # TGIT: the instance's tag table body (a class hash, then its data)

    SIZE = 0x70
    COLLIDES = 0x2
    NO_COLLISION = 0x10

    @staticmethod
    def read(data, tags=None):
        if len(data) != Instance.SIZE:
            raise ValueError("TGIH of %d bytes" % len(data))
        flags, level, mset, pad = struct.unpack_from("<IBBH", data, 0)
        if pad:
            raise ValueError("TGIH with its padding set")
        rot = struct.unpack_from("<4f", data, 8)
        pos = struct.unpack_from("<3f", data, 0x18)
        wmin = struct.unpack_from("<3f", data, 0x24)
        wmax = struct.unpack_from("<3f", data, 0x30)
        name_hash, geo, parent, me, region = struct.unpack_from("<5I", data, 0x3C)
        return Instance(flags, rot, pos, geo, me, c_string(data[0x50:0x70]), name_hash, parent, region, level, mset, wmin, wmax, tags)

    def write(self):
        return struct.pack("<IBBH4f3f3f3f5I", self.flags, self.level, self.material_set, 0, *self.rotation, *self.position,
                           *self.world_min, *self.world_max, self.name_hash, self.geometry_id, self.parent, self.instance_id,
                           self.region) + c_field(self.name, 32)

    def chunk(self):
        kids = [Chunk(b"TGIH", self.write())]
        if self.tags is not None:
            kids.append(Chunk(b"TGIT", self.tags))
        return Chunk(b"TGIN", kids=kids)

    @staticmethod
    def make(name, geometry, position, rotation=(0.0, 0.0, 0.0, 1.0), instance_id=0, collides=True, tags=None):
        """An instance of a GeoObject, its world box from the object's bounds rotated (all eight corners)."""
        corners = [(x, y, z) for x in (geometry.header.bounds_min[0], geometry.header.bounds_max[0])
                   for y in (geometry.header.bounds_min[1], geometry.header.bounds_max[1])
                   for z in (geometry.header.bounds_min[2], geometry.header.bounds_max[2])]
        world = [tuple(a + b for a, b in zip(L.quat_rotate(rotation, c), position)) for c in corners]
        wmin = tuple(min(w[a] for w in world) for a in range(3))
        wmax = tuple(max(w[a] for w in world) for a in range(3))
        return Instance(Instance.COLLIDES if collides else 0, rotation, position, geometry.header.id, instance_id or L.name_hash(name),
                        name, L.name_hash(name), world_min=wmin, world_max=wmax, tags=tags)


@dataclass
class Instances:
    """An INS file: FORM { TGIN { TGIH, TGIT? } ... }."""
    instances: List[Instance]

    INSTANCE = Instance   # (a game's subclass names its own)

    @classmethod
    def read(cls, data):
        tree = L.parse(data)
        if len(tree) != 1 or tree[0].tag != b"FORM":
            raise ValueError("an INS is one FORM")
        out = []
        for g in tree[0].kids:
            if g.tag != b"TGIN":
                raise ValueError("a %r chunk in an INS" % g.tag)
            tags = [k.tag for k in g.kids]
            if tags not in ([b"TGIH"], [b"TGIH", b"TGIT"]):
                raise ValueError("a TGIN of %r" % tags)
            out.append(cls.INSTANCE.read(g.kids[0].data, g.kids[1].data if len(g.kids) > 1 else None))
        return cls(out)

    def write(self):
        return L.serialize([Chunk(b"FORM", kids=[i.chunk() for i in self.instances])])


# ---- DAT: scene points and view regions -------------------------------------------

SPAWN = L.name_hash("Spawn")
GLOBALS = L.name_hash("Globals")
PLAYER_ONE = L.name_hash("PlayerOne")
RESPAWN = L.name_hash("Respawn")


@dataclass
class ScenePoint:
    """SDPT (readSdptChunk): a scene point: its kind (a class name's hash: Spawn, Globals, ...), name hash,
    index (findScenePoint's third key), position and orientation (a quaternion), then its tag data (the
    class's record) to the chunk's end."""
    kind: int
    name: int
    position: Tuple[float, float, float]
    orientation: Tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0)
    index: int = 0
    tags: bytes = b""
    HEADER = 0x2C

    @staticmethod
    def read(data):
        header, kind, name, index, pad = struct.unpack_from("<3I2H", data, 0)
        if header != ScenePoint.HEADER or pad:
            raise ValueError("SDPT header of %d bytes (pad %x)" % (header, pad))
        pos = struct.unpack_from("<3f", data, 0x10)
        rot = struct.unpack_from("<4f", data, 0x1C)
        return ScenePoint(kind, name, pos, rot, index, data[header:])

    def write(self):
        return struct.pack("<3I2H3f4f", self.HEADER, self.kind, self.name, self.index, 0, *self.position, *self.orientation) + self.tags

    @staticmethod
    def spawn(name, position, orientation=(0.0, 0.0, 0.0, 1.0), tags=b""):
        return ScenePoint(SPAWN, L.name_hash(name), position, orientation, tags=tags)


@dataclass
class ViewRegion:
    """PVSO (readPvsoChunk; zge::Region): a visibility zone: the mesh hash that is the zone (an instance's
    region word names it), its box, and the mesh hashes of the zones hidden from it. `flag` is the
    chunk's flag word (0 or 1 in the stock levels; the region keeps it, nothing reads it); None writes the
    older body without one (AI's multiplayer Cannery; the only form DM2 reads)."""
    mesh: int
    low: Tuple[float, float, float]
    high: Tuple[float, float, float]
    hidden: List[int]
    flag: Optional[int] = 0

    @staticmethod
    def read(data):
        mesh = struct.unpack_from("<I", data, 0)[0]
        low = struct.unpack_from("<3f", data, 4)
        high = struct.unpack_from("<3f", data, 0x10)
        count = struct.unpack_from("<I", data, 0x1C)[0]
        if len(data) == 0x24 + 4 * count:
            flag, first = struct.unpack_from("<I", data, 0x20)[0], 0x24
        elif len(data) == 0x20 + 4 * count:
            flag, first = None, 0x20
        else:
            raise ValueError("PVSO of %d bytes with %d zones listed" % (len(data), count))
        return ViewRegion(mesh, low, high, list(struct.unpack_from("<%dI" % count, data, first)), flag)

    def write(self):
        out = struct.pack("<I3f3fI", self.mesh, *self.low, *self.high, len(self.hidden))
        if self.flag is not None:
            out += struct.pack("<I", self.flag)
        return out + struct.pack("<%dI" % len(self.hidden), *self.hidden)


@dataclass
class SceneData:
    """A DAT file: FORM { SDPT ..., ZLIT (empty), ZPVS { PVSO ... } }, in the file's order."""
    points: List[ScenePoint]
    regions: Optional[List[ViewRegion]] = None    # None: no ZPVS chunk
    order: List[bytes] = field(default_factory=list)   # the FORM's kids' tags
    other: List[Chunk] = field(default_factory=list)   # chunks this writer does not type (ZLIT, SDTD), in order

    # The point and zone records (a game's subclass names its own).
    POINT = ScenePoint
    REGION = ViewRegion

    @classmethod
    def read(cls, data):
        tree = L.parse(data)
        if len(tree) != 1 or tree[0].tag != b"FORM":
            raise ValueError("a DAT is one FORM")
        self = cls([])
        for k in tree[0].kids:
            self.order.append(k.tag)
            if k.tag == b"SDPT":
                self.points.append(cls.POINT.read(k.data))
            elif k.tag == b"ZPVS":
                self.regions = [cls.REGION.read(p.data) for p in k.kids if p.tag == b"PVSO"]
                if len(self.regions) != len(k.kids):
                    raise ValueError("a ZPVS with %r" % [p.tag for p in k.kids if p.tag != b"PVSO"])
            else:
                self.other.append(k)
        return self

    def write(self):
        points, other, kids = iter(self.points), iter(self.other), []
        order = self.order or [b"SDPT"] * len(self.points) + [b"ZLIT"] + ([b"ZPVS"] if self.regions is not None else [])
        if not self.order:
            other = iter([Chunk(b"ZLIT", b"")])
        for tag in order:
            if tag == b"SDPT":
                kids.append(Chunk(b"SDPT", next(points).write()))
            elif tag == b"ZPVS":
                kids.append(Chunk(b"ZPVS", kids=[Chunk(b"PVSO", r.write()) for r in self.regions]))
            else:
                kids.append(next(other))
        return L.serialize([Chunk(b"FORM", kids=kids)])


# ---- the round trip -----------------------------------------------------------------

LEVEL_FOLDERS = ["MOVIE", "CIVIC", "INDUST", "BRDWLK", "CANNERY", "HANGER", "MUSEUM", "TUT", "LOCAL", "ZOO",
                 "MP/MOVIE", "MP/CIVIC", "MP/INDUST", "MP/BRDWLK", "MP/CANNERY", "MP/HANGER", "MP/MUSEUM", "MP/LOCAL", "MP/ZOO",
                 "MP/COMPA", "MP/COMPB"]


def level_files(disc, folder):
    name = folder.rsplit("/", 1)[-1]
    out = {}
    for ext in ("ZIT", "GEO", "INS", "TXR", "DAT"):
        data = disc.read("ASSETS/LEVELS/%s/%s.%s" % (folder, name, ext))
        if data is None:
            raise ValueError("ASSETS/LEVELS/%s/%s.%s is not on the disc" % (folder, name, ext))
        out[ext] = data
    return out


def first_difference(a, b):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return n if len(a) != len(b) else None


class RoundTrip:
    """Chunk-by-chunk and whole-file comparison of a level read and written back."""

    def __init__(self):
        from collections import Counter, defaultdict
        self.same = Counter()
        self.differ = Counter()
        self.reasons = defaultdict(Counter)
        self.files = {}

    def chunk(self, tag, original, written, why=None):
        if original == written:
            self.same[tag] += 1
        else:
            self.differ[tag] += 1
            at = first_difference(original, written)
            self.reasons[tag][why or ("byte 0x%x" % at)] += 1

    def report(self):
        lines = []
        for tag in sorted(set(self.same) | set(self.differ), key=lambda t: t.decode("latin1")):
            s, d = self.same[tag], self.differ[tag]
            text = "    %-4s %7d identical" % (tag.decode("latin1"), s)
            if d:
                text += ", %d differ: %s" % (d, "; ".join("%s (%d)" % (w, n) for w, n in self.reasons[tag].most_common(3)))
            lines.append(text)
        return lines


def roundtrip_geo(data, rt):
    """Each typed chunk of the GEO written back and compared, then the whole file."""
    tree = L.parse(data)
    geo = Geometry.read(data)
    objects = [k for k in tree[0].kids if k.tag == b"TGEO"]
    for obj, rec in zip(objects, geo.objects):
        rt.chunk(b"TGHD", next(k.data for k in obj.kids if k.tag == b"TGHD"), rec.header.write())
        for k in obj.kids:
            if k.tag == b"TGCI":
                rt.chunk(b"TGCI", k.data, rec.collision.write())
        parts = [k for k in obj.kids if k.tag == b"TGPT"]
        for part, (prec, lod, table) in zip(parts, rec.parts):
            chunks = prec.chunks()
            for k in part.kids:
                if k.kids is None:
                    rt.chunk(k.tag, k.data, chunks[k.tag].data)
                else:
                    rt.chunk(k.tag, L.serialize([k]), L.serialize([chunks[k.tag]]))
            rt.chunk(b"TGPT", L.serialize([part]), L.serialize([prec.write()]))
        for k in obj.kids:
            if k.tag == b"TGLO":
                rt.chunk(b"TGLO", k.data, LevelOfDetail.read(k.data).write())
        rt.chunk(b"TGEO", L.serialize([obj]), L.serialize([rec.write()]))
    out = geo.write()
    rt.files["GEO"] = (data == out, first_difference(data, out), len(data), len(out))
    return geo


def roundtrip_ins(data, rt):
    tree = L.parse(data)
    ins = Instances.read(data)
    for g, rec in zip(tree[0].kids, ins.instances):
        rt.chunk(b"TGIH", g.kids[0].data, rec.write())
        rt.chunk(b"TGIN", L.serialize([g]), L.serialize([rec.chunk()]))
    out = ins.write()
    rt.files["INS"] = (data == out, first_difference(data, out), len(data), len(out))
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
            rt.chunk(b"ZPVS", L.serialize([k]), L.serialize([Chunk(b"ZPVS", kids=[Chunk(b"PVSO", r.write()) for r in dat.regions])]))
    out = dat.write()
    rt.files["DAT"] = (data == out, first_difference(data, out), len(data), len(out))
    return dat


def readers_agree(geo_a, ins_a, geo_b, ins_b):
    """ai_level.py's collision surface and grind edges from one pair of files equal those from the other."""
    a = L.collision_triangles(geo_a, ins_a)
    b = L.collision_triangles(geo_b, ins_b)
    if a != b:
        raise ValueError("the collision triangles read back differ")
    if L.grind_edges(geo_a, ins_a) != L.grind_edges(geo_b, ins_b):
        raise ValueError("the grind edges read back differ")
    return len(a), sum(len(g[4]) for g in a)


def cmd_roundtrip(args):
    disc = L.Disc(args.disc)
    folders = args.level or LEVEL_FOLDERS
    all_files_identical = True
    for folder in folders:
        folder = folder.upper()
        try:
            files = level_files(disc, folder)
        except ValueError as e:
            print("%s: %s" % (folder, e))
            continue
        rt = RoundTrip()
        geo = roundtrip_geo(files["GEO"], rt)
        ins = roundtrip_ins(files["INS"], rt)
        dat = roundtrip_dat(files["DAT"], rt)
        groups, tris = readers_agree(files["GEO"], files["INS"], geo.write(), ins.write())
        parts = sum(len(o.parts) for o in geo.objects)
        max_group = max((len(g.entries) for o in geo.objects for p, _, _ in o.parts for g in p.groups), default=0)
        print("%s: %d objects, %d parts, %d instances, %d points, %s zones; %d collidable instances (%d triangles) and the "
              "grind edges read back the same; largest strip group %d" %
              (folder, len(geo.objects), parts, len(ins.instances), len(dat.points),
               len(dat.regions) if dat.regions is not None else "no", groups, tris, max_group))
        for ext in ("GEO", "INS", "DAT"):
            same, at, n_in, n_out = rt.files[ext]
            all_files_identical &= same
            print("  %s: %s (%d bytes)" % (ext, "byte-identical" if same else "differs at 0x%x (%d written)" % (at, n_out), n_in))
        if args.verbose or rt.differ:
            print("\n".join(rt.report()))
    print("all files byte-identical" if all_files_identical else "some files differ")
    return 0 if all_files_identical else 1


def variant(geo, trees=None, plain_streams=False):
    """The geometry in the writers' other encodings: every collision tree built here (trees="stock": over
    the faces the stock tree lists; "all": over every face with 0x2000, which the stock trees leave some
    of out) and, with plain_streams, every indexed stream written plain. Returns (trees rebuilt, streams
    expanded)."""
    rebuilt = streams = 0
    for obj in geo.objects:
        if plain_streams:
            for part, _, _ in obj.parts:
                before = [isinstance(getattr(part, n), IndexedStream) for n in STREAMS]
                part.expand_streams()
                streams += sum(before)
        if trees and obj.collision is not None:
            part = obj.collision_part()
            if part is not None:
                tree = CollisionTree.build(part.id, part.positions, part.faces, only=obj.collision.faces() if trees == "stock" else None)
                if tree is not None:
                    obj.collision = tree
                    rebuilt += 1
    return rebuilt, streams


def cmd_rewrite(args):
    disc = L.Disc(args.disc)
    folder = args.level.upper()
    name = folder.rsplit("/", 1)[-1]
    files = level_files(disc, folder)
    out_dir = os.path.join(args.out, "ASSETS", "LEVELS", *folder.split("/"))
    os.makedirs(out_dir, exist_ok=True)
    geo, ins, dat = Geometry.read(files["GEO"]), Instances.read(files["INS"]), SceneData.read(files["DAT"])
    if args.rebuild_trees or args.plain_streams:
        trees, streams = variant(geo, args.rebuild_trees, args.plain_streams)
        print("variant: %d collision trees built here%s, %d indexed streams written plain" %
              (trees, {"all": " over every face with 0x2000", "stock": " over the stock trees' faces"}.get(args.rebuild_trees, ""), streams))
    written = {"GEO": geo.write(), "INS": ins.write(), "DAT": dat.write()}
    groups, tris = readers_agree(files["GEO"], files["INS"], written["GEO"], written["INS"])
    print("%d collidable instances (%d triangles) and the grind edges read back the same" % (groups, tris))
    for ext, data in written.items():
        with open(os.path.join(out_dir, "%s.%s" % (name, ext)), "wb") as f:
            f.write(data)
        print("%s.%s: %d bytes%s" % (name, ext, len(data), "" if data == files[ext] else " (not the disc's bytes)"))
    for other in ("%s.ZIT" % name, "%s.TXR" % name, "%s.BIN" % name, "%sT.BIN" % name, "MOTIONS.ZAL"):
        data = disc.read("ASSETS/LEVELS/%s/%s" % (folder, other))
        if data is not None:
            with open(os.path.join(out_dir, other), "wb") as f:
                f.write(data)
            print("%s: copied" % other)


# ---- a level from records ------------------------------------------------------------

def make_test_level():
    """A floor of two triangles with a grindable ledge edge, a quarter-pipe face tag, one instance, a
    PlayerOne spawn and one visibility zone: the smallest level these writers make."""
    positions = [(-10.0, 0.0, -10.0), (10.0, 0.0, -10.0), (10.0, 0.0, 10.0), (-10.0, 0.0, 10.0)]
    faces = [Face.make(0, 0, 1, 2), Face.make(0, 0, 2, 3)]
    groups = StripGroup.from_strip(0, [0, 1, 3, 2])
    edges = TagBuffer([EdgeTag(L.name_hash("ledge"), 0, EdgeTag.GRINDABLE, 2)], [0] + [None] * 5)
    tags = TagBuffer([FaceTag(1, 1)], [0, None])
    part = Part(1, positions, faces, groups, colours=IndexedStream.make([b"\x80\x80\x80\x80"] * 4),
                uvs=b"".join(struct.pack("<2f", u, v) for u, v in ((0, 0), (1, 0), (1, 1), (0, 1))),
                edge_tags=edges, face_tags=tags, material_count=1)
    header = GeoHeader((-10.0, 0.0, -10.0), (10.0, 0.0, 10.0), 1, "floor")
    obj = GeoObject(header, b"\x01\0\x01\0", [(part, LevelOfDetail(500.0, 1), Chunk(b"TMTB", kids=[]))],
                    type_data=struct.pack("<I", L.name_hash("Terrain")),
                    collision=CollisionTree.build(1, positions, faces))
    geo = Geometry([obj])
    inst = Instance.make("floor_01", obj, (0.0, 0.0, 0.0))
    ins = Instances([inst])
    dat = SceneData([ScenePoint.spawn("PlayerOne", (0.0, -1.0, 0.0))], [ViewRegion(L.name_hash("zone_a"), (-20.0, -20.0, -20.0), (20.0, 20.0, 20.0), [])])
    return geo, ins, dat


def cmd_selftest(args):
    geo, ins, dat = make_test_level()
    g, i, d = geo.write(), ins.write(), dat.write()
    # read back through this module and ai_level.py
    g2, i2, d2 = Geometry.read(g), Instances.read(i), SceneData.read(d)
    assert Geometry.read(g2.write()).write() == g and i2.write() == i and d2.write() == d
    tris = L.collision_triangles(g, i)
    assert len(tris) == 1 and len(tris[0][4]) == 2, tris
    edges = L.grind_edges(g, i)
    assert edges == [((-10.0, 0.0, -10.0), (10.0, 0.0, -10.0), "floor_01", 0)], edges
    assert L.topmost(tris, 0.0, 0.0) == 0.0
    part = g2.objects[0].parts[0][0]
    assert [len(x.entries) for x in part.groups] == [4] and part.groups[0].entries == [0x8000, 0x8001, 3, 0x4002]
    assert part.edge_tags.records[0].material == 2 and part.face_tags.records[0].kind == 1
    leaves = g2.objects[0].collision.leaves()
    assert len(leaves) == 1 and leaves[0].faces == [0, 1]
    # a long strip is cut into groups of 60 with the restart pair
    groups = StripGroup.from_strip(0, list(range(100)))
    assert [len(x.entries) for x in groups] == [60, 42] and groups[1].entries[:2] == [0x8000 | 58, 0x8000 | 59]
    assert all(len(x.write()) % 4 == 0 for x in groups)
    # a tag buffer's indices pad to 16, a tree's chunk to 4
    assert len(TagBuffer([], [None] * 3).write()) == 8 + 16
    assert len(g2.objects[0].collision.write()) % 4 == 0
    assert d2.points[0].kind == SPAWN and d2.points[0].name == PLAYER_ONE and d2.regions[0].flag == 0
    # a made header's word is its name's hash, of the whole name when the field cuts it (AI's disc: CARPLANT's object
    # PropDef_vert-truss_short_carplant, field "PropDef_vert-truss_short_carpla", word 0x0075F094)
    assert g2.objects[0].header.name_hash == L.name_hash("floor") and struct.unpack_from("<I", g, 0x10 + 0x8 + 0x3C)[0] == L.name_hash("floor")
    long = GeoHeader((0.0,) * 3, (0.0,) * 3, 1, "PropDef_vert-truss_short_carplant")
    assert long.name == "PropDef_vert-truss_short_carpla" and long.name_hash == 0x0075F094
    assert GeoHeader.read(long.write()) == long
    assert i2.instances[0].world_min == (-10.0, 0.0, -10.0) and i2.instances[0].world_max == (10.0, 0.0, 10.0)
    # the plain encoding of an indexed stream reads back the same
    geo.objects[0].parts[0][0].expand_streams()
    g3 = geo.write()
    assert L.collision_triangles(g3, i) == tris and isinstance(Geometry.read(g3).objects[0].parts[0][0].colours, bytes)
    print("ai_level_write selftest: ok (%d + %d + %d bytes)" % (len(g), len(i), len(d)))


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("roundtrip")
    r.add_argument("--disc", required=True)
    r.add_argument("--level", action="append")
    r.add_argument("--verbose", action="store_true")
    r.set_defaults(fn=cmd_roundtrip)
    w = sub.add_parser("rewrite")
    w.add_argument("--disc", required=True)
    w.add_argument("--level", required=True)
    w.add_argument("--out", required=True)
    w.add_argument("--rebuild-trees", choices=["stock", "all"], help="collision trees built here, over the stock trees' faces or every 0x2000 face")
    w.add_argument("--plain-streams", action="store_true", help="indexed streams (TIV*, TIFN) written plain")
    w.set_defaults(fn=cmd_rewrite)
    t = sub.add_parser("selftest")
    t.set_defaults(fn=cmd_selftest)
    args = p.parse_args(argv)
    try:
        code = args.fn(args)
    except ValueError as e:
        raise SystemExit("ai_level_write.py: %s" % e)
    return code or 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
