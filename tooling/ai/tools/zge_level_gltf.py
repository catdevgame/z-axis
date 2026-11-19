#!/usr/bin/env python3
"""A level of any of the three games (AI's ZGE3, Dave Mirra BMX 2's ZGE2, BMX XXX's ZGE4) as a glTF model in the
converters' terms (ai_level_convert.py's tags): the form every game's converter reads, so a stock level of one game
becomes a map of another (ai_level_convert.py, dm2_level_convert.py, bmx_level_convert.py convert MODEL.gltf), and
a modder can open a stock level in Blender, change it and convert it back.

    python3 -I zge_level_gltf.py export --game ai|dm2|bmx FOLDER --out MODEL.gltf
        FOLDER is a level folder (ASSETS/LEVELS/<L> or ASSETS/EDITOR/<T> of a disc copy or a map): its
        <name>.{GEO,INS,DAT,TXR}. MODEL.gltf gets its .bin and one PNG per texture beside it.
    python3 -I zge_level_gltf.py info --game ai|dm2|bmx FOLDER       what the export sees, nothing written
    python3 -I zge_level_gltf.py selftest     ai_level_convert's example park converted for each game, exported
                                              back and converted again: the same faces, grind edges, surface
                                              kinds, spawn and sky

What the model holds (the converters' tags, ai_level_convert.py's docstring):
  objects       every placed object, one glTF mesh per object, one node per instance at the instance's world
                transform (its parents composed). An object whose drawn part (the part with a TGLO) is not its
                collision part (the part its tree names, else its last: what the engines' query takes) gives two
                meshes: the drawn part tagged #deco and the collision part tagged #hidden. Faces without the
                collides bit in a collidable part go in a #deco mesh of their own. Instances that neither collide
                nor are shown (script objects, markers) are left out.
  the sky       the instance with the sky bit (0x10, kNodeSky: every stock level's panorama) is a #sky node;
                the converter draws it as the target game's sky and takes the level's clear and fog colours from
                its picture (ai_level_convert.Converter.sky_look_of).
  materials     one per texture, surface kind and sidedness, named "<texture>#<tags>": #qp, #spine, #kicker=P
                with #launch=P (the bike games' launch byte), #slope=G and #wall (AI's), #metal / #wood (BMX XXX's
                material sound); #2sided where the MATI draws both sides, #2sided=collide where only the faces'
                flag says both (most stock faces); #layer=<state> where the first layer blends (ABE) or tests alpha
                (ATE): the GS fields, read the same in all three games (light shafts, puddles, water). Surface kinds with no converter tag (AI's 13: roads, floors and columns;
                the bike games' kinds above 3 but BMX XXX's wall) are left as ground, and the report counts them.
                DM2's TBMP textures key magenta to transparent (dm2_level_convert.py KEY); TZMP textures keep their
                palette's alpha.
  grind edges   every grindable edge record of a collidable part (and AI's vertical poles, kind 3, which a rail
                search does not take) in world space, as line meshes #grind=ledge|lip|spine|vpole|hpole with the
                edge's sound (#metal, #wood: AI's edge material; BMX XXX's face material's).
  spawns        the DAT's Spawn points: PlayerOne and PlayerTwo as #spawn=PlayerOne and #spawn=PlayerTwo, the
                respawns (AI's "Respawn", DM2's "Player", BMX XXX's other spawns but paintball's) as #spawn=respawn;
                spawns only a game's scripts or modes name are left out.
  cameras       the DAT's "CameraPos" points named "idle" (BMX XXX's idle cameras; DM2's level camera points) in
                their index order as #camera nodes, #fov=N from the first tag byte; the eye where the game puts it
                (DM2's 5 m above its point).
  props         an object of the DynamicsObject type with its ZDYN definition (the DDEF its name hash) is a
                #dynamics node; its mesh's extras "dynamics" carry the definition's other chunks, the class data
                after the type word and the instance's tags (ai_level_convert.DynamicsDef): a trash can of
                Woodward's is a prop in AI's and BMX XXX's maps too, with that game's sounds.
  name hashes   an object whose TGHD name field cut its name (31 characters) keeps the stock word, the hash of
                its whole name, in its mesh's extras "name_hash", which the converters write.
Axes: the games' +y is down; glTF's (x, y, z) is the games' (x, -y, -z) (ai_level_convert.to_ai, its own inverse).

Python 3.8+, standard library only.
"""
import argparse
import math
import os
import struct
import sys
from collections import Counter, defaultdict

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ai_level as L  # noqa: E402
import ai_level_convert as C  # noqa: E402
import ai_level_write as W  # noqa: E402
import bmx_level_write as B  # noqa: E402
import dm2_level as D  # noqa: E402
import dm2_level_write as M  # noqa: E402
import gltf_model as G  # noqa: E402

KEY = (255, 0, 255)   # DM2's colour key (dm2_level_convert.py KEY)


class Game:
    """What differs between the three games' level files for the export."""

    def __init__(self, name, records, mati_texture, mati_sound=None, sounds=None, colour_key=False, flags_size=4):
        self.name = name
        self.records = records            # the record module: Geometry, Instances, SceneData
        self.mati_texture = mati_texture  # the texture id's offset in a MATI's one layer
        self.mati_layer = mati_texture - 0x10   # the first layer's four state words
        self.flags_size = flags_size      # the MATI's flags word (bit 1: drawn on both sides)
        self.mati_sound = mati_sound      # the material sound byte's offset, when the game names its values
        self.sounds = sounds or {}        # material sound value -> the converters' sound tag
        self.colour_key = colour_key      # TBMP texels of KEY are transparent


GAMES = {
    # AI: ai_level_convert.mati (the layer's texture hash after the 16-byte state at 0x30)
    "ai": Game("ai", W, 0x40),
    # DM2: dm2_level_convert.mati (the layer state at 0x2C); its sound byte's values are not named
    "dm2": Game("dm2", M, 0x3C, colour_key=True, flags_size=2),
    # BMX XXX: AI's MATI, the sound byte at 0x1C (bmx_level_convert.py SOUNDS: 1 metal, 2 wood)
    "bmx": Game("bmx", B, 0x40, mati_sound=0x1C, sounds={1: "metal", 2: "wood"}),
}

# AI's FaceTag kinds -> the converters' tags (ai_level_write.FaceTag): 1 quarter pipe, 2 spine, 3 slope, 8 wall ride.
AI_SURFACES = {1: "qp", 2: "spine", 3: "slope", 8: "wall"}
# The bike games' FaceTag12 surface kinds (dm2_level_convert.py, bmx_level_convert.py): 1 quarter pipe, 2 spine,
# 3 kicker (its launch byte), and BMX XXX's 8, a wall.
BIKE_SURFACES = {"dm2": {1: "qp", 2: "spine", 3: "kicker"}, "bmx": {1: "qp", 2: "spine", 3: "kicker", 8: "wall"}}
# AI's EdgeTag kinds -> #grind words (ai_level_write.EdgeTag: 1 and 9 a lip, 2 a spine, 3 a vertical pole, 4 a
# horizontal pole; any other kind with the grindable flag is a rail search's edge, a ledge).
AI_GRINDS = {0: "ledge", 1: "lip", 9: "lip", 2: "spine", 3: "vpole", 4: "hpole"}
AI_EDGE_SOUNDS = {1: "wood", 2: "metal", 8: "metal"}   # EdgeTag.material: 0 concrete, 1 wood, 2 and 8 metal
# The bike games' EdgeTag8 link kinds -> #grind words (1 a lip, 2 a spine, 0 a rail or ledge).
BIKE_GRINDS = {0: "ledge", 1: "lip", 2: "spine"}


def to_gltf(p):
    return (float(p[0]), -float(p[1]), -float(p[2]))


def quat_to_gltf(q):
    return (float(q[0]), -float(q[1]), -float(q[2]), float(q[3]))


def stream(part, name, count, stride, fmt):
    """A part's per-vertex stream as tuples, or None (an indexed stream expanded)."""
    s = getattr(part, name)
    if s is None:
        return None
    data = s.expand() if isinstance(s, W.IndexedStream) else s
    if len(data) < count * stride:
        return None
    return [struct.unpack_from(fmt, data, stride * i) for i in range(count)]


def decode_bmp(data, colour_key, with_alpha):
    """A texture level's BMP as (width, height, rows of (r, g, b, a), top row first): 4 and 8 bits (dm2_level's
    reader), 24 and 32 bits uncompressed. A TBMP is opaque (its palette's alpha is 0), DM2's magenta its key; a
    TZMP keeps its palette's alpha."""
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp in (4, 8):
        w, h, rows = D.decode_bmp(data, opaque=not with_alpha)
    elif bpp in (24, 32):
        pixels = struct.unpack_from("<I", data, 10)[0]
        w, h = struct.unpack_from("<ii", data, 18)
        stride = (w * bpp // 8 + 3) & ~3
        step = bpp // 8
        rows = []
        for y in range(abs(h)):
            at = pixels + (abs(h) - 1 - y if h > 0 else y) * stride
            rows.append([(data[at + step * x + 2], data[at + step * x + 1], data[at + step * x],
                          data[at + step * x + 3] if bpp == 32 and with_alpha else 255) for x in range(w)])
        h = abs(h)
    else:
        raise ValueError("%d-bit BMP" % bpp)
    if colour_key and not with_alpha:
        rows = [[(0, 0, 0, 0) if px[:3] == KEY else px for px in row] for row in rows]
    return w, h, rows


# The spawns a game reads by name and what each is in the model (#spawn=<role>): the starts, and the points a fallen
# player is put back at (AI's "Respawn", DM2's "Player", BMX XXX's any but the starts and paintball's). The rest
# (AI's lesson points and the like, which its scripts name; BMX XXX's paintball sniper points) have no reader in
# another game and are left out.
RESPAWN = L.name_hash("Respawn")
PLAYER = L.name_hash("Player")
PLAYER_TWO = L.name_hash("PlayerTwo")
PAINTBALL = (L.name_hash("sniper_shooter"), L.name_hash("sniper_target"))
# What a game adds to an idle camera point's position for its eye (DM2's level-point camera stands 5 m above it).
CAMERA_LIFTS = {"ai": 0.0, "dm2": -5.0, "bmx": 0.0}


def spawn_role(game, name):
    if name == W.PLAYER_ONE:
        return "PlayerOne"
    if name == PLAYER_TWO:
        return "PlayerTwo"
    if (game == "ai" and name == RESPAWN) or (game == "dm2" and name == PLAYER) or (game == "bmx" and name not in PAINTBALL):
        return "respawn"
    return None


class Export:
    """A level's files into a glTF Builder, with a report of what was carried and what was left."""

    def __init__(self, game, files, name):
        self.game = GAMES[game]
        self.files = files
        self.name = name
        self.builder = G.Builder()
        self.lines = []
        self.counts = Counter()
        self.textures = {}         # texture id -> glTF texture index (None: not decodable)
        self.materials = {}        # (texture id, tags) -> glTF material index
        self.library = {}

    def say(self, text):
        self.lines.append(text)

    # -- textures and materials --

    def read_textures(self):
        tree = L.parse(self.files["TXR"])
        if len(tree) != 1 or tree[0].tag != b"FORM":
            raise ValueError("a TXR is one FORM")
        for chunk in tree[0].kids:
            head = chunk.kids[0].data
            tid = struct.unpack_from("<I", head, 0)[0]
            self.library[tid] = (W.c_string(head[0x14:0x34]), chunk.kids[1:])

    def texture(self, tid):
        if tid in self.textures:
            return self.textures[tid]
        name, levels = self.library[tid]
        index = None
        if levels and levels[0].tag in (b"TBMP", b"TZMP"):
            data = levels[0].data
            size = struct.unpack_from("<I", data, 2)[0]
            try:
                w, h, rows = decode_bmp(data[:size], self.game.colour_key, levels[0].tag == b"TZMP")
                index = self.builder.image(self.image_name(name or "tex%08x" % tid), rows)
            except ValueError as e:
                self.say("texture %s: %s, left out" % (name, e))
        self.textures[tid] = index
        return index

    def image_name(self, name):
        base = "".join(c if c.isalnum() or c in "_-" else "_" for c in name) or "texture"
        out, n = base, 2
        used = {img["name"] for img in self.builder.images}
        while out in used:
            out, n = "%s_%d" % (base, n), n + 1
        return out

    def material_layer(self, mati):
        """(drawn on both sides, the first layer's 16 state bytes when it blends or tests alpha, else None)."""
        data = mati.data
        flags = int.from_bytes(data[:self.game.flags_size], "little")
        state = data[self.game.mati_layer:self.game.mati_layer + 16]
        if len(state) < 16:
            return bool(flags & 2), None
        words = struct.unpack("<4I", state)
        blends, tests = words[0] & 0x20, words[3] & (1 << 10)    # ABE; ATE (material_state.cpp kMatiFields)
        return bool(flags & 2), (state if blends or tests else None)

    def material_texture(self, mati):
        """(texture id or None, sound tag or None) of a MATI's one layer."""
        data = mati.data
        tid = struct.unpack_from("<I", data, self.game.mati_texture)[0] if len(data) >= self.game.mati_texture + 4 else None
        if tid not in self.library:
            tid = next((t for t in (struct.unpack_from("<I", data, at)[0] for at in range(0, len(data) - 3, 4)) if t in self.library), None)
        sound = None
        if self.game.mati_sound is not None and len(data) > self.game.mati_sound:
            sound = self.game.sounds.get(data[self.game.mati_sound])
        return tid, sound

    def material(self, tid, tags):
        key = (tid, tags)
        if key in self.materials:
            return self.materials[key]
        texture = self.texture(tid) if tid is not None else None
        base = self.library[tid][0] if tid is not None else "untextured"
        name = base + "".join("#" + t for t in tags)
        colour = (1.0, 1.0, 1.0, 1.0) if texture is not None else (0.5, 0.5, 0.5, 1.0)
        self.materials[key] = self.builder.material(name, colour, texture)
        return self.materials[key]

    def surface_tags(self, record):
        """The converters' tags for a face's tag record, or None for ground."""
        if record is None:
            return ()
        if self.game.name == "ai":
            word = AI_SURFACES.get(record.kind)
            if word is None:
                if record.kind:
                    self.counts["AI surface kind %d (no converter tag): ground" % record.kind] += 1
                return ()
            if word == "slope":
                return ("slope=%g" % round(record.gravity, 4),)
            return (word,)
        word = BIKE_SURFACES[self.game.name].get(record.surface_kind)
        if word is None:
            if record.surface_kind:
                self.counts["%s surface kind %d (no converter tag): ground" % (self.game.name, record.surface_kind)] += 1
            return ()
        if word == "kicker":
            return ("kicker=%d" % record.launch, "launch=%d" % record.launch)
        if word == "qp":
            return ("qp", "launch=%d" % record.launch)
        return (word,)

    # -- parts --

    def mesh_of(self, name, part, table, faces, with_tags, extras=None):
        """A glTF mesh of the part's faces (indices into part.faces): a primitive per material."""
        count = len(part.positions)
        normals = stream(part, "normals", count, 12, "<3f")
        uvs = stream(part, "uvs", count, 8, "<2f")
        colours = stream(part, "colours", count, 4, "<4B")
        matis = [k for k in (table.kids if table is not None else []) if k.tag == b"MATI"]
        layers = [self.material_texture(m) for m in matis]
        looks = [self.material_layer(m) for m in matis]
        records = [None] * len(part.faces)
        if with_tags and part.face_tags is not None:
            for f, index in enumerate(part.face_tags.indices[:len(part.faces)]):
                if index is not None:
                    records[f] = part.face_tags.records[index]
        by_material = defaultdict(list)
        for f in faces:
            face = part.faces[f]
            a, b, c = face.vertices
            if max(a, b, c) >= count or len({a, b, c}) < 3:
                continue
            tid, sound = layers[face.material] if face.material < len(layers) else (None, None)
            tags = self.surface_tags(records[f]) if with_tags else ()
            both, state = looks[face.material] if face.material < len(looks) else (False, None)
            if both:
                tags += ("2sided",)
            elif face.flags & W.Face.TWO_SIDED:
                tags += ("2sided=collide",)
            if state is not None:
                tags += ("layer=" + state.hex(),)
            if sound:
                tags += (sound,)
            by_material[self.material(tid, tags)].append(f)
        prims = []
        for material, fs in by_material.items():
            remap, positions, ns, us, cs, tris = {}, [], [], [], [], []
            for f in fs:
                tri = []
                for v in part.faces[f].vertices:
                    if v not in remap:
                        remap[v] = len(positions)
                        positions.append(to_gltf(part.positions[v]))
                        if normals:
                            ns.append(to_gltf(normals[v]))
                        if uvs:
                            us.append(uvs[v])
                        cs.append(tuple(x / 128.0 for x in colours[v][:3]) + (colours[v][3] / 255.0,) if colours else (1.0, 1.0, 1.0, 1.0))
                    tri.append(remap[v])
                tris.append(tuple(tri))
            prims.append(dict(positions=positions, normals=ns or None, uvs=us or None, colours=cs, triangles=tris, material=material))
        if not prims:
            return None
        self.counts["faces"] += sum(len(p["triangles"]) for p in prims)
        return self.builder.mesh(name, prims, extras)

    # -- the level --

    def run(self):
        R = self.game.records
        self.read_textures()
        geo = R.Geometry.read(self.files["GEO"])
        ins = R.Instances.read(self.files["INS"])
        dat = R.SceneData.read(self.files["DAT"])
        objects = {o.header.id: o for o in geo.objects}
        by_id = {i.instance_id: i for i in ins.instances}
        # The dynamics definitions (ZDYN: DDEF's name hash first), each a prop object's by its name hash.
        definitions = {}
        for _, k in geo.other:
            if k.tag == b"ZDYN" and k.kids and k.kids[0].tag == b"DDEF":
                definitions.setdefault(struct.unpack_from("<I", k.kids[0].data)[0], [(c.tag, c.data) for c in k.kids[1:]])

        def object_extras(obj, inst):
            """The mesh's extras: the stock name hash where the TGHD's field cut the name (the hash is of the
            whole name), and a dynamics prop's definition with its class data and the instance's tags."""
            extras = {}
            if obj.header.name_hash != L.name_hash(obj.header.name):
                extras["name_hash"] = "0x%08X" % obj.header.name_hash
            if is_prop(obj):
                extras["dynamics"] = C.DynamicsDef(definitions[obj.header.name_hash], self.game.name, obj.type_data[4:],
                                                   inst.tags or b"").to_extras()
            return extras or None

        def is_prop(obj):
            return (obj.type_data is not None and obj.type_data[:4] == struct.pack("<I", C.DYNAMICS_OBJECT)
                    and obj.header.name_hash in definitions)

        def world_transform(inst):
            position, rotation = tuple(inst.position), tuple(inst.rotation)
            parent = by_id.get(inst.parent) if inst.parent else None
            for _ in range(16):
                if parent is None:
                    break
                position = tuple(a + b for a, b in zip(L.quat_rotate(parent.rotation, position), parent.position))
                rotation = G.quat_multiply(tuple(parent.rotation), rotation)
                parent = by_id.get(parent.parent) if parent.parent else None
            return position, rotation

        meshes = {}      # (object id, role) -> mesh index or None
        edges = defaultdict(list)   # (grind word, sound) -> [(a, b)] in glTF axes
        placed = Counter()
        for inst in ins.instances:
            obj = objects.get(inst.geometry_id)
            if obj is None or not obj.parts:
                self.counts["instances of no object, left out"] += 1
                continue
            shown = bool(inst.flags & C.INSTANCE_FLAG_SHOWN)
            sky = bool(inst.flags & C.INSTANCE_FLAG_SKY)
            collides = not sky and bool(inst.flags & W.Instance.COLLIDES)
            if not shown and not collides:
                self.counts["instances neither shown nor collidable (script objects, markers), left out"] += 1
                continue
            if inst.flags & 0x400:
                self.counts["mirrored instances (flag 0x400) placed unmirrored"] += 1
            drawn = next((p for p, lod, _ in obj.parts if lod is not None), obj.parts[0][0])
            drawn_table = next(t for p, _, t in obj.parts if p is drawn)
            hit = obj.collision_part() if collides else None
            hit_table = next((t for p, _, t in obj.parts if p is hit), None) if hit is not None else None
            position, rotation = world_transform(inst)
            node_name = inst.name or obj.header.name or "inst%08x" % inst.instance_id
            roles = []
            if is_prop(obj) and collides:
                # a dynamics prop: its one part, every face (the body collides for it in the game)
                roles.append(("dynamics", drawn, drawn_table, range(len(drawn.faces)), True))
                self.counts["dynamics props placed (#dynamics)"] += 1
            elif sky:
                roles.append(("sky", drawn, drawn_table, range(len(drawn.faces)), False))
            elif hit is None or hit is not drawn:
                if shown:
                    roles.append(("deco", drawn, drawn_table, range(len(drawn.faces)), False))
                if hit is not None:
                    roles.append(("hidden", hit, hit_table, [f for f, face in enumerate(hit.faces) if face.flags & W.Face.COLLIDES], True))
            else:
                solid = [f for f, face in enumerate(hit.faces) if face.flags & W.Face.COLLIDES]
                loose = [f for f, face in enumerate(hit.faces) if not face.flags & W.Face.COLLIDES]
                roles.append(("solid" if shown else "hidden", hit, hit_table, solid, True))
                if loose and shown:
                    roles.append(("deco", hit, hit_table, loose, False))
            for role, part, table, faces, with_tags in roles:
                key = (obj.header.id, role)
                if key not in meshes:
                    meshes[key] = self.mesh_of(obj.header.name or "obj%08x" % obj.header.id, part, table, faces, with_tags,
                                               object_extras(obj, inst)) if faces else None
                if meshes[key] is None:
                    continue
                tag = {"sky": "#sky", "deco": "#deco", "hidden": "#hidden", "solid": "", "dynamics": "#dynamics"}[role]
                self.builder.node(node_name + tag, meshes[key], to_gltf(position), quat_to_gltf(rotation))
                placed[role] += 1
            if hit is not None:
                self.grind_edges(hit, hit_table, position, rotation, edges)
        for (word, sound), segs in sorted(edges.items(), key=lambda kv: (kv[0][0], kv[0][1] or "")):
            points, index, lines = [], {}, []
            for a, b in segs:
                ends = []
                for p in (a, b):
                    if p not in index:
                        index[p] = len(points)
                        points.append(p)
                    ends.append(index[p])
                lines.append(tuple(ends))
            mesh = self.builder.mesh("grind_%s%s" % (word, "_" + sound if sound else ""), [dict(positions=points, lines=lines)])
            self.builder.node("grind_%s%s#grind=%s%s" % (word, "_" + sound if sound else "", word, "#" + sound if sound else ""), mesh)
            self.say("grind edges #grind=%s%s: %d" % (word, " #" + sound if sound else "", len(segs)))
        spawns = 0
        for p in dat.points:
            if p.kind != W.SPAWN:
                continue
            role = spawn_role(self.game.name, p.name)
            if role is None:
                self.counts["spawns with a name no other game reads (scripts', modes'): left out"] += 1
                continue
            name = "Respawn" if role == "respawn" else role
            self.builder.node("%s#spawn=%s" % (name, role), None, to_gltf(p.position), quat_to_gltf(p.orientation))
            spawns += 1
        cameras = sorted((p for p in dat.points if p.kind == C.CAMERA_POINT and p.name == C.IDLE_CAMERA and p.index > 0),
                         key=lambda p: p.index)
        lift = CAMERA_LIFTS[self.game.name]
        for n, p in enumerate(cameras):
            eye = (p.position[0], p.position[1] + lift, p.position[2])
            fov = p.tags[0] if p.tags and p.tags[0] else C.CAMERA_FOV
            self.builder.node("Idle%d#camera#fov=%d" % (n + 1, fov), None, to_gltf(eye), quat_to_gltf(p.orientation))
        if cameras:
            self.say("idle cameras: %d" % len(cameras))
        self.say("%s: %d objects, %d instances: %s; %d faces, %d materials, %d textures, %d spawns%s" %
                 (self.name, len(geo.objects), len(ins.instances), ", ".join("%s %d" % kv for kv in sorted(placed.items())),
                  self.counts.pop("faces", 0), len(self.materials), sum(1 for t in self.textures.values() if t is not None), spawns,
                  "" if any(p.kind == W.SPAWN and p.name == W.PLAYER_ONE for p in dat.points) else " (no PlayerOne)"))
        for what, n in sorted(self.counts.items()):
            self.say("%s: %d" % (what, n))
        return self.builder

    def grind_edges(self, part, table, position, rotation, edges):
        if part.edge_tags is None:
            return
        world = [to_gltf(tuple(a + b for a, b in zip(L.quat_rotate(rotation, p), position))) for p in part.positions]
        matis = [k for k in (table.kids if table is not None else []) if k.tag == b"MATI"]
        sounds = [self.material_texture(m)[1] for m in matis]
        for f, face in enumerate(part.faces):
            for k in range(3):
                at = 3 * f + k
                index = part.edge_tags.indices[at] if at < len(part.edge_tags.indices) else None
                if index is None:
                    continue
                r = part.edge_tags.records[index]
                if self.game.name == "ai":
                    if not r.flags & W.EdgeTag.GRINDABLE and r.kind != 3:
                        continue
                    word = AI_GRINDS.get(r.kind, "ledge")
                    sound = AI_EDGE_SOUNDS.get(r.material)
                else:
                    if not r.grindable:
                        continue
                    word = BIKE_GRINDS.get(r.link_kind, "ledge")
                    sound = sounds[face.material] if face.material < len(sounds) else None
                a, b = face.vertices[k], face.vertices[(k + 1) % 3]
                if max(a, b) < len(world):
                    edges[(word, sound)].append((world[a], world[b]))


def level_files(folder):
    """(the level's name, {EXT: bytes}) of a level folder: <name>.{GEO,INS,DAT,TXR}, the name the folder's."""
    name = os.path.basename(os.path.normpath(folder))
    names = {n.upper(): n for n in os.listdir(folder)}
    files = {}
    for ext in ("GEO", "INS", "DAT", "TXR"):
        real = names.get("%s.%s" % (name.upper(), ext))
        if real is None:
            raise FileNotFoundError("%s: no %s.%s" % (folder, name, ext))
        with open(os.path.join(folder, real), "rb") as f:
            files[ext] = f.read()
    return name.upper(), files


def export(game, folder, out=None):
    name, files = level_files(folder)
    ex = Export(game, files, name)
    builder = ex.run()
    if out:
        builder.write(out)
        ex.say("written %s (%d nodes, %d meshes, %d images, %d KB of buffer)" % (out, len(builder.nodes), len(builder.meshes), len(builder.images), len(builder.bin) // 1024))
    print("\n".join(ex.lines))
    return ex


def cmd_export(args):
    export(args.game, args.folder, args.out)


def cmd_info(args):
    export(args.game, args.folder, None)


def cmd_selftest(args):
    import tempfile
    import bmx_level_convert as BC
    with tempfile.TemporaryDirectory() as tmp:
        C.make_example(os.path.join(tmp, "model"))
        model = os.path.join(tmp, "model", "park.gltf")
        sky = os.path.join(tmp, "model", "sky.png")
        ok = True
        props = C.make_prop_example(os.path.join(tmp, "model"))
        for game_dir, game, make, folder in (("ai", "ai", lambda out: C.convert(model, out, sky=sky), "TUT"),
                                             ("bmx", "bmx", lambda out: BC.convert(model, out, sky=sky), "CHL_MULY"),
                                             ("ai-props", "ai", lambda out: C.convert(props, out), "TUT"),
                                             ("bmx-props", "bmx", lambda out: BC.convert(props, out), "CHL_MULY")):
            first = os.path.join(tmp, game_dir)
            make(first)
            again = os.path.join(tmp, game_dir + "-model", "level.gltf")
            export(game, os.path.join(first, "ASSETS", "LEVELS", folder), again)
            second = os.path.join(tmp, game_dir + "-again")
            if game == "ai":
                C.convert(again, second)
            else:
                BC.convert(again, second)
            R = GAMES[game].records

            def read(root):
                base = os.path.join(root, "ASSETS", "LEVELS", folder, folder)
                return (R.Geometry.read(open(base + ".GEO", "rb").read()), R.Instances.read(open(base + ".INS", "rb").read()),
                        R.SceneData.read(open(base + ".DAT", "rb").read()))

            def terms(level):
                geo, ins, dat = level
                faces = sum(len(o.parts[0][0].faces) for o in geo.objects)
                grinds = sum(1 for o in geo.objects for p, _, _ in o.parts if p.edge_tags for i in p.edge_tags.indices if i is not None)
                kinds = sorted({getattr(r, "kind", getattr(r, "surface_kind", None)) for o in geo.objects for p, _, _ in o.parts if p.face_tags
                                for r in p.face_tags.records})
                spawn = next((tuple(round(v, 4) for v in p.position) for p in dat.points if p.kind == W.SPAWN and p.name == W.PLAYER_ONE), None)
                skies = sum(1 for i in ins.instances if i.flags & C.INSTANCE_FLAG_SKY)
                globals_point = any(p.kind == W.GLOBALS for p in dat.points)
                # the props: each object's name, its whole name's hash (an exported name the field cut keeps the stock
                # word in the mesh's extras), its ZDYN, its instances
                props = C.prop_terms(geo, ins, R)
                return dict(faces=faces, grinds=grinds, kinds=kinds, spawn=spawn, skies=skies, globals=globals_point, props=props)
            a, b = terms(read(first)), terms(read(second))
            for key in a:
                passed = a[key] == b[key]
                print("%s: %s %s: %s vs %s" % ("ok" if passed else "FAIL", game_dir, key, a[key], b[key]))
                ok = ok and passed
        if not ok:
            raise SystemExit("zge_level_gltf.py: selftest failed")
        print("selftest ok")


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    for name, fn in (("export", cmd_export), ("info", cmd_info)):
        q = sub.add_parser(name)
        q.add_argument("folder")
        q.add_argument("--game", required=True, choices=sorted(GAMES))
        if name == "export":
            q.add_argument("--out", required=True)
        q.set_defaults(fn=fn)
    q = sub.add_parser("selftest")
    q.set_defaults(fn=cmd_selftest)
    args = p.parse_args(argv)
    args.fn(args)


if __name__ == "__main__":
    main(sys.argv[1:])
