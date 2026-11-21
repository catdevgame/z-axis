#!/usr/bin/env python3
"""A glTF model with named objects into BMX XXX's level files (ZGE4): ai_level_convert.py's converter (the same
model, tags, axes, budgets, strips, textures and materials) with BMX XXX's records written by
bmx_level_write.py, served from a Mods folder in a level row's slot.

    python3 -I bmx_level_convert.py convert MODEL.gltf --out DIR [--slot CHL_MULY] [--level-name NAME] [--sky SKY.png]
                                      DIR/ASSETS/LEVELS/<slot>/<slot>.{ZIT,GEO,INS,TXR,DAT}, the level's four asset
                                      libraries, empty (ANIMS, CUTCOMP, CUTSCENE, MODELS.ZAL), and an OBJCTV.TXT with
                                      no objectives: a Mods folder's layout. --sky: the picture as the level's sky,
                                      as ai_level_convert's, in BMX XXX's form (BMX_SKY)
    python3 -I bmx_level_convert.py example --out DIR    ai_level_convert.py's test park (park.gltf)
    python3 -I bmx_level_convert.py selftest              the example converted, read back by the bike games'
                                                         readers and checked

The slot. A level row (game/bmx/profile/players.h LevelInfo) is a plain level load from ASSETS/LEVELS/<name>,
reached by HARDCORE TOUR's level select, and played in the row's game mode (LevelInfo::gameMode). CHL_MULY (The
Bronx, the select's first row and the one level open from the start) is the default: a challenge level, ridden
with no clock for as long as the player likes; a competition row (CMP_RMPG, CMP_ROOT) is a two-minute run. Every
stock level folder has an OBJCTV.TXT naming the stock level's objects, and the mods are read over the disc, so the
converter writes one with no objectives: the level has no goals (loadLevelObjectives counts the player's goals in
that level's record from the file: none). Park level 12 is
not a level slot: it is the park editor's Mullaly theme (ASSETS/EDITOR/MULLALY) with a saved park built over it
(buildParkCells puts a terrain tower on each of its 30 x 20 cells), so a converted park there would have the
editor's floor laid through it.

What is BMX XXX's here, against AI's converter (bmx_level_write.py's docstring has the formats):
  - Surface kinds (a TGFT record's byte 8, as the stock levels use them; game/bmx/rider): #qp is 1 (a
    quarter pipe's or lip's face, linked to the lip edge of link kind 1), #spine 2, #kicker=P 3 (read by
    the rider as 4: a take-off at P percent of the top speed, the record's launch byte; 50 by default),
    #wall 8 (the face is never ground: a wall). AI's #slope has no BMX XXX kind and is left as ground,
    which the report says.
  - Grind edges (TGFE records of 8 bytes): every #grind mark is grindable (flags bit 0), its side found
    by the face's normal (grind side 3, as every stock record); #grind=lip links the edge to the
    quarter pipe's faces (link kind 1), #grind=spine to a spine's (2); BMX XXX has no poles, so #vpole
    and #hpole are plain grind edges.
  - Sounds come from a face's material (the MATI's tag record, its first byte: materialSound), not
    from the edge: a material tagged #metal (or named "metal") is 1, #wood ("wood") 2, else 0, concrete
    (the stock materials' values for textures of those names). The AI converter's #metal and #wood on
    a grind mark have no BMX XXX field.
  - Every object's and part's id is its name's hash, as BMX XXX's are (all 3,675 stock parts' ids are
    over 0xFFFF, which is how the bike games' reader tells BMX XXX's collision trees from DM2's).
    Every collidable object gets a collision tree: BMX XXX's query has no tree-less fallback in the
    readers the playbot uses.
  - The strips keep AI's winding mark (0x4000 on an entry whose triangle is flipped): BMX XXX's own strips
    have it (310,838 of 947,927 stock entries, in 4,091 of 4,121 parts), unlike DM2's.
  - The spawn is a Spawn point named PlayerOne (PlayerTwo the second player's, the respawns unnamed). The stock
    levels' points have 4 tag bytes; this converter writes none, as AI's.
  - The idle cameras are CameraPos points named idle, 1 to N, the first tag byte the field of view (the other
    three, which vary on the disc and nothing reads, 0); a model with none gets one behind its PlayerOne spawn.

Python 3.8+, standard library only.
"""
import argparse
import math
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ai_level as L  # noqa: E402
import ai_level_convert as C  # noqa: E402
import ai_level_write as W  # noqa: E402
import bmx_level_write as B  # noqa: E402
import dm2_level as D  # noqa: E402
import gltf_model as G  # noqa: E402
import zge_library as Z  # noqa: E402
import zge_tags as T  # noqa: E402

Chunk = L.Chunk

DEFAULT_SLOT = "CHL_MULY"
# The surface kinds a material's tag gives (TGFT byte 8, as stored).
SURFACE_KINDS = {"qp": 1, "spine": 2, "kicker": 3, "wall": 8}
DEFAULT_LAUNCH = 50
# The grind marks: (link kind) by AI's #grind words.
LINK_KINDS = {"ledge": 0, "rail": 0, "lip": 1, "spine": 2, "vpole": 0, "hpole": 0}
# A material's sound (MATI +0x1C, the tag record's first byte), by its tags.
SOUNDS = {"metal": 1, "wood": 2}
MATI_SOUND = 0x1C
LIBRARIES = B.LIBRARIES["level"]


def bmx_globals_tags(g):
    """BMX XXX's Globals record (game/bmx/game/level_globals.cpp: words at 0 the ambient colour, 8 the fog colour;
    floats at 12 the fog depth, 16 the far clip; the word at 4, DM2's clear colour, is not read: BMX XXX clears to
    black), 20 bytes as every stock level's, the fog's alpha in each colour's fourth byte as the stock levels have
    it. The fog ends at the far clip and starts its depth before it (initActionCameras)."""
    return struct.pack("<4B4B4Bff", *g.light, g.fog_alpha, *g.clear, g.fog_alpha, *g.fog, g.fog_alpha,
                       g.fog_end - g.fog_start, g.fog_end)


# BMX XXX's sky, AI's form (ZGE4 as ZGE3): the panorama part's render class 3 (each stock level's one panorama,
# class 3, its instance 0x73) and the instance a decoration with the sky bit. No fog: BMX XXX draws its eight fog
# planes after the scene, over the sky, each at the fog colour's alpha byte (As/0x80) with the colour at half its
# bytes, so the stock challenge levels' 0x1C leaves 14% of the sky (freeplay3's GS traces: the converted park's sky
# a flat (96, 113, 131), half the horizon colour); the stock competition levels' alpha 0 (CMP_RMPG, CMP_ROOT) draws
# none, and the sky is the picture. The far clip 360 m, the fog's depth 120 before it as AI's 240 to 360.
BMX_SKY = C.SkyForm(part_flags=C.PART_FLAGS_SKY, instance_flags=C.INSTANCE_FLAGS_DECORATION | C.INSTANCE_FLAG_SKY,
                    fog_alpha=0, fog_start=240.0, fog_end=360.0, far_clip=360.0, tags=bmx_globals_tags)


def objectives_file():
    """An OBJCTV.TXT with no objectives (ObjectivesFile's empty form: one line end, no tokens)."""
    return B.ObjectivesFile().write()


# BMX XXX: a prop's class data is its sound name, a 64-byte text (dynamicsObjectTouched plays "DynamicsObject.<it>"
# and the knock's kind); another game's prop gets "default", dynamicsdef_beer's (The Underground's). Its instance's
# tags are one zero word, as every stock prop's.
BMX_DYNAMICS = C.DynamicsForm("bmx", 0x63, W.c_field("default", 64), b"\0" * 4)


class BmxConverter(C.Converter):
    """ai_level_convert.Converter, writing BMX XXX's records (RECORDS: bmx_level_write's Geometry, Instance,
    Instances) and its own parts, grind edges and ZIT. The budgets are AI's: the texel budget and the texture
    side are the PS2 GS's, and the parts in view have not been measured on BMX XXX's levels."""
    RECORDS = B
    # BMX XXX's respawn takes the nearest spawn of any name (findSpawnFace): its own are unnamed (name hash 0).
    SPAWN_NAMES = {"playerone": "PlayerOne", "playertwo": "PlayerTwo", "respawn": ""}
    GAME = "bmx"
    MAKES_CAMERA = True
    SKY = BMX_SKY
    DYNAMICS = BMX_DYNAMICS
    PARTS_IN_VIEW_SOURCE = "AI's Civic Center's worst zone (not measured on BMX XXX's levels)"
    LEVEL_BYTES = 8733620
    LEVEL_BYTES_SOURCE = "CHL_MNCO's, BMX XXX's largest stock level"

    def __init__(self, model, slot=DEFAULT_SLOT, level_name=None, force=False):
        super().__init__(model, slot, level_name, force)
        self.ids = {}

    def surface_of(self, material_index):
        i = super().surface_of(material_index)
        s = self.surfaces[i]
        if not hasattr(s, "_bmx"):
            tags, base = {}, ""
            if material_index is not None and material_index >= 0:
                m = self.model.materials[material_index]
                base, tags = C.split_tags(m.name, m.extras)
            s._bmx = True
            s.kind, s.launch = 0, DEFAULT_LAUNCH
            for word, kind in SURFACE_KINDS.items():
                if word in tags:
                    s.kind = kind
                    if word == "kicker" and isinstance(tags[word], str):
                        s.launch = max(0, min(255, int(float(tags[word]))))
            if "slope" in tags:
                self.report.did("material %s: #slope has no BMX XXX surface kind; its faces are ground" % s.name)
            s.sound = next((v for k, v in SOUNDS.items() if k in tags), SOUNDS.get(base.lower(), 0))
        return i

    def add_edges(self, node, mesh, tags):
        word = tags["grind"] if isinstance(tags["grind"], str) else "ledge"
        if word not in LINK_KINDS:
            raise ValueError("node %r: #grind=%s is not one of %s" % (node.name, word, ", ".join(LINK_KINDS)))
        if word in ("vpole", "hpole"):
            self.report.did("node %s: BMX XXX has no poles; #grind=%s is a plain grind edge" % (node.name, word))
        for prim in mesh.primitives:
            world = [tuple(a + b for a, b in zip(C.to_ai(G.quat_rotate(node.rotation, tuple(c * s for c, s in zip(p, node.scale)))),
                                                 C.to_ai(node.translation))) for p in prim.positions]
            for i, k in prim.lines:
                self.edge_marks.append(C.EdgeMark(world[i], world[k], LINK_KINDS[word], T.EdgeTag8.GRINDABLE, 0, C.tag_word(tags)))

    def unique_id(self, name):
        """A name's hash, the next one up if another object or part took it: an id names one (two objects of the
        same name, a #deco mesh and its collidable one, say, get two ids; with one the instances of both drew the
        last one's geometry and the other's faces were lost)."""
        h = L.name_hash(name)
        while h in self.ids or h <= 0xFFFF:
            h = (h + 1) & 0xFFFFFFFF
        self.ids[h] = name
        return h

    def geo_object(self, piece, object_id):
        # (the base's numbered id is not used: BMX XXX's ids are name hashes)
        object_id = self.unique_id(piece.name)
        part_id = self.unique_id(piece.name + "_part")
        surfaces = sorted({s for s, _, _, _ in piece.faces})
        local = {s: i for i, s in enumerate(surfaces)}
        faces = [W.Face.make(local[s], a, b, c, collides=piece.collides, two_sided=self.surfaces[s].faces_two_sided or self.surfaces[s].two_sided) for s, a, b, c in piece.faces]
        groups = []
        for s in surfaces:
            groups += C.pack_strips(local[s], C.stripify([(a, b, c) for ss, a, b, c in piece.faces if ss == s]))
        if len(piece.positions) > C.VERTICES_PER_PART:
            raise ValueError("%s: %d vertices in one part" % (piece.name, len(piece.positions)))
        face_normals = [struct.pack("<3f", *C.face_normal(piece.positions[a], piece.positions[b], piece.positions[c])) for _, a, b, c in piece.faces]
        part = B.BmxPart(part_id, [tuple(p) for p in piece.positions], faces, groups,
                         normals=W.IndexedStream.make([struct.pack("<3f", *n) for n in piece.normals]),
                         colours=W.IndexedStream.make([struct.pack("<4B", *c) for c in piece.colours]),
                         uvs=W.IndexedStream.make([struct.pack("<2f", *uv) for uv in piece.uvs]),
                         face_normals=W.IndexedStream.make(face_normals),
                         flags=self.SKY.part_flags if piece.sky else C.PART_FLAGS, material_count=len(surfaces))
        # a record for each (kind, launch, tag word) the faces have (the word the objectives find faces by)
        kinds = [(self.surfaces[s].kind, self.surfaces[s].launch, self.surfaces[s].word) for s, _, _, _ in piece.faces]
        used = sorted({k for k in kinds if k[0] or k[2]})
        if used and piece.collides:
            part.face_tags = W.TagBuffer([T.FaceTag12(word, kind, launch) for kind, launch, word in used],
                                         [used.index(k) if k[0] or k[2] else None for k in kinds])
        mats = []
        for s in surfaces:
            m = C.mati(self.surfaces[s].name, self.surfaces[s].texture, self.surfaces[s].two_sided, self.surfaces[s].layer)
            data = bytearray(m.data)
            data[MATI_SOUND] = self.surfaces[s].sound
            mats.append(Chunk(b"MATI", bytes(data)))
        lo = tuple(min(p[k] for p in piece.positions) for k in range(3))
        hi = tuple(max(p[k] for p in piece.positions) for k in range(3))
        obj = B.BmxObject(W.GeoHeader(lo, hi, object_id, piece.name, piece.name_hash), b"\x00\x00\x01\x00",
                          [(part, W.LevelOfDetail(C.LOD_DISTANCE, part_id), Chunk(b"TMTB", kids=mats))],
                          type_data=struct.pack("<I", C.TERRAIN))
        if piece.collides:
            obj.collision = W.CollisionTree.build(part_id, part.positions, faces)
        return obj

    def tag_edges(self, geo, instances):
        """Every collidable face edge along a grind mark gets the mark's record (on the object, shared by its
        instances; the first placement decides)."""
        if not self.edge_marks:
            return
        index = C.MarkIndex(self.edge_marks)   # the marks near a point, in their order: the scan's first match
        objects = {o.header.id: o for o in geo.objects}
        tagged = set()
        for inst in instances:
            if not (inst.flags & W.Instance.COLLIDES) or inst.geometry_id in tagged:
                continue
            obj = objects[inst.geometry_id]
            part = obj.parts[0][0]
            world = [tuple(a + b for a, b in zip(L.quat_rotate(inst.rotation, p), inst.position)) for p in part.positions]
            records, index_of, indices = [], {}, [None] * (3 * len(part.faces))
            for f, face in enumerate(part.faces):
                for k in range(3):
                    a, b = world[face.vertices[k]], world[face.vertices[(k + 1) % 3]]
                    mark = next((m for m in index.near(a) if C.on_segment(a, m) and C.on_segment(b, m)), None)
                    if mark is None:
                        continue
                    key = (mark.kind, mark.word)
                    if key not in index_of:
                        index_of[key] = len(records)
                        records.append(T.EdgeTag8(mark.word, link_kind=mark.kind, flags=mark.flags))
                    indices[3 * f + k] = index_of[key]
            if records:
                part.edge_tags = W.TagBuffer(records, indices)
                tagged.add(inst.geometry_id)
                self.report.say("%s: %d grind face edges, %s" % (obj.header.name, sum(1 for i in indices if i is not None),
                                                                 ", ".join("link kind %d x%d" % (r.link_kind, sum(1 for i in indices if i == n))
                                                                           for n, r in enumerate(records))))

    def scene_file(self, low, high):
        return B.SceneFile.make(self.slot, low, high).write()

    def build(self):
        files = super().build()
        for s in self.surfaces:
            if s.sound:
                self.report.say("material %s: sound %d" % (s.name, s.sound))
        return files


def convert(model_path, out_dir=None, slot=DEFAULT_SLOT, level_name=None, force=False, sky=None, sky_radius=C.SKY_RADIUS):
    conv = BmxConverter(G.read_model(model_path), slot, level_name, force)
    if sky:
        conv.use_sky(sky, sky_radius)
    files = conv.build()
    conv.check_level_bytes(files)
    report = conv.report
    if report.errors and not force and out_dir:
        print(report.text())
        raise SystemExit("bmx_level_convert.py: over budget (see above; --force writes anyway)")
    if out_dir:
        folder = os.path.join(out_dir, "ASSETS", "LEVELS", conv.slot)
        os.makedirs(folder, exist_ok=True)
        written = {"%s.%s" % (conv.slot, ext): data for ext, data in files.items()}
        for lib in LIBRARIES:
            written[lib] = Z.AssetLibrary().write()
        written["OBJCTV.TXT"] = objectives_file()
        for name, data in written.items():
            with open(os.path.join(folder, name), "wb") as f:
                f.write(data)
        report.say("written to %s: %s" % (folder, ", ".join("%s %d B" % (n, len(d)) for n, d in written.items())))
    print(report.text())
    return conv, files


def cmd_selftest(args):
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        model = C.make_example(os.path.join(tmp, "model"))
        conv, files = convert(model, os.path.join(tmp, "mods"), DEFAULT_SLOT, "Test Park")
        folder = os.path.join(tmp, "mods", "ASSETS", "LEVELS", DEFAULT_SLOT)
        with open(os.path.join(folder, DEFAULT_SLOT + ".GEO"), "rb") as f:
            assert f.read() == files["GEO"]
        geo = B.BmxGeometry.read(files["GEO"])
        assert geo.write() == files["GEO"] and B.Instances.read(files["INS"]).write() == files["INS"]
        dat = B.SceneData.read(files["DAT"])
        zit = B.SceneFile.read(files["ZIT"])
        assert zit.names[b"ZFGE"] == DEFAULT_SLOT + ".GEO" and zit.low[0] <= -40 and zit.high[0] >= 40
        # every part id over 0xFFFF, unique; every collidable object has a tree over all its faces
        ids = [p.id for o in geo.objects for p, _, _ in o.parts] + [o.header.id for o in geo.objects]
        assert all(i > 0xFFFF for i in ids) and len(set(ids)) == len(ids)
        assert all(o.collision and len(o.collision.faces()) == len(o.parts[0][0].faces) for o in geo.objects)
        # the bike games' readers: the ground at 0 under the spawn, the ledge top at -0.45, the deck at -3
        groups = D.collision_triangles(files["GEO"], files["INS"])
        assert abs(D.topmost(groups, 0.0, -14.0)) < 1e-4
        assert abs(D.topmost(groups, 6.5, 0.0) + 0.45) < 1e-4
        assert abs(D.topmost(groups, 0.0, 20.5) + 3.0) < 1e-4 and D.topmost(groups, 60.0, 0.0) is None
        # grind edges: the coping linked to the quarter pipe (link kind 1), the bar's and the ledge's plain
        edges = B.grind_edges(files["GEO"], files["INS"])
        kinds = Counter(k for _, _, _, k in edges)
        assert kinds[1] >= 2 and kinds[0] >= 4, kinds
        assert all(abs(a[1] + 3.0) < 1e-4 and abs(a[2] - 19.0) < 1e-4 for a, b, _, k in edges if k == 1)
        # the quarter pipe's faces are kind 1, the coping sounds as its faces' wood (2), the bar's metal (1)
        qp = next(o for o in geo.objects if o.header.name == "qpipe_01").parts[0][0]
        assert {r.surface_kind for r in qp.face_tags.records} == {1}
        sounds = {o.header.name: sorted({m.data[MATI_SOUND] for m in o.parts[0][2].kids}) for o in geo.objects}
        assert sounds["flat_bar"] == [1] and 2 in sounds["qpipe_01"], sounds
        assert dat.points[0].kind == W.SPAWN and dat.points[0].name == W.PLAYER_ONE
        # the strips keep the winding mark, as BMX XXX's stock strips do
        assert any(e & W.StripGroup.FLIPPED for o in geo.objects for p, _, _ in o.parts for g in p.groups for e in g.entries)
        for lib in LIBRARIES:
            with open(os.path.join(folder, lib), "rb") as f:
                assert Z.AssetLibrary.read(f.read()).entries == []
        objectives = B.ObjectivesFile.read(open(os.path.join(folder, "OBJCTV.TXT"), "rb").read())
        assert objectives.items == [] and objectives.write() == b"\r\n" and not conv.report.errors
        assert not any(p.kind == W.GLOBALS for p in dat.points)
        # a sky: its one part of render class 3, its instance the sky bit, the Globals in BMX XXX's 20 bytes
        conv, files = convert(model, None, DEFAULT_SLOT, "Test Park", sky=os.path.join(os.path.dirname(model), "sky.png"))
        geo = B.BmxGeometry.read(files["GEO"])
        skies = [o for o in geo.objects if o.parts[0][0].flags & 7 == C.PART_FLAGS_SKY]
        assert len(skies) == 1 and skies[0].header.name == "panorama" and skies[0].collision is None
        sky_instances = [i for i in B.Instances.read(files["INS"]).instances if i.flags & C.INSTANCE_FLAG_SKY]
        assert len(sky_instances) == 1 and sky_instances[0].geometry_id == skies[0].header.id and sky_instances[0].flags == BMX_SKY.instance_flags
        point = next(p for p in B.SceneData.read(files["DAT"]).points if p.kind == W.GLOBALS)
        ambient, _, fog, depth, far = struct.unpack("<4s4s4sff", point.tags)
        assert len(point.tags) == 20 and fog == bytes(conv.globals.fog) + b"\0" and (depth, far) == (120.0, 360.0)
    print("bmx_level_convert selftest: ok")


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("convert")
    c.add_argument("model")
    c.add_argument("--out", required=True)
    c.add_argument("--slot", default=DEFAULT_SLOT, help="the level row whose folder and file names the files take")
    c.add_argument("--level-name")
    c.add_argument("--force", action="store_true")
    C.add_sky_arguments(c)
    c.set_defaults(fn=lambda a: convert(a.model, a.out, a.slot, a.level_name, a.force, a.sky, a.sky_radius))
    e = sub.add_parser("example")
    e.add_argument("--out", required=True)
    e.set_defaults(fn=lambda a: print("written %s" % C.make_example(a.out)))
    t = sub.add_parser("selftest")
    t.set_defaults(fn=cmd_selftest)
    args = p.parse_args(argv)
    try:
        args.fn(args)
    except ValueError as e:
        raise SystemExit("bmx_level_convert.py: %s" % e)


if __name__ == "__main__":
    main(sys.argv[1:])
