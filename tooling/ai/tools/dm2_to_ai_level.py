#!/usr/bin/env python3
"""A Dave Mirra BMX 2 level into Aggressive Inline's level files (ZGE2 into ZGE3): a map across games, so the
collection's Custom Freeplay lists a map built for DM2 under AI too (docs/CUSTOM-LEVELS.md step 4).

    python3 -I dm2_to_ai_level.py convert FOLDER --out DIR [--theme LEWAREHO | --level WOODWARD] [--slot TUT] [--force]
        FOLDER is a DM2 map folder (a mod: FOLDER/ASSETS/EDITOR/<theme>/<theme>.{GEO,INS,DAT,TXR}, the form
        dm2_level_convert.py writes for the park slot; --level L reads FOLDER/ASSETS/LEVELS/L/L.* instead, a disc
        root works too). DIR gets AI's files as ai_level_convert.py writes them: DIR/ASSETS/LEVELS/<slot>/<slot>.{ZIT,
        GEO,INS,TXR,DAT}, the empty scripts and MOTIONS.ZAL, a map folder's layout for AI's slot TUT.
    python3 -I dm2_to_ai_level.py info FOLDER [--theme T | --level L]      what the converter sees, nothing written
    python3 -I dm2_to_ai_level.py selftest --disc <DM2 disc>              ai_level_convert's example park converted
                                                                           for DM2, then back to AI, read and checked

What crosses (dm2_level_write.py reads DM2's records, ai_level_convert.Converter builds AI's from the same
level terms it builds a glTF model into):
  objects       every instanced object's first part (its positions, normals, UVs, colours, faces by material):
                a Piece per instance's object, placed by the instance (a parent's transform composed). The
                park theme's own editor pieces, towers, markers and rails, which no instance places, are left
                out: they are the editor's catalogue, not the level.
  collision     an instance collides when its flags say so (bit 1 set, 0x10 clear) and its object has a
                collision tree; its faces keep their collide and two-sided bits.
  materials     a material is its texture (the MATI's layer names it by the texture's id) with the faces'
                surface kind: DM2's FaceTag12 surface kinds 1 (a quarter pipe: AI's kind 1 with TUT's launch
                push) and 2 (a spine) carry; 3 (DM2's kicker) has no AI kind and is reported, the face stays
                ground. Two-sided faces keep both sides.
  grind edges   DM2's EdgeTag8 grindable edges become AI's edge records: link kind 1 (a lip) AI kind 1,
                2 (a spine) kind 2, none kind 0 (a ledge or rail). DM2 keeps a grind's sound in the material,
                with values the game does not name: every AI edge writes sound 0 (concrete), reported.
  textures      each TXR texture's largest level (an 8- or 4-bit BMP; magenta is DM2's colour key: alpha 0)
                into AI's TXR through the converter's budgets.
  spawns        the DAT's Spawn points; PlayerOne when there is one, else the park's start cell from the
                folder's THEMES.TXT (the cell's centre, facing +z turned by its quarter turns), reported.
  zones         none (DM2's converted levels have none; AI's parts-in-view budget still applies).
  the sky       an instance with the sky bit (0x10, kNodeSky: every stock DM2 level's panorama_01 and the dome
                dm2_level_convert.py --sky writes) is AI's sky: a #sky placement, its part of AI's sky class
                (ai_level_convert.SkyForm); the level's Globals point gives AI's its clear and fog colours (DM2's
                words 4 and 8), with AI's own fog (AI_SKY). It neither collides nor is cut.

Python 3.8+, standard library only.
"""
import argparse
import math
import os
import re
import struct
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ai_level as L  # noqa: E402
import ai_level_convert as C  # noqa: E402
import ai_level_write as W  # noqa: E402
import dm2_level as D  # noqa: E402
import dm2_level_write as M  # noqa: E402
import zge_tags as T  # noqa: E402

KEY = (255, 0, 255)          # DM2's colour key (dm2_level_convert.py KEY)
CELL = 5.0                   # a park cell (edit_grid.cpp kCellSize)
PARK_LEVEL = 1               # the converted theme's cell level: the ground stands at y -1
KICKER = 3                   # DM2's kicker surface kind, no AI kind
AI_SURFACE_KINDS = {1: 1, 2: 2}   # DM2's FaceTag12 surface kind -> AI's FaceTag kind
AI_EDGE_KINDS = {0: 0, 1: 1, 2: 2}  # DM2's EdgeTag8 link kind -> AI's EdgeTag kind
MATI_SOUND = 0x18            # the material's sound byte (dm2_level_convert.py mati)


def decode_bmp(data):
    """A texture level's BMP as (width, height, rows of (r, g, b, 255), top first): dm2_level's 4- and 8-bit reader,
    and the 24- and 32-bit uncompressed forms a few stock textures use."""
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp in (4, 8):
        return D.decode_bmp(data, opaque=True)
    if bpp not in (24, 32):
        raise ValueError("%d-bit BMP" % bpp)
    pixels = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    stride = (w * bpp // 8 + 3) & ~3
    rows = []
    for y in range(abs(h)):
        at = pixels + (abs(h) - 1 - y if h > 0 else y) * stride
        row = data[at:at + stride]
        step = bpp // 8
        rows.append([(row[x * step + 2], row[x * step + 1], row[x * step], 255) for x in range(w)])
    return w, abs(h), rows


def quat_multiply(a, b):
    """a then b as rotations: the quaternion of b * a (x, y, z, w)."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (bw * ax + bx * aw + by * az - bz * ay,
            bw * ay - bx * az + by * aw + bz * ax,
            bw * az + bx * ay - by * ax + bz * aw,
            bw * aw - bx * ax - by * ay - bz * az)


def plain(stream, count, stride):
    """A part stream's bytes (an indexed one expanded), or None."""
    if stream is None:
        return None
    data = stream.expand() if isinstance(stream, W.IndexedStream) else stream
    if len(data) < count * stride:
        return None
    return data


def level_files(folder, theme=None, level=None):
    """(the slot's name, {EXT: bytes}) of a map folder's or disc root's DM2 level."""
    if level:
        name = level.upper()
        base = os.path.join(folder, "ASSETS", "LEVELS", name, name)
    else:
        name = (theme or "LEWAREHO").upper()
        base = os.path.join(folder, "ASSETS", "EDITOR", name, name)
    files = {}
    for ext in ("GEO", "INS", "DAT", "TXR"):
        path = base + "." + ext
        if not os.path.exists(path):
            raise FileNotFoundError("%s: no %s" % (folder, os.path.relpath(path, folder)))
        with open(path, "rb") as f:
            files[ext] = f.read()
    return name, files


def theme_start(folder, theme):
    """The park's start from the folder's THEMES.TXT block whose FILE is the theme: (position, quarter turns), or
    None. The cell's centre as cellWorldPosition places it (dm2_level_convert.py park_grid), facing +z."""
    path = os.path.join(folder, "ASSETS", "EDITOR", "THEMES.TXT")
    if not os.path.exists(path):
        return None
    text = open(path, "rb").read().decode("latin1")
    for block in re.finditer(r"(?ms)THEME\s*\{(.*?)^\s*\}", text):
        body = block.group(1)
        file = re.search(r'FILE\s+"([^"]*)"', body)
        if not file or file.group(1).upper() != theme.upper():
            continue
        cursor = re.search(r"CURSOR\s+(\d+)\s+(\d+)", body)
        width = re.search(r"WIDTH\s+(\d+)", body)
        height = re.search(r"HEIGHT\s+(\d+)", body)
        if not (cursor and width and height):
            return None
        column, row = int(cursor.group(1)), int(cursor.group(2))
        w, h = int(width.group(1)), int(height.group(1))
        return (column * CELL - (w - 1) * CELL / 2, -float(PARK_LEVEL), row * CELL - (h - 1) * CELL / 2), 0
    return None


class Dm2ToAi(C.Converter):
    """ai_level_convert's converter fed from a DM2 level's records instead of a glTF model."""

    def __init__(self, name, files, slot="TUT", level_name=None, force=False, start=None):
        super().__init__(None, slot, level_name or slot, force)
        self.source_name = name
        self.files = files
        self.start = start          # (position, turns) when the level has no PlayerOne spawn
        self.texture_names = {}     # texture id -> the converter's texture name
        self.surface_keys = {}      # (texture name, kind, two sided) -> surface index

    # -- the level read into level terms --

    def read_textures(self):
        """The TXR's textures by id; each is decoded when a placed object's material first names it (a park
        theme's TXR holds the theme's own textures too, which the level's objects do not use)."""
        self.library = {t.id: t for t in M.TextureLibrary.read(self.files["TXR"]).textures}

    def texture_named(self, tid):
        if tid in self.texture_names:
            return self.texture_names[tid]
        t = self.library[tid]
        if not t.levels:
            self.report.did("texture %s has no levels: left out" % t.name)
            self.texture_names[tid] = None
            return None
        w, h, rows = decode_bmp(t.levels[0])
        rows = [[(0, 0, 0, 0) if px[:3] == KEY else px for px in row] for row in rows]
        name = C.short(t.name or ("tex%08x" % t.id), self.names)
        self.textures[name] = C.Texture(name, w, h, rows)
        self.texture_names[tid] = name
        return name

    def material_texture(self, mati):
        """The texture a MATI's layer names (a word of the record equal to a texture's id), and its sound byte."""
        data = mati.data if mati is not None else b""
        sound = data[MATI_SOUND] if len(data) > MATI_SOUND else 0
        for at in range(0, len(data) - 3, 4):
            tid = struct.unpack_from("<I", data, at)[0]
            if tid in self.library:
                return self.texture_named(tid), sound
        return None, sound

    def surface_for(self, texture, kind, two_sided):
        key = (texture, kind, two_sided)
        if key in self.surface_keys:
            return self.surface_keys[key]
        if texture is None:
            texture = C.short("untextured", self.names)
            if texture not in self.textures:
                self.textures[texture] = C.Texture(texture, 8, 8, [[(128, 128, 128, 255)] * 8 for _ in range(8)])
        name = texture if kind == 0 and not two_sided else "%s_%s%s" % (texture, {1: "qp", 2: "spine"}.get(kind, "k%d" % kind), "_2s" if two_sided else "")
        s = C.Surface(name[:C.NAME_BYTES], texture, AI_SURFACE_KINDS.get(kind, 0), faces_two_sided=two_sided)   # (the faces' flag: collision)
        self.surfaces.append(s)
        self.surface_keys[key] = len(self.surfaces) - 1
        return self.surface_keys[key]

    def piece_of(self, obj, collides, sky=False):
        """The object's first part as a Piece (surfaces made as the faces name their materials)."""
        part, _, table = obj.parts[0]
        count = len(part.positions)
        normals = plain(part.normals, count, 12)
        uvs = plain(part.uvs, count, 8)
        colours = plain(part.colours, count, 4)
        matis = [k for k in (table.kids if table is not None else []) if k.tag == b"MATI"]
        textures = [self.material_texture(m) for m in matis]
        kinds = [None] * len(part.faces)
        if part.face_tags is not None:
            for f, index in enumerate(part.face_tags.indices[:len(part.faces)]):
                if index is not None:
                    kinds[f] = part.face_tags.records[index].surface_kind
        faces = []
        kickers = 0
        for f, face in enumerate(part.faces):
            material = face.material
            texture, _ = textures[material] if material < len(textures) else (None, 0)
            kind = kinds[f] or 0
            if kind == KICKER:
                kickers += 1
                kind = 0
            elif kind not in AI_SURFACE_KINDS:
                kind = 0
            two_sided = bool(face.flags & W.Face.TWO_SIDED)
            a, b, c = face.vertices
            if max(a, b, c) >= count or len({a, b, c}) < 3:
                continue
            faces.append((self.surface_for(texture, kind, two_sided), a, b, c))
        if kickers:
            self.report.did("%s: %d kicker faces (DM2's surface kind 3) have no AI kind: ground" % (obj.header.name, kickers))
        positions = [tuple(p) for p in part.positions]
        piece = C.Piece(obj.header.name or ("obj%d" % obj.header.id), positions,
                        [struct.unpack_from("<3f", normals, 12 * i) for i in range(count)] if normals else [None] * count,
                        [struct.unpack_from("<2f", uvs, 8 * i) for i in range(count)] if uvs else [(0.0, 0.0)] * count,
                        [tuple(colours[4 * i:4 * i + 4]) for i in range(count)] if colours else [(0x80, 0x80, 0x80, 0xFF)] * count,
                        faces, collides, sky)
        if any(n is None for n in piece.normals):
            C.Converter.make_pieces  # (the glTF path's face-normal fill is inline there; the same here)
            from collections import defaultdict
            acc = defaultdict(list)
            for _, a, b, c in faces:
                fn = C.face_normal(positions[a], positions[b], positions[c])
                for v in (a, b, c):
                    acc[v].append(fn)
            for i, n in enumerate(piece.normals):
                if n is None:
                    total = [sum(fn[k] for fn in acc[i]) for k in range(3)] if acc[i] else [0.0, -1.0, 0.0]
                    length = math.sqrt(sum(v * v for v in total)) or 1.0
                    piece.normals[i] = tuple(v / length for v in total)
        pieces = self.cut(piece)
        if sky and len(pieces) > 1:
            raise ValueError("sky %s has %d faces: the world keeps one sky node, and one object holds at most %d" % (piece.name, len(faces), C.FACES_PER_OBJECT))
        if len(pieces) > 1:
            self.report.did("object %s cut into %d pieces: %d faces over %d an object" % (piece.name, len(pieces), len(faces), C.FACES_PER_OBJECT))
        used = set()
        for i, p in enumerate(pieces):
            p.name = C.distinct(piece.name if i == 0 else "%s_%d" % (piece.name, i + 1), used)
        if len(pieces) == 1:
            pieces[0].name_hash = obj.header.name_hash   # the stock word: the hash of the whole name, which the field may cut
        return pieces, part

    def edge_marks_of(self, part, world):
        """The part's grindable edges in the world as marks (ai_level_convert's tag_edges matches them back)."""
        if part.edge_tags is None:
            return 0
        n = 0
        for f, face in enumerate(part.faces):
            for k in range(3):
                index = part.edge_tags.indices[3 * f + k] if 3 * f + k < len(part.edge_tags.indices) else None
                if index is None:
                    continue
                record = part.edge_tags.records[index]
                if not record.grindable:
                    continue
                a, b = face.vertices[k], face.vertices[(k + 1) % 3]
                if max(a, b) >= len(world):
                    continue
                self.edge_marks.append(C.EdgeMark(world[a], world[b], AI_EDGE_KINDS.get(record.link_kind, 0), W.EdgeTag.GRINDABLE, 0))
                n += 1
        return n

    def read_model(self):
        self.read_textures()
        geo = M.Geometry.read(self.files["GEO"])
        ins = M.Instances.read(self.files["INS"])
        dat = M.SceneData.read(self.files["DAT"])
        objects = {o.header.id: o for o in geo.objects}
        by_id = {i.instance_id: i for i in ins.instances}

        def world_transform(inst):
            position, rotation = tuple(inst.position), tuple(inst.rotation)
            parent = by_id.get(inst.parent) if inst.parent else None
            for _ in range(16):
                if parent is None:
                    break
                position = tuple(a + b for a, b in zip(L.quat_rotate(parent.rotation, position), parent.position))
                rotation = quat_multiply(rotation, tuple(parent.rotation))
                parent = by_id.get(parent.parent) if parent.parent else None
            return position, rotation

        placed = 0
        edges = 0
        for inst in ins.instances:
            obj = objects.get(inst.geometry_id)
            if obj is None or not obj.parts:
                self.report.did("instance %s: no object %d: left out" % (inst.name, inst.geometry_id))
                continue
            sky = (inst.flags & C.INSTANCE_FLAG_SKY) != 0
            collides = not sky and (inst.flags & W.Instance.COLLIDES) != 0 and obj.collision is not None
            key = (obj.header.id, collides, sky)
            if key not in self.pieces:
                self.pieces[key], part = self.piece_of(obj, collides, sky)
            else:
                part = obj.parts[0][0]
            position, rotation = world_transform(inst)
            name = C.short(inst.name or obj.header.name or ("inst%d" % inst.instance_id), self.names)
            self.placements.append(C.Placement(name, key, position, rotation, collides, None, sky=sky))
            if sky:
                self.report.say("instance %s: the sky" % name)
            placed += 1
            if collides:
                world = [tuple(a + b for a, b in zip(L.quat_rotate(rotation, p), position)) for p in part.positions]
                edges += self.edge_marks_of(part, world)
        left_out = len(geo.objects) - len({k[0] for k in self.pieces})
        self.report.say("%s: %d of %d objects placed by %d instances (%d objects no instance places left out), %d grind edges" %
                        (self.source_name, len({k[0] for k in self.pieces}), len(geo.objects), placed, left_out, edges))
        if edges:
            self.report.did("every grind edge's sound written 0 (concrete): DM2 keeps the sound in the material, with values it does not name")
        globals_point = next((p for p in dat.points if p.kind == W.GLOBALS and len(p.tags) >= 12), None)
        if globals_point is not None and any(pl.sky for pl in self.placements):
            clear, fog = tuple(globals_point.tags[4:7]), tuple(globals_point.tags[8:11])
            self.globals = self.SKY.globals_for(C.SkyLook(zenith=clear, horizon=fog))
        for p in dat.points:
            if p.kind != W.SPAWN:
                continue
            name = "PlayerOne" if p.name == W.PLAYER_ONE else "Spawn%08X" % p.name
            self.spawns.append((name, tuple(p.position), tuple(p.orientation)))
        if not any(n == "PlayerOne" for n, _, _ in self.spawns):
            if self.start is not None:
                position, turns = self.start
                half = math.radians(90.0 * turns) / 2.0
                self.spawns.append(("PlayerOne", position, (0.0, math.sin(half), 0.0, math.cos(half))))
                self.report.did("no PlayerOne spawn in the level: the park's start cell (%.1f, %.1f, %.1f), %d quarter turns" % (*position, turns))
            else:
                self.report.error("no PlayerOne spawn and no park start: the skater has nowhere to start")


def convert(folder, out_dir=None, theme=None, level=None, slot="TUT", force=False):
    name, files = level_files(folder, theme, level)
    start = theme_start(folder, name) if not level else None
    conv = Dm2ToAi(name, files, slot, slot, force, start)
    out = conv.build()
    report = conv.report
    if report.errors and not force and out_dir:
        print(report.text())
        raise SystemExit("dm2_to_ai_level.py: over budget or incomplete (see above; --force writes anyway)")
    if out_dir:
        target = os.path.join(out_dir, "ASSETS", "LEVELS", conv.slot)
        os.makedirs(target, exist_ok=True)
        written = {"%s.%s" % (conv.slot, ext): data for ext, data in out.items()}
        written["%s.BIN" % conv.slot] = C.empty_script()
        written["%sT.BIN" % conv.slot] = C.empty_script()
        written["MOTIONS.ZAL"] = C.empty_library()
        for file, data in written.items():
            with open(os.path.join(target, file), "wb") as f:
                f.write(data)
        report.say("written to %s: %s" % (target, ", ".join("%s %d B" % (n, len(d)) for n, d in written.items())))
    print(report.text())
    return conv, out


def cmd_convert(args):
    convert(args.folder, args.out, args.theme, args.level, args.slot, args.force)


def cmd_info(args):
    convert(args.folder, None, args.theme, args.level, args.slot, True)


def cmd_selftest(args):
    import tempfile
    import dm2_level_convert as DC
    with tempfile.TemporaryDirectory() as tmp:
        model = os.path.join(tmp, "model")
        C.make_example(model)
        dm2 = os.path.join(tmp, "dm2")
        DC.convert(os.path.join(model, "park.gltf"), dm2, disc=args.disc)
        ai = os.path.join(tmp, "ai")
        conv, out = convert(dm2, ai)
        geo = W.Geometry.read(out["GEO"])
        ins = W.Instances.read(out["INS"])
        dat = W.SceneData.read(out["DAT"])
        # The same park straight from the model, for the counts.
        direct = C.Converter(C.G.read_model(os.path.join(model, "park.gltf")))
        direct_files = direct.build()
        dgeo = W.Geometry.read(direct_files["GEO"])
        faces = sum(len(p.faces) for o in geo.objects for p, _, _ in o.parts)
        dfaces = sum(len(p.faces) for o in dgeo.objects for p, _, _ in o.parts)
        grinds = sum(1 for o in geo.objects for p, _, _ in o.parts if p.edge_tags for i in p.edge_tags.indices if i is not None)
        dgrinds = sum(1 for o in dgeo.objects for p, _, _ in o.parts if p.edge_tags for i in p.edge_tags.indices if i is not None)
        kinds = sorted({r.kind for o in geo.objects for p, _, _ in o.parts if p.face_tags for r in p.face_tags.records})
        dkinds = sorted({r.kind for o in dgeo.objects for p, _, _ in o.parts if p.face_tags for r in p.face_tags.records})
        checks = [
            ("the level's own objects, none of the theme's", len(geo.objects) == len(dgeo.objects), "%d vs %d" % (len(geo.objects), len(dgeo.objects))),
            ("the same faces", faces == dfaces, "%d vs %d" % (faces, dfaces)),
            ("the same instances", len(ins.instances) == len(W.Instances.read(direct_files["INS"]).instances), "%d" % len(ins.instances)),
            ("grind edges carried", grinds == dgrinds, "%d vs %d" % (grinds, dgrinds)),
            ("the same surface kinds tagged", kinds == dkinds, "%s vs %s" % (kinds, dkinds)),
            ("a PlayerOne spawn", any(p.kind == W.SPAWN and p.name == W.PLAYER_ONE for p in dat.points), ""),
            ("collision trees on the collidable objects", all((o.collision is not None) == any(f.flags & W.Face.COLLIDES for p, _, _ in o.parts for f in p.faces) for o in geo.objects), ""),
            ("textures carried", len(conv.textures) >= 3, "%d" % len(conv.textures)),
        ]
        # with a sky: DM2's dome (its instance's sky bit) becomes AI's (the sky class, the sky bit), its colours AI's Globals
        dm2sky = os.path.join(tmp, "dm2sky")
        DC.convert(os.path.join(model, "park.gltf"), dm2sky, disc=args.disc, sky=os.path.join(model, "sky.png"))
        sconv, sout = convert(dm2sky, None)
        sgeo, sins = W.Geometry.read(sout["GEO"]), W.Instances.read(sout["INS"])
        skies = [i for i in sins.instances if i.flags & C.INSTANCE_FLAG_SKY]
        sky_parts = [o.parts[0][0].flags for o in sgeo.objects if skies and o.header.id == skies[0].geometry_id]
        point = next((p for p in W.SceneData.read(sout["DAT"]).points if p.kind == W.GLOBALS), None)
        dm2_globals = next(p for p in M.SceneData.read(open(os.path.join(dm2sky, "ASSETS", "EDITOR", "LEWAREHO", "LEWAREHO.DAT"), "rb").read()).points if p.kind == W.GLOBALS)
        checks += [
            ("the sky: one instance, AI's sky form", len(skies) == 1 and skies[0].flags == C.AI_SKY.instance_flags and sky_parts == [C.AI_SKY.part_flags],
             "%s %s" % ([hex(i.flags) for i in skies], sky_parts)),
            ("the sky's colours in AI's Globals", point is not None and point.tags[4:7] == dm2_globals.tags[4:7] and point.tags[8:11] == dm2_globals.tags[8:11]
             and len(point.tags) == 28, point.tags.hex() if point else "none"),
        ]
        ok = True
        for what, passed, detail in checks:
            print("%s: %s %s" % ("ok" if passed else "FAIL", what, detail))
            ok = ok and passed
        if not ok:
            raise SystemExit("dm2_to_ai_level.py: selftest failed")
        print("selftest ok")


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    for name, fn in (("convert", cmd_convert), ("info", cmd_info)):
        q = sub.add_parser(name)
        q.add_argument("folder")
        q.add_argument("--out", default=None)
        q.add_argument("--theme", default=None, help="the park theme's folder (default LEWAREHO)")
        q.add_argument("--level", default=None, help="a level folder instead of a park theme")
        q.add_argument("--slot", default="TUT")
        q.add_argument("--force", action="store_true")
        q.set_defaults(fn=fn)
    q = sub.add_parser("selftest")
    q.add_argument("--disc", required=True)
    q.set_defaults(fn=cmd_selftest)
    args = p.parse_args(argv)
    args.fn(args)


if __name__ == "__main__":
    main(sys.argv[1:])
