#!/usr/bin/env python3
"""A glTF model with named objects into Dave Mirra BMX 2's level files (ZGE2): ai_level_convert.py's converter
(the same model, tags, budgets and report) with DM2's records (dm2_level_write.py) and DM2's forms of the
gameplay data, served in DM2's park slot (FREERIDE's level 8) or a level's.

    python3 -I dm2_level_convert.py convert MODEL.gltf --out DIR --disc <DM2 disc> [--theme 0] [--theme-name NAME]
                                    [--level WOODWARD] [--sky SKY.png] [--force]
        the park slot (default): the level in park theme --theme's place (0: Warehouse). The theme's files,
        DIR/ASSETS/EDITOR/<FILE>/<FILE>.{ZIT,GEO,INS,DAT,TXR} (FILE the theme's, LEWAREHO: its scene in SCENE.TXT,
        which enterScene opens by that name), become the level with the theme's own editor objects (the pieces,
        towers, start markers and rails the park code makes nodes of) and textures, and DIR/ASSETS/EDITOR/THEMES.TXT
        is the disc's with that theme's block replaced: the level's grid, every cell buildable and none with a tower
        (its MAP all -1: kind 1, level 1). (The park menu's theme choice offers the disc's nine themes and SCENE.TXT
        names their scenes, so a level takes a theme's place rather than adding one.) A park saved on
        that theme (an empty one: only its start) plays the level from FREERIDE's level select, value 8
        (playbot/dm2modlevel.py card, ride). The start cell for the PlayerOne spawn is reported.
        --level L: the level in L's slot instead (DIR/ASSETS/LEVELS/L/L.*, a stock level's files replaced).
        --sky SKY.png: the picture as the level's sky, as ai_level_convert's (a dome the game draws behind
        everything, and a Globals point with the clear colour its zenith's), in DM2's form (DM2_SKY). Without it a
        park keeps the theme's Globals, whose clear colour is all a level without the theme's panorama shows.
    python3 -I dm2_level_convert.py info MODEL.gltf      what the converter sees and the budgets, nothing written
    python3 -I dm2_level_convert.py table-cameras FOLDER --elf SLUS_201.59
        a stock level folder (ASSETS/LEVELS/<L>) gets the executable's five camera points for its session level
        (g_levelCameraPoints) as "CameraPos" idle points 1 to 5 in its DAT, each looking toward the level's centre:
        the level's own cameras for its map, which DM2's level camera points option reads in their place and the
        other games' converters carry (zge_level_gltf.py export).
    python3 -I dm2_level_convert.py selftest --disc <DM2 disc>   ai_level_convert's example park converted for DM2,
                                                                 read back with DM2's readers and checked

DM2's forms (from DM2's own park pieces in the editor themes, which a bike rides; game/dm2/rider/collision/tags.cpp
reads them):
  #qp (a quarter pipe)    faces: FaceTag12 surface kind 1, launch QP_LAUNCH (70: Editobj_qpipe_qpipe4's; #launch=N
                          on the material sets it: 50 for a 2 m pipe, 80 for 6 m); its coping edge #grind=lip:
                          EdgeTag8 link kind 1, grindable (the rider's link pass ties the lip to the kind-1 faces)
  #spine                  faces surface kind 2; #grind=spine edges link kind 2
  #kicker                 faces surface kind 3 (DM2's kickers: Editobj_kicker_*, launch 50 to 100 by height)
  #grind (rail, ledge)    EdgeTag8 grindable, no link (Editobj_rail_low's)
  #grind=vpole / hpole    no DM2 form: written as a plain rail, and the report says so
  #wall, #slope=G         no DM2 form the converter writes (DM2's wall kind is an edge byte whose values are not
                          named; it has no gravity field): the faces stay ground, and the report says so
  #metal, #wood           DM2 takes a grind's sound from the face's material (MATI's tag record), not the edge;
                          its values are not named, so every material writes 0 and the report says so
  spawns                  SDPT Spawn PlayerOne with DM2's 4 tag bytes (zero); in the park slot the game places the
                          rider at the park's start cell (setParkSpawn), the converter reports which
                          #spawn=respawn as DM2's "Player" spawns (its respawns); PlayerTwo left out (DM2 has none)
  cameras                 SDPT CameraPos idle 1 to N, each 5 m below its eye (the level-point camera's lift), the
                          first tag byte the field of view (DM2 reads only the position); one behind the PlayerOne
                          spawn when the model has none
  zones                   PVSO's older body (DM2's only form)
  parts                   plain streams (DM2 reads no TIV*/TIFN), strips without 0x4000 (Part.unwind), TGPH flags 0 (every stock part's), a collision tree
                          on every collidable object (DM2 makes none at run time), MATI of 0x9C bytes
  textures                TXRH + TBMP, an 8-bit BMP whose palette alpha is 0; transparency is the colour key
                          (magenta, texels with alpha under 128)
Budgets as ai_level_convert's, with DM2's numbers: texels 2,560 KB (dm2_xbox.py's TEXTURE_BUDGET_KB), side 128,
groups of 60, faces 1,024 an object, vertices 16,384 a part, and parts drawn PARTS_IN_VIEW (682: Devil's Peak, the
most instanced parts of DM2's levels, which have no zones).

Python 3.8+, standard library only.
"""
import argparse
import math
import os
import re
import struct
import sys
from collections import Counter

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ai_level as L  # noqa: E402
import ai_level_convert as C  # noqa: E402
import ai_level_write as W  # noqa: E402
import dm2_level as D  # noqa: E402
import dm2_level_write as M  # noqa: E402
import gltf_model as G  # noqa: E402
import zge_tags as T  # noqa: E402

Chunk = L.Chunk

QP_LAUNCH = 70              # Editobj_qpipe_qpipe4 (2 m: 50, 6 m: 80)
KICKER_LAUNCH = 60          # Editobj_kicker_2
GROUND_LAUNCH = 50          # every untagged stock face's byte 9
SURFACE_KINDS = {"qp": 1, "spine": 2, "kicker": 3}
EDGE_LINKS = {1: 1, 2: 2}   # ai_level_convert's grind kind (lip, spine) -> EdgeTag8.link_kind
PART_FLAGS = 0              # every stock DM2 part's TGPH flags
CELL = 5.0                  # a park cell (edit_grid.cpp kCellSize)
PARK_LEVEL = 1              # the theme cells' level (MAP -1: kind 1, buildable, no tower); the start sits at y -1
PARK_STREAM_VERSION = 0x100 # a saved park stream's first word (loadPark; every card park's)
KEY = (255, 0, 255)         # the colour key (dm2_xbox.py KEY)
# A DM2 material (MATI, 0x9C; zge::MatiChunk's ZGE2 form): flags 0, the three colours, the two unread floats, the
# tag record (its first byte the surface sound), one layer of the stock levels' opaque state (the most used: 1,253
# of LEWAREHO's materials) naming the texture.
MATI_COLOURS = bytes.fromhex("e5e5e5ff" "ffffffff" "191919ff")
MATI_LAYER_STATE = bytes.fromhex("1c00000000000040f082f7090101001c")


def dm2_globals_tags(g):
    """DM2's Globals record (game/dm2/level/level_globals.cpp: colours group at offsets 0 the scene light, 4 the
    clear colour, 8 the fog colour; floats at 12 the fog start, 16 the far clip), as all 17 stock levels' and
    themes' 20 bytes, their colours' fourth bytes 0."""
    return struct.pack("<4B4B4Bff", *g.light, 0, *g.clear, 0, *g.fog, g.fog_alpha, g.fog_start, g.far_clip)


# DM2's sky (every stock level and park theme has one: panorama_01, its instance 0x33, its part's TGPH flags 0 as
# every DM2 part's). ZGE2 gives a part its render type at every draw from its node: a sky node's rigid part is the
# sky (0x14, part_render.cpp resolveRenderType), so the part needs no class of its own. The instance is the stock
# panorama's 0x33 without the collides bit (the dome has no collision tree): shown, the sky bit, 0x20 as every
# stock instance. The Globals: DM2 draws no fog planes (its camera never sets a fog end) and every stock fog alpha
# is 0; the far clip is the park's own (g_levelFarClips gives level 8 none, so the Globals' is read): 500 m, the
# largest of the stock levels' (Cloverleaf's, Commercial's). A park theme's light colour is kept (use_theme).
DM2_SKY = C.SkyForm(part_flags=PART_FLAGS, instance_flags=0x31, fog_alpha=0, fog_start=240.0, fog_end=360.0,
                    far_clip=500.0, tags=dm2_globals_tags)


def mati(texture_name, sound=0, two_sided=False, layer=None):
    """DM2's MATI: flags bit 1 draws both sides (loadMaterialLayerStates: kStateTwoSided from the MATI's flags; a
    #2sided material's back faces were not drawn before), one layer of the stock opaque state (with a #layer's
    blending and tests: ai_level_convert.layer_state)."""
    state = struct.pack("<4I", *C.layer_state(struct.unpack("<4I", MATI_LAYER_STATE), layer))
    out = struct.pack("<H", 0x2 if two_sided else 0) + MATI_COLOURS + b"\0\0" + struct.pack("<2f", 0.25, 1.0) + bytes([sound]) + b"\0" * 7 + b"\0" * 8
    out += struct.pack("<I", 1) + state + struct.pack("<I", L.name_hash(texture_name)) + b"\0" * 8
    return Chunk(b"MATI", out.ljust(D.DM2_MATI, b"\0"))


def texture(t):
    """A converter Texture as DM2's (TXRH + one TBMP): texels with alpha under 128 become the key."""
    rows = [[KEY + (255,) if px[3] < 128 else px[:3] + (255,) for px in row] for row in t.rows]
    keyed = any(px[:3] == KEY for row in rows for px in row)
    palette, index_rows = C.palettize(rows, False)
    if keyed and KEY + (255,) not in palette:
        raise ValueError("texture %s: the key colour was merged in its palette" % t.name)
    return M.Texture.make(t.name, index_rows, palette)


def yaw_turns(q):
    """A rotation's heading about y in quarter turns (0 faces +z, the game's identity), nearest."""
    x, y, z, w = q
    forward_x, forward_z = 2 * (x * z + w * y), 1 - 2 * (x * x + y * y)   # +z turned by q
    return int(round(math.degrees(math.atan2(forward_x, forward_z)) / 90.0)) % 4


# DM2: a prop's class data is the type word alone; its instance's tag word 1 is its object sound (startObjectSound: 1 to
# 16, 0 none; Woodward's trash can 11, cones 12, ball 8). Another game's prop has none: DM2's sounds are numbered
# samples of the level's bank, and no other game's prop says which.
DM2_DYNAMICS = C.DynamicsForm("dm2", 0x23, b"", struct.pack("<2I", 0, 0))


class Dm2Converter(C.Converter):
    RECORDS = M
    SKY = DM2_SKY
    DYNAMICS = DM2_DYNAMICS
    TEXEL_BUDGET_KB = 2560
    TEXTURE_SIDE = 128
    PARTS_IN_VIEW = 682
    PARTS_IN_VIEW_SOURCE = "Devil's Peak, DM2's most instanced parts (its levels have no zones)"
    LEVEL_BYTES = 10068316
    LEVEL_BYTES_SOURCE = "Woodward Camp's, DM2's largest stock level"
    # DM2's respawns are its "Player" spawns (respawnAtStart: the ground under the nearest); it has no second
    # player's start (its multiplayer takes turns). Its level-point camera (with the level camera points option)
    # stands 5 m above a "CameraPos" idle point, so a camera's point is its eye 5 m lower (y grows down).
    SPAWN_NAMES = {"playerone": "PlayerOne", "playertwo": None, "respawn": "Player"}
    SPAWN_TAGS = b"\0" * 4
    MAKES_CAMERA = True
    CAMERA_LIFT = -5.0
    PARK_LEVEL_BYTES = 9781404      # a park slot's files, the theme's editor objects and textures in
    PARK_LEVEL_BYTES_SOURCE = "LECOMMER's, DM2's largest stock park theme"

    def __init__(self, model, slot="LEWAREHO", level_name=None, force=False):
        super().__init__(model, slot, level_name, force)
        self.base = None    # (Geometry, TextureLibrary, SceneData, SceneFile) of the theme whose editor objects go in

    # -- the level read --

    def read_model(self):
        super().read_model()
        if self.base is not None:
            self.fit_to_park_start()

    def fit_to_park_start(self):
        """In the park slot the game places the rider at the start cell's centre at the cell's level (setParkSpawn:
        cellMarkerPlace of an empty cell, y -PARK_LEVEL), not at the level's PlayerOne spawn, so the whole level is
        moved to put that spawn there: x and z onto the nearest cell centre (the grid has an even number of cells
        each way, centred on the origin: centres at 2.5 + 5k m), y onto the cell's level. A spawn elsewhere had the
        rider placed inside or under the level's ground (BMX XXX's Root: PlayerOne 2 m above the cell's level)."""
        spawn = next((p for n, p, _ in self.spawns if L.name_hash(n) == W.PLAYER_ONE), self.spawns[0][1] if self.spawns else None)
        if spawn is None:
            return
        target = tuple(round((spawn[k] - CELL / 2) / CELL) * CELL + CELL / 2 for k in (0, 2))
        d = (target[0] - spawn[0], -PARK_LEVEL - spawn[1], target[1] - spawn[2])
        if max(abs(v) for v in d) < 1e-6:
            return

        def moved(p):
            return tuple(p[k] + d[k] for k in range(3))
        for pl in self.placements:
            pl.position = moved(pl.position)
        for m in self.edge_marks:
            m.a, m.b = moved(m.a), moved(m.b)
        self.spawns = [(n, moved(p), r) for n, p, r in self.spawns]
        for c in self.cameras:
            c.position = moved(c.position)
        for z in self.zones:
            z.low, z.high = moved(z.low), moved(z.high)
        self.report.did("the level moved by (%.2f, %.2f, %.2f) m: the park places the rider at its start cell's centre and level, "
                        "and the PlayerOne spawn is there now" % d)

    # -- the parts --

    def surface_of(self, material_index):
        i = super().surface_of(material_index)
        s = self.surfaces[i]
        if not hasattr(s, "_dm2"):
            tags = C.split_tags(self.model.materials[material_index].name, self.model.materials[material_index].extras)[1] \
                if material_index is not None and material_index >= 0 else {}
            s._dm2 = next((k for w, k in SURFACE_KINDS.items() if w in tags), 0)
            default = {1: QP_LAUNCH, 3: KICKER_LAUNCH}.get(s._dm2, GROUND_LAUNCH)
            try:
                s._launch = int(tags.get("launch", default))
            except (TypeError, ValueError):
                s._launch = default
            for word in ("wall", "slope"):
                if word in tags:
                    self.report.did("material %s: #%s has no DM2 form: its faces are ground" % (s.name, word))
        return i

    def geo_object(self, piece, object_id):
        part_id = object_id + 1
        surfaces = sorted({s for s, _, _, _ in piece.faces})
        local = {s: i for i, s in enumerate(surfaces)}
        faces = [W.Face.make(local[s], a, b, c, collides=piece.collides, two_sided=self.surfaces[s].faces_two_sided or self.surfaces[s].two_sided) for s, a, b, c in piece.faces]
        groups = []
        for s in surfaces:
            groups += C.pack_strips(local[s], C.stripify([(a, b, c) for ss, a, b, c in piece.faces if ss == s]))
        if len(piece.positions) > C.VERTICES_PER_PART:
            raise ValueError("%s: %d vertices in one part" % (piece.name, len(piece.positions)))
        part = M.Part(part_id, [tuple(p) for p in piece.positions], faces, groups,
                      normals=b"".join(struct.pack("<3f", *n) for n in piece.normals),
                      colours=b"".join(struct.pack("<4B", *c) for c in piece.colours),
                      uvs=b"".join(struct.pack("<2f", *uv) for uv in piece.uvs),
                      face_normals=b"".join(struct.pack("<3f", *C.face_normal(piece.positions[a], piece.positions[b], piece.positions[c]))
                                            for _, a, b, c in piece.faces),
                      flags=self.SKY.part_flags if piece.sky else PART_FLAGS, material_count=len(surfaces))
        part.unwind()
        kinds = [(self.surfaces[s]._dm2, self.surfaces[s]._launch) for s, _, _, _ in piece.faces]
        if piece.collides and any(k for k, _ in kinds):
            records = sorted({k for k in kinds if k[0]})
            part.face_tags = W.TagBuffer([T.FaceTag12(0, surface_kind=k, launch=n) for k, n in records],
                                         [records.index(k) if k[0] else None for k in kinds])
        table = Chunk(b"TMTB", kids=[mati(self.surfaces[s].texture, two_sided=self.surfaces[s].two_sided, layer=self.surfaces[s].layer) for s in surfaces])
        lo = tuple(min(p[k] for p in piece.positions) for k in range(3))
        hi = tuple(max(p[k] for p in piece.positions) for k in range(3))
        obj = M.GeoObject(W.GeoHeader(lo, hi, object_id, piece.name, piece.name_hash), b"\x00\x00\x01\x00",
                          [(part, W.LevelOfDetail(C.LOD_DISTANCE, part_id), table)], type_data=struct.pack("<I", C.TERRAIN))
        if piece.collides:
            obj.collision = M.InlineCollisionTree.build(part_id, part.positions, faces)
        return obj

    def tag_edges(self, geo, instances):
        """ai_level_convert's matching of face edges to the grind marks, with DM2's records."""
        if not self.edge_marks:
            return
        poles = sum(1 for m in self.edge_marks if m.kind in (3, 4))
        if poles:
            self.report.did("%d pole segments (#grind=vpole/hpole) have no DM2 form: written as rails" % poles)
        if any(m.sound for m in self.edge_marks):
            self.report.did("#metal/#wood on grind marks not written: DM2 takes a grind's sound from the face's material, "
                            "whose sound values are not named (every material sound 0)")
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
                    key = EDGE_LINKS.get(mark.kind, 0)
                    if key not in index_of:
                        index_of[key] = len(records)
                        records.append(T.EdgeTag8(0, link_kind=key, flags=T.EdgeTag8.GRINDABLE))
                    indices[3 * f + k] = index_of[key]
            if records:
                part.edge_tags = W.TagBuffer(records, indices)
                tagged.add(inst.geometry_id)
                self.report.say("%s: %d grind face edges, %s" % (obj.header.name, sum(1 for i in indices if i is not None),
                                                                 ", ".join("link kind %d x%d" % (r.link_kind, sum(1 for i in indices if i == n))
                                                                           for n, r in enumerate(records))))

    def scene_data(self):
        points = [M.ScenePoint.spawn(name, position, rotation, tags=self.SPAWN_TAGS) for name, position, rotation in self.spawns]
        if not any(p.name == W.PLAYER_ONE for p in points):
            self.report.error("no PlayerOne spawn (an empty node tagged #spawn=PlayerOne): the rider has nowhere to start")
        points += self.camera_points()
        regions = None
        if self.zones:
            names = [z.name for z in self.zones]
            regions = [W.ViewRegion(L.name_hash(z.name), z.low, z.high, [L.name_hash(n) for n in names if n != z.name and n not in z.sees], None)
                       for z in self.zones]
        if self.base is not None:
            # The theme's own points but its spawns (Globals and the rest the park keeps; the start is the park's),
            # and its Globals when the level has its own (a sky's).
            mine = (W.SPAWN, W.GLOBALS) if self.globals is not None else (W.SPAWN,)
            points = [p for p in self.base[2].points if p.kind not in mine] + points
        if self.globals is not None:
            points.append(self.globals_point())
        return M.SceneData(points, regions)

    def use_sky_look(self, look):
        """As the base's, with the park theme's light colour kept (its Globals' first word)."""
        super().use_sky_look(look)
        theirs = next((p for p in self.base[2].points if p.kind == W.GLOBALS), None) if self.base is not None else None
        if theirs is not None and len(theirs.tags) >= 4:
            self.globals.light = tuple(theirs.tags[:3])

    def texture_file(self):
        mine = [texture(t) for t in self.textures.values()]
        if self.base is None:
            return M.TextureLibrary(mine).write()
        ids = {t.id for t in self.base[1].textures}
        for t in mine:
            if t.id in ids:
                raise ValueError("texture %s: its id is the theme's texture's" % t.name)
        return M.TextureLibrary(self.base[1].textures + mine).write()

    def scene_file(self, low, high):
        if self.base is not None:
            theme = self.base[3]
            low = tuple(min(a, b) for a, b in zip(low, theme.low))
            high = tuple(max(a, b) for a, b in zip(high, theme.high))
        return M.SceneFile.make(self.slot, low, high, C.CELL).write()

    def build(self):
        files = super().build()
        if self.base is not None:
            geo = M.Geometry.read(files["GEO"])
            ours = {o.header.id for o in geo.objects}
            theirs = [o for o in self.base[0].objects if o.header.id not in self.base_instanced]
            clash = ours & {o.header.id for o in theirs}
            if clash:
                raise ValueError("object ids %s are the theme's" % sorted(clash))
            # the theme's chunks at their places, the level's (its props' ZDYN) after every object
            files["GEO"] = M.Geometry(theirs + geo.objects, list(self.base[0].other) +
                                      [(len(theirs) + i, k) for i, k in geo.other]).write()
            self.report.say("with %s's %d editor objects (the pieces, towers, markers and rails) and %d textures" %
                            (self.base_name, len(theirs), len(self.base[1].textures)))
        return files

    def use_theme(self, disc, name):
        """The base theme's editor objects, textures, points and box (the theme's instanced objects, its own
        surroundings, are left out)."""
        src = M.FolderFiles(L.Disc(disc), "EDITOR/" + name.upper())
        files = src.files()
        ins = M.Instances.read(files["INS"])
        self.base = (M.Geometry.read(files["GEO"]), M.TextureLibrary.read(files["TXR"]), M.SceneData.read(files["DAT"]), M.SceneFile.read(files["ZIT"]))
        self.base_instanced = {i.geometry_id for i in ins.instances}
        # The theme's textures go in with the level's, each id its name's hash: a level texture of the same name
        # (Root's TIRE04 and Warehouse's) takes another name.
        self.names.update(t.name.upper() for t in self.base[1].textures)
        used = [o.header.id for o in self.base[0].objects] + [part.id for o in self.base[0].objects for part, _, _ in o.parts]
        self.FIRST_OBJECT_ID = (max(used, default=0) + 2) | 1    # past the theme's object and part ids (findPartById is global)
        self.base_name = name.upper()
        self.LEVEL_BYTES, self.LEVEL_BYTES_SOURCE = self.PARK_LEVEL_BYTES, self.PARK_LEVEL_BYTES_SOURCE


# ---- the park theme ---------------------------------------------------------------------------------------------

# The park's stream as the game reads it: the card's park buffer is PARK_BYTES (memory_card.h kParkBytes, 0x2565), and
# a park file longer than that is cut there (dave_mirra_saves.cpp: "kParkBytes at most, the rest zero"). loadPark reads
# the version and start (PARK_HEAD), three bytes an empty cell, then the gaps' header (gap_tool.cpp loadGaps: 2 + 4 +
# 1), the high scores (park_scores.h, 0x21C) and the brightness (4): PARK_TAIL. A grid whose stream runs past the
# buffer has its last cells and tail read from whatever follows it: a 64 x 64 grid (12,297 bytes) read an object
# where there was none and the load never ended (newPlacedObject's cell loop, crossmap1 2026-11-16); 56 x 56 and 48 x
# 48 loaded.
PARK_BYTES = 0x2565
PARK_HEAD = 4 + 5
PARK_TAIL = 2 + 4 + 1 + 0x21C + 4
PARK_MAX_SIDE = 64          # park_editor.h kParkMaxColumns, kParkMaxRows


def park_fits(width, height):
    return PARK_HEAD + 3 * width * height + PARK_TAIL <= PARK_BYTES


def park_grid(conv):
    """The theme's grid (an even number of 5 m cells each way, centred on the origin as cellWorldPosition centres
    it, covering the level's collidable instances, at most 64, and no more cells than the park's stream holds:
    park_fits) and the start cell, turns for the PlayerOne spawn. A level larger than that is ridden past the grid's
    edge."""
    ins = M.Instances.read(conv.files["INS"])
    box = [i for i in ins.instances if i.flags & W.Instance.COLLIDES] or ins.instances
    reach_x = max(max(abs(i.world_min[0]), abs(i.world_max[0])) for i in box)
    reach_z = max(max(abs(i.world_min[2]), abs(i.world_max[2])) for i in box)
    width = min(PARK_MAX_SIDE, max(4, 2 * int(math.ceil(reach_x / CELL))))
    height = min(PARK_MAX_SIDE, max(4, 2 * int(math.ceil(reach_z / CELL))))
    while not park_fits(width, height):
        if width >= height:
            width -= 2
        else:
            height -= 2
    name, position, rotation = next(((n, p, r) for n, p, r in conv.spawns if L.name_hash(n) == W.PLAYER_ONE), conv.spawns[0])
    column = min(width - 1, max(0, int(round(position[0] / CELL + (width - 1) / 2.0))))
    row = min(height - 1, max(0, int(round(position[2] / CELL + (height - 1) / 2.0))))
    at = (column * CELL - (width - 1) * CELL / 2, -PARK_LEVEL, row * CELL - (height - 1) * CELL / 2)
    return width, height, column, row, yaw_turns(rotation), at


def park_stream(theme_index, width, height, start):
    """An empty park on the theme with only its start, as the editor saves one (loadPark's order, playbot/parkmap.py
    dm2_read_park): the version, the theme, the start (column, row, quarter turns, placed), then every cell by row at
    PARK_LEVEL with no object and no rails. FREERIDE's level 8 loads it to play the level (the collection's Custom
    Freeplay gives it to the game as the map folder's park.dat: dave_mirra_saves.cpp, PS2X_DM2_PARK_FILE)."""
    column, row, turns = start
    return struct.pack("<I", PARK_STREAM_VERSION) + bytes([theme_index, column, row, turns, 1]) + bytes([PARK_LEVEL, 0, 0]) * (width * height)


def theme_block(name, file, width, height, cursor, ambient=(128, 128, 128)):
    """A THEMES.TXT block (readTheme's keys): every MAP cell -PARK_LEVEL (kind 1: buildable, no tower)."""
    rows = "\n".join("\t\t" + "\t".join(["-%d" % PARK_LEVEL] * width) for _ in range(height))
    return ('THEME {\n\tNAME\t"%s"\n\tFILE\t"%s"\n\tAMBIANT\t%d %d %d\n\tCURSOR\t%d %d\n\tWIDTH\t%d\n\tHEIGHT\t%d\n\tBORDER\t0\n'
            '\tMAP\t%s\n}\n' % (name, file, ambient[0], ambient[1], ambient[2], cursor[0], cursor[1], width, height, rows.lstrip("\t")))


def themes_with(disc, block, index):
    """The disc's THEMES.TXT with its theme `index` replaced by the block. (The park menu's theme choice,
    "ParkMenu.SelectTheme" in the UI library, offers the disc's nine themes and no more: a theme added after them is
    never chosen, so the level takes a stock theme's place.)"""
    text = L.Disc(disc).read("ASSETS/EDITOR/THEMES.TXT").decode("latin1")
    blocks = list(re.finditer(r"(?ms)^[ \t]*THEME\s*\{.*?^[ \t]*\}[ \t]*\r?\n", text))
    if not 0 <= index < len(blocks):
        raise ValueError("THEMES.TXT has %d themes, no theme %d" % (len(blocks), index))
    b = blocks[index]
    return (text[:b.start()] + block.replace("\n", "\r\n") + text[b.end():]).encode("latin1")


def stock_theme(disc, index):
    """A disc theme's NAME and FILE (its scene's name in SCENE.TXT, and its folder's)."""
    text = L.Disc(disc).read("ASSETS/EDITOR/THEMES.TXT").decode("latin1")
    names, files = re.findall(r'NAME\s+"([^"]*)"', text), re.findall(r'FILE\s+"([^"]*)"', text)
    if not 0 <= index < len(files):
        raise ValueError("THEMES.TXT has %d themes, no theme %d" % (len(files), index))
    return dict(name=names[index], file=files[index])


def convert(model_path, out_dir=None, disc=None, theme_index=0, theme_name="Custom", level=None, force=False, sky=None,
            sky_radius=C.SKY_RADIUS):
    model = G.read_model(model_path)
    if level is None and not disc:
        raise ValueError("the park slot needs --disc (the theme's editor objects and THEMES.TXT)")
    theme_file = stock_theme(disc, theme_index)["file"] if level is None else None
    slot = (level or theme_file).upper()
    conv = Dm2Converter(model, slot, theme_name, force)
    if level is None:
        if not disc:
            raise ValueError("the park slot needs --disc (the base theme's editor objects and THEMES.TXT)")
        conv.use_theme(disc, slot)
    if sky:
        conv.use_sky(sky, sky_radius)
    conv.files = files = conv.build()
    conv.check_level_bytes(files)
    report = conv.report
    if report.errors and not force and out_dir:
        print(report.text())
        raise SystemExit("dm2_level_convert.py: over budget (see above; --force writes anyway)")
    written = {}
    if level is None:
        width, height, column, row, turns, at = park_grid(conv)
        conv.park = dict(theme=theme_index, file=slot, width=width, height=height, start=(column, row, turns), at=at)
        report.say("park theme %s (%s) in theme %d's place (%s): %d x %d cells of %g m, all buildable at level %d, no towers; the start "
                   "for PlayerOne: column %d, row %d, %d quarter turns, placed at (%.1f, %.1f, %.1f)" %
                   (theme_name, slot, theme_index, stock_theme(disc, theme_index)["name"], width, height, CELL, PARK_LEVEL, column, row, turns, *at))
        folder = os.path.join("ASSETS", "EDITOR", slot)
        written[os.path.join("ASSETS", "EDITOR", "THEMES.TXT")] = themes_with(disc, theme_block(theme_name, theme_file, width, height, (column, row)), theme_index)
        written["park.dat"] = park_stream(theme_index, width, height, (column, row, turns))
    else:
        folder = os.path.join("ASSETS", "LEVELS", slot)
    for ext, data in files.items():
        written[os.path.join(folder, "%s.%s" % (slot, ext))] = data
    if out_dir:
        for rel, data in written.items():
            path = os.path.join(out_dir, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        report.say("written to %s: %s" % (out_dir, ", ".join("%s %d B" % (os.path.basename(n), len(d)) for n, d in written.items())))
    print(report.text())
    return conv, files


def cmd_convert(args):
    convert(args.model, args.out, args.disc, args.theme, args.theme_name, args.level, args.force, args.sky, args.sky_radius)


def cmd_info(args):
    conv = Dm2Converter(G.read_model(args.model), "LEWAREHO")
    conv.files = conv.build()
    print(conv.report.text())


def cmd_selftest(args):
    import tempfile
    import dm2_xbox
    with tempfile.TemporaryDirectory() as tmp:
        C.make_example(tmp)
        model = os.path.join(tmp, "park.gltf")
        conv, files = convert(model, os.path.join(tmp, "out"), args.disc)
        folder = os.path.join(tmp, "out", "ASSETS", "EDITOR", "LEWAREHO")
        level = {ext: open(os.path.join(folder, "LEWAREHO." + ext), "rb").read() for ext in ("GEO", "INS", "DAT", "TXR", "ZIT")}
        geo, ins, dat = M.Geometry.read(level["GEO"]), M.Instances.read(level["INS"]), M.SceneData.read(level["DAT"])
        assert geo.write() == level["GEO"] and ins.write() == level["INS"] and dat.write() == level["DAT"]
        assert M.TextureLibrary.read(level["TXR"]).write() == level["TXR"] and M.SceneFile.read(level["ZIT"]).names[b"ZFGE"] == "LEWAREHO.GEO"
        mine = [o for o in geo.objects if not o.header.name.lower().startswith(("editobj", "decal"))]
        assert mine and all(o.collision is None or isinstance(o.collision, M.InlineCollisionTree) for o in mine)
        assert all(not isinstance(getattr(p, n), W.IndexedStream) for o in mine for p, _, _ in o.parts for n in W.STREAMS)
        assert all(len(i.write()) == 0x6C for i in ins.instances)
        groups = D.collision_triangles(level["GEO"], level["INS"])
        assert D.topmost(groups, 1.0137, -3.0171) is not None and abs(D.topmost(groups, 1.0137, -3.0171)) < 1e-4
        edges = dm2_xbox.grind_edges(level["GEO"], level["INS"])
        kinds = Counter(k for _, _, _, k in edges)
        assert edges and set(kinds) <= {0, 1}, kinds
        qp = [o for o in mine if "qpipe" in o.header.name]
        assert qp and any(r.surface_kind == 1 and r.launch == QP_LAUNCH for r in qp[0].parts[0][0].face_tags.records)
        assert any(p.name == W.PLAYER_ONE for p in dat.points)
        themes = open(os.path.join(tmp, "out", "ASSETS", "EDITOR", "THEMES.TXT"), "rb").read().decode("latin1")
        assert themes.count("THEME {") == 9 and 'NAME\t"Custom"' in themes and "Warehouse" not in themes and "LETrain" in themes
        # without --sky the theme's Globals stays; with it: one sky instance in DM2's form, its part's flags 0, one
        # Globals point (the theme's replaced) with the theme's light and the sky's colours in DM2's 20 bytes
        theirs = [p for p in dat.points if p.kind == W.GLOBALS]
        assert len(theirs) == 1 and theirs[0].tags[:4] == bytes.fromhex("68686800")
        conv2, files2 = convert(model, None, args.disc, sky=os.path.join(tmp, "sky.png"))
        geo2, ins2, dat2 = M.Geometry.read(files2["GEO"]), M.Instances.read(files2["INS"]), M.SceneData.read(files2["DAT"])
        skies = [i for i in ins2.instances if i.flags & C.INSTANCE_FLAG_SKY]
        assert len(skies) == 1 and skies[0].flags == DM2_SKY.instance_flags, [hex(i.flags) for i in skies]
        sky_obj = next(o for o in geo2.objects if o.header.id == skies[0].geometry_id)
        assert sky_obj.header.name == "panorama" and sky_obj.parts[0][0].flags == 0 and sky_obj.collision is None
        points = [p for p in dat2.points if p.kind == W.GLOBALS]
        light, clear, fog, start, far = struct.unpack("<4s4s4sff", points[0].tags)
        assert len(points) == 1 and len(points[0].tags) == 20 and light == bytes.fromhex("68686800")
        assert clear[:3] == bytes(conv2.globals.clear) and fog == bytes(conv2.globals.fog) + b"\0" and far == DM2_SKY.far_clip
    print("dm2_level_convert selftest: ok (%d objects, %d instances, %d grind edges %s, park %s)" %
          (len(geo.objects), len(ins.instances), len(edges), dict(kinds), conv.park))


# DM2's level-point camera table (game/dm2/camera/camera.cpp g_levelCameraPoints, readable_symbols.tsv): five
# points (x, y, z, w) a session level, the levels in the session's order (the park, level 8, last).
LEVEL_CAMERA_TABLE = 0x0021C9A0
LEVEL_CAMERA_POINTS = 5
SESSION_LEVELS = ["SWAMPYTR", "TRAINYAR", "WOODWARD", "COMMERCI", "DEVILSPE", "CLOVERLE", "GALLOON", "AIRPORT"]
TABLE_CAMERA_PITCH = 0.25   # a table camera's view: toward the level's centre, this much down for each metre across


def elf_bytes_at(elf, address, size):
    """size bytes of the ELF's loaded image at a virtual address (its program headers' file offsets)."""
    phoff, = struct.unpack_from("<I", elf, 0x1C)
    count, = struct.unpack_from("<H", elf, 0x2C)
    for i in range(count):
        kind, offset, vaddr, _paddr, filesz = struct.unpack_from("<5I", elf, phoff + 32 * i)
        if kind == 1 and vaddr <= address and address + size <= vaddr + filesz:
            return elf[offset + address - vaddr:offset + address - vaddr + size]
    raise ValueError("no loaded segment holds 0x%X" % address)


def cmd_table_cameras(args):
    folder = args.folder.rstrip("/")
    level = os.path.basename(folder).upper()
    if level not in SESSION_LEVELS:
        raise ValueError("%s is no session level (%s)" % (level, ", ".join(SESSION_LEVELS)))
    row = elf_bytes_at(open(args.elf, "rb").read(), LEVEL_CAMERA_TABLE + SESSION_LEVELS.index(level) * 16 * LEVEL_CAMERA_POINTS,
                       16 * LEVEL_CAMERA_POINTS)
    dat_path, ins_path = (os.path.join(folder, "%s.%s" % (level, ext)) for ext in ("DAT", "INS"))
    dat = M.SceneData.read(open(dat_path, "rb").read())
    # the level's centre: the middle of what a rider can touch (the collidable instances, not the sky's dome)
    boxes = [(i.world_min, i.world_max) for i in M.Instances.read(open(ins_path, "rb").read()).instances
             if i.flags & 0x2 and not i.flags & C.INSTANCE_FLAG_SKY]
    centre = [(min(b[0][k] for b in boxes) + max(b[1][k] for b in boxes)) / 2 for k in (0, 2)]
    points = []
    for n in range(LEVEL_CAMERA_POINTS):
        x, y, z, _w = struct.unpack_from("<4f", row, 16 * n)
        dx, dz = centre[0] - x, centre[1] - z
        across = math.hypot(dx, dz)
        d = (dx, TABLE_CAMERA_PITCH * across, dz)
        length = math.sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2])
        rotation = C.look_rotation(tuple(v / length for v in d)) if across > 1e-6 else (0.0, 0.0, 0.0, 1.0)
        points.append(M.ScenePoint(C.CAMERA_POINT, C.IDLE_CAMERA, (x, y, z), rotation, n + 1, bytes([C.CAMERA_FOV, 0, 0, 0])))
        print("camera %d: (%.1f, %.1f, %.1f), eye 5 m above, looking toward (%.1f, %.1f)" % (n + 1, x, y, z, centre[0], centre[1]))
    kept = [p for p in dat.points if not (p.kind == C.CAMERA_POINT and p.name == C.IDLE_CAMERA)]
    dat.points = kept + points
    if dat.order:   # the other chunks where they were, the points where the first was
        first = dat.order.index(b"SDPT") if b"SDPT" in dat.order else 0
        rest = [t for t in dat.order if t != b"SDPT"]
        dat.order = rest[:first] + [b"SDPT"] * len(dat.points) + rest[first:]
    with open(dat_path, "wb") as f:
        f.write(dat.write())
    print("%s: %d camera points written" % (dat_path, len(points)))


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("convert")
    c.add_argument("model")
    c.add_argument("--out", required=True)
    c.add_argument("--disc", help="the DM2 PS2 disc (folder or .iso): the base theme and THEMES.TXT")
    c.add_argument("--theme-name", default="Custom")
    c.add_argument("--theme", type=int, default=0, help="the park theme whose place the level takes (0: Warehouse)")
    c.add_argument("--level", help="a level's slot instead of the park's (its files replaced)")
    c.add_argument("--force", action="store_true")
    C.add_sky_arguments(c)
    c.set_defaults(fn=cmd_convert)
    i = sub.add_parser("info")
    i.add_argument("model")
    i.set_defaults(fn=cmd_info)
    k = sub.add_parser("table-cameras")
    k.add_argument("folder")
    k.add_argument("--elf", required=True)
    k.set_defaults(fn=cmd_table_cameras)
    t = sub.add_parser("selftest")
    t.add_argument("--disc", required=True)
    t.set_defaults(fn=cmd_selftest)
    args = p.parse_args(argv)
    try:
        args.fn(args)
    except ValueError as e:
        raise SystemExit("dm2_level_convert.py: %s" % e)


if __name__ == "__main__":
    main(sys.argv[1:])
