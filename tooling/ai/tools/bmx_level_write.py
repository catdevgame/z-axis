#!/usr/bin/env python3
"""BMX XXX's level files written: the ZGE4 chunks of a level's GEO, INS, DAT and ZIT as typed records, its
asset libraries (zge_library.py) and its objectives (OBJCTV.TXT), read and written back as bytes the game
loads, with the round trips that prove them on the stock levels and the editor's themes.

    python3 -I bmx_level_write.py roundtrip [--disc <BMX disc folder>] [--level L ...] [--verbose]
                                       every level folder (the eight levels and the park editor's five
                                       folders) read, written, read back and compared, chunk by chunk and as
                                       whole files; the collision surface, the grind edges and the goals' places
                                       read back the same from the written files
    python3 -I bmx_level_write.py rewrite [--disc <BMX disc folder>] --level L --out <folder>
                                         [--rebuild-trees stock|all] [--plain-streams]
                                       the level written through the writers into <folder>/ASSETS/LEVELS/L/ (or
                                       ASSETS/EDITOR/L/: a Mods folder's layout), every other file of the folder
                                       copied; the options as ai_level_write.py's
    python3 -I bmx_level_write.py census [--disc <BMX disc folder>]
                                       the fields the game does not read that are not zero on disc
    python3 -I bmx_level_write.py selftest   a small level made from records, written, read back

ZGE4 is AI's ZGE3 with these differences, each taken from BMX XXX's own loaders and readers (game/zge
with its kIsBmx branches, game/bmx):

  - The tag records are the bike games' (zge_tags.py): TGFE records of 8 bytes and TGFT records of
    12 (AI's are 24 and 28), read by game/bmx/rider/collision/tags.cpp. A part can also have TGFV, its
    corner tags (one index per face corner: the fast cars' skitch holds; registerGeometryChunkHandlers
    adds readTgfvChunk after TGFE).
  - The collision tree (TGCI) is AI's: offset leaves, and the first word is the collision part's id.
    BMX XXX's part ids are large numbers (name hashes): dm2_level.tree_faces tells a BMX XXX tree from
    a DM2 one by a first word over 0xFFFF, so a part of our own takes its name's hash as its id.
  - The GEO has more top-level chunks after its objects, kept as they are: ZDYN (dynamics bodies, as
    AI's), ZSCD and ZSCR (cut scenes: zge/scene/cut_scenes.cpp), ZFPH, ZFPD and ZFPT (the fast cars'
    paths: fast_paths.cpp), WSMC (WSMO records: chunk_reader.cpp readWsmcChunk) and ZPEC (particle
    emitters: particle_emitter_objects.cpp). A level of our own needs none of them.
  - TGIH, SDPT, PVSO, TEHD, MATI (0xA0 bytes, one layer read: kMatiLoadedLayers) and the TXR are AI's
    layouts (node.h InstanceChunk: the name at +0x50 in ZGE3 and ZGE4).
  - The level's goals are a text file, OBJCTV.TXT (game/bmx/objectives/objectives.cpp loadLevelObjectives:
    read in a session of kind 0, the tour; a level without the file has none). It is nested blocks
    (NAME_BEGIN ... NAME_END) of KEY "value" lines with /* */ and // comments (ObjectivesFile).
  - A level folder (ASSETS/LEVELS/<L>) also has ANIMS.ZAL, CUTCOMP.ZAL, CUTSCENE.ZAL and MODELS.ZAL; a
    park editor theme (ASSETS/EDITOR/<T>) ANIMS.ZAL, MODELS.ZAL and its grid (<T>.MAP), its object index
    (<T>.ZIX) and the exporter's logs (<T>.TXT, OUTPUTF), copied as they are.

Fields the game does not read that are not zero on disc are kept in the records, so the stock files round
trip byte for byte: FaceTag12.leftover, CornerTag4.leftover, the tree nodes' leftover byte (AI's) and the
asset libraries' slack. A level of our own writes zeros there (the census command lists them).

Python 3.8+, standard library only.
"""
import argparse
import os
import re
import struct
import sys
from collections import Counter
from dataclasses import dataclass, field
from typing import List, Optional, Tuple, Union

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ai_level as L  # noqa: E402
import ai_level_write as W  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
import zpaths  # noqa: E402  (tooling/paths.env: BMX_DATA)
import dm2_level as D  # noqa: E402
import zge_library as Z  # noqa: E402
import zge_scene as S  # noqa: E402
import zge_tags as T  # noqa: E402

Chunk = L.Chunk


# ---- GEO ---------------------------------------------------------------------------------

@dataclass
class BmxPart(W.Part):
    """A ZGE4 part: AI's TGPT with the bike games' tag records and the corner tags (TGFV, CornerTag4 records,
    one index per face corner: 3 x face + corner)."""
    corner_tags: Optional[W.TagBuffer] = None

    EDGE_TAG = T.EdgeTag8
    FACE_TAG = T.FaceTag12
    CORNER_TAG = T.CornerTag4

    # BMX XXX's exporter's order (every stock part: the streams first, then positions, faces, strips, tags).
    DEFAULT_ORDER = [b"TGPH", b"TIVC", b"TGVC", b"TIVN", b"TGVN", b"TIVU", b"TGVU", b"TIFN", b"TGFN", b"TGVP", b"TGFP",
                     b"TGFM", b"TGFV", b"TGFE", b"TGFT"]

    @classmethod
    def read(cls, part):
        self = super(BmxPart, cls).read(part)
        kept = []
        for k in self.other:
            if k.tag == b"TGFV":
                self.corner_tags = W.TagBuffer.read(k.data, cls.CORNER_TAG)
            else:
                kept.append(k)
        self.other = kept
        return self

    def chunks(self):
        out = super().chunks()
        if self.corner_tags is not None:
            out[b"TGFV"] = Chunk(b"TGFV", self.corner_tags.write())
        return out


class BmxObject(W.GeoObject):
    """A ZGE4 geometry object (TGEO): AI's, with ZGE4's parts. The tree is AI's offset-leaf one."""
    PART = BmxPart


class BmxGeometry(W.Geometry):
    """A ZGE4 GEO: FORM { TGEO ..., ZDYN, ZSCD, ZSCR, ZFPH, ZFPD, ZFPT, WSMC, ZPEC } (the module's docstring);
    the chunks after the objects are kept as they are."""
    OBJECT = BmxObject


# INS and DAT: AI's records (their layouts are ZGE4's too). The names a converter takes its records by
# (ai_level_convert.Converter.RECORDS): Geometry, Instance, Instances.
Geometry = BmxGeometry
Instance = W.Instance
Instances = W.Instances
SceneData = W.SceneData


# ---- ZIT -----------------------------------------------------------------------------------

# The ZIT is the same form in all three games: zge_scene.SceneFile (BMX XXX's names are in capitals).
SceneFile = S.SceneFile


# ---- OBJCTV.TXT ------------------------------------------------------------------------------

@dataclass
class ObjectiveField:
    """A `KEY "value"` line of an objectives file. `space` is the text before the key and before the value
    (whitespace and comments: what the game's reader skips), kept from the file; None writes the canonical
    layout (a new line, the block's tabs, a tab)."""
    key: str
    value: str
    space: Optional[Tuple[str, str]] = None


@dataclass
class ObjectiveBlock:
    """A `KIND_BEGIN ... KIND_END` block (OBJECTIVE, PARAMETERS, TRANSFER, ACTIONS, ...): its fields and
    blocks in order. `space` is the text before KIND_BEGIN and before KIND_END, kept as ObjectiveField's."""
    kind: str
    items: List[Union["ObjectiveField", "ObjectiveBlock"]] = field(default_factory=list)
    space: Optional[Tuple[str, str]] = None

    def get(self, key, default=None):
        return next((i.value for i in self.items if isinstance(i, ObjectiveField) and i.key == key), default)

    def blocks(self, kind=None):
        return [i for i in self.items if isinstance(i, ObjectiveBlock) and (kind is None or i.kind == kind)]


@dataclass
class ObjectivesFile:
    """OBJCTV.TXT: the level's objectives, OBJECTIVE blocks at the top level. `tail` is the text after the
    last block. Read as tokens (a word, or a quoted string) with the text between them kept, so a stock
    file writes back byte for byte; a file of our own is written with tabs and CRLF line ends, as the stock
    files are."""
    items: List[Union[ObjectiveField, ObjectiveBlock]] = field(default_factory=list)
    tail: str = "\r\n"

    TOKEN = re.compile(r'(\s+|/\*.*?\*/|//[^\n]*)*("[^"]*"|[^\s"]+)', re.S)
    SPACE = re.compile(r'(\s+|/\*.*?\*/|//[^\n]*)*', re.S)

    @classmethod
    def read(cls, data):
        text = data.decode("latin1")
        tokens, at = [], 0
        while True:
            m = cls.TOKEN.match(text, at)
            if not m:
                break
            tokens.append((text[at:m.start(2)], m.group(2)))
            at = m.end()
        tail = text[at:]
        if cls.SPACE.fullmatch(tail) is None:
            raise ValueError("objectives text past the last token at %d" % at)
        root = ObjectiveBlock("")
        stack = [root]
        i = 0
        while i < len(tokens):
            space, word = tokens[i]
            if word.endswith("_BEGIN") and not word.startswith('"'):
                block = ObjectiveBlock(word[:-6], [], (space, ""))
                stack[-1].items.append(block)
                stack.append(block)
                i += 1
            elif word.endswith("_END") and not word.startswith('"'):
                if len(stack) < 2 or stack[-1].kind != word[:-4]:
                    raise ValueError("%s closes %s" % (word, stack[-1].kind or "nothing"))
                stack[-1].space = (stack[-1].space[0], space)
                stack.pop()
                i += 1
            else:
                if i + 1 >= len(tokens) or not tokens[i + 1][1].startswith('"') or word.startswith('"'):
                    raise ValueError("%r is not a KEY \"value\" pair" % word)
                stack[-1].items.append(ObjectiveField(word, tokens[i + 1][1][1:-1], (space, tokens[i + 1][0])))
                i += 2
        if len(stack) != 1:
            raise ValueError("%s_BEGIN without its end" % stack[-1].kind)
        return cls(root.items, tail)

    def write(self):
        out = []

        def emit(items, depth):
            for it in items:
                if isinstance(it, ObjectiveField):
                    before, between = it.space or ("\r\n" + "\t" * depth, "\t")
                    out.append('%s%s%s"%s"' % (before, it.key, between, it.value))
                else:
                    before, end = it.space or (("\r\n\r\n" if depth == 0 else "\r\n") + "\t" * depth, "\r\n" + "\t" * depth)
                    out.append("%s%s_BEGIN" % (before, it.kind))
                    emit(it.items, depth + 1)
                    out.append("%s%s_END" % (end, it.kind))

        emit(self.items, 0)
        return ("".join(out) + self.tail).encode("latin1")

    @property
    def objectives(self):
        return [i for i in self.items if isinstance(i, ObjectiveBlock) and i.kind == "OBJECTIVE"]

    def shape(self):
        """The file without its layout: what the game reads ((kind, [...]) and (key, value) in order)."""
        def walk(items):
            return [(i.key, i.value) if isinstance(i, ObjectiveField) else (i.kind, walk(i.items)) for i in items]
        return walk(self.items)


# ---- level folders -------------------------------------------------------------------------------

LEVELS = ["CHL_DAM", "CHL_MNCO", "CHL_MULY", "CHL_NASA", "CHL_SHEP", "CHL_SYRA", "CMP_RMPG", "CMP_ROOT"]
THEMES = ["MULLALY", "UGP", "RAMPAGE", "ZAXIS", "OBJECTS"]   # ASSETS/EDITOR/<T>: the park editor's four themes and its pieces
LIBRARIES = {"level": ["ANIMS.ZAL", "CUTCOMP.ZAL", "CUTSCENE.ZAL", "MODELS.ZAL"], "theme": ["ANIMS.ZAL", "MODELS.ZAL"]}
DEFAULT_DISC = os.environ.get("BMX_DISC", os.path.join(zpaths.BMX_DATA, "disc"))


def folder_of(name):
    """(the folder's path on the disc, its kind) for a level or theme name."""
    name = name.upper()
    if name in THEMES:
        return "ASSETS/EDITOR/" + name, "theme"
    return "ASSETS/LEVELS/" + name, "level"


def folder_files(disc, name):
    """{file name: bytes} of everything in a level or theme folder on the disc (a folder, as BMX's disc
    files are kept)."""
    path, _ = folder_of(name)
    root = os.path.join(disc.root, *path.split("/"))
    names = {n.upper(): n for n in os.listdir(root)} if os.path.isdir(root) else {}
    if not names:
        raise ValueError("%s is not on the disc" % path)
    out = {}
    for upper, real in sorted(names.items()):
        with open(os.path.join(root, real), "rb") as f:
            out[upper] = f.read()
    return out


# ---- readers -------------------------------------------------------------------------------------

def grind_edges(geo, ins):
    """The grindable edges, by the bike games' reader (dm2_xbox.grind_edges: TGFE byte 5 bit 0)."""
    import dm2_xbox
    return dm2_xbox.grind_edges(geo, ins)


def readers_agree(geo_a, ins_a, geo_b, ins_b):
    """The bike games' readers (dm2_level's collision surface and goals' places, dm2_xbox's grind edges: what
    the playbot's BMX XXX map and the panel map read) give the same from both pairs of files."""
    a, b = D.collision_triangles(geo_a, ins_a), D.collision_triangles(geo_b, ins_b)
    if a != b:
        raise ValueError("the collision triangles read back differ")
    ea = grind_edges(geo_a, ins_a)
    if ea != grind_edges(geo_b, ins_b):
        raise ValueError("the grind edges read back differ")
    ga = D.goal_places(geo_a, ins_a)
    if ga != D.goal_places(geo_b, ins_b):
        raise ValueError("the goals' places read back differ")
    return len(a), sum(len(g[4]) for g in a), len(ea), len(ga)


# ---- the round trip --------------------------------------------------------------------------------

def roundtrip_geo(data, rt):
    """Each typed chunk of the GEO written back and compared (ai_level_write's, with ZGE4's records), then
    the whole file."""
    tree = L.parse(data)
    geo = BmxGeometry.read(data)
    objects = [k for k in tree[0].kids if k.tag == b"TGEO"]
    for obj, rec in zip(objects, geo.objects):
        rt.chunk(b"TGHD", next(k.data for k in obj.kids if k.tag == b"TGHD"), rec.header.write())
        for k in obj.kids:
            if k.tag == b"TGCI":
                rt.chunk(b"TGCI", k.data, rec.collision.write())
        parts = [k for k in obj.kids if k.tag == b"TGPT"]
        for part, (prec, _, _) in zip(parts, rec.parts):
            chunks = prec.chunks()
            for k in part.kids:
                rt.chunk(k.tag, k.data if k.kids is None else L.serialize([k]),
                         chunks[k.tag].data if k.kids is None else L.serialize([chunks[k.tag]]))
            rt.chunk(b"TGPT", L.serialize([part]), L.serialize([prec.write()]))
        rt.chunk(b"TGEO", L.serialize([obj]), L.serialize([rec.write()]))
    out = geo.write()
    rt.files["GEO"] = (data == out, W.first_difference(data, out), len(data), len(out))
    return geo


def roundtrip_folder(files, kind, rt):
    """Every file of a level or theme folder the writers know, read and written back: GEO, INS, DAT and
    ZIT, the libraries and OBJCTV.TXT. Returns the records."""
    name = next(n[:-4] for n in files if n.endswith(".ZIT"))
    geo = roundtrip_geo(files[name + ".GEO"], rt)
    ins = W.roundtrip_ins(files[name + ".INS"], rt)
    dat = W.roundtrip_dat(files[name + ".DAT"], rt)
    zit = SceneFile.read(files[name + ".ZIT"])
    out = zit.write()
    rt.files["ZIT"] = (out == files[name + ".ZIT"], W.first_difference(files[name + ".ZIT"], out), len(out), len(out))
    for lib in LIBRARIES[kind]:
        if lib in files:
            out = Z.AssetLibrary.read(files[lib]).write()
            rt.files[lib] = (out == files[lib], W.first_difference(files[lib], out), len(files[lib]), len(out))
    if "OBJCTV.TXT" in files:
        obj = ObjectivesFile.read(files["OBJCTV.TXT"])
        out = obj.write()
        if ObjectivesFile.read(out).shape() != obj.shape():
            raise ValueError("OBJCTV.TXT reads back other objectives")
        rt.files["OBJCTV.TXT"] = (out == files["OBJCTV.TXT"], W.first_difference(files["OBJCTV.TXT"], out), len(out), len(out))
    return name, geo, ins, dat


def cmd_roundtrip(args):
    disc = L.Disc(args.disc)
    names = [n.upper() for n in (args.level or LEVELS + THEMES)]
    all_same = True
    for name in names:
        files = folder_files(disc, name)
        _, kind = folder_of(name)
        rt = W.RoundTrip()
        stem, geo, ins, dat = roundtrip_folder(files, kind, rt)
        groups, tris, edges, goals = readers_agree(files[stem + ".GEO"], files[stem + ".INS"], geo.write(), ins.write())
        parts = sum(len(o.parts) for o in geo.objects)
        corners = sum(1 for o in geo.objects for p, _, _ in o.parts if p.corner_tags is not None)
        print("%s: %d objects, %d parts (%d with corner tags), %d instances, %d points, %s zones; %d collidable instances "
              "(%d triangles), %d grind edges and %d goal names read back the same" %
              (name, len(geo.objects), parts, corners, len(ins.instances), len(dat.points),
               len(dat.regions) if dat.regions is not None else "no", groups, tris, edges, goals))
        for ext, (same, at, n_in, n_out) in rt.files.items():
            all_same &= same
            print("  %s: %s (%d bytes)" % (ext, "byte-identical" if same else "differs at 0x%x (%d written)" % (at, n_out), n_in))
        if args.verbose or rt.differ:
            print("\n".join(rt.report()))
    print("all files byte-identical" if all_same else "some files differ")
    return 0 if all_same else 1


def cmd_census(args):
    """What the stock files hold in the fields no reader of BMX XXX's reads. (The padding the writers write
    as zeros, the tag buffers' index pads and the chunks' pads to 4, is zero in every stock file: the round
    trip is byte-identical.)"""
    disc = L.Disc(args.disc)
    c = Counter()
    for name in LEVELS + THEMES:
        files = folder_files(disc, name)
        stem = next(n[:-4] for n in files if n.endswith(".ZIT"))
        geo = BmxGeometry.read(files[stem + ".GEO"])
        for o in geo.objects:
            if o.collision is not None:
                for n in _nodes(o.collision.root):
                    c["tree nodes"] += 1
                    c["tree nodes with a leftover byte"] += n.leftover != 0
            for p, _, _ in o.parts:
                for buf, label in ((p.edge_tags, "TGFE"), (p.face_tags, "TGFT"), (p.corner_tags, "TGFV")):
                    if buf is None:
                        continue
                    for r in buf.records:
                        c[label + " records"] += 1
                        if label == "TGFT":
                            c["TGFT leftover not zero"] += r.leftover != b"\0\0"
                            c["TGFT link not zero"] += r.link != 0
                        if label == "TGFV":
                            c["TGFV leftover not zero"] += r.leftover != b"\0\0\0"
                        if label == "TGFE":
                            c["TGFE grind side not 3"] += r.grind_side != 3
        for lib in LIBRARIES["theme" if name in THEMES else "level"]:
            if lib in files:
                z = Z.AssetLibrary.read(files[lib])
                c["library slack bytes not zero"] += sum(1 for e in z.entries for b in (e.slack or b"") if b) + sum(1 for b in z.lead or b"" if b)
    for k, n in sorted(c.items()):
        print("  %-36s %d" % (k, n))


def _nodes(n):
    yield n
    if isinstance(n, W.FaceTreeSplit):
        yield from _nodes(n.greater)
        yield from _nodes(n.lesser)


def cmd_rewrite(args):
    disc = L.Disc(args.disc)
    name = args.level.upper()
    path, kind = folder_of(name)
    files = folder_files(disc, name)
    out_dir = os.path.join(args.out, *path.split("/"))
    os.makedirs(out_dir, exist_ok=True)
    geo, ins, dat = BmxGeometry.read(files[name + ".GEO"]), Instances.read(files[name + ".INS"]), SceneData.read(files[name + ".DAT"])
    if args.rebuild_trees or args.plain_streams:
        trees, streams = W.variant(geo, args.rebuild_trees, args.plain_streams)
        print("variant: %d collision trees built here, %d indexed streams written plain" % (trees, streams))
    written = {name + ".GEO": geo.write(), name + ".INS": ins.write(), name + ".DAT": dat.write(),
               name + ".ZIT": SceneFile.read(files[name + ".ZIT"]).write()}
    for lib in LIBRARIES[kind]:
        if lib in files:
            written[lib] = Z.AssetLibrary.read(files[lib]).write()
    if "OBJCTV.TXT" in files:
        written["OBJCTV.TXT"] = ObjectivesFile.read(files["OBJCTV.TXT"]).write()
    groups, tris, edges, goals = readers_agree(files[name + ".GEO"], files[name + ".INS"], written[name + ".GEO"], written[name + ".INS"])
    print("%d collidable instances (%d triangles), %d grind edges, %d goal names read back the same" % (groups, tris, edges, goals))
    for fname, data in sorted(files.items()):
        out = written.get(fname, data)
        with open(os.path.join(out_dir, fname), "wb") as f:
            f.write(out)
        print("%s: %d bytes, %s" % (fname, len(out), "copied" if fname not in written else
                                    "written" + ("" if out == data else " (not the disc's bytes)")))


# ---- a level from records --------------------------------------------------------------------------

def make_test_level():
    """A floor of two triangles with a grindable edge, a quarter pipe's face tag, a corner tag, one
    instance, a PlayerOne spawn, a zone, a ZIT and an objectives file with no objectives."""
    positions = [(-10.0, 0.0, -10.0), (10.0, 0.0, -10.0), (10.0, 0.0, 10.0), (-10.0, 0.0, 10.0)]
    faces = [W.Face.make(0, 0, 1, 2), W.Face.make(0, 0, 2, 3)]
    part_id = L.name_hash("floor")
    part = BmxPart(part_id, positions, faces, W.StripGroup.from_strip(0, [0, 1, 3, 2]),
                   colours=W.IndexedStream.make([b"\x80\x80\x80\x80"] * 4),
                   uvs=W.IndexedStream.make([struct.pack("<2f", u, v) for u, v in ((0, 0), (1, 0), (1, 1), (0, 1))]),
                   edge_tags=W.TagBuffer([T.EdgeTag8(L.name_hash("ledge"), 0, T.EdgeTag8.GRINDABLE)], [0] + [None] * 5),
                   face_tags=W.TagBuffer([T.FaceTag12(0, 1)], [0, None]),
                   corner_tags=W.TagBuffer([T.CornerTag4(T.CornerTag4.HOLD)], [None] * 5 + [0]),
                   material_count=1)
    header = W.GeoHeader((-10.0, 0.0, -10.0), (10.0, 0.0, 10.0), L.name_hash("floor_geo"), "floor")
    obj = BmxObject(header, b"\x01\0\x01\0", [(part, W.LevelOfDetail(500.0, part_id), Chunk(b"TMTB", kids=[]))],
                    type_data=struct.pack("<I", L.name_hash("Terrain")),
                    collision=W.CollisionTree.build(part_id, positions, faces))
    geo = BmxGeometry([obj])
    ins = Instances([W.Instance.make("floor_01", obj, (0.0, 0.0, 0.0))])
    dat = SceneData([W.ScenePoint.spawn("PlayerOne", (0.0, -1.0, 0.0))],
                    [W.ViewRegion(L.name_hash("zone_a"), (-20.0, -20.0, -20.0), (20.0, 20.0, 20.0), [])])
    zit = SceneFile.make("TEST", (-50.0, -50.0, -50.0), (50.0, 50.0, 50.0))
    objectives = ObjectivesFile([])
    return geo, ins, dat, zit, objectives


def cmd_selftest(args):
    geo, ins, dat, zit, objectives = make_test_level()
    g, i, d, z = geo.write(), ins.write(), dat.write(), zit.write()
    g2 = BmxGeometry.read(g)
    assert g2.write() == g and Instances.read(i).write() == i and SceneData.read(d).write() == d
    assert SceneFile.read(z) == zit and SceneFile.read(z).write() == z and zit.names[b"ZFGE"] == "TEST.GEO"
    part = g2.objects[0].parts[0][0]
    assert isinstance(part, BmxPart) and part.corner_tags.records == [T.CornerTag4(1)] and part.corner_tags.indices[5] == 0
    assert part.edge_tags.records[0].grindable and part.face_tags.records[0].surface_kind == 1
    assert [k.tag for k in L.parse(g)[0].kids[0].kids[3].kids][-3:] == [b"TGFV", b"TGFE", b"TGFT"]
    # the bike games' readers see the floor, the edge and the tags
    tris = D.collision_triangles(g, i)
    assert len(tris) == 1 and len(tris[0][4]) == 2 and D.tree_faces(g2.objects[0].collision.write()) == {0, 1}
    assert [e[:2] for e in grind_edges(g, i)] == [((-10.0, 0.0, -10.0), (10.0, 0.0, -10.0))]
    assert D.goal_places(g, i)[L.name_hash("ledge")]["edge"] == [(-10.0, 0.0, -10.0), (10.0, 0.0, -10.0)]
    # objectives: a written file reads back, a stock-like one keeps its layout
    sample = (b'/*\r\n *\tA level\r\n */\r\n\r\n\r\nOBJECTIVE_BEGIN\r\n\tNAME\t\t\t"Gap01"\r\n\tTYPE\t"TRANSFER"\r\n'
              b'\tPARAMETERS_BEGIN\r\n\t\tTRANSFER_BEGIN\r\n\t\t\tFROM_FACE\t"gap_01a" // a comment\r\n\t\tTRANSFER_END\r\n'
              b'\tPARAMETERS_END\r\nOBJECTIVE_END\r\n')
    o = ObjectivesFile.read(sample)
    assert o.write() == sample and o.objectives[0].get("NAME") == "Gap01"
    assert o.objectives[0].blocks("PARAMETERS")[0].blocks()[0].get("FROM_FACE") == "gap_01a"
    o.objectives[0].items.append(ObjectiveField("PERSISTENT", "YES"))
    assert ObjectivesFile.read(o.write()).shape() == o.shape()
    made = ObjectivesFile([ObjectiveBlock("OBJECTIVE", [ObjectiveField("NAME", "a"), ObjectiveBlock("PARAMETERS")])])
    assert ObjectivesFile.read(made.write()).shape() == made.shape() == [("OBJECTIVE", [("NAME", "a"), ("PARAMETERS", [])])]
    assert objectives.write() == b"\r\n" and ObjectivesFile.read(b"").items == []
    # libraries
    lib = Z.AssetLibrary([Z.LibraryEntry(5, b"FORM" + b"\0" * 4)])
    assert Z.AssetLibrary.read(lib.write()).entries[0].data == b"FORM\0\0\0\0"
    print("bmx_level_write selftest: ok (%d + %d + %d + %d bytes)" % (len(g), len(i), len(d), len(z)))


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("roundtrip")
    r.add_argument("--disc", default=DEFAULT_DISC)
    r.add_argument("--level", action="append")
    r.add_argument("--verbose", action="store_true")
    r.set_defaults(fn=cmd_roundtrip)
    c = sub.add_parser("census")
    c.add_argument("--disc", default=DEFAULT_DISC)
    c.set_defaults(fn=cmd_census)
    w = sub.add_parser("rewrite")
    w.add_argument("--disc", default=DEFAULT_DISC)
    w.add_argument("--level", required=True)
    w.add_argument("--out", required=True)
    w.add_argument("--rebuild-trees", choices=["stock", "all"])
    w.add_argument("--plain-streams", action="store_true")
    w.set_defaults(fn=cmd_rewrite)
    t = sub.add_parser("selftest")
    t.set_defaults(fn=cmd_selftest)
    args = p.parse_args(argv)
    try:
        code = args.fn(args)
    except ValueError as e:
        raise SystemExit("bmx_level_write.py: %s" % e)
    return code or 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
